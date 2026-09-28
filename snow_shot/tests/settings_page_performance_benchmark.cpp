#include "snow_shot/presentation/components/settingspagewidget.h"
#include "snow_shot/presentation/globalshortcutmanager.h"
#include "snow_shot/presentation/languagemanager.h"
#include "snow_shot/presentation/settings/settingsbackend.h"
#include "snow_shot/presentation/settings/settingsruntimesession.h"
#include "snow_shot/presentation/styles/thememanager.h"
#include "snow_shot/storage/applicationstorage.h"
#include "widgets/color_picker.h"

#include <QApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QEvent>
#include <QFile>
#include <QFontDatabase>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTemporaryDir>

#include <algorithm>
#include <cstdlib>
#include <iostream>

namespace settings = snow_shot::presentation::settings;

namespace {
void drainEvents() {
    for (int i = 0; i < 4; ++i) {
        QCoreApplication::processEvents();
        QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
    }
}

double milliseconds(const QElapsedTimer& timer) {
    return static_cast<double>(timer.nsecsElapsed()) / 1000000.0;
}

QJsonObject distribution(QVector<double> values) {
    std::sort(values.begin(), values.end());
    return {{QStringLiteral("median"), values.at(values.size() / 2)},
            {QStringLiteral("p95"), values.at((values.size() - 1) * 95 / 100)}};
}
} // namespace

