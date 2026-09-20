#ifndef SNOW_SHOT_PLATFORM_PHYSICALCURSOR_H
#define SNOW_SHOT_PLATFORM_PHYSICALCURSOR_H

#include <QPoint>
#include <QPointF>

#include <functional>
#include <optional>

namespace snow_shot::platform {

enum class PhysicalCursorDirection {
    Up,
    Down,
    Left,
    Right,
};

enum class PhysicalCursorMoveStatus {
    Unsupported,
    ReadFailed,
    InvalidTarget,
    WriteFailed,
    Applied,
    AppliedPositionUnavailable,
};

struct PhysicalCursorMoveResult {
    PhysicalCursorMoveStatus status = PhysicalCursorMoveStatus::Unsupported;
    std::optional<QPoint> position;
    bool mouseMoveDispatched = false;

    [[nodiscard]] bool commandApplied() const noexcept {
        return status == PhysicalCursorMoveStatus::Applied ||
               status == PhysicalCursorMoveStatus::AppliedPositionUnavailable;
    }
};

struct PhysicalCursorAccess {
    bool supported = false;
    std::function<std::optional<QPoint>()> readPosition;
    std::function<bool(const QPoint&)> writePosition;
    std::function<std::optional<QPointF>()> readLogicalPosition = {};
    // Cursor warps on macOS do not generate native mouse movement events.
    bool generatesMouseMoveEvents = true;
};

class PhysicalCursor final {
  public:
    PhysicalCursor();
    explicit PhysicalCursor(PhysicalCursorAccess access);

    [[nodiscard]] bool isSupported() const noexcept;
    [[nodiscard]] bool canRead() const noexcept;
    [[nodiscard]] std::optional<QPoint> position() const;
    [[nodiscard]] std::optional<QPointF> logicalPosition() const;
    [[nodiscard]] PhysicalCursorMoveResult moveOnePixel(PhysicalCursorDirection direction) const;

  private:
    PhysicalCursorAccess m_access;
};

} // namespace snow_shot::platform

#endif // SNOW_SHOT_PLATFORM_PHYSICALCURSOR_H
