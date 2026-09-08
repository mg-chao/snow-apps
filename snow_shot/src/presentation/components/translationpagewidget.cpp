#include "snow_shot/presentation/components/translationpagewidget.h"

#include "snow_shot/network/snowshotapiclient.h"
#include "snow_shot/presentation/components/icons/snowshoticons.h"
#include "snow_shot/presentation/components/pagecontainerwidget.h"
#include "snow_shot/presentation/languagemanager.h"
#include "snow_shot/presentation/styles/thememanager.h"
#include "snow_shot/presentation/translationlanguages.h"
#include "snow_shot/presentation/translationpagecontroller.h"
#include "snow_shot/storage/applicationstorage.h"
#include "antd_icons.h"
#include "widgets/alert.h"
#include "widgets/button.h"
#include "widgets/input_text_edit.h"
#include "widgets/popover.h"
#include "widgets/select.h"
#include "widgets/scroll_area.h"

#include <QApplication>
#include <QClipboard>
#include <QGridLayout>
#include <QHideEvent>
#include <QInputMethodEvent>
#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
#include <QScopedValueRollback>
#include <QScrollBar>
#include <QTextCursor>
#include <QTextBlockFormat>
#include <QTimer>
#include <QVBoxLayout>

#include <algorithm>

namespace {
using namespace adqt::widgets;
namespace translation = snow_shot::presentation;
namespace outlined = adqt::icons::antd::outlined;

class TranslationTextPolicy final : public AdInputTextPolicy {
  public:
    using AdInputTextPolicy::AdInputTextPolicy;
    int characterCount(const QString& text) const override {
        int count = 0;
        for (qsizetype index = 0; index < text.size(); ++index, ++count) {
            if (text.at(index).isHighSurrogate() && index + 1 < text.size() &&
                text.at(index + 1).isLowSurrogate()) {
                ++index;
            }
        }
        return count;
    }
    QString normalizeText(const QString& text, int maximum) const override {
        qsizetype end = 0;
        for (int count = 0; end < text.size() && count < maximum; ++count, ++end) {
            if (text.at(end).isHighSurrogate() && end + 1 < text.size() &&
                text.at(end + 1).isLowSurrogate()) {
                ++end;
            }
        }
        return text.left(end);
    }
};

AdButton* iconButton(const adqt::icons::IconRef& icon, QWidget* parent) {
    auto* button = new AdButton(parent);
    button->setIconRef(icon);
    button->setButtonStyle(AdButton::ButtonStyle::Text);
    button->setShape(AdButton::Shape::Circle);
    button->setFocusPolicy(Qt::StrongFocus);
    return button;
}
} // namespace

