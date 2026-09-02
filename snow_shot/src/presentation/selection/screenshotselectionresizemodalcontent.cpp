#include "snow_shot/presentation/screenshotselectionresizemodalcontent.h"

#include "snow_shot/presentation/components/icons/snowshoticons.h"
#include "snow_shot/presentation/screenshotselectionlimits.h"

#include "antd_icons.h"
#include "widgets/button.h"
#include "widgets/color_picker.h"
#include "widgets/form.h"
#include "widgets/input_line_edit.h"
#include "widgets/input_number.h"
#include "widgets/modal.h"
#include "widgets/select.h"
#include "theme/theme_manager.h"

#include <QAbstractItemDelegate>
#include <QAbstractButton>
#include <QCoreApplication>
#include <QEvent>
#include <QIcon>
#include <QListView>
#include <QMouseEvent>
#include <QPainter>
#include <QPointer>
#include <QStyle>
#include <QStyleOptionViewItem>
#include <QVariant>
#include <QVBoxLayout>

#include <algorithm>
#include <cmath>
#include <functional>

namespace {
using snow_shot::presentation::kScreenshotSelectionCornerRadiusMax;
using snow_shot::presentation::kScreenshotSelectionShadowWidthMax;

constexpr int kModalContentWidth = 452;
constexpr int kNormalFormColumnWidth = 218;
constexpr int kNormalFormColumnGap = kModalContentWidth - 2 * kNormalFormColumnWidth;
constexpr int kAspectRatioLockButtonSize = 32;
constexpr int kAspectRatioLockIconSize = 20;
constexpr int kDimensionFieldWidth = (kModalContentWidth - kAspectRatioLockButtonSize) / 2;
constexpr int kDimensionLockControlHeight = 62;
constexpr int kColorPickerWidth = 154;
constexpr int kPresetActionWidth = 32;
constexpr int kPresetActionIconSize = 16;
constexpr auto kQuickSetCurrent = "current";
constexpr auto kQuickSetPrevious = "previous";
constexpr auto kQuickSetPresetPrefix = "preset:";
constexpr auto kFieldQuickSet = "quickSet";
constexpr auto kFieldX = "x";
constexpr auto kFieldY = "y";
constexpr auto kFieldWidth = "width";
constexpr auto kFieldHeight = "height";
constexpr auto kFieldRadius = "radius";
constexpr auto kFieldShadowWidth = "shadowWidth";
constexpr auto kFieldShadowColor = "shadowColor";
constexpr auto kFieldPresetName = "presetName";
constexpr auto kTranslationSourceProperty = "selectionResizeTranslationSource";

namespace outlined_icons = adqt::icons::antd::outlined;

bool isPresetKey(const QString& key) {
    return key.startsWith(QString::fromLatin1(kQuickSetPresetPrefix));
}

QRect presetActionRect(const QRect& optionRect) {
    return QRect(optionRect.right() - kPresetActionWidth + 1, optionRect.top(), kPresetActionWidth,
                 optionRect.height());
}

void configureIntegerInput(adqt::widgets::AdInputNumber* input) {
    if (input == nullptr) {
        return;
    }
    input->setControlSize(adqt::widgets::AdInputNumber::ControlSize::Medium);
    input->setVariant(adqt::widgets::AdInputNumber::Variant::Outlined);
    input->setDecimals(0);
    input->setSingleStep(1.0);
    input->setStepButtonLayout(adqt::widgets::AdInputNumber::StepButtonLayout::Compact);
    input->setWheelStepEnabled(true);
    input->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
}

class PresetOptionActionDelegate final : public QAbstractItemDelegate {
  public:
    using DeleteRequested = std::function<void(const QString&)>;

    PresetOptionActionDelegate(adqt::widgets::AdSelect* select, QListView* view,
                               QAbstractItemDelegate* baseDelegate, DeleteRequested deleteRequested)
        : QAbstractItemDelegate(select), m_select(select), m_view(view),
          m_baseDelegate(baseDelegate), m_deleteRequested(std::move(deleteRequested)) {
        if (m_view != nullptr && m_view->viewport() != nullptr) {
            m_view->viewport()->installEventFilter(this);
        }
    }

    QSize sizeHint(const QStyleOptionViewItem& option, const QModelIndex& index) const override {
        return m_baseDelegate != nullptr ? m_baseDelegate->sizeHint(option, index) : QSize();
    }

    void paint(QPainter* painter, const QStyleOptionViewItem& option,
               const QModelIndex& index) const override {
        if (m_baseDelegate != nullptr) {
            m_baseDelegate->paint(painter, option, index);
        }
        if (painter == nullptr || m_select == nullptr || m_view == nullptr ||
            (option.state & QStyle::State_MouseOver) == 0) {
            return;
        }

        const QString key = index.data(Qt::UserRole).toString();
        if (!isPresetKey(key)) {
            return;
        }

        const auto theme = adqt::theme::ThemeManager::instance().resolveTheme(m_view);
        const bool selected = m_select->currentValue().toString() == key;
        const bool actionHovered = m_hoveredActionKey == key;
        const QRect actionRect = presetActionRect(option.rect);
        const QRect actionBackground = actionRect.adjusted(-4, 2, -2, -2);

        painter->save();
        painter->setRenderHint(QPainter::Antialiasing, true);
        painter->fillRect(actionRect.adjusted(-4, 0, 0, 0), theme.colorBgElevated);
        painter->fillRect(actionRect.adjusted(-4, 0, 0, 0),
                          selected ? theme.colorPrimaryBg : theme.colorFillTertiary);
        if (actionHovered) {
            painter->setPen(Qt::NoPen);
            painter->setBrush(theme.colorErrorBgHover);
            painter->drawRoundedRect(actionBackground, 4, 4);
        }

        const auto colors = adqt::icons::IconColors::primary(actionHovered ? theme.colorErrorHover
                                                                           : theme.colorError);
        const QPixmap icon = adqt::icons::renderIconPixmap(
            outlined_icons::IconDelete(colors),
            {QSize(kPresetActionIconSize, kPresetActionIconSize), m_view->devicePixelRatioF()});
        if (!icon.isNull()) {
            const QPoint iconTopLeft(actionRect.center().x() - kPresetActionIconSize / 2,
                                     actionRect.center().y() - kPresetActionIconSize / 2);
            painter->drawPixmap(iconTopLeft, icon);
        }
        painter->restore();
    }

