// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "snow_shot/presentation/screenshotrecognitionwindow.h"
#include "widgets/context_menu.h"

struct ScreenshotRecognitionWindowTestAccess {
    static QRectF canvasSelection(const ScreenshotRecognitionWindow& window) {
        return window.m_canvasSelection;
    }
    static ScreenshotOcrPresentation* ocrPresentation(const ScreenshotRecognitionWindow& window) {
        return window.m_ocrPresentation.get();
    }
    static void populateOcrMenu(ScreenshotRecognitionWindow& window,
                                adqt::widgets::AdContextMenu& menu) {
        window.populateOcrContextMenu(menu);
    }
    static int recognitionRetryReceivers(const ScreenshotRecognitionWindow& window) {
        return window.receivers(SIGNAL(recognitionRetryRequested()));
    }
    static void populateTextMenu(ScreenshotRecognitionWindow& window,
                                 adqt::widgets::AdContextMenu& menu) {
        window.populateTextEditorContextMenu(menu);
    }
};
