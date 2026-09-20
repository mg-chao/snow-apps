#include "snow_shot/platform/physicalcursor.h"

#include <QPoint>
#include <QApplication>
#include <QMouseEvent>
#include <QWidget>
#include <QWindow>
#include <QVector>
#include <qpa/qwindowsysteminterface.h>
#include <private/qhighdpiscaling_p.h>

#include <array>
#include <cstdlib>
#include <iostream>
#include <limits>
#include <optional>
#include <stdexcept>
#include <vector>

#if defined(Q_OS_WIN) || defined(_WIN32)
#include <qt_windows.h>
#endif

namespace {
using snow_shot::platform::PhysicalCursor;
using snow_shot::platform::PhysicalCursorAccess;
using snow_shot::platform::PhysicalCursorDirection;
using snow_shot::platform::PhysicalCursorMoveStatus;

void require(bool condition, const char* message) {
    if (!condition) {
        throw std::runtime_error(message);
    }
}

class PointerSurface final : public QWidget {
  public:
    using QWidget::QWidget;
    int moves = 0;
    QPointF lastPosition;
    QPointF lastGlobalPosition;
    Qt::MouseButtons lastButtons;
    Qt::KeyboardModifiers lastModifiers;
    QRectF selection;
    QPointF anchor;

  protected:
    void mousePressEvent(QMouseEvent* event) override {
        anchor = event->position();
        event->accept();
    }
    void mouseMoveEvent(QMouseEvent* event) override {
        ++moves;
        lastPosition = event->position();
        lastGlobalPosition = event->globalPosition();
        lastButtons = event->buttons();
        lastModifiers = event->modifiers();
        if (event->buttons().testFlag(Qt::LeftButton))
            selection = QRectF(anchor, lastPosition).normalized();
        event->accept();
    }
};

void silentWarpsDeliverMovementThroughQt() {
    QWidget window;
    window.setGeometry(100, 100, 240, 180);
    PointerSurface surface(&window);
    surface.setGeometry(10, 10, 100, 100);
    surface.setMouseTracking(true);
    PointerSurface sibling(&window);
    sibling.setGeometry(110, 10, 100, 100);
    sibling.setMouseTracking(true);
    window.show();
    QCoreApplication::processEvents();
    QPointF desktop = surface.mapToGlobal(QPointF(20, 30));
    QPoint pixels = (desktop * 2).toPoint();
    bool writeSucceeds = true;
    bool generatesEvents = false;
    auto makeCursor = [&] {
        return PhysicalCursor(
            PhysicalCursorAccess{true, [&] { return std::optional<QPoint>(pixels); },
                                 [&](const QPoint& target) {
                                     if (!writeSucceeds)
                                         return false;
                                     pixels = target;
                                     desktop = QPointF(pixels) / 2;
                                     return true;
                                 },
                                 [&] { return std::optional<QPointF>(desktop); }, generatesEvents});
    };
    auto cursor = makeCursor();
    const auto hover = cursor.moveOnePixel(PhysicalCursorDirection::Right);
    require(hover.mouseMoveDispatched && surface.moves == 1 &&
                surface.lastPosition == QPointF(20.5, 30) &&
                surface.lastGlobalPosition == desktop && surface.lastButtons == Qt::NoButton,
            "a silent Retina warp must deliver fractional hover movement to the child");

    const QPointF pressLocal = window.windowHandle()->mapFromGlobal(desktop);
    QMouseEvent press(QEvent::MouseButtonPress, pressLocal, pressLocal, desktop, Qt::LeftButton,
                      Qt::LeftButton, Qt::ShiftModifier);
    QCoreApplication::sendEvent(window.windowHandle(), &press);
    require(QGuiApplication::mouseButtons().testFlag(Qt::LeftButton),
            "the drag fixture must retain Qt's pressed mouse button");
    const auto drag = cursor.moveOnePixel(PhysicalCursorDirection::Down);
    require(drag.mouseMoveDispatched && surface.moves == 2 &&
                surface.lastButtons.testFlag(Qt::LeftButton) &&
                surface.lastModifiers == Qt::ShiftModifier &&
                surface.selection == QRectF(QPointF(20.5, 30), QPointF(20.5, 30.5)),
            "keyboard cursor movement must update the pressed child's selection drag");
    // Move the live cursor over a sibling while the original surface owns the press.
    desktop = sibling.mapToGlobal(QPointF(20, 30));
    pixels = (desktop * 2).toPoint();
    static_cast<void>(cursor.moveOnePixel(PhysicalCursorDirection::Right));
    require(surface.moves == 3 && sibling.moves == 0 && surface.lastGlobalPosition == desktop,
            "cursor movement must preserve Qt's implicit grab across child boundaries");
    QWidget otherWindow;
    otherWindow.setGeometry(400, 100, 140, 140);
    PointerSurface otherSurface(&otherWindow);
    otherSurface.setGeometry(0, 0, 140, 140);
    otherSurface.setMouseTracking(true);
    otherWindow.show();
    QCoreApplication::processEvents();
    desktop = otherSurface.mapToGlobal(QPointF(20, 30));
    pixels = (desktop * 2).toPoint();
    static_cast<void>(cursor.moveOnePixel(PhysicalCursorDirection::Right));
    require(surface.moves == 4 && otherSurface.moves == 0 && surface.lastGlobalPosition == desktop,
            "a keyboard-driven drag must remain with its owner across top-level windows");
    const QPointF releaseLocal = window.windowHandle()->mapFromGlobal(desktop);
    QMouseEvent release(QEvent::MouseButtonRelease, releaseLocal, releaseLocal, desktop,
                        Qt::LeftButton, Qt::NoButton, Qt::NoModifier);
    QCoreApplication::sendEvent(window.windowHandle(), &release);
    writeSucceeds = false;
    require(!cursor.moveOnePixel(PhysicalCursorDirection::Left).mouseMoveDispatched &&
                surface.moves == 4 && sibling.moves == 0 && otherSurface.moves == 0,
            "failed cursor writes must not fabricate movement");
    writeSucceeds = true;
    generatesEvents = true;
    cursor = makeCursor();
    require(!cursor.moveOnePixel(PhysicalCursorDirection::Left).mouseMoveDispatched &&
                surface.moves == 4 && sibling.moves == 0 && otherSurface.moves == 0,
            "backends producing native movement must not receive duplicate events");
}

void warpsKeepNativePointerStateSynchronized() {
    PointerSurface surface;
    surface.setGeometry(100, 100, 200, 160);
    surface.setMouseTracking(true);
    surface.show();
    QCoreApplication::processEvents();
    QWindow* window = surface.windowHandle();
    const QPointF original = surface.mapToGlobal(QPointF(40, 50));
    const auto nativeMove = [&](const QPointF& global) {
        QWindowSystemInterface::handleMouseEvent<QWindowSystemInterface::SynchronousDelivery>(
            window, QHighDpi::toNativeLocalPosition(window->mapFromGlobal(global), window),
            QHighDpi::toNativePixels(global, window), Qt::NoButton, Qt::NoButton,
            QEvent::MouseMove);
    };
    nativeMove(original);
    QPoint pixels = (original * 2).toPoint();
    PhysicalCursor cursor(
        PhysicalCursorAccess{true, [&] { return std::optional<QPoint>(pixels); },
                             [&](const QPoint& target) {
                                 pixels = target;
                                 return true;
                             },
                             [&] { return std::optional<QPointF>(QPointF(pixels) / 2); }, false});
    const int before = surface.moves;
    static_cast<void>(cursor.moveOnePixel(PhysicalCursorDirection::Right));
    require(surface.moves == before + 1 && surface.lastGlobalPosition == original + QPointF(.5, 0),
            "the silent warp must advance the pointer by one physical Retina pixel");
    nativeMove(original);
    require(surface.moves == before + 2 && surface.lastGlobalPosition == original,
            "the next real movement back to the pre-warp position must not be discarded");
    surface.grabMouse();
    static_cast<void>(cursor.moveOnePixel(PhysicalCursorDirection::Down));
    require(surface.moves == before + 3 && surface.lastGlobalPosition == QPointF(pixels) / 2,
            "an explicit mouse grab must receive keyboard cursor movement");
    surface.releaseMouse();
}

void everyDirectionRequestsOnePhysicalPixel() {
    QPoint livePosition(300, 240);
    QVector<QPoint> writes;
    PhysicalCursor cursor(PhysicalCursorAccess{
        true,
        [&livePosition]() { return std::optional<QPoint>(livePosition); },
        [&livePosition, &writes](const QPoint& target) {
            writes.push_back(target);
            livePosition = target;
            return true;
        },
    });

    const struct {
        PhysicalCursorDirection direction;
        QPoint expected;
    } cases[] = {
        {PhysicalCursorDirection::Up, QPoint(300, 239)},
        {PhysicalCursorDirection::Down, QPoint(300, 240)},
        {PhysicalCursorDirection::Left, QPoint(299, 240)},
        {PhysicalCursorDirection::Right, QPoint(300, 240)},
    };
    for (const auto& testCase : cases) {
        const auto result = cursor.moveOnePixel(testCase.direction);
        require(result.status == PhysicalCursorMoveStatus::Applied &&
                    result.position == testCase.expected && writes.constLast() == testCase.expected,
                "a cursor direction did not request exactly one physical pixel");
    }
}

void everyMoveStartsFromTheLivePosition() {
    QPoint livePosition(10, 20);
    int readCount = 0;
    QVector<QPoint> writes;
    PhysicalCursor cursor(PhysicalCursorAccess{
        true,
        [&livePosition, &readCount]() {
            ++readCount;
            return std::optional<QPoint>(livePosition);
        },
        [&livePosition, &writes](const QPoint& target) {
            writes.push_back(target);
            livePosition = target;
            return true;
        },
    });

    require(cursor.moveOnePixel(PhysicalCursorDirection::Right).position == QPoint(11, 20),
            "the first cursor move used the wrong live position");
    livePosition = QPoint(80, 90);
    require(cursor.moveOnePixel(PhysicalCursorDirection::Up).position == QPoint(80, 89) &&
                readCount == 4 && writes == QVector<QPoint>{QPoint(11, 20), QPoint(80, 89)},
            "a cursor move reused a cached position");
}

void operatingSystemResolutionIsReadBack() {
    const QPoint livePosition(199, 230);
    QPoint requested;
    PhysicalCursor cursor(PhysicalCursorAccess{
        true,
        [&livePosition]() { return std::optional<QPoint>(livePosition); },
        [&requested](const QPoint& target) {
            requested = target;
            return true;
        },
    });

    const auto result = cursor.moveOnePixel(PhysicalCursorDirection::Right);
    require(requested == QPoint(200, 230),
            "the cursor service clamped a target before submitting it to the operating system");
    require(result.status == PhysicalCursorMoveStatus::Applied && result.position == livePosition &&
                result.commandApplied(),
            "the cursor service did not return the operating system's resolved position");
}

void failuresHaveDistinctOutcomes() {
    PhysicalCursor readOnly(
        PhysicalCursorAccess{true, [] { return std::optional<QPoint>(QPoint(-10, 20)); }, {}});
    require(readOnly.canRead() && !readOnly.isSupported() &&
                readOnly.position() == QPoint(-10, 20) &&
                !readOnly.moveOnePixel(PhysicalCursorDirection::Right).commandApplied(),
            "pointer reading must remain available without a cursor writer");
    PhysicalCursor unsupported;
#if defined(Q_OS_WIN) || defined(_WIN32)
    require(unsupported.isSupported(), "the Windows physical cursor backend is unavailable");
#else
    require(!unsupported.isSupported() &&
                unsupported.moveOnePixel(PhysicalCursorDirection::Up).status ==
                    PhysicalCursorMoveStatus::Unsupported,
            "a process without a native GUI exposed cursor movement");
#endif

    PhysicalCursor readFailure(PhysicalCursorAccess{
        true,
        []() -> std::optional<QPoint> { return std::nullopt; },
        [](const QPoint&) { return true; },
    });
    require(readFailure.moveOnePixel(PhysicalCursorDirection::Up).status ==
                PhysicalCursorMoveStatus::ReadFailed,
            "an initial physical cursor read failure was misreported");

    int writeCount = 0;
    PhysicalCursor invalidTarget(PhysicalCursorAccess{
        true,
        []() { return std::optional<QPoint>(QPoint(std::numeric_limits<int>::max(), 0)); },
        [&writeCount](const QPoint&) {
            ++writeCount;
            return true;
        },
    });
    require(invalidTarget.moveOnePixel(PhysicalCursorDirection::Right).status ==
                    PhysicalCursorMoveStatus::InvalidTarget &&
                writeCount == 0,
            "an overflowing physical cursor coordinate reached the writer");

    PhysicalCursor writeFailure(PhysicalCursorAccess{
        true,
        []() { return std::optional<QPoint>(QPoint(20, 30)); },
        [](const QPoint&) { return false; },
    });
    require(writeFailure.moveOnePixel(PhysicalCursorDirection::Down).status ==
                PhysicalCursorMoveStatus::WriteFailed,
            "a physical cursor write failure was misreported");

    int readCount = 0;
    PhysicalCursor finalReadFailure(PhysicalCursorAccess{
        true,
        [&readCount]() -> std::optional<QPoint> {
            ++readCount;
            return readCount == 1 ? std::optional<QPoint>(QPoint(40, 50)) : std::nullopt;
        },
        [](const QPoint&) { return true; },
    });
    const auto finalReadResult = finalReadFailure.moveOnePixel(PhysicalCursorDirection::Left);
    require(finalReadResult.status == PhysicalCursorMoveStatus::AppliedPositionUnavailable &&
                finalReadResult.commandApplied() && !finalReadResult.position.has_value(),
            "a successful write followed by a failed read was not consumed safely");
}

#if defined(Q_OS_WIN) || defined(_WIN32)
class CursorRestorer final {
  public:
    CursorRestorer() : m_valid(GetPhysicalCursorPos(&m_position) != FALSE) {}
    ~CursorRestorer() {
        if (m_valid) {
            static_cast<void>(SetPhysicalCursorPos(m_position.x, m_position.y));
        }
    }