TranslationPageWidget::TranslationPageWidget(QWidget* parent, SnowShotApiClient* client,
                                             int debounceMilliseconds)
    : QWidget(parent), m_scheme(translation::styles::ThemeManager::instance().themeColorScheme()) {
    setObjectName(QStringLiteral("translationPage"));
    setAutoFillBackground(false);
    const auto& metric = m_scheme.metricAlias;
    if (client == nullptr) {
        client = new SnowShotApiClient(SnowShotApiClient::configuredBaseUrl(), this);
    }
    m_controller = new translation::TranslationPageController(
        *client, snow_shot::storage::ApplicationStorage::instance().configuration(),
        translation::LanguageManager::instance().currentLocale(), this, debounceMilliseconds);
    auto* root = new QVBoxLayout(this);
    root->setContentsMargins(0, 0, 0, 0);
    m_container = new PageContainerWidget(metric, this);
    // Keep the toolkit's focus and click effects within the scrolling content surface.
    m_container->scrollArea()->viewport()->setProperty("adqt.interaction.surface", true);
    root->addWidget(m_container);
    auto* body = m_container->contentWidget();
    body->installEventFilter(this);
    auto* layout = m_container->contentLayout();
    layout->setSpacing(metric.paddingSM);

    m_error = new AdAlert(body);
    m_error->setObjectName(QStringLiteral("translationError"));
    m_error->setSeverity(AdAlert::Severity::Error);
    m_retry = new AdButton(m_error);
    m_retry->setObjectName(QStringLiteral("translationRetry"));
    m_retry->setButtonStyle(AdButton::ButtonStyle::Text);
    m_retry->setAccentRole(AdButton::AccentRole::Primary);
    m_error->setActionsWidget(m_retry);
    layout->addWidget(m_error);

    m_form = new QWidget(body);
    m_formLayout = new QGridLayout(m_form);
    m_formLayout->setContentsMargins(0, 0, 0, 0);
    for (int index = 0; index < 3; ++index) {
        auto* field = new QWidget(m_form);
        m_fields[index] = field;
        auto* fieldLayout = new QVBoxLayout(field);
        fieldLayout->setContentsMargins(0, 0, 0, 0);
        fieldLayout->setSpacing(metric.paddingXXS);
        m_labels[index] = new QLabel(field);
        fieldLayout->addWidget(m_labels[index]);
        auto* row = new QHBoxLayout;
        row->setSpacing(metric.paddingXXS);
        auto* select = new AdSelect(field);
        m_selects[index] = select;
        select->setVariant(AdSelect::Variant::Underlined);
        select->setSearchEnabled(true);
        select->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Fixed);
        m_labels[index]->setBuddy(select);
        row->addWidget(select, 1);
        if (index == 0) {
            m_swap = iconButton(outlined::Swap(), field);
            m_swap->setObjectName(QStringLiteral("translationSwap"));
            row->addWidget(m_swap);
        }
        fieldLayout->addLayout(row);
        connect(select, &AdSelect::currentValueChanged, this, [this, index]() {
            if (!m_syncing) {
                m_controller->setPreferences(m_selects[0]->currentValue().toString(),
                                             m_selects[1]->currentValue().toString(),
                                             index == 2 ? m_selects[2]->currentValue().toString()
                                                        : m_controller->preferences().modelId);
            }
        });
    }
    m_selects[0]->setObjectName(QStringLiteral("translationSourceLanguage"));
    m_selects[1]->setObjectName(QStringLiteral("translationTargetLanguage"));
    m_selects[2]->setObjectName(QStringLiteral("translationService"));
    layout->addWidget(m_form);

    auto* editors = new QWidget(body);
    m_editorsLayout = new QGridLayout(editors);
    m_editorsLayout->setContentsMargins(0, 0, 0, 0);
    m_source = new AdTextEdit(editors);
    m_source->setObjectName(QStringLiteral("translationSourceText"));
    m_source->setAcceptRichText(false);
    m_source->setTextPolicy(new TranslationTextPolicy(m_source));
    m_source->setMaximumCharacterCount(5000);
    m_source->setCountVisible(true);
    m_source->setAllowClear(true);
    m_source->setHeightMode(AdTextEdit::HeightMode::FixedGeometry);
    m_source->setMinimumVisibleRows(12);
    m_source->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Expanding);
    m_resultPane = new QWidget(editors);
    m_resultPane->installEventFilter(this);
    auto* resultLayout = new QVBoxLayout(m_resultPane);
    resultLayout->setContentsMargins(0, 0, 0, 0);
    m_result = new AdTextEdit(m_resultPane);
    m_result->setObjectName(QStringLiteral("translationResultText"));
    m_result->setAcceptRichText(false);
    m_result->setReadOnly(true);
    m_result->setVariant(AdTextEdit::Variant::Filled);
    m_result->setHeightMode(AdTextEdit::HeightMode::FixedGeometry);
    m_result->setMinimumVisibleRows(12);
    m_result->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Expanding);
    resultLayout->addWidget(m_result);
    m_resultCopy = iconButton(outlined::Copy(), m_resultPane);
    m_resultCopy->setObjectName(QStringLiteral("translationResultCopy"));
    layout->addWidget(editors, 1);

    m_status = new QLabel(this);
    m_status->setObjectName(QStringLiteral("translationStatus"));

    m_floating = new AdButton(this);
    m_floating->setObjectName(QStringLiteral("translationActions"));
    m_floating->setIconRef(translation::icons::custom::outlined::Keyboard());
    m_floating->setShape(AdButton::Shape::Circle);
    m_floating->setFocusPolicy(Qt::StrongFocus);
    m_popover = new AdPopover(this);
    m_popover->setObjectName(QStringLiteral("translationActionsPopover"));
    m_popover->setSourceWidget(m_floating);
    m_popover->setPlacement(AdPopover::Placement::TopRight);
    m_popover->setTriggers(AdPopover::Trigger::Hover | AdPopover::Trigger::Click);
    m_popover->setArrowPointAtCenter(true);
    m_actions = new QWidget;
    auto* actionsLayout = new QVBoxLayout(m_actions);
    actionsLayout->setContentsMargins(0, 0, 0, 0);
    actionsLayout->setSpacing(metric.paddingXXS);
    m_copy = new AdButton(m_actions);
    m_copy->setObjectName(QStringLiteral("translationCopy"));
    m_copyHide = new AdButton(m_actions);
    m_copyHide->setObjectName(QStringLiteral("translationCopyAndHide"));
    for (auto* button : {m_copy, m_copyHide}) {
        button->setButtonStyle(AdButton::ButtonStyle::Text);
        button->setFocusPolicy(Qt::StrongFocus);
        actionsLayout->addWidget(button);
    }
    m_popover->setContentWidget(m_actions);

    connect(m_swap, &AdButton::clicked, m_controller,
            &translation::TranslationPageController::swapLanguages);
    connect(m_source, &AdTextEdit::plainTextChanged, m_controller,
            &translation::TranslationPageController::setSourceText);
    connect(m_retry, &AdButton::clicked, m_controller,
            &translation::TranslationPageController::retry);
    connect(m_copy, &AdButton::clicked, this, [this]() { copyResult(false); });
    connect(m_copyHide, &AdButton::clicked, this, [this]() { copyResult(true); });
    connect(m_resultCopy, &AdButton::clicked, this, [this]() { copyResult(false); });
    connect(m_controller, &translation::TranslationPageController::stateChanged, this,
            &TranslationPageWidget::syncState);
    qApp->installEventFilter(this);
    retranslateUi();
    applyTheme(m_scheme);
    m_controller->activate();
    QTimer::singleShot(0, this, [this]() {
        if (m_active && isVisible()) {
            m_source->setFocus(Qt::OtherFocusReason);
        }
    });
}

