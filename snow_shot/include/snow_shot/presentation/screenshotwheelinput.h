#pragma once

#include "snow_draw_engine_qt/snow_canvas_wheel_input.h"

#include <QWheelEvent>

namespace snow_shot::presentation {

inline bool usesPreciseWheelDelta(const QWheelEvent& event) {
#ifdef Q_OS_MACOS
    // Qt Cocoa supplies estimated pixels for ordinary mouse notches too. Its
    // angle delta preserves the notch, while the pixel estimate is accelerated
    // and can be as small as two pixels. True precise Cocoa input is marked
    // MouseEventSynthesizedBySystem, including devices without scroll phases.
    if (event.source() == Qt::MouseEventNotSynthesized && event.phase() == Qt::NoScrollPhase &&
        !event.angleDelta().isNull()) {
        return false;
    }
#endif
    return !event.pixelDelta().isNull();
}

// The canvas library owns discrete adjustment thresholds and gesture boundaries.
using WheelStepAccumulator = snow_canvas_wheel::StepAccumulator;

} // namespace snow_shot::presentation
