// SPDX-License-Identifier: GPL-3.0-or-later
#include "snow_shot/app/applicationinputguard.h"
#include "snow_shot/app/quiescencesession.h"

#include <QCoreApplication>

#include <cstdlib>
#include <iostream>

namespace {
void require(bool condition, const char* message) {
    if (!condition) {
        std::cerr << message << '\n';
        std::exit(EXIT_FAILURE);
    }
}

class InputTarget final : public QObject {
  public:
    int deliveries = 0;
    bool event(QEvent*) override {
        ++deliveries;
        return true;
    }
};
} // namespace

int main(int argc, char** argv) {
    QCoreApplication application(argc, argv);
    snow_shot::app::ApplicationInputGuard input(nullptr);
    application.installEventFilter(&input);
    InputTarget target;
    input.active = true;
    for (const auto type : {QEvent::KeyPress,
                            QEvent::KeyRelease,
                            QEvent::Shortcut,
                            QEvent::ShortcutOverride,
                            QEvent::InputMethod,
                            QEvent::MouseButtonPress,
                            QEvent::MouseButtonRelease,
                            QEvent::MouseButtonDblClick,
                            QEvent::MouseMove,
                            QEvent::Wheel,
                            QEvent::TabletPress,
                            QEvent::TabletMove,
                            QEvent::TabletRelease,
                            QEvent::TouchBegin,
                            QEvent::TouchUpdate,
                            QEvent::TouchEnd,
                            QEvent::TouchCancel,
                            QEvent::NativeGesture,
                            QEvent::Gesture,
                            QEvent::GestureOverride,
                            QEvent::DragEnter,
                            QEvent::DragMove,
                            QEvent::Drop,
                            QEvent::ContextMenu,
                            QEvent::Close,
                            QEvent::Quit}) {
        QEvent event(type);
        const auto before = target.deliveries;
        QCoreApplication::sendEvent(&target, &event);
        require(target.deliveries == before,
                "paused input cannot mutate widgets through a new or continuing gesture");
        if (type == QEvent::Close || type == QEvent::Quit)
            require(!event.isAccepted(), "reversible pause declines ordinary close and quit");
    }
    for (const auto type : {QEvent::Paint, QEvent::Timer, QEvent::MetaCall,
                            QEvent::InputMethodQuery, QEvent::LanguageChange}) {
        QEvent event(type);
        const auto before = target.deliveries;
        QCoreApplication::sendEvent(&target, &event);
        require(target.deliveries == before + 1,
                "pause preserves painting, completion, queries and configuration delivery");
    }
    input.active = false;
    QEvent quit(QEvent::Quit);
    const auto before = target.deliveries;
    QCoreApplication::sendEvent(&target, &quit);
    require(target.deliveries == before + 1, "terminal handoff can explicitly admit quit");

    int releases = 0;
    {
        snow_shot::app::QuiescenceSession pause;
        pause.acquired([&] { ++releases; });
        pause.commit();
        // A terminal callback can run before the synchronous preparation stack returns.
        pause.acquired([&] { ++releases; });
        pause.restore();
    }
    require(releases == 0,
            "terminal commitment also retains resources acquired later on the stack");
    {
        snow_shot::app::QuiescenceSession pause;
        pause.acquired([&] { ++releases; });
        pause.acquired([&] { pause.commit(); });
        pause.restore();
    }
    require(releases == 0,
            "terminal commitment during restoration stops remaining reopen callbacks");
    return EXIT_SUCCESS;
}