TranslationPageWidget::~TranslationPageWidget() {
    deactivate();
}

void TranslationPageWidget::deactivate() {
    m_active = false;
    qApp->removeEventFilter(this);
    dismissPopups();
    m_controller->deactivate();
}

void TranslationPageWidget::dismissPopups() {
    m_popover->hide();
    for (auto* select : m_selects) {
        select->setPopupVisible(false);
    }
}

void TranslationPageWidget::hideEvent(QHideEvent* event) {
    dismissPopups();
    QWidget::hideEvent(event);
}

void TranslationPageWidget::copyResult(bool hideWindow) {
    if (!m_active || m_controller->resultText().isEmpty()) {
        return;
    }
    QApplication::clipboard()->setText(m_controller->resultText());
    m_popover->hide();
    if (hideWindow) {
        emit hideWindowRequested();
    }
}

bool TranslationPageWidget::ownsFocusWidget(const QWidget* widget) const {
    return widget != nullptr && (widget->window() == window() || isAncestorOf(widget) ||
                                 widget == m_actions || m_actions->isAncestorOf(widget));
}

bool TranslationPageWidget::eventFilter(QObject* watched, QEvent* event) {
    if ((watched == m_container->contentWidget() || watched == m_resultPane) &&
        event->type() == QEvent::Resize && !m_layoutQueued) {
        m_layoutQueued = true;
        QTimer::singleShot(0, this, [this]() {
            m_layoutQueued = false;
            updateLayout();
        });
    }
    if (m_active && watched == m_source && event->type() == QEvent::InputMethod) {
        auto* input = static_cast<QInputMethodEvent*>(event);
        m_controller->setComposing(!input->preeditString().isEmpty());
    }
    auto* widget = qobject_cast<QWidget*>(watched);
    if (!m_active || !isVisible() || !ownsFocusWidget(widget)) {
        return QWidget::eventFilter(watched, event);
    }
    if (event->type() == QEvent::KeyPress || event->type() == QEvent::ShortcutOverride) {
        auto* key = static_cast<QKeyEvent*>(event);
        const bool copy = key->matches(QKeySequence::Copy);
        const bool copyHide = key->key() == Qt::Key_Q && key->modifiers() == Qt::ControlModifier;
        if (copy) {
            if (const auto* editor = qobject_cast<QTextEdit*>(widget);
                editor != nullptr && editor->textCursor().hasSelection()) {
                return false;
            }
            if (const auto* editor = qobject_cast<QLineEdit*>(widget);
                editor != nullptr && editor->hasSelectedText()) {
                return false;
            }
        }
        if (copy || copyHide) {
            key->accept();
            if (event->type() == QEvent::KeyPress) {
                copyResult(copyHide);
            }
            return true;
        }
        if (event->type() == QEvent::KeyPress && key->key() == Qt::Key_Escape &&
            m_popover->isVisible()) {
            m_popover->hide();
            m_floating->setFocus();
            return true;
        }
        if (event->type() == QEvent::KeyPress && widget == m_floating &&
            (key->key() == Qt::Key_Return || key->key() == Qt::Key_Enter ||
             key->key() == Qt::Key_Space || key->key() == Qt::Key_Up ||
             key->key() == Qt::Key_Down)) {
            m_popover->show();
            if (m_copy->isEnabled()) {
                m_copy->setFocus();
            }
            return true;
        }
    }
    return QWidget::eventFilter(watched, event);
}