  protected:
    bool eventFilter(QObject* watched, QEvent* event) override {
        QWidget* viewport = m_view != nullptr ? m_view->viewport() : nullptr;
        if (watched != viewport || event == nullptr) {
            return QAbstractItemDelegate::eventFilter(watched, event);
        }

        if (event->type() == QEvent::Leave) {
            setHoveredActionKey(QString());
            m_pressedActionKey.clear();
            return false;
        }
        if (event->type() != QEvent::MouseMove && event->type() != QEvent::MouseButtonPress &&
            event->type() != QEvent::MouseButtonRelease &&
            event->type() != QEvent::MouseButtonDblClick) {
            return false;
        }

        auto* mouseEvent = static_cast<QMouseEvent*>(event);
        const QModelIndex index = m_view->indexAt(mouseEvent->position().toPoint());
        const QString key = index.data(Qt::UserRole).toString();
        const bool overAction =
            index.isValid() && isPresetKey(key) &&
            presetActionRect(m_view->visualRect(index)).contains(mouseEvent->position().toPoint());
        setHoveredActionKey(overAction ? key : QString());

        if (event->type() == QEvent::MouseMove) {
            return false;
        }
        if (mouseEvent->button() != Qt::LeftButton) {
            return false;
        }
        if (event->type() == QEvent::MouseButtonPress ||
            event->type() == QEvent::MouseButtonDblClick) {
            if (!overAction) {
                m_pressedActionKey.clear();
                return false;
            }
            m_pressedActionKey = key;
            return true;
        }
        if (event->type() == QEvent::MouseButtonRelease && !m_pressedActionKey.isEmpty()) {
            const QString pressedKey = m_pressedActionKey;
            m_pressedActionKey.clear();
            if (overAction && key == pressedKey && m_deleteRequested) {
                m_deleteRequested(key);
            }
            return true;
        }
        return false;
    }

  private:
    void setHoveredActionKey(const QString& key) {
        if (m_hoveredActionKey == key) {
            return;
        }
        m_hoveredActionKey = key;
        if (m_view != nullptr && m_view->viewport() != nullptr) {
            m_view->viewport()->update();
        }
    }

    QPointer<adqt::widgets::AdSelect> m_select;
    QPointer<QListView> m_view;
    QPointer<QAbstractItemDelegate> m_baseDelegate;
    DeleteRequested m_deleteRequested;
    QString m_hoveredActionKey;
    QString m_pressedActionKey;
};

int roundToInt(double value) {
    return static_cast<int>(std::lround(value));
}

int inputIntValue(const adqt::widgets::AdInputNumber* input, int fallback = 0) {
    if (input == nullptr || !input->hasValue()) {
        return fallback;
    }
    return roundToInt(input->value());
}

int formIntValue(const QVariantMap& values, const char* fieldName, int fallback = 0) {
    const auto it = values.constFind(QString::fromLatin1(fieldName));
    if (it == values.cend() || !it->isValid()) {
        return fallback;
    }
    return roundToInt(it->toDouble());
}

QColor formColorValue(const QVariantMap& values, const char* fieldName) {
    const QVariant value = values.value(QString::fromLatin1(fieldName));
    if (value.canConvert<adqt::widgets::AdColorValue>()) {
        const adqt::widgets::AdColorValue colorValue = value.value<adqt::widgets::AdColorValue>();
        if (colorValue.isSolid() && colorValue.solidColor.isValid()) {
            return colorValue.solidColor;
        }
    }
    return QColor(0x33, 0x33, 0x33);
}

adqt::widgets::AdSelect::Option option(const QString& value, const QString& label,
                                       bool disabled = false, const QString& group = QString()) {
    adqt::widgets::AdSelect::Option result;
    result.value = value;
    result.label = label;
    result.disabled = disabled;
    result.group = group;
    return result;
}

} // namespace

ScreenshotSelectionResizeModalContent::ScreenshotSelectionResizeModalContent(
    const ScreenshotSelectionParams& currentParams, const QRect& selectionBounds,
    bool hasPreviousParams, const ScreenshotSelectionParams& previousParams,
    const QVector<ScreenshotSelectionPreset>& presets, QWidget* parent)
    : QWidget(parent), m_selectionBounds(selectionBounds.isValid() && !selectionBounds.isEmpty()
                                             ? selectionBounds
                                             : QRect(0, 0, 1, 1)),
      m_currentParams(clampScreenshotSelectionParams(currentParams, m_selectionBounds)),
      m_hasPreviousParams(hasPreviousParams),
      m_previousParams(clampScreenshotSelectionParams(previousParams, m_selectionBounds)),
      m_presets(sanitizeScreenshotSelectionPresets(presets, m_selectionBounds)) {
    setObjectName(QStringLiteral("screenshotSelectionResizeModalContent"));
    setFixedWidth(kModalContentWidth);

    auto* rootLayout = new QVBoxLayout(this);
    rootLayout->setContentsMargins(0, 0, 0, 0);
    rootLayout->setSpacing(0);

    rootLayout->addWidget(createNormalPage());

    applyParamsToFields(m_currentParams);
    updateQuickSetOptions();
}

