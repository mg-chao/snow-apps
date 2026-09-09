#include "snow_shot/presentation/components/screenshottranslationsettingsdialog.h"

#include "snow_shot/translation/translationservice.h"
#include "snow_shot/translation/translationlanguages.h"
#include "snow_shot/presentation/languagemanager.h"
#include "snow_shot/storage/settingsadapters.h"
#include "widgets/alert.h"
#include "widgets/button.h"
#include "widgets/form.h"
#include "widgets/modal.h"
#include "widgets/select.h"
#include "widgets/switch.h"

#include <QCoreApplication>
#include <QEvent>
#include <QScopedValueRollback>
#include <memory>
#include <QVBoxLayout>

namespace snow_shot::presentation {
namespace {
class TranslationSettingsBody final : public QWidget {
  public:
    std::function<void()> retranslate;

  protected:
    void changeEvent(QEvent* event) override {
        QWidget::changeEvent(event);
        if (event->type() == QEvent::LanguageChange && retranslate)
            retranslate();
    }
};
struct TranslationSettingsDraft {
    bool applying = false;
    bool sourceEdited = false;
    bool targetEdited = false;
    bool modelEdited = false;
};
QString text(const char* source) {
    return QCoreApplication::translate("ScreenshotTranslationSettingsDialog", source);
}
} // namespace

adqt::widgets::AdModal*
createScreenshotTranslationSettingsDialog(translation::TranslationService& service, QWidget* owner,
                                          QObject* parent,
                                          std::function<void(bool)> displayModeChanged) {
    using namespace adqt::widgets;
    const QPointer<translation::TranslationService> liveService(&service);
    auto* modal = new AdModal(parent);
    modal->setObjectName(QStringLiteral("screenshotTranslationSettingsModal"));
    modal->setOwnerWindow(owner);
    modal->setMode(AdModal::Mode::Window);
    modal->setWindowModality(Qt::ApplicationModal);
    modal->setCentered(true);
    modal->setPreferredWidth(440);
    modal->setMaskVisible(false);
    modal->setCloseOnMaskClick(false);
    modal->setClosePolicy(AdModal::ClosePolicy::Manual);
    modal->setStandardButtons(AdModal::StandardButton::Ok | AdModal::StandardButton::Cancel);
    auto* body = new TranslationSettingsBody;
    auto draft = std::make_shared<TranslationSettingsDraft>();
    auto* layout = new QVBoxLayout(body);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(12);
    auto* error = new AdAlert(body);
    error->setObjectName(QStringLiteral("screenshotTranslationSettingsError"));
    error->setSeverity(AdAlert::Severity::Error);
    auto* retry = new AdButton(error);
    retry->setObjectName(QStringLiteral("screenshotTranslationSettingsRetry"));
    retry->setButtonStyle(AdButton::ButtonStyle::Text);
    retry->setAccentRole(AdButton::AccentRole::Primary);
    retry->setSizeClass(AdButton::SizeClass::Small);
    error->setActionsWidget(retry);
    layout->addWidget(error);
    auto* form = new AdForm(body);
    form->setFormLayout(AdForm::FormLayout::Vertical);
    auto* source = new AdSelect(form);
    auto* target = new AdSelect(form);
    auto* models = new AdSelect(form);
    source->setObjectName(QStringLiteral("screenshotTranslationSourceLanguage"));
    target->setObjectName(QStringLiteral("screenshotTranslationTargetLanguage"));
    models->setObjectName(QStringLiteral("screenshotTranslationService"));
    for (auto* select : {source, target, models})
        select->setPopupLayerMode(AdSelect::PopupLayerMode::QtTool);
    auto* imageRow = new QWidget(form);
    auto* imageLayout = new QHBoxLayout(imageRow);
    imageLayout->setContentsMargins(0, 0, 0, 0);
    auto* image = new AdSwitch(imageRow);
    image->setObjectName(QStringLiteral("screenshotTranslationOriginalImage"));
    image->setControlSize(AdSwitch::ControlSize::Medium);
    image->setChecked(storage::ScreenshotTranslationSettings().originalImageTranslationEnabled());
    imageLayout->addWidget(image);
    imageLayout->addStretch();
    form->addField({}, source, QStringLiteral("source"));
    form->addField({}, target, QStringLiteral("target"));
    form->addField({}, models, QStringLiteral("service"));
    form->addField({}, imageRow, QStringLiteral("originalImage"));
    layout->addWidget(form);
    modal->setContentWidget(body);
    modal->setInitialFocusWidget(source);

    const auto retranslate = [=, &service] {
        const QScopedValueRollback guard(draft->applying, true);
        modal->setWindowTitle(
            text(QT_TRANSLATE_NOOP("ScreenshotTranslationSettingsDialog", "Translation settings")));
        modal->setAcceptText(text(QT_TRANSLATE_NOOP("ScreenshotTranslationSettingsDialog", "OK")));
        modal->setRejectText(
            text(QT_TRANSLATE_NOOP("ScreenshotTranslationSettingsDialog", "Cancel")));
        retry->setText(text(QT_TRANSLATE_NOOP("ScreenshotTranslationSettingsDialog", "Retry")));
        const char* labels[] = {
            QT_TRANSLATE_NOOP("ScreenshotTranslationSettingsDialog", "Source language"),
            QT_TRANSLATE_NOOP("ScreenshotTranslationSettingsDialog", "Target language"),
            QT_TRANSLATE_NOOP("ScreenshotTranslationSettingsDialog", "Translation service"),
            QT_TRANSLATE_NOOP("ScreenshotTranslationSettingsDialog", "Original Image Translation")};
        const QString keys[] = {QStringLiteral("source"), QStringLiteral("target"),
                                QStringLiteral("service"), QStringLiteral("originalImage")};
        for (int i = 0; i < 4; ++i)
            form->field(keys[i])->setLabel(text(labels[i]));
        source->setAccessibleName(text(labels[0]));
        target->setAccessibleName(text(labels[1]));
        models->setAccessibleName(text(labels[2]));
        image->setAccessibleName(text(labels[3]));
        const auto selectedSource = source->currentValue();
        const auto selectedTarget = target->currentValue();
        QVector<AdSelect::Option> sources{
            {QStringLiteral("auto"), translation::translationLanguageName(QStringLiteral("auto"))}};
        QVector<AdSelect::Option> targets;
        for (const auto& language : translation::translationLanguages()) {
            const QString code = QString::fromLatin1(language.code);
            AdSelect::Option option{code, translation::translationLanguageName(code), false,
                                    code.left(1).toUpper()};
            sources.append(option);
            targets.append(option);
        }
        source->setOptions(sources);
        target->setOptions(targets);
        source->setCurrentValue(selectedSource.isValid()
                                    ? selectedSource
                                    : QVariant(service.preferences().sourceLanguage));
        target->setCurrentValue(selectedTarget.isValid()
                                    ? selectedTarget
                                    : QVariant(service.preferences().targetLanguage));
    };
    const auto sync = [=, &service] {
        const QScopedValueRollback guard(draft->applying, true);
        QVector<AdSelect::Option> options;
        for (const auto& model : service.models())
            options.append(
                {model.id, model.name, false, translation::translationModelGroup(model)});
        QString selected =
            draft->modelEdited ? models->currentValue().toString() : service.preferences().modelId;
        if (selected.isEmpty())
            selected = service.preferences().modelId;
        const int index = translation::translationModelIndex(service.models(), selected);
        models->setOptions(options);
        models->setCurrentValue(index >= 0 ? QVariant(service.models().at(index).id) : QVariant());
        models->setLoading(options.isEmpty() && service.loadingModels());
        models->setEnabled(!options.isEmpty());
        if (modal->acceptButton() != nullptr)
            modal->acceptButton()->setEnabled(!options.isEmpty());
        error->setText(service.errorText());
        error->setVisible(!service.errorText().isEmpty());
        retry->setBusy(service.loadingModels());
    };
    QObject::connect(&service, &translation::TranslationService::catalogChanged, modal, sync);
    QObject::connect(&service, &translation::TranslationService::preferencesChanged, modal,
                     [=, &service] {
                         const QScopedValueRollback guard(draft->applying, true);
                         if (!draft->sourceEdited)
                             source->setCurrentValue(service.preferences().sourceLanguage);
                         if (!draft->targetEdited)
                             target->setCurrentValue(service.preferences().targetLanguage);
                         sync();
                     });
    QObject::connect(source, &AdSelect::currentValueChanged, modal, [draft] {
        if (!draft->applying)
            draft->sourceEdited = true;
    });
    QObject::connect(target, &AdSelect::currentValueChanged, modal, [draft] {
        if (!draft->applying)
            draft->targetEdited = true;
    });
    QObject::connect(models, &AdSelect::currentValueChanged, modal, [draft] {
        if (!draft->applying)
            draft->modelEdited = true;
    });
    body->retranslate = [retranslate, sync] {
        retranslate();
        sync();
    };
    QObject::connect(&service, &QObject::destroyed, modal, [modal, body] {
        body->retranslate = {};
        modal->reject();
    });
    QObject::connect(&LanguageManager::instance(), &LanguageManager::languageChanged, modal,
                     [retranslate, sync, liveService](const QString&, const QLocale& locale) {
                         if (liveService == nullptr)
                             return;
                         liveService->setLocale(locale);
                         retranslate();
                         sync();
                     });
    QObject::connect(retry, &AdButton::clicked, modal, [liveService] {
        if (liveService != nullptr)
            liveService->refreshModels(true);
    });
    QObject::connect(modal, &AdModal::closeRequested, modal, [=](AdModal::CloseReason reason) {
        if (reason != AdModal::CloseReason::OkAction || liveService == nullptr) {
            modal->reject();
            return;
        }
        if (!models->isEnabled())
            return;
        if (!liveService->savePreferences({source->currentValue().toString(),
                                           target->currentValue().toString(),
                                           models->currentValue().toString()}))
            return;
        const storage::ScreenshotTranslationSettings settings;
        const bool changed = settings.originalImageTranslationEnabled() != image->isChecked();
        if (changed && !settings.setOriginalImageTranslationEnabled(image->isChecked())) {
            error->setText(
                QCoreApplication::translate("snow_shot::translation::TranslationService",
                                            "Unable to save translation preferences. Your "
                                            "previous selections were restored."));
            error->show();
            return;
        }
        if (changed && displayModeChanged) {
            displayModeChanged(true);
            displayModeChanged(false);
        }
        modal->accept();
    });
    QObject::connect(modal, &AdModal::finished, modal, &QObject::deleteLater);
    retranslate();
    modal->open();
    sync();
    service.refreshModels();
    return modal;
}
} // namespace snow_shot::presentation