void TranslationPageWidget::updateResult(const QString& text) {
    const QString previous = m_result->toPlainText();
    if (previous == text) {
        return;
    }
    if (text.isEmpty() || !text.startsWith(previous)) {
        m_result->setPlainText(text);
        return;
    }
    auto* scroll = m_result->verticalScrollBar();
    const int position = scroll->value();
    const bool atBottom = position == scroll->maximum();
    const int anchor = m_result->textCursor().anchor();
    const int cursorPosition = m_result->textCursor().position();
    QTextCursor append(m_result->document());
    append.movePosition(QTextCursor::End);
    auto lastBlockFormat = append.blockFormat();
    lastBlockFormat.setBottomMargin(0);
    append.setBlockFormat(lastBlockFormat);
    append.insertText(text.mid(previous.size()));
    // Reserve a final line of space for the copy affordance without adding copyable whitespace.
    lastBlockFormat = append.blockFormat();
    lastBlockFormat.setBottomMargin(36);
    append.setBlockFormat(lastBlockFormat);
    // QTextCursor copies track insertions, so preserve numeric endpoints rather than a live cursor.
    QTextCursor selection(m_result->document());
    selection.setPosition(anchor);
    selection.setPosition(cursorPosition, QTextCursor::KeepAnchor);
    m_result->setTextCursor(selection);
    scroll->setValue(atBottom && !selection.hasSelection() ? scroll->maximum() : position);
}

