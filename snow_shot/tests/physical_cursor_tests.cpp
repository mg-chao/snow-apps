#include "snow_shot/platform/physicalcursor.h"

#include <QPoint>
#include <QVector>

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
    PhysicalCursor unsupported;
#if defined(Q_OS_WIN) || defined(_WIN32)
    require(unsupported.isSupported(), "the Windows physical cursor backend is unavailable");
#else
    require(!unsupported.isSupported() &&
                unsupported.moveOnePixel(PhysicalCursorDirection::Up).status ==
                    PhysicalCursorMoveStatus::Unsupported,
            "a non-Windows build exposed logical cursor movement as physical movement");
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

int main() {
    try {
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
