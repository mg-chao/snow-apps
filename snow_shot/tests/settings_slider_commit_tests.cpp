#include "snow_shot/presentation/components/settingspagewidget.h"
#include "snow_shot/presentation/components/formfields.h"
#include "snow_shot/presentation/globalshortcutmanager.h"
#include "snow_shot/presentation/languagemanager.h"
#include "snow_shot/presentation/settings/settingsbackend.h"
#include "snow_shot/presentation/settings/settingsruntimesession.h"
#include "snow_shot/presentation/styles/thememanager.h"
#include "snow_shot/storage/applicationstorage.h"

#include <QApplication>
#include <QFocusEvent>
#include <QKeyEvent>
#include <QLabel>
#include <QMouseEvent>
#include <QTemporaryDir>

#include <cstdlib>
#include <iostream>
#include <memory>

namespace settings = snow_shot::presentation::settings;
namespace fields = snow_shot::presentation::components::form_fields;
namespace storage = snow_shot::storage;

namespace {
void require(bool condition, const char* message) {
    if (!condition) {
        std::cerr << message << '\n';
        std::exit(EXIT_FAILURE);
    }
}

void sendMouse(adqt::widgets::AdSlider* slider, QEvent::Type type, const QPoint& position) {
    const auto button = type == QEvent::MouseMove ? Qt::NoButton : Qt::LeftButton;
    const auto buttons = type == QEvent::MouseButtonRelease ? Qt::NoButton : Qt::LeftButton;
    QMouseEvent event(type, position, slider->mapToGlobal(position), button, buttons,
                      Qt::NoModifier);
    QCoreApplication::sendEvent(slider, &event);
    QCoreApplication::processEvents();
}

void sendKey(adqt::widgets::AdSlider* slider, QEvent::Type type, int key, bool repeat = false) {
    QKeyEvent event(type, key, Qt::NoModifier, {}, repeat);
    QCoreApplication::sendEvent(slider, &event);
    QCoreApplication::processEvents();
}

void settingsSlidersCommitOnCompletion() {
    snow_shot::presentation::GlobalShortcutManager shortcuts;
    settings::BuiltInSettingsBackend backend(shortcuts);
    const auto registry = settings::buildBuiltInSettingsRegistry();
    settings::SettingsRuntimeSession session(registry, backend);
    std::unique_ptr<SettingsPageWidget> page;
    int tested = 0;
    for (const auto& descriptor : registry.fields()) {
        if (descriptor.kind != settings::SettingsFieldKind::Slider) {
            continue;
        }
        std::cout << "Checking " << descriptor.id.toStdString() << '\n';
        if (!page || page->pageId() != descriptor.pageId) {
            page = std::make_unique<SettingsPageWidget>(registry, descriptor.pageId, session);
            page->resize(880, 760);
            page->show();
            QCoreApplication::processEvents();
        }
        page->reveal({descriptor.pageId, descriptor.sectionId, descriptor.id});
        QCoreApplication::processEvents();
        auto* row = page->findChild<QWidget*>(
            settings::generatedObjectName(QStringLiteral("settings-item"), descriptor.id));
        auto* slider = row ? row->findChild<adqt::widgets::AdSlider*>() : nullptr;
        auto* label = row ? row->findChild<QLabel*>(QStringLiteral("form-field-value")) : nullptr;
        auto* field = row ? row->findChild<fields::FormField*>() : nullptr;
        require(slider && label && field && slider->isEnabled(),
                "every settings slider has an enabled editor and value preview");
        const auto& definition =
            std::get<settings::SettingsSliderDefinition>(descriptor.definition->payload);
        const int minimum = static_cast<int>(slider->minimum());
        const int maximum = static_cast<int>(slider->maximum());
        require(session.applySliderValue(definition.binding, minimum),
                "initialize the slider through the runtime session");
        QCoreApplication::processEvents();
        require(slider->value() == minimum, "external settings changes synchronize the slider");
        int writes = 0;
        int commits = 0;
        const auto writeConnection = QObject::connect(
            &storage::ApplicationStorage::instance().configuration(),
            &storage::ConfigurationStore::valueChanged, page.get(),
            [&writes, key = descriptor.configurationKey](const QString& changedKey) {
                if (changedKey == key) {
                    ++writes;
                }
            });
        const auto commitConnection = QObject::connect(field, &fields::FormField::valueCommitted,
                                                       page.get(), [&commits] { ++commits; });
        const quint64 revision = session.state(descriptor.id).revision;
        const QPoint start(0, slider->height() / 2);
        const QPoint maximumPoint(slider->width() - 1, slider->height() / 2);
        const QPoint end(slider->width() + 20, slider->height() / 2);
        sendMouse(slider, QEvent::MouseButtonPress, start);
        require(slider->sliderDown(), "mouse press starts a slider interaction");
        for (const int percentage : {25, 50, 75, 110}) {
            sendMouse(slider, QEvent::MouseMove,
                      QPoint(slider->width() * percentage / 100, slider->height() / 2));
            require(label->text() ==
                        QString::number(slider->value()) + definition.suffix.translated(),
                    "dragging updates the local numeric preview");
            require(backend.sliderValue(definition.binding) == minimum && writes == 0 &&
                        commits == 0 && session.state(descriptor.id).revision == revision,
                    "dragging must not persist values or update the runtime session");
        }
        require(slider->value() == maximum, "dragging previews the final endpoint");
        sendMouse(slider, QEvent::MouseButtonRelease, end);
        require(!slider->sliderDown() && backend.sliderValue(definition.binding) == maximum &&
                    writes == 1 && commits == 1,
                "releasing outside the slider commits its final value exactly once");

        sendMouse(slider, QEvent::MouseButtonPress, maximumPoint);
        sendMouse(slider, QEvent::MouseButtonRelease, maximumPoint);
        require(writes == 1 && commits == 1, "clicking the current endpoint makes no change");
        sendMouse(slider, QEvent::MouseButtonPress, maximumPoint);
        sendMouse(slider, QEvent::MouseMove, slider->rect().center());
        sendMouse(slider, QEvent::MouseMove, end);
        require(writes == 1, "dragging away and back must not apply intermediate values");
        sendMouse(slider, QEvent::MouseButtonRelease, end);
        require(writes == 1, "returning to the original value does not write settings");

        const QPoint track(slider->width() / 3, slider->height() / 2);
        sendMouse(slider, QEvent::MouseButtonPress, track);
        const int clickedValue = static_cast<int>(slider->value());
        require(clickedValue != maximum && writes == 1,
                "a track click previews its value until release");
        sendMouse(slider, QEvent::MouseButtonRelease, track);
        require(backend.sliderValue(definition.binding) == clickedValue && writes == 2,
                "a completed track click commits once");

        sendKey(slider, QEvent::KeyPress, Qt::Key_Right);
        sendKey(slider, QEvent::KeyPress, Qt::Key_Right, true);
        sendKey(slider, QEvent::KeyRelease, Qt::Key_Right, true);
        require(slider->value() > clickedValue && writes == 2,
                "keyboard repeats preview values without applying intermediate settings");
        sendKey(slider, QEvent::KeyRelease, Qt::Key_Right);
        require(backend.sliderValue(definition.binding) == slider->value() && writes == 3,
                "keyboard release commits the final value once");

        sendKey(slider, QEvent::KeyPress, Qt::Key_Right);
        QFocusEvent focusOut(QEvent::FocusOut, Qt::TabFocusReason);
        QCoreApplication::sendEvent(slider, &focusOut);
        QCoreApplication::processEvents();
        require(backend.sliderValue(definition.binding) == slider->value() && writes == 4,
                "losing focus completes an unfinished keyboard adjustment");
        sendKey(slider, QEvent::KeyRelease, Qt::Key_Right);
        require(writes == 4, "a late key release cannot duplicate the focus completion");
        const int completedCommits = commits;
        require(session.applySliderValue(definition.binding, minimum),
                "apply a later external settings change");
        QCoreApplication::processEvents();
        require(slider->value() == minimum && writes == 5 && commits == completedCommits,
                "external synchronization updates the editor without submitting a user edit");
        QObject::disconnect(writeConnection);
        QObject::disconnect(commitConnection);
        ++tested;
    }
    require(tested == 7, "cover all seven settings sliders");
}
} // namespace

int main(int argc, char** argv) {
    QApplication application(argc, argv);
    QTemporaryDir temporary;
    require(temporary.isValid(), "create isolated settings storage");
    auto& appStorage = storage::ApplicationStorage::instance();
    require(appStorage.initialize({temporary.path(), temporary.path(), 60000}).success,
            "initialize settings storage");
    snow_shot::presentation::LanguageManager::instance().initialize();
    snow_shot::presentation::styles::ThemeManager::instance().initialize(application);
    settingsSlidersCommitOnCompletion();
    appStorage.shutdown();
    return 0;
}
