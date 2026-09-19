#include "snow_shot/presentation/windowgeometrymemory.h"

#include "snow_shot/storage/persistedwindowgeometry.h"
#include "snow_shot/storage/settingsadapters.h"

#include <QEvent>
#include <QGuiApplication>
#include <QList>
#include <QScreen>
#include <QTimer>

namespace snow_shot::presentation {
namespace {
// Interactive drags must not reach the configuration store on every step; a short
// debounce keeps the persisted geometry current while a session is still crash-safe.
constexpr int GEOMETRY_SAVE_DEBOUNCE_MILLISECONDS = 250;

QList<QRect> availableScreenGeometriesPrimaryFirst() {
    // The clamp fallback targets the first entry, so list the primary screen first
    // regardless of how QGuiApplication ordered the connected screens.
    QList<QScreen*> screens = QGuiApplication::screens();
    if (QScreen* primary = QGuiApplication::primaryScreen()) {
        screens.removeAll(primary);
        screens.prepend(primary);
    }
    QList<QRect> availableGeometries;
    availableGeometries.reserve(screens.size());
    for (QScreen* screen : screens) {
        availableGeometries.push_back(screen->availableGeometry());
    }
    return availableGeometries;
}
} // namespace

QRect persistableNormalGeometry(const QWidget& widget) {
    QRect normalGeometry = widget.normalGeometry();
    if (!normalGeometry.isValid() || normalGeometry.isEmpty()) {
        normalGeometry = widget.geometry();
    }
    return normalGeometry;
}

QSize persistableWindowSize(const QWidget& widget) {
    return persistableNormalGeometry(widget).size();
}

WindowGeometryMemory::WindowGeometryMemory(QWidget* widget) : m_widget(widget) {
    m_saveTimer = new QTimer(this);
    m_saveTimer->setSingleShot(true);
    connect(m_saveTimer, &QTimer::timeout, this, &WindowGeometryMemory::save);
    if (m_widget != nullptr) {
        m_widget->installEventFilter(this);
    }
}

WindowGeometryMemory::~WindowGeometryMemory() {
    m_saveTimer->stop();
    // WA_DeleteOnClose and the application-shutdown delete both funnel here; the
    // debounced timer may still hold the latest geometry, so persist immediately.
    save();
}

void WindowGeometryMemory::restoreMainWindow(const QSize& minimumSize) {
    const std::optional<storage::PersistedWindowGeometry> saved =
        storage::WindowMemorySettings().mainWindowGeometry();
    if (!saved.has_value()) {
        return;
    }
    const QList<QRect> availableGeometries = availableScreenGeometriesPrimaryFirst();
    if (availableGeometries.isEmpty()) {
        return;
    }
    const storage::PersistedWindowGeometry fitted =
        storage::fitPersistedWindowGeometry(*saved, minimumSize, availableGeometries);
    m_pendingRestoreGeometry = fitted.normalGeometry;
    m_restoreMaximized = fitted.maximized;
}

void WindowGeometryMemory::captureAcceptedClose() {
    if (m_widget == nullptr || !m_wasShown) {
        return;
    }
    const QRect normalGeometry = persistableNormalGeometry(*m_widget);
    if (!normalGeometry.isValid() || normalGeometry.isEmpty()) {
        return;
    }
    m_closeGeometry = normalGeometry;
    m_closeGeometryMaximized = m_widget->isMaximized();
    m_hasCloseGeometry = true;
}

bool WindowGeometryMemory::eventFilter(QObject* watched, QEvent* event) {
    if (watched != m_widget || event == nullptr) {
        return false;
    }
    switch (event->type()) {
    case QEvent::Show:
        m_wasShown = true;
        applyPendingRestore();
        break;
    case QEvent::Move:
    case QEvent::Resize:
    case QEvent::WindowStateChange:
        scheduleSave();
        break;
    default:
        break;
    }
    return false;
}

void WindowGeometryMemory::applyPendingRestore() {
    if (m_widget == nullptr || !m_pendingRestoreGeometry.has_value()) {
        return;
    }
    const QRect geometry = *m_pendingRestoreGeometry;
    m_pendingRestoreGeometry.reset();
    m_widget->setGeometry(geometry);
    if (m_restoreMaximized) {
        m_widget->setWindowState(m_widget->windowState() | Qt::WindowMaximized);
    }
}

void WindowGeometryMemory::scheduleSave() {
    if (!m_wasShown) {
        return;
    }
    m_saveTimer->start(GEOMETRY_SAVE_DEBOUNCE_MILLISECONDS);
}

void WindowGeometryMemory::save() {
    if (m_widget == nullptr || !m_wasShown) {
        return;
    }
    if (m_widget->isVisible()) {
        const QRect normalGeometry = persistableNormalGeometry(*m_widget);
        if (normalGeometry.isValid() && !normalGeometry.isEmpty()) {
            storage::WindowMemorySettings().setMainWindowGeometry(normalGeometry,
                                                                  m_widget->isMaximized());
        }
        return;
    }
    // Hidden windows report unreliable normalGeometry(), so a window deleted after
    // its close (WA_DeleteOnClose) persists the geometry snapshot captured at close.
    if (m_hasCloseGeometry) {
        storage::WindowMemorySettings().setMainWindowGeometry(m_closeGeometry,
                                                              m_closeGeometryMaximized);
    }
}

} // namespace snow_shot::presentation