void ScreenshotSelectionResizeModalContent::changeEvent(QEvent* event) {
    if (event != nullptr && event->type() == QEvent::LanguageChange) {
        retranslateUi();
    }
    QWidget::changeEvent(event);
}

void ScreenshotSelectionResizeModalContent::retranslateUi() {
    const auto translate = [](const char* source) {
        return QCoreApplication::translate("ScreenshotSelectionResizeModalContent", source);
    };

    if (m_normalForm != nullptr) {
        for (adqt::widgets::AdFormItem* item : m_normalForm->items()) {
            if (item == nullptr) {
                continue;
            }
            const QString source = item->property(kTranslationSourceProperty).toString();
            if (!source.isEmpty()) {
                const QByteArray sourceUtf8 = source.toUtf8();
                item->setLabel(translate(sourceUtf8.constData()));
            }
        }
    }

    if (m_quickSet != nullptr) {
        m_quickSet->setPlaceholder(translate("Quick set"));
    }
    if (m_lockAspectRatioButton != nullptr) {
        const QString translated = translate("Lock aspect ratio");
        m_lockAspectRatioButton->setToolTip(translated);
        m_lockAspectRatioButton->setAccessibleName(translated);
    }
    if (auto* addButton =
            findChild<adqt::widgets::AdButton*>(QStringLiteral("selectionPresetAddButton"))) {
        addButton->setText(translate("Add"));
        const QString translated = translate("Add preset");
        addButton->setToolTip(translated);
        addButton->setAccessibleName(translated);
    }

    if (m_createPresetModal != nullptr) {
        m_createPresetModal->setWindowTitle(translate("Add preset"));
        m_createPresetModal->setAcceptText(translate("Add"));
        m_createPresetModal->setRejectText(translate("Cancel"));
    }
    if (m_createPresetNameItem != nullptr) {
        m_createPresetNameItem->setLabel(translate("Preset name"));
        m_createPresetNameItem->setRequiredMessage(translate("Please enter a preset name"));
    }
    if (m_deletePresetModal != nullptr) {
        m_deletePresetModal->setWindowTitle(translate("Delete preset"));
        m_deletePresetModal->setText(
            translate("Delete preset \"%1\"? This action cannot be undone")
                .arg(m_deletePresetName));
        m_deletePresetModal->setAcceptText(translate("Delete"));
        m_deletePresetModal->setRejectText(translate("Cancel"));
    }
    updateQuickSetOptions();
}

ScreenshotSelectionResizeModalContent::CommitResult
ScreenshotSelectionResizeModalContent::commit(ScreenshotSelectionParams* params,
                                              QVector<ScreenshotSelectionPreset>* presets,
                                              bool* presetsChanged) {
    if (presetsChanged != nullptr) {
        *presetsChanged = false;
    }

    if (!validateNormalFields()) {
        return CommitResult::Invalid;
    }

    if (params != nullptr) {
        *params = paramsFromFields();
    }
    if (presets != nullptr) {
        *presets = m_presets;
    }
    if (presetsChanged != nullptr) {
        *presetsChanged = m_presetsChanged;
    }
    return CommitResult::ApplySelection;
}

QWidget* ScreenshotSelectionResizeModalContent::initialFocusWidget() const {
    return m_quickSet;
}

