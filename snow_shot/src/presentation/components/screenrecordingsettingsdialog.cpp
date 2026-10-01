#include "snow_shot/presentation/components/screenrecordingsettingsdialog.h"
#include "snow_shot/presentation/components/screenrecordingmodal.h"

#include "snow_shot/presentation/globalshortcutmanager.h"
#include "snow_shot/presentation/settings/settingsformfield.h"
#include "snow_shot/presentation/settings/settingsruntimesession.h"
#include "snow_shot/presentation/styles/thememanager.h"
#include "widgets/form.h"
#include "widgets/modal.h"

#include <QCoreApplication>
#include <QEvent>
#include <QGridLayout>
#include <QVBoxLayout>

#include <utility>

namespace snow_shot::presentation {
namespace {
namespace settings = snow_shot::presentation::settings;
namespace fields = components::form_fields;
using namespace adqt::widgets;

settings::SettingsRegistry recordingRegistry() {
    QVector<settings::SettingsPageDefinition> pages;
    for (const auto& page : settings::builtInSettingsRegistry().pages()) {
        auto recordingPage = page;
        recordingPage.sections.clear();
        for (const auto& section : page.sections) {
            if (section.reset == settings::SettingsSectionReset::ScreenRecording ||
                section.reset == settings::SettingsSectionReset::ScreenRecordingCapture) {
                recordingPage.sections.append(section);
            }
        }
        if (!recordingPage.sections.isEmpty())
            pages.append(std::move(recordingPage));
    }
    const settings::SettingsLocation initial{
        pages.first().id, pages.first().sections.first().id, {}};
    return settings::SettingsRegistry(settings::SettingsCatalog(std::move(pages), {}, initial));
}

class ScreenRecordingSettingsBody final : public QWidget {
  public:
    explicit ScreenRecordingSettingsBody(AdModal& modal)
        : m_modal(modal), m_registry(recordingRegistry()), m_backend(m_shortcuts),
          m_session(m_registry, m_backend) {
        auto* layout = new QVBoxLayout(this);
        layout->setContentsMargins(0, 0, 0, 0);
        layout->setSpacing(
            styles::ThemeManager::instance().themeColorScheme().metricAlias.marginSM);
        m_form = new AdForm(this);
        m_form->setObjectName(QStringLiteral("screenRecordingSettingsForm"));
        fields::configureForm(m_form);
        auto* grid = new QGridLayout;
        fields::configureTwoColumnGrid(grid);

        for (const auto& descriptor : m_registry.fields()) {
            fields::Options options;
            options.parent = m_form;
            options.form = m_form;
            options.popupInModal = true;
            auto* field = settings::SettingsFormField::create(descriptor, m_session, options);
            Q_ASSERT(field != nullptr);
            if (field != nullptr) {
                field->editor()->setObjectName(descriptor.id);
                m_fields.append(field);
            }
        }
        // Adding a field rebuilds AdForm's default layout. Arrange the complete
        // set only after registration so those rebuilds cannot undo the grid.
        for (int index = 0; index < m_fields.size(); ++index) {
            auto* item = m_fields[index]->controller()->item();
            m_form->layout()->removeWidget(item);
            grid->addWidget(item, index / 2, index % 2, Qt::AlignTop);
        }
        static_cast<QVBoxLayout*>(m_form->layout())->addLayout(grid);
        m_form->setAutoFillBackground(false);
        layout->addWidget(m_form);
        connect(&styles::ThemeManager::instance(), &styles::ThemeManager::themeChanged, this,
                [layout, grid](const styles::ThemeColorScheme& scheme) {
                    layout->setSpacing(scheme.metricAlias.marginSM);
                    grid->setHorizontalSpacing(scheme.metricAlias.marginLG);
                });
        retranslate();
        m_form->setInitialValues(m_form->values());
        m_form->resetFields();
        for (auto* field : m_fields)
            field->sync();
        modal.setInitialFocusWidget(m_fields.first()->focusTarget());
    }

  protected:
    void changeEvent(QEvent* event) override {
        QWidget::changeEvent(event);
        if (event->type() == QEvent::LanguageChange)
            retranslate();
    }

  private:
    void retranslate() {
        m_modal.setWindowTitle(
            QCoreApplication::translate("ScreenRecordingSettingsDialog", "Recording settings"));
        m_modal.setAcceptText(QCoreApplication::translate("ScreenRecordingSettingsDialog", "Done"));
        for (auto* field : m_fields)
            field->retranslateUi();
    }

    AdModal& m_modal;
    settings::SettingsRegistry m_registry;
    // Recording fields do not register shortcuts. The built-in backend shares the
    // same validation, storage notifications and write state as the settings page.
    GlobalShortcutManager m_shortcuts;
    settings::BuiltInSettingsBackend m_backend;
    settings::SettingsRuntimeSession m_session;
    AdForm* m_form = nullptr;
    QVector<settings::SettingsFormField*> m_fields;
};
} // namespace

adqt::widgets::AdModal* createScreenRecordingSettingsDialog(QWidget* owner, QObject* parent) {
    using namespace adqt::widgets;
    auto* modal = new AdModal(parent);
    modal->setObjectName(QStringLiteral("screenRecordingSettingsModal"));
    configureScreenRecordingModal(*modal, owner);
    modal->setPreferredWidth(660);
    modal->setStandardButtons(AdModal::StandardButton::Ok);
    modal->setContentWidget(new ScreenRecordingSettingsBody(*modal));
    QObject::connect(modal, &AdModal::finished, modal, &QObject::deleteLater);
    return modal;
}
} // namespace snow_shot::presentation