void TranslationPageWidget::syncState() {
    QScopedValueRollback guard(m_syncing, true);
    const auto& preferences = m_controller->preferences();
    m_selects[0]->setCurrentValue(preferences.sourceLanguage);
    m_selects[1]->setCurrentValue(preferences.targetLanguage);
    QVector<AdSelect::Option> services;
    for (const auto& model : m_controller->models()) {
        if (!model.supportsVision) {
            services.push_back({model.id, model.name, false,
                                model.translationMode == QStringLiteral("default")
                                    ? tr("General Models")
                                    : tr("Translation Models")});
        }
    }
    // Preserve an open selector's search and focus while output tokens arrive.
    const auto previousServices = m_selects[2]->options();
    if (!std::equal(previousServices.cbegin(), previousServices.cend(), services.cbegin(),
                    services.cend(),
                    [](const AdSelect::Option& first, const AdSelect::Option& second) {
                        return first.value == second.value && first.label == second.label &&
                               first.group == second.group;
                    })) {
        m_selects[2]->setOptions(services);
    }
    m_selects[2]->setCurrentValue(preferences.modelId);
    m_selects[2]->setLoading(m_controller->loadingModels());
    m_selects[2]->setEnabled(!services.isEmpty() && !m_controller->loadingModels());
    m_swap->setEnabled(preferences.sourceLanguage != QStringLiteral("auto") &&
                       preferences.sourceLanguage != preferences.targetLanguage);
    updateResult(m_controller->resultText());
    const bool hasResult = !m_controller->resultText().isEmpty();
    m_copy->setEnabled(hasResult);
    m_copyHide->setEnabled(hasResult);
    m_resultCopy->setEnabled(hasResult);
    m_resultCopy->setVisible(hasResult);
    const QString error = m_controller->errorText();
    m_error->setText(error);
    m_error->setVisible(!error.isEmpty());
    m_retry->setEnabled(!m_controller->loadingModels() && !m_controller->translating());
    m_status->setText(m_controller->loadingModels() ? tr("Loading translation services…")
                      : m_controller->translating() ? tr("Translating…")
                                                    : QString());
    m_status->setAccessibleName(m_status->text());
    m_status->setVisible(!m_status->text().isEmpty());
}

void TranslationPageWidget::retranslateUi() {
    QScopedValueRollback guard(m_syncing, true);
    setAccessibleName(tr("Translation"));
    const QString labels[]{tr("Source language"), tr("Target language"), tr("Translation service")};
    for (int index = 0; index < 3; ++index) {
        m_labels[index]->setText(labels[index]);
        m_selects[index]->setAccessibleName(labels[index]);
    }
    QVector<AdSelect::Option> sourceOptions{{QStringLiteral("auto"), tr("Auto Detect")}};
    QVector<AdSelect::Option> targetOptions;
    for (const auto& language : translation::translationLanguages()) {
        const QString code = QString::fromLatin1(language.code);
        const AdSelect::Option option{code, translation::translationLanguageName(code)};
        sourceOptions.push_back(option);
        targetOptions.push_back(option);
    }
    m_selects[0]->setOptions(sourceOptions);
    m_selects[1]->setOptions(targetOptions);
    m_selects[2]->setPlaceholder(tr("Select a service"));
    m_source->setPlaceholderText(tr("Enter text to translate"));
    m_source->setAccessibleName(tr("Source text"));
    m_result->setPlaceholderText(tr("Translation appears here"));
    m_result->setAccessibleName(tr("Translated text"));
    m_swap->setAccessibleName(tr("Swap languages"));
    m_swap->setToolTip(tr("Swap languages"));
    m_floating->setAccessibleName(tr("Translation actions"));
    m_resultCopy->setAccessibleName(tr("Copy translated text"));
    m_resultCopy->setToolTip(tr("Copy translated text"));
    m_copy->setText(tr("Copy (Ctrl+C)"));
    m_copyHide->setText(tr("Copy and Hide (Ctrl+Q)"));
    m_retry->setText(tr("Retry"));
    m_controller->setLocale(translation::LanguageManager::instance().currentLocale());
    syncState();
}