QWidget* ScreenshotSelectionResizeModalContent::createNormalPage() {
    auto* page = new QWidget(this);
    auto* layout = new QVBoxLayout(page);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);

    m_normalForm = new adqt::widgets::AdForm(page);
    m_normalForm->setObjectName(QStringLiteral("selectionResizeForm"));
    m_normalForm->setFormLayout(adqt::widgets::AdForm::FormLayout::Inline);
    m_normalForm->setLabelAlign(adqt::widgets::AdForm::LabelAlign::Left);
    m_normalForm->setRequiredMark(adqt::widgets::AdForm::RequiredMark::Hidden);
    m_normalForm->setControlSize(adqt::widgets::AdForm::ControlSize::Medium);
    m_normalForm->setVariant(adqt::widgets::AdForm::Variant::Outlined);
    m_normalForm->setColon(false);
    m_normalForm->setScrollToFirstError(true);
    layout->addWidget(m_normalForm);

    m_quickSet = new adqt::widgets::AdSelect(page);
    m_quickSet->setObjectName(QStringLiteral("selectionPresetSelect"));
    m_quickSet->setMode(adqt::widgets::AdSelect::Mode::Single);
    m_quickSet->setControlSize(adqt::widgets::AdSelect::ControlSize::Middle);
    m_quickSet->setVariant(adqt::widgets::AdSelect::Variant::Outlined);
    m_quickSet->setPlaceholder(tr("Quick set"));
    m_quickSet->setFixedWidth(240);
    addNormalField(tr("Quick set"), m_quickSet, QString::fromLatin1(kFieldQuickSet), true);
    connect(m_quickSet, &adqt::widgets::AdSelect::selected, this,
            [this](const QVariant& value, const QString&) { handleQuickSetValue(value); });
    m_quickSet->setPopupExtraContentFactory([this](QWidget* parent) {
        auto* addButton = new adqt::widgets::AdButton(tr("Add"), parent);
        addButton->setObjectName(QStringLiteral("selectionPresetAddButton"));
        addButton->setButtonStyle(adqt::widgets::AdButton::ButtonStyle::Text);
        addButton->setAccentRole(adqt::widgets::AdButton::AccentRole::Primary);
        addButton->setIconRef(outlined_icons::Plus());
        addButton->setIconPosition(adqt::widgets::AdButton::IconPosition::Leading);
        addButton->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
        addButton->setToolTip(tr("Add preset"));
        addButton->setAccessibleName(tr("Add preset"));
        connect(addButton, &QAbstractButton::clicked, this, [this]() {
            if (m_quickSet != nullptr) {
                m_quickSet->hidePopup();
            }
            openCreatePresetModal();
        });
        return addButton;
    });

    QListView* quickSetView = m_quickSet->view();
    auto* presetDelegate = new PresetOptionActionDelegate(
        m_quickSet, quickSetView, quickSetView != nullptr ? quickSetView->itemDelegate() : nullptr,
        [this](const QString& key) {
            if (m_quickSet != nullptr) {
                m_quickSet->hidePopup();
            }
            openDeletePresetModal(key);
        });
    m_quickSet->setItemDelegate(presetDelegate);

    m_xInput = createIntegerInput(m_selectionBounds.left(), m_selectionBounds.right());
    m_yInput = createIntegerInput(m_selectionBounds.top(), m_selectionBounds.bottom());
    adqt::widgets::AdFormItem* xItem =
        addNormalField(tr("Position X"), m_xInput, QString::fromLatin1(kFieldX));
    addNormalColumnSpacer();
    adqt::widgets::AdFormItem* yItem =
        addNormalField(tr("Position Y"), m_yInput, QString::fromLatin1(kFieldY));

    m_widthInput = createIntegerInput(1, m_selectionBounds.width());
    m_heightInput = createIntegerInput(1, m_selectionBounds.height());
    adqt::widgets::AdFormItem* widthItem =
        addNormalField(tr("Width"), m_widthInput, QString::fromLatin1(kFieldWidth));
    widthItem->setFixedWidth(kDimensionFieldWidth);

    auto* aspectRatioLockControl = new QWidget(m_normalForm);
    aspectRatioLockControl->setFixedSize(kAspectRatioLockButtonSize, kDimensionLockControlHeight);
    auto* aspectRatioLockLayout = new QVBoxLayout(aspectRatioLockControl);
    aspectRatioLockLayout->setContentsMargins(0, 30, 0, 0);
    aspectRatioLockLayout->setSpacing(0);

    m_lockAspectRatioButton = new adqt::widgets::AdButton(aspectRatioLockControl);
    m_lockAspectRatioButton->setObjectName(QStringLiteral("selectionAspectRatioLockButton"));
    m_lockAspectRatioButton->setButtonStyle(adqt::widgets::AdButton::ButtonStyle::Text);
    m_lockAspectRatioButton->setInteractionBackgroundVisible(false);
    m_lockAspectRatioButton->setAccentRole(adqt::widgets::AdButton::AccentRole::Neutral);
    m_lockAspectRatioButton->setCheckable(true);
    m_lockAspectRatioButton->setIconSize(QSize(kAspectRatioLockIconSize, kAspectRatioLockIconSize));
    m_lockAspectRatioButton->setToolTip(tr("Lock aspect ratio"));
    m_lockAspectRatioButton->setAccessibleName(tr("Lock aspect ratio"));
    m_lockAspectRatioButton->setFixedSize(kAspectRatioLockButtonSize, kAspectRatioLockButtonSize);
    aspectRatioLockLayout->addWidget(m_lockAspectRatioButton);

    auto* aspectRatioLockItem = m_normalForm->addField(QString(), aspectRatioLockControl);
    aspectRatioLockItem->setItemLayout(adqt::widgets::AdFormItem::ItemLayout::Vertical);
    aspectRatioLockItem->setNoStyle(true);
    aspectRatioLockItem->setFixedWidth(kAspectRatioLockButtonSize);

    adqt::widgets::AdFormItem* heightItem =
        addNormalField(tr("Height"), m_heightInput, QString::fromLatin1(kFieldHeight));
    heightItem->setFixedWidth(kDimensionFieldWidth);

    m_radiusInput = createIntegerInput(0, kScreenshotSelectionCornerRadiusMax);
    addNormalField(tr("Corner radius"), m_radiusInput, QString::fromLatin1(kFieldRadius));
    addNormalColumnSpacer();

    m_shadowWidthInput = createIntegerInput(0, kScreenshotSelectionShadowWidthMax);
    m_shadowColorPicker = new adqt::widgets::AdColorPicker(page);
    m_shadowColorPicker->setSize(adqt::widgets::AdColorPicker::Size::Middle);
    m_shadowColorPicker->setModeOptions({adqt::widgets::AdColorPicker::Mode::Solid});
    m_shadowColorPicker->setMode(adqt::widgets::AdColorPicker::Mode::Solid);
    m_shadowColorPicker->setAlphaChannelEnabled(false);
    m_shadowColorPicker->setTriggerTextVisible(true);
    m_shadowColorPicker->setFixedWidth(kColorPickerWidth);
    addNormalField(tr("Shadow width"), m_shadowWidthInput, QString::fromLatin1(kFieldShadowWidth));
    addNormalField(tr("Shadow color"), m_shadowColorPicker, QString::fromLatin1(kFieldShadowColor));

    const QStringList geometryFields{
        QString::fromLatin1(kFieldX),
        QString::fromLatin1(kFieldY),
        QString::fromLatin1(kFieldWidth),
        QString::fromLatin1(kFieldHeight),
    };
    const auto geometryValidator = [this](const QVariant&, adqt::widgets::AdFormItem*) {
        adqt::widgets::AdFormItem::ValidationResult result;
        if (m_normalForm == nullptr) {
            return result;
        }

        const QVariantMap values = m_normalForm->values();
        const QRect selection(formIntValue(values, kFieldX, m_selectionBounds.left()),
                              formIntValue(values, kFieldY, m_selectionBounds.top()),
                              formIntValue(values, kFieldWidth, 1),
                              formIntValue(values, kFieldHeight, 1));
        const bool valid =
            selection.isValid() && !selection.isEmpty() &&
            m_selectionBounds.contains(selection.topLeft()) &&
            m_selectionBounds.contains(QPoint(selection.left() + selection.width() - 1,
                                              selection.top() + selection.height() - 1));
        if (!valid) {
            result.status = adqt::widgets::AdFormItem::ValidateStatus::Error;
        }
        return result;
    };
    for (adqt::widgets::AdFormItem* item : {xItem, yItem, widthItem, heightItem}) {
        item->setRequired(true);
        item->setDependencies(geometryFields);
        item->setFormValidator(geometryValidator);
    }

    connect(m_lockAspectRatioButton, &QAbstractButton::toggled, this,
            &ScreenshotSelectionResizeModalContent::handleAspectRatioToggle);
    connect(&adqt::theme::ThemeManager::instance(), &adqt::theme::ThemeManager::themeChanged, this,
            [this]() { updateAspectRatioLockIcon(); });
    connect(m_normalForm, &adqt::widgets::AdForm::fieldPathValueChanged, this,
            [this](const QStringList& fieldPath, const QVariant&) {
                const bool quickSetChanged =
                    fieldPath.size() == 1 &&
                    fieldPath.constFirst() == QString::fromLatin1(kFieldQuickSet);
                if (!m_syncing && !m_applyingQuickSet && !quickSetChanged) {
                    clearQuickSetSelection();
                }
            });
    connect(m_xInput, &adqt::widgets::AdInputNumber::valueChanged, this,
            [this](double) { updateGeometryRanges(); });
    connect(m_yInput, &adqt::widgets::AdInputNumber::valueChanged, this,
            [this](double) { updateGeometryRanges(); });
    connect(m_widthInput, &adqt::widgets::AdInputNumber::valueChanged, this, [this](double) {
        updateGeometryRanges();
        syncHeightFromWidth();
    });
    connect(m_heightInput, &adqt::widgets::AdInputNumber::valueChanged, this, [this](double) {
        updateGeometryRanges();
        syncWidthFromHeight();
    });

    return page;
}

