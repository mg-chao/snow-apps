#include "snow_shot/presentation/fullscreencanvascontroller.h"
#include "snow_shot/presentation/fullscreencanvaswindow.h"

#include <QCursor>
#include <QGuiApplication>
#include <QScreen>

namespace snow_shot::presentation {
FullscreenCanvasController::FullscreenCanvasController(QObject* parent) : QObject(parent) {}

FullscreenCanvasController::~FullscreenCanvasController() {
    shutdown();
}

void FullscreenCanvasController::activate() {
    if (m_window) {
        static_cast<void>(m_window->setClickThrough(!m_window->clickThrough()));
        return;
    }
    QScreen* screen = QGuiApplication::screenAt(QCursor::pos());
    if (screen == nullptr)
        screen = QGuiApplication::primaryScreen();
    if (screen == nullptr) {
        emit operationFailed(tr("No screen is available for the full-screen canvas."), true);
        return;
    }
    auto* window = new FullscreenCanvasWindow(screen);
    m_window = window;
    window->setAttribute(Qt::WA_DeleteOnClose);
    connect(window, &FullscreenCanvasWindow::operationFailed, this,
            &FullscreenCanvasController::operationFailed);
    connect(window, &FullscreenCanvasWindow::closed, this, [this, window] {
        if (m_window == window)
            m_window = nullptr;
    });
    if (!window->present()) {
        m_window = nullptr;
        delete window;
    }
}

void FullscreenCanvasController::shutdown() {
    delete m_window.data();
    m_window = nullptr;
}

FullscreenCanvasWindow* FullscreenCanvasController::window() const {
    return m_window.data();
}
} // namespace snow_shot::presentation
