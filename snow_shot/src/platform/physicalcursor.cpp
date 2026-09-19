#include "snow_shot/platform/physicalcursor.h"

#include <QtGlobal>

#include <limits>
#include <utility>

#if defined(Q_OS_WIN) || defined(_WIN32)
#include <qt_windows.h>
#elif defined(Q_OS_MACOS)
#include <CoreGraphics/CoreGraphics.h>
#include <QGuiApplication>
#include <QScreen>
#include <QPointer>
#include <memory>
#include <cmath>
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
#elif defined(Q_OS_MACOS)
    if (!qGuiApp || QGuiApplication::platformName() != QStringLiteral("cocoa"))
        return {};
    auto display = std::make_shared<QPointer<QScreen>>();
    return PhysicalCursorAccess{
        true,
        [display]() -> std::optional<QPoint> {
            CGEventRef event = CGEventCreate(nullptr);
            if (!event)
                return std::nullopt;
            const CGPoint point = CGEventGetLocation(event);
            CFRelease(event);
            const QPointF desktop(point.x, point.y);
            *display = QGuiApplication::screenAt(QPoint(static_cast<int>(std::floor(desktop.x())),
                                                        static_cast<int>(std::floor(desktop.y()))));
            if (!*display)
                return std::nullopt;
            const QPoint origin = (*display)->geometry().topLeft();
            return origin + ((desktop - origin) * (*display)->devicePixelRatio()).toPoint();
        },
        [display](const QPoint& pixels) {
            if (!*display)
                return false;
            const QPoint origin = (*display)->geometry().topLeft();
            const QPointF desktop =
                QPointF(origin) + QPointF(pixels - origin) / (*display)->devicePixelRatio();
            if (!QGuiApplication::screenAt(QPoint(static_cast<int>(std::floor(desktop.x())),
                                                  static_cast<int>(std::floor(desktop.y())))))
                return false;
            if (CGWarpMouseCursorPosition(CGPointMake(desktop.x(), desktop.y())) != kCGErrorSuccess)
                return false;
            CGEventRef event = CGEventCreate(nullptr);
            if (!event)
                return false;
            const CGPoint actual = CGEventGetLocation(event);
            CFRelease(event);
            const qreal tolerance = .51 / (*display)->devicePixelRatio();
            return qAbs(actual.x - desktop.x()) < tolerance &&
                   qAbs(actual.y - desktop.y()) < tolerance;
        },
        []() -> std::optional<QPointF> {
            CGEventRef event = CGEventCreate(nullptr);
            if (!event)
                return std::nullopt;
            const CGPoint point = CGEventGetLocation(event);
            CFRelease(event);
            return QPointF(point.x, point.y);
        }};
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

bool PhysicalCursor::canRead() const noexcept {
    return m_access.supported && static_cast<bool>(m_access.readPosition);
}

std::optional<QPoint> PhysicalCursor::position() const {
    if (!canRead()) {
        return std::nullopt;
    }
    return m_access.readPosition();
}

std::optional<QPointF> PhysicalCursor::logicalPosition() const {
    return m_access.readLogicalPosition ? m_access.readLogicalPosition() : std::nullopt;
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