    [[nodiscard]] bool valid() const noexcept {
        return m_valid;
    }

  private:
    POINT m_position{};
    bool m_valid = false;
};

BOOL CALLBACK collectMonitorRect(HMONITOR monitor, HDC, LPRECT, LPARAM data) {
    auto* monitors = reinterpret_cast<std::vector<RECT>*>(data);
    MONITORINFO info{};
    info.cbSize = static_cast<DWORD>(sizeof(info));
    if (monitors == nullptr || GetMonitorInfoW(monitor, &info) == FALSE) {
        return TRUE;
    }
    monitors->push_back(info.rcMonitor);
    return TRUE;
}

void windowsBackendMovesOnePhysicalPixelOnEveryMonitor() {
    CursorRestorer restorer;
    require(restorer.valid(), "the original physical cursor position could not be read");

    std::vector<RECT> monitors;
    require(EnumDisplayMonitors(nullptr, nullptr, collectMonitorRect,
                                reinterpret_cast<LPARAM>(&monitors)) != FALSE &&
                !monitors.empty(),
            "physical monitor rectangles could not be enumerated");

    PhysicalCursor cursor;
    const std::array<std::pair<PhysicalCursorDirection, QPoint>, 4> movements = {{
        {PhysicalCursorDirection::Up, QPoint(0, -1)},
        {PhysicalCursorDirection::Down, QPoint(0, 1)},
        {PhysicalCursorDirection::Left, QPoint(-1, 0)},
        {PhysicalCursorDirection::Right, QPoint(1, 0)},
    }};
    for (const RECT& monitor : monitors) {
        const QPoint start(static_cast<int>(monitor.left + (monitor.right - monitor.left) / 2),
                           static_cast<int>(monitor.top + (monitor.bottom - monitor.top) / 2));
        for (const auto& movement : movements) {
            require(SetPhysicalCursorPos(start.x(), start.y()) != FALSE,
                    "the native test cursor could not be placed on a monitor");
            const auto result = cursor.moveOnePixel(movement.first);
            require(result.status == PhysicalCursorMoveStatus::Applied &&
                        result.position == start + movement.second,
                    "the Windows backend did not move by one physical monitor pixel");
        }
    }
}
#endif

} // namespace

int main(int argc, char** argv) {
    qputenv("QT_QPA_PLATFORM", "offscreen");
    QApplication app(argc, argv);
    PhysicalCursor logical(
        PhysicalCursorAccess{true,
                             [] { return std::optional<QPoint>(QPoint(-1, 40)); },
                             {},
                             [] { return std::optional<QPointF>(QPointF(-0.5, 20)); }});
    require(logical.logicalPosition() == QPointF(-0.5, 20),
            "fractional desktop position must retain display ownership at a Retina boundary");

    try {
        silentWarpsDeliverMovementThroughQt();
        warpsKeepNativePointerStateSynchronized();
        everyDirectionRequestsOnePhysicalPixel();
        everyMoveStartsFromTheLivePosition();
        operatingSystemResolutionIsReadBack();
        failuresHaveDistinctOutcomes();
#if defined(Q_OS_WIN) || defined(_WIN32)
        windowsBackendMovesOnePhysicalPixelOnEveryMonitor();
#endif
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return EXIT_FAILURE;
    }
    return EXIT_SUCCESS;
}