adqt::widgets::AdInputNumber*
ScreenshotSelectionResizeModalContent::createIntegerInput(int minimum, int maximum,
                                                          QWidget* parent) {
    auto* input = new adqt::widgets::AdInputNumber(parent != nullptr ? parent : this);
    configureIntegerInput(input);
    input->setRange(static_cast<double>(minimum), static_cast<double>(maximum));
    return input;
}

void ScreenshotSelectionResizeModalContent::addNormalColumnSpacer() {
    if (m_normalForm == nullptr) {
        return;
    }

    auto* spacer = new QWidget(m_normalForm);
    spacer->setFixedSize(kNormalFormColumnGap, 1);
    auto* item = m_normalForm->addField(QString(), spacer);
    item->setItemLayout(adqt::widgets::AdFormItem::ItemLayout::Vertical);
    item->setNoStyle(true);
    item->setFixedWidth(kNormalFormColumnGap);
}

adqt::widgets::AdFormItem*
ScreenshotSelectionResizeModalContent::addNormalField(const QString& label, QWidget* control,
                                                      const QString& fieldName, bool fullWidth) {
    if (m_normalForm == nullptr) {
        return nullptr;
    }

    adqt::widgets::AdFormItem* item = m_normalForm->addField(label, control, fieldName);
    const char* source = nullptr;
    if (fieldName == QString::fromLatin1(kFieldQuickSet)) {
        source = "Quick set";
    } else if (fieldName == QString::fromLatin1(kFieldX)) {
        source = "Position X";
    } else if (fieldName == QString::fromLatin1(kFieldY)) {
        source = "Position Y";
    } else if (fieldName == QString::fromLatin1(kFieldWidth)) {
        source = "Width";
    } else if (fieldName == QString::fromLatin1(kFieldHeight)) {
        source = "Height";
    } else if (fieldName == QString::fromLatin1(kFieldRadius)) {
        source = "Corner radius";
    } else if (fieldName == QString::fromLatin1(kFieldShadowWidth)) {
        source = "Shadow width";
    } else if (fieldName == QString::fromLatin1(kFieldShadowColor)) {
        source = "Shadow color";
    }
    if (source != nullptr) {
        item->setProperty(kTranslationSourceProperty, QString::fromLatin1(source));
    }
    item->setItemLayout(adqt::widgets::AdFormItem::ItemLayout::Vertical);
    item->setFixedWidth(fullWidth ? kModalContentWidth : kNormalFormColumnWidth);
    return item;
}