void TranslationPageWidget::applyTheme(const translation::styles::ThemeColorScheme& scheme) {
    m_scheme = scheme;
    QPalette muted = palette();
    muted.setColor(QPalette::WindowText, scheme.map.colorTextTertiary);
    QFont labelFont = font();
    labelFont.setPixelSize(scheme.metricMap.font.fontSizeSM);
    for (auto* label : m_labels) {
        label->setPalette(muted);
        label->setFont(labelFont);
    }
    m_status->setPalette(muted);
    m_status->setFont(labelFont);
    m_status->setMinimumHeight(QFontMetrics(labelFont).height());
    const auto& metric = scheme.metricAlias;
    // Keep the floating actions outside the scroll viewport, including when an error adds rows.
    layout()->setContentsMargins(0, 0, 0, 40 + metric.padding);
    m_container->contentLayout()->setContentsMargins(metric.paddingLG, metric.paddingLG,
                                                     metric.paddingLG, 0);
    m_formLayout->setHorizontalSpacing(metric.paddingLG);
    m_formLayout->setVerticalSpacing(metric.paddingSM);
    m_editorsLayout->setSpacing(metric.paddingLG);
    const int countHeight = m_source->sizeHint().height() - m_result->sizeHint().height();
    m_resultPane->layout()->setContentsMargins(0, 0, 0, std::max(0, countHeight));
    m_floating->setFixedSize(40, 40);
    m_resultCopy->setFixedSize(32, 32);
    updateLayout();
}

void TranslationPageWidget::updateLayout() {
    m_container->contentWidget()->setMinimumHeight(m_container->scrollArea()->viewport()->height());
    const auto margins = m_container->contentLayout()->contentsMargins();
    const int available = m_container->contentWidget()->width() - margins.left() - margins.right();
    const int columns = available >= 560 ? 2 : 1;
    if (columns != m_layoutColumns) {
        m_layoutColumns = columns;
        for (auto* field : m_fields) {
            m_formLayout->removeWidget(field);
        }
        for (int index = 0; index < 3; ++index) {
            m_formLayout->setColumnStretch(index, columns == 2 ? 1 : 0);
            m_formLayout->addWidget(m_fields[index], columns == 2 ? 0 : index,
                                    columns == 2 ? index : 0);
        }
        m_editorsLayout->removeWidget(m_source);
        m_editorsLayout->removeWidget(m_resultPane);
        m_editorsLayout->addWidget(m_source, 0, 0);
        m_editorsLayout->addWidget(m_resultPane, columns == 2 ? 0 : 1, columns == 2 ? 1 : 0);
        m_editorsLayout->setColumnStretch(0, 1);
        m_editorsLayout->setColumnStretch(1, columns == 2 ? 1 : 0);
        m_editorsLayout->setRowStretch(0, 1);
        m_editorsLayout->setRowStretch(1, columns == 2 ? 0 : 1);
    }
    const int inset = m_scheme.metricAlias.padding;
    m_floating->move(std::max(0, width() - m_floating->width() - inset),
                     std::max(0, height() - m_floating->height() - inset));
    m_floating->raise();
    m_status->setGeometry(margins.left(), m_floating->y(),
                          std::max(0, m_floating->x() - margins.left() - inset),
                          m_floating->height());
    m_resultCopy->move(std::max(0, m_result->width() - m_resultCopy->width() - 12),
                       std::max(0, m_result->height() - m_resultCopy->height() - 8));
    m_resultCopy->raise();
}

void TranslationPageWidget::resizeEvent(QResizeEvent* event) {
    QWidget::resizeEvent(event);
    updateLayout();
}

void TranslationPageWidget::changeEvent(QEvent* event) {
    QWidget::changeEvent(event);
    if (event->type() == QEvent::LanguageChange) {
        retranslateUi();
    } else if (event->type() == QEvent::FontChange) {
        applyTheme(translation::styles::ThemeManager::instance().themeColorScheme());
    }
}
