#pragma once

#include <QApplication>
#include <QPointer>
#include <QWidget>

namespace snow_shot::presentation {

// Close dialogs before capture so their modality cannot block the screenshot overlays.
[[nodiscard]] inline bool closeActiveCaptureModalWindows() {
    while (QWidget* modal = QApplication::activeModalWidget()) {
        const QPointer<QWidget> guard(modal);
        modal->close();
        // Some dialogs ignore QWidget's Close event while closing their own modal surface.
        if (guard != nullptr && guard->isVisible()) {
            return false;
        }
    }
    return true;
}

} // namespace snow_shot::presentation