void ScreenshotSelectionResizeModalContent::applyParamsToFields(
    const ScreenshotSelectionParams& params) {
    const ScreenshotSelectionParams clamped =
        clampScreenshotSelectionParams(params, m_selectionBounds);
    const bool lockAspectRatio = clamped.lockAspectRatio || clamped.lockDragAspectRatio;
    m_syncing = true;
    if (m_normalForm != nullptr) {
        m_normalForm->setValues({
            {QString::fromLatin1(kFieldX), clamped.selection.left()},
            {QString::fromLatin1(kFieldY), clamped.selection.top()},
            {QString::fromLatin1(kFieldWidth), clamped.selection.width()},
            {QString::fromLatin1(kFieldHeight), clamped.selection.height()},
            {QString::fromLatin1(kFieldRadius), clamped.radius},
            {QString::fromLatin1(kFieldShadowWidth), clamped.shadowWidth},
            {QString::fromLatin1(kFieldShadowColor),
             QVariant::fromValue(adqt::widgets::AdColorValue::solid(clamped.shadowColor))},
        });
        m_normalForm->resetValidation();
    }
    if (m_lockAspectRatioButton != nullptr) {
        m_lockAspectRatioButton->setChecked(lockAspectRatio);
        m_lockAspectRatioButton->setAccentRole(lockAspectRatio
                                                   ? adqt::widgets::AdButton::AccentRole::Primary
                                                   : adqt::widgets::AdButton::AccentRole::Neutral);
        updateAspectRatioLockIcon();
    }
    m_editAspectRatio = lockAspectRatio
                            ? static_cast<double>(std::max(1, clamped.selection.height())) /
                                  static_cast<double>(std::max(1, clamped.selection.width()))
                            : 0.0;
    m_syncing = false;
    updateGeometryRanges();
}

ScreenshotSelectionParams ScreenshotSelectionResizeModalContent::paramsFromFields() const {
    const QVariantMap values = m_normalForm != nullptr ? m_normalForm->values() : QVariantMap();
    ScreenshotSelectionParams params;
    params.selection =
        QRect(formIntValue(values, kFieldX, m_selectionBounds.left()),
              formIntValue(values, kFieldY, m_selectionBounds.top()),
              formIntValue(values, kFieldWidth, 1), formIntValue(values, kFieldHeight, 1));
    params.radius = formIntValue(values, kFieldRadius, 0);
    params.shadowWidth = formIntValue(values, kFieldShadowWidth, 0);
    params.shadowColor = formColorValue(values, kFieldShadowColor);
    const bool lockAspectRatio =
        m_lockAspectRatioButton != nullptr && m_lockAspectRatioButton->isChecked();
    params.lockAspectRatio = lockAspectRatio;
    params.lockDragAspectRatio = lockAspectRatio;
    return clampScreenshotSelectionParams(params, m_selectionBounds);
}

ScreenshotSelectionPreset
ScreenshotSelectionResizeModalContent::presetFromFields(const QString& name) const {
    ScreenshotSelectionPreset preset;
    preset.name = name;
    preset.params = paramsFromFields();
    return preset;
}

void ScreenshotSelectionResizeModalContent::updateQuickSetOptions() {
    if (m_quickSet == nullptr) {
        return;
    }

    QVector<adqt::widgets::AdSelect::Option> options;
    options.push_back(option(QString::fromLatin1(kQuickSetCurrent), tr("Current selection"), false,
                             tr("Selection")));
    options.push_back(option(QString::fromLatin1(kQuickSetPrevious), tr("Previous selection"),
                             !m_hasPreviousParams, tr("Selection")));
    for (int i = 0; i < m_presets.size(); ++i) {
        options.push_back(option(QString::fromLatin1(kQuickSetPresetPrefix) + QString::number(i),
                                 m_presets.at(i).name, false, tr("Preset")));
    }

    m_quickSet->setOptions(options);
    m_quickSet->setCurrentValue(QVariant());
}

void ScreenshotSelectionResizeModalContent::handleQuickSetValue(const QVariant& value) {
    const QString key = value.toString();
    const auto applyQuickSet = [this](const ScreenshotSelectionParams& params) {
        m_applyingQuickSet = true;
        applyParamsToFields(params);
        m_applyingQuickSet = false;
    };
    if (key == QString::fromLatin1(kQuickSetCurrent)) {
        applyQuickSet(m_currentParams);
        return;
    }
    if (key == QString::fromLatin1(kQuickSetPrevious) && m_hasPreviousParams) {
        applyQuickSet(m_previousParams);
        return;
    }
    if (isPresetKey(key)) {
        bool ok = false;
        const int index = key.mid(QString::fromLatin1(kQuickSetPresetPrefix).size()).toInt(&ok);
        if (ok && index >= 0 && index < m_presets.size()) {
            const ScreenshotSelectionPreset& preset = m_presets.at(index);
            applyQuickSet(preset.params);
        }
    }
}

void ScreenshotSelectionResizeModalContent::clearQuickSetSelection() {
    if (!m_applyingQuickSet && m_quickSet != nullptr && m_quickSet->currentValue().isValid()) {
        m_quickSet->setCurrentValue(QVariant());
    }
}