int main(int argc, char** argv) {
    QApplication application(argc, argv);
#ifdef Q_OS_WIN
    const QDir fonts(qEnvironmentVariable("WINDIR") + QStringLiteral("/Fonts"));
    if (QFontDatabase::addApplicationFont(fonts.filePath(QStringLiteral("segoeui.ttf"))) < 0 ||
        QFontDatabase::addApplicationFont(fonts.filePath(QStringLiteral("msyh.ttc"))) < 0) {
        std::cerr << "Windows UI fonts are required for representative layout measurements\n";
        return EXIT_FAILURE;
    }
#endif
    const auto args = application.arguments();
    const int outputIndex = args.indexOf(QStringLiteral("--output"));
    const int samplesIndex = args.indexOf(QStringLiteral("--samples"));
    const int samples = samplesIndex >= 0 ? args.value(samplesIndex + 1).toInt() : 15;
    if (samples < 3 || outputIndex < 0 || args.value(outputIndex + 1).isEmpty()) {
        std::cerr << "Usage: settings benchmark --output result.json [--samples 15]\n";
        return EXIT_FAILURE;
    }
    QTemporaryDir temporary;
    auto& storage = snow_shot::storage::ApplicationStorage::instance();
    if (!temporary.isValid() ||
        !storage.initialize({temporary.path(), temporary.path(), 60000}).success) {
        return EXIT_FAILURE;
    }
    snow_shot::presentation::LanguageManager::instance().initialize();
    snow_shot::presentation::styles::ThemeManager::instance().initialize(application);
    snow_shot::presentation::GlobalShortcutManager shortcuts;
    settings::BuiltInSettingsBackend backend(shortcuts);
    const auto registry = settings::buildBuiltInSettingsRegistry();
    settings::SettingsRuntimeSession session(registry, backend);
    drainEvents();
    QJsonArray pages;
    for (const auto& definition : registry.catalog().pages()) {
        if (definition.kind != settings::SettingsPageKind::GeneratedSettings) {
            continue;
        }
        QVector<double> construction, firstDisplay, complete, retranslation, slowestSection;
        QVector<double> popupOpen, allInteractions;
        int initialWidgets = 0;
        int totalWidgets = 0;
        for (int sample = -1; sample < samples; ++sample) {
            QElapsedTimer timer;
            timer.start();
            auto page = std::make_unique<SettingsPageWidget>(registry, definition.id, session);
            const double constructed = milliseconds(timer);
            page->resize(880, 760);
            page->show();
            drainEvents();
            const double displayed = milliseconds(timer);
            initialWidgets = static_cast<int>(page->findChildren<QWidget*>().size());
            double sectionMaximum = 0;
            for (const auto& section : definition.sections) {
                QElapsedTimer sectionTimer;
                sectionTimer.start();
                page->reveal({definition.id, section.id, {}});
                drainEvents();
                // Exercise expensive editors below the fold too. Merely visiting
                // a section need not instantiate all of its custom controls.
                for (const auto& item : section.items) {
                    if (std::holds_alternative<settings::SettingsCustomDefinition>(item.payload)) {
                        page->reveal({definition.id, section.id, item.id});
                        drainEvents();
                    }
                }
                sectionMaximum = std::max(sectionMaximum, milliseconds(sectionTimer));
            }
            const double completed = milliseconds(timer);
            double popupMaximum = 0;
            for (const auto& field : registry.fields()) {
                if (field.pageId != definition.id ||
                    field.kind != settings::SettingsFieldKind::Color) {
                    continue;
                }
                page->reveal({field.pageId, field.sectionId, field.id});
                drainEvents();
                auto* picker = page->findChild<adqt::widgets::AdColorPicker*>(
                    settings::generatedObjectName(QStringLiteral("settings-control"), field.id));
                if (picker == nullptr) {
                    std::cerr << "Missing color control: " << field.id.toStdString() << '\n';
                    return EXIT_FAILURE;
                }
                QElapsedTimer popupTimer;
                popupTimer.start();
                picker->setPopupVisible(true);
                drainEvents();
                if (!picker->popupVisible()) {
                    std::cerr << "Color popup did not open\n";
                    return EXIT_FAILURE;
                }
                popupMaximum = std::max(popupMaximum, milliseconds(popupTimer));
                picker->setPopupVisible(false);
                drainEvents();
            }
            const double interacted = milliseconds(timer);
            totalWidgets = static_cast<int>(page->findChildren<QWidget*>().size());
            timer.restart();
            page->retranslateUi();
            drainEvents();
            const double translated = milliseconds(timer);
            page.reset();
            drainEvents();
            if (sample >= 0) {
                construction.append(constructed);
                firstDisplay.append(displayed);
                complete.append(completed);
                retranslation.append(translated);
                slowestSection.append(sectionMaximum);
                popupOpen.append(popupMaximum);
                allInteractions.append(interacted);
            }
        }
        const QJsonObject result{
            {QStringLiteral("page"), definition.id},
            {QStringLiteral("construction_ms"), distribution(construction)},
            {QStringLiteral("first_display_ms"), distribution(firstDisplay)},
            {QStringLiteral("all_sections_ms"), distribution(complete)},
            {QStringLiteral("retranslation_ms"), distribution(retranslation)},
            {QStringLiteral("slowest_section_ms"), distribution(slowestSection)},
            {QStringLiteral("max_popup_open_ms"), distribution(popupOpen)},
            {QStringLiteral("all_interactions_ms"), distribution(allInteractions)},
            {QStringLiteral("initial_widgets"), initialWidgets},
            {QStringLiteral("total_widgets"), totalWidgets}};
        pages.append(result);
        std::cout << QJsonDocument(result).toJson(QJsonDocument::Compact).constData() << '\n';
    }
    QVector<double> bursts;
    int refreshCount = 0;
    QObject::connect(&session, &settings::SettingsRuntimeSession::refreshed, &application,
                     [&] { ++refreshCount; });
    for (int sample = -1; sample < samples; ++sample) {
        refreshCount = 0;
        QElapsedTimer timer;
        timer.start();
        for (int i = 0; i < 25; ++i) {
            emit backend.synchronized();
        }
        drainEvents();
        if (sample >= 0) {
            bursts.append(milliseconds(timer));
        }
    }
    const QJsonObject report{{QStringLiteral("samples"), samples},
                             {QStringLiteral("qt_version"), QString::fromLatin1(qVersion())},
                             {QStringLiteral("platform"), QApplication::platformName()},
                             {QStringLiteral("pages"), pages},
                             {QStringLiteral("notification_burst_ms"), distribution(bursts)},
                             {QStringLiteral("refreshes_per_25_notifications"), refreshCount}};
    QFile output(args.value(outputIndex + 1));
    if (!output.open(QIODevice::WriteOnly) || output.write(QJsonDocument(report).toJson()) < 0) {
        return EXIT_FAILURE;
    }
    storage.shutdown();
    return EXIT_SUCCESS;
}
