#ifndef SNOW_SHOT_PRESENTATION_SCREENSHOTRECORDINGWORKFLOW_H
#define SNOW_SHOT_PRESENTATION_SCREENSHOTRECORDINGWORKFLOW_H

#include <QObject>
#include <QRect>
#include <QTimer>

#include <functional>

namespace snow_shot::presentation::recording {

struct ScreenshotRecordingWorkflowContext {
    QObject& owner;
    std::function<bool()> ensureRecording;
    std::function<void()> stopScrolling;
    std::function<void()> resetEditing;
    std::function<void()> invalidateRecognition;
    std::function<void()> cancelCapture;
    std::function<void()> resetHistoryNavigation;
    std::function<void(const QRect&)> openRecording;
};

inline void startScreenshotRecording(const QRect& selection, const QPoint& canvasOrigin,
                                     const ScreenshotRecordingWorkflowContext& context) {
    if (selection.isEmpty() || !context.ensureRecording()) {
        return;
    }
    const QRect physicalRegion = selection.translated(canvasOrigin);
    if (physicalRegion.width() < 2 || physicalRegion.height() < 2) {
        return;
    }
    context.stopScrolling();
    context.resetEditing();
    context.invalidateRecognition();
    context.cancelCapture();
    context.resetHistoryNavigation();
    QTimer::singleShot(0, &context.owner,
                       [open = context.openRecording, physicalRegion]() { open(physicalRegion); });
}

} // namespace snow_shot::presentation::recording

#endif // SNOW_SHOT_PRESENTATION_SCREENSHOTRECORDINGWORKFLOW_H