void ScreenshotSelectionResizeModalContent::openCreatePresetModal() {
    if (!validateNormalFields()) {
        return;
    }

    const ScreenshotSelectionParams params = paramsFromFields();
    const QString defaultName =
        tr("%1 x %2").arg(params.selection.width()).arg(params.selection.height());

    auto* form = new adqt::widgets::AdForm();
    form->setObjectName(QStringLiteral("selectionPresetCreateForm"));
    form->setFixedWidth(352);
    form->setFormLayout(adqt::widgets::AdForm::FormLayout::Vertical);
    form->setLabelAlign(adqt::widgets::AdForm::LabelAlign::Left);
    form->setRequiredMark(adqt::widgets::AdForm::RequiredMark::Visible);
    form->setControlSize(adqt::widgets::AdForm::ControlSize::Medium);
    form->setVariant(adqt::widgets::AdForm::Variant::Outlined);
    form->setColon(false);
    form->setScrollToFirstError(true);

    auto* nameInput = new adqt::widgets::AdLineEdit(form);
    nameInput->setObjectName(QStringLiteral("selectionPresetNameInput"));
    nameInput->setAllowClear(true);
    nameInput->setMaxLength(80);
    nameInput->setText(defaultName);
    auto* nameItem =
        form->addField(tr("Preset name"), nameInput, QString::fromLatin1(kFieldPresetName));
    nameItem->setItemLayout(adqt::widgets::AdFormItem::ItemLayout::Vertical);
    nameItem->setRequired(true);
    nameItem->setRequiredMessage(tr("Please enter a preset name"));
    nameItem->setFormValidator([this](const QVariant& value, adqt::widgets::AdFormItem*) {
        adqt::widgets::AdFormItem::ValidationResult result;
        if (value.toString().trimmed().isEmpty()) {
            result.status = adqt::widgets::AdFormItem::ValidateStatus::Error;
            result.errors.push_back(tr("Please enter a preset name"));
        }
        return result;
    });

    auto* modal = new adqt::widgets::AdModal(this);
    modal->setObjectName(QStringLiteral("selectionPresetCreateModal"));
    modal->setOwnerWindow(modalOwnerWindow());
    modal->setMode(adqt::widgets::AdModal::Mode::Window);
    modal->setWindowModality(Qt::ApplicationModal);
    modal->setWindowTitle(tr("Add preset"));
    modal->setCentered(true);
    modal->setPreferredWidth(400);
    modal->setMaskVisible(false);
    modal->setCloseOnMaskClick(false);
    modal->setClosePolicy(adqt::widgets::AdModal::ClosePolicy::Manual);
    modal->setAcceptText(tr("Add"));
    modal->setRejectText(tr("Cancel"));
    modal->setStandardButtons(adqt::widgets::AdModal::StandardButton::Ok |
                              adqt::widgets::AdModal::StandardButton::Cancel);
    modal->setContentWidget(form);
    modal->setInitialFocusWidget(nameInput);

    m_createPresetModal = modal;
    m_createPresetForm = form;
    m_createPresetNameItem = nameItem;
    m_createPresetNameInput = nameInput;

    const QPointer<adqt::widgets::AdForm> formGuard(form);
    const QPointer<adqt::widgets::AdLineEdit> nameGuard(nameInput);
    connect(modal, &adqt::widgets::AdModal::closeRequested, modal,
            [this, modal, formGuard, nameGuard](adqt::widgets::AdModal::CloseReason reason) {
                if (reason != adqt::widgets::AdModal::CloseReason::OkAction) {
                    modal->reject();
                    return;
                }
                if (formGuard == nullptr || nameGuard == nullptr || !formGuard->submit()) {
                    return;
                }

                m_presets.push_back(presetFromFields(nameGuard->text().trimmed()));
                m_presets = sanitizeScreenshotSelectionPresets(m_presets, m_selectionBounds);
                m_presetsChanged = true;
                updateQuickSetOptions();
                emit presetsUpdated(m_presets);
                modal->accept();
            });
    connect(modal, &adqt::widgets::AdModal::finished, modal,
            [this, modal](adqt::widgets::AdModal::DialogCode) {
                if (m_createPresetModal == modal) {
                    m_createPresetModal = nullptr;
                    m_createPresetForm = nullptr;
                    m_createPresetNameItem = nullptr;
                    m_createPresetNameInput = nullptr;
                }
                modal->deleteLater();
            });
    modal->open();
    nameInput->focusEditor(adqt::widgets::AdLineEdit::FocusSelection::SelectAll);
}

void ScreenshotSelectionResizeModalContent::openDeletePresetModal(const QString& presetKey) {
    bool ok = false;
    const int index = presetKey.mid(QString::fromLatin1(kQuickSetPresetPrefix).size()).toInt(&ok);
    if (!ok || index < 0 || index >= m_presets.size()) {
        return;
    }

    const QString presetName = m_presets.at(index).name;
    auto* modal = new adqt::widgets::AdModal(this);
    modal->setObjectName(QStringLiteral("selectionPresetDeleteModal"));
    modal->setOwnerWindow(modalOwnerWindow());
    modal->setMode(adqt::widgets::AdModal::Mode::Window);
    modal->setWindowModality(Qt::ApplicationModal);
    modal->setWindowTitle(tr("Delete preset"));
    modal->setCentered(true);
    modal->setPreferredWidth(400);
    modal->setMaskVisible(false);
    modal->setCloseOnMaskClick(false);
    modal->setClosePolicy(adqt::widgets::AdModal::ClosePolicy::Manual);
    modal->setPreset(adqt::widgets::AdModal::Preset::Confirm);
    modal->setText(tr("Delete preset \"%1\"? This action cannot be undone").arg(presetName));
    modal->setAcceptText(tr("Delete"));
    modal->setRejectText(tr("Cancel"));
    modal->setAcceptAccentRole(adqt::widgets::AdButton::AccentRole::Danger);
    modal->setStandardButtons(adqt::widgets::AdModal::StandardButton::Ok |
                              adqt::widgets::AdModal::StandardButton::Cancel);

    m_deletePresetModal = modal;
    m_deletePresetName = presetName;

    connect(modal, &adqt::widgets::AdModal::closeRequested, modal,
            [this, modal, presetKey](adqt::widgets::AdModal::CloseReason reason) {
                if (reason == adqt::widgets::AdModal::CloseReason::OkAction) {
                    deletePreset(presetKey);
                    modal->accept();
                    return;
                }
                modal->reject();
            });
    connect(modal, &adqt::widgets::AdModal::finished, modal,
            [this, modal](adqt::widgets::AdModal::DialogCode) {
                if (m_deletePresetModal == modal) {
                    m_deletePresetModal = nullptr;
                    m_deletePresetName.clear();
                }
                modal->deleteLater();
            });
    modal->open();
    if (modal->rejectButton() != nullptr) {
        modal->rejectButton()->setFocus(Qt::OtherFocusReason);
    }
}

