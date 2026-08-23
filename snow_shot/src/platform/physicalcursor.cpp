#include "snow_shot/platform/physicalcursor.h"

#include <QtGlobal>

#include <limits>
#include <utility>

#if defined(Q_OS_WIN) || defined(_WIN32)
#include <qt_windows.h>
#endif

namespace snow_shot::platform {
namespace {

PhysicalCursorAccess nativeAccess() {
#if defined(Q_OS_WIN) || defined(_WIN32)
    return PhysicalCursorAccess{
        true,
        []() -> std::optional<QPoint> {
            POINT position{};
            if (GetPhysicalCursorPos(&position) == FALSE) {
                return std::nullopt;
            }
            return QPoint(position.x, position.y);
        },
        [](const QPoint& position) {
            return SetPhysicalCursorPos(position.x(), position.y()) != FALSE;
        },
    };
#else
    return {};
#endif
}

QPoint offsetForDirection(PhysicalCursorDirection direction) {
    switch (direction) {
    case PhysicalCursorDirection::Up:
        return QPoint(0, -1);
    case PhysicalCursorDirection::Down:
        return QPoint(0, 1);
    case PhysicalCursorDirection::Left:
        return QPoint(-1, 0);
    case PhysicalCursorDirection::Right:
        return QPoint(1, 0);
    }
    Q_UNREACHABLE_RETURN(QPoint());
}

std::optional<QPoint> targetPosition(const QPoint& current, PhysicalCursorDirection direction) {
    const QPoint offset = offsetForDirection(direction);
    const qint64 x = static_cast<qint64>(current.x()) + offset.x();
    const qint64 y = static_cast<qint64>(current.y()) + offset.y();
    if (x < std::numeric_limits<int>::min() || x > std::numeric_limits<int>::max() ||
        y < std::numeric_limits<int>::min() || y > std::numeric_limits<int>::max()) {
        return std::nullopt;
    }
    return QPoint(static_cast<int>(x), static_cast<int>(y));
}

} // namespace

PhysicalCursor::PhysicalCursor() : m_access(nativeAccess()) {}

PhysicalCursor::PhysicalCursor(PhysicalCursorAccess access) : m_access(std::move(access)) {}

bool PhysicalCursor::isSupported() const noexcept {
    return m_access.supported && static_cast<bool>(m_access.readPosition) &&
           static_cast<bool>(m_access.writePosition);
}

std::optional<QPoint> PhysicalCursor::position() const {
    if (!isSupported()) {
        return std::nullopt;
    }
    return m_access.readPosition();
}

PhysicalCursorMoveResult PhysicalCursor::moveOnePixel(PhysicalCursorDirection direction) const {
    if (!isSupported()) {
        return {PhysicalCursorMoveStatus::Unsupported, std::nullopt};
    }

    const std::optional<QPoint> current = m_access.readPosition();
    if (!current.has_value()) {
        return {PhysicalCursorMoveStatus::ReadFailed, std::nullopt};
    }

    const std::optional<QPoint> target = targetPosition(current.value(), direction);
    if (!target.has_value()) {
        return {PhysicalCursorMoveStatus::InvalidTarget, std::nullopt};
    }
    if (!m_access.writePosition(target.value())) {
        return {PhysicalCursorMoveStatus::WriteFailed, std::nullopt};
    }

    const std::optional<QPoint> actual = m_access.readPosition();
    if (!actual.has_value()) {
        return {PhysicalCursorMoveStatus::AppliedPositionUnavailable, std::nullopt};
    }
    return {PhysicalCursorMoveStatus::Applied, actual};
}

} // namespace snow_shot::platform