void ScreenshotSelectionResizeModalContent::deletePreset(const QString& presetKey) {
    bool ok = false;
    const int index = presetKey.mid(QString::fromLatin1(kQuickSetPresetPrefix).size()).toInt(&ok);
    if (!ok || index < 0 || index >= m_presets.size()) {
        return;
    }

    m_presets.removeAt(index);
    m_presetsChanged = true;
    updateQuickSetOptions();
    emit presetsUpdated(m_presets);
}

QWidget* ScreenshotSelectionResizeModalContent::modalOwnerWindow() const {
    QWidget* owner = window();
    return owner != nullptr ? owner : const_cast<ScreenshotSelectionResizeModalContent*>(this);
}

void ScreenshotSelectionResizeModalContent::updateGeometryRanges() {
    if (m_syncing || m_xInput == nullptr || m_yInput == nullptr || m_widthInput == nullptr ||
        m_heightInput == nullptr) {
        return;
    }

    const int x = inputIntValue(m_xInput, m_selectionBounds.left());
    const int y = inputIntValue(m_yInput, m_selectionBounds.top());
    const int maxWidth = std::max(1, m_selectionBounds.right() - x + 1);
    const int maxHeight = std::max(1, m_selectionBounds.bottom() - y + 1);
    m_widthInput->setRange(1, maxWidth);
    m_heightInput->setRange(1, maxHeight);
}

void ScreenshotSelectionResizeModalContent::updateAspectRatioLockIcon() {
    if (m_lockAspectRatioButton == nullptr) {
        return;
    }

    adqt::icons::IconColors colors;
    if (m_lockAspectRatioButton->isChecked()) {
        const QColor primaryActive = adqt::theme::ThemeManager::instance()
                                         .resolveTheme(m_lockAspectRatioButton)
                                         .colorPrimaryActive;
        colors = adqt::icons::IconColors::primary(
            primaryActive.isValid() ? primaryActive : QColor(QStringLiteral("#0958d9")));
    }
    m_lockAspectRatioButton->setIconRef(
        snow_shot::presentation::icons::custom::outlined::SelectionLockAspect(colors));
}

void ScreenshotSelectionResizeModalContent::handleAspectRatioToggle(bool checked) {
    if (m_lockAspectRatioButton != nullptr) {
        m_lockAspectRatioButton->setAccentRole(checked
                                                   ? adqt::widgets::AdButton::AccentRole::Primary
                                                   : adqt::widgets::AdButton::AccentRole::Neutral);
        updateAspectRatioLockIcon();
    }
    if (m_syncing) {
        return;
    }
    clearQuickSetSelection();
    if (checked) {
        updateAspectRatioFromFields();
    } else {
        m_editAspectRatio = 0.0;
    }
}

void ScreenshotSelectionResizeModalContent::updateAspectRatioFromFields() {
    const int width = std::max(1, inputIntValue(m_widthInput, 1));
    const int height = std::max(1, inputIntValue(m_heightInput, 1));
    m_editAspectRatio = static_cast<double>(height) / static_cast<double>(width);
}

void ScreenshotSelectionResizeModalContent::syncHeightFromWidth() {
    if (m_updatingAspectRatioPeer || m_lockAspectRatioButton == nullptr ||
        !m_lockAspectRatioButton->isChecked() || m_editAspectRatio <= 0.0) {
        return;
    }

    m_updatingAspectRatioPeer = true;
    const int width = std::max(1, inputIntValue(m_widthInput, 1));
    const int nextHeight = std::clamp(roundToInt(static_cast<double>(width) * m_editAspectRatio), 1,
                                      m_selectionBounds.height());
    m_heightInput->setValue(nextHeight);
    m_updatingAspectRatioPeer = false;
}

void ScreenshotSelectionResizeModalContent::syncWidthFromHeight() {
    if (m_updatingAspectRatioPeer || m_lockAspectRatioButton == nullptr ||
        !m_lockAspectRatioButton->isChecked() || m_editAspectRatio <= 0.0) {
        return;
    }

    m_updatingAspectRatioPeer = true;
    const int height = std::max(1, inputIntValue(m_heightInput, 1));
    const int nextWidth = std::clamp(roundToInt(static_cast<double>(height) / m_editAspectRatio), 1,
                                     m_selectionBounds.width());
    m_widthInput->setValue(nextWidth);
    m_updatingAspectRatioPeer = false;
}

bool ScreenshotSelectionResizeModalContent::validateNormalFields() {
    return m_normalForm != nullptr && m_normalForm->submit();
}
