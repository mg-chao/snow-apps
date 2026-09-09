#include "snow_shot/presentation/screenrecordingcontroller.h"
#include "snow_shot/presentation/screenrecordingareawindow.h"
#include "snow_shot/presentation/screenrecordingtoolbarwindow.h"
#include "snow_shot/presentation/screenshotgeometry.h"
#include "snow_shot/presentation/screenshottoolpalettehost.h"
#include "widgets/button.h"

#include <QApplication>
#include <QDebug>
#include <QElapsedTimer>
#include <QDir>
#include <QImage>
#include <QScreen>
#include <QThread>
#include <QWindow>
#include <qt_windows.h>

namespace {
void settleWindows() {
    QElapsedTimer timer;
    timer.start();
    while (timer.elapsed() < 300) {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
        QThread::msleep(1);
    }
}

QRect nativeClient(QWidget& widget) {
    const HWND handle = reinterpret_cast<HWND>(widget.winId());
    RECT client{};
    POINT origin{};
    if (!GetClientRect(handle, &client) || !ClientToScreen(handle, &origin)) {
        return {};
    }
    return {origin.x, origin.y, client.right - client.left, client.bottom - client.top};
}

QImage captureDesktopRect(const QRect& bounds) {
    if (bounds.isEmpty()) {
        return {};
    }
    QImage result(bounds.size(), QImage::Format_RGB32);
    const HDC desktop = GetDC(nullptr);
    if (!desktop) {
        return {};
    }
    const HDC memory = CreateCompatibleDC(desktop);
    BITMAPINFO info{};
    info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    info.bmiHeader.biWidth = bounds.width();
    info.bmiHeader.biHeight = -bounds.height();
    info.bmiHeader.biPlanes = 1;
    info.bmiHeader.biBitCount = 32;
    info.bmiHeader.biCompression = BI_RGB;
    void* pixels = nullptr;
    const HBITMAP bitmap = CreateDIBSection(desktop, &info, DIB_RGB_COLORS, &pixels, nullptr, 0);
    if (!desktop || !memory || !bitmap || !pixels) {
        if (bitmap)
            DeleteObject(bitmap);
        if (memory)
            DeleteDC(memory);
        if (desktop)
            ReleaseDC(nullptr, desktop);
        return {};
    }
    const HGDIOBJ previous = SelectObject(memory, bitmap);
    const bool copied = BitBlt(memory, 0, 0, bounds.width(), bounds.height(), desktop, bounds.x(),
                               bounds.y(), SRCCOPY | CAPTUREBLT) != FALSE;
    GdiFlush();
    if (copied) {
        result = QImage(static_cast<const uchar*>(pixels), bounds.width(), bounds.height(),
                        QImage::Format_RGB32)
                     .copy();
    }
    SelectObject(memory, previous);
    DeleteObject(bitmap);
    DeleteDC(memory);
    ReleaseDC(nullptr, desktop);
    return copied ? result : QImage();
}

bool renderedContentMatchesDesktop(ScreenRecordingToolbarWindow& toolbar, const char* phase) {
    // Capture the compositor output FIRST. QWidget::render/grab can repaint a stale
    // backing store and would otherwise hide the exact defect this test must detect.
    const QImage actual = captureDesktopRect(nativeClient(toolbar));
    const qreal dpr = toolbar.devicePixelRatioF();
    QImage expected(QSize(qRound(toolbar.width() * dpr), qRound(toolbar.height() * dpr)),
                    QImage::Format_ARGB32_Premultiplied);
    expected.setDevicePixelRatio(dpr);
    expected.fill(Qt::transparent);
    toolbar.render(&expected);
    const QString directory = QDir::current().filePath(QStringLiteral("toolbar-display-artifacts"));
    if (!QDir().mkpath(directory) ||
        !actual.save(directory + QLatin1Char('/') + QString::fromLatin1(phase) + "-desktop.png") ||
        !expected.save(directory + QLatin1Char('/') + QString::fromLatin1(phase) + "-render.png")) {
        qCritical() << "Could not save rendering evidence" << directory;
        return false;
    }
    if (actual.size() != expected.size()) {
        return false;
    }
    int opaquePixels = 0;
    int differentPixels = 0;
    for (int y = 0; y < expected.height(); ++y) {
        for (int x = 0; x < expected.width(); ++x) {
            const QColor reference = expected.pixelColor(x, y);
            // Ignore transparent desktop/shadow pixels and blended rounded edges.
            if (reference.alpha() != 255) {
                continue;
            }
            ++opaquePixels;
            const QColor observed = actual.pixelColor(x, y);
            if (qMax(qAbs(reference.red() - observed.red()),
                     qMax(qAbs(reference.green() - observed.green()),
                          qAbs(reference.blue() - observed.blue()))) > 40) {
                ++differentPixels;
            }
        }
    }
    qInfo() << phase << "opaque pixels" << opaquePixels << "different pixels" << differentPixels
            << "evidence" << directory;
    // Tolerate small font/animation differences, but not missing or enlarged rows.
    return opaquePixels > 1000 && differentPixels <= opaquePixels / 50;
}
} // namespace

// Invoked by the controller fixture, with its fake capture backend and isolated settings.
// Real Windows surfaces are essential: offscreen QPA cannot exercise WM_DPICHANGED.
int recordingToolbarAcrossNativeDisplays(bool startCapture) {
    QScreen* displayA = nullptr;
    QScreen* displayB = nullptr;
    for (QScreen* a : QGuiApplication::screens()) {
        const QRect aBounds = ScreenshotGeometryMapper::physicalRectForScreen(*a);
        qInfo() << "Display" << a->name() << "DPR" << a->devicePixelRatio() << aBounds;
        for (QScreen* b : QGuiApplication::screens()) {
            const QRect bBounds = ScreenshotGeometryMapper::physicalRectForScreen(*b);
            if (qAbs(a->devicePixelRatio() - 1.5) < 0.01 &&
                qAbs(b->devicePixelRatio() - 1.0) < 0.01 && bBounds.right() + 1 == aBounds.left() &&
                qMax(aBounds.top(), bBounds.top()) <= qMin(aBounds.bottom(), bBounds.bottom())) {
                displayA = a;
                displayB = b;
            }
        }
    }
    if (QGuiApplication::platformName() != QStringLiteral("windows") || !displayA || !displayB) {
        qWarning()
            << "SKIP: requires Windows with a 100% display B immediately left of a 150% display A";
        return 77;
    }

    int failures = 0;
    const auto check = [&](bool condition, const char* message) {
        if (!condition) {
            qCritical() << "FAIL:" << message;
            ++failures;
        }
    };
    ScreenRecordingController controller;
    const QRect aBounds = ScreenshotGeometryMapper::physicalRectForScreen(*displayA);
    const QRect bBounds = ScreenshotGeometryMapper::physicalRectForScreen(*displayB);
    controller.open(QRect(aBounds.center() - QPoint(240, 160), QSize(480, 320)));
    ScreenRecordingAreaWindow* area = nullptr;
    ScreenRecordingToolbarWindow* toolbar = nullptr;
    for (auto* widget : QApplication::topLevelWidgets()) {
        if (auto* candidate = qobject_cast<ScreenRecordingAreaWindow*>(widget)) {
            area = candidate;
        }
        if (auto* candidate = qobject_cast<ScreenRecordingToolbarWindow*>(widget)) {
            toolbar = candidate;
        }
    }
    if (!area || !toolbar) {
        qCritical() << "FAIL: controller must create both recording windows";
        return 1;
    }
    if (startCapture) {
        controller.startRecording();
    }
    settleWindows();
    check(controller.isRecording() == startCapture,
          "source recording state must match the scenario");
    auto* settings = toolbar->palette()->findChild<adqt::widgets::AdButton*>(
        QStringLiteral("screenRecordingExportSettings"));
    if (!settings) {
        qCritical() << "FAIL: export settings control must exist";
        return 1;
    }
    if (!toolbar->palette()->recordingExportSettingsVisible()) {
        settings->click();
    }
    settleWindows();
    check(area->inputMode() == ScreenRecordingAreaWindow::InputMode::RegionEditing,
          "export settings must enable moving the active recording area");
    check(toolbar->screen() == displayA &&
              GetDpiForWindow(reinterpret_cast<HWND>(toolbar->winId())) == 144,
          "source toolbar must begin on A at 144 DPI");
    if (failures) {
        return 1;
    }

    check(renderedContentMatchesDesktop(*toolbar, startCapture ? "active-source" : "source"),
          "source toolbar compositor output must match its complete rendered content");
    if (failures) {
        return 1;
    }

    const HWND areaHandle = reinterpret_cast<HWND>(area->winId());
    const QPoint hitPoint = area->physicalRegion().center();
    check(SendMessageW(areaHandle, WM_NCHITTEST, 0, MAKELPARAM(hitPoint.x(), hitPoint.y())) ==
              HTCAPTION,
          "PREREQUISITE: recording area must accept dragging in the requested recording state");
    if (failures) {
        return 1;
    }
    int starts = 0;
    int finishes = 0;
    QObject::connect(area, &ScreenRecordingAreaWindow::regionInteractionStarted, area,
                     [&]() { ++starts; });
    QObject::connect(area, &ScreenRecordingAreaWindow::regionInteractionFinished, area,
                     [&]() { ++finishes; });
    // Replay the native move lifecycle without taking over the user's mouse. SetWindowPos
    // crosses the real monitor boundary and lets Windows deliver the actual DPI change.
    SendMessageW(areaHandle, WM_ENTERSIZEMOVE, 0, 0);
    check(!toolbar->isVisible(), "toolbar must hide during the drag");
    const QPoint destination = bBounds.center() - QPoint(240, 160);
    check(SetWindowPos(areaHandle, nullptr, destination.x(), destination.y(), 0, 0,
                       SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE) != FALSE,
          "native recording area move must succeed");
    settleWindows();
    check(!toolbar->isVisible(), "toolbar must stay hidden until dragging ends");
    SendMessageW(areaHandle, WM_EXITSIZEMOVE, 0, 0);

    const auto verifyToolbar = [&](const char* phase) {
        const QRect client = nativeClient(*toolbar);
        const UINT dpi = GetDpiForWindow(reinterpret_cast<HWND>(toolbar->winId()));
        const qreal dpr = toolbar->devicePixelRatioF();
        const QRect localContent =
            toolbar->occupiedContentRect().translated(toolbar->contentPosition() - toolbar->pos());
        const QRect physicalContent(
            client.topLeft() +
                QPoint(qRound(localContent.x() * dpr), qRound(localContent.y() * dpr)),
            QSize(qRound(localContent.width() * dpr), qRound(localContent.height() * dpr)));
        qInfo() << phase << "native DPI" << dpi << "Qt DPR" << dpr << "client" << client << "widget"
                << toolbar->size() << "hint" << toolbar->windowSizeHint() << "content"
                << physicalContent << "area" << nativeClient(*area);
        check(toolbar->isVisible(), "toolbar must reappear after release");
        check(dpi == 96 && qAbs(dpr - 1.0) < 0.01 &&
                  qAbs(toolbar->windowHandle()->devicePixelRatio() - 1.0) < 0.01 &&
                  toolbar->screen() == displayB,
              "native and Qt toolbar surfaces must adopt display B's 96 DPI");
        check(client.size() ==
                  QSize(qRound(toolbar->width() * dpr), qRound(toolbar->height() * dpr)),
              "native client size must agree with the Qt window at target DPI");
        check(toolbar->size() == toolbar->windowSizeHint() &&
                  toolbar->rect().contains(toolbar->paletteHost()->geometry()) &&
                  toolbar->rect().contains(localContent) && client.contains(physicalContent),
              "all toolbar rows must fit inside the native window without clipping");
        for (QWidget* child : toolbar->palette()->findChildren<QWidget*>()) {
            if (!child->isVisibleTo(toolbar) || child->isWindow() || child->size().isEmpty()) {
                continue;
            }
            const QRect bounds(child->mapTo(toolbar, QPoint()), child->size());
            if (!toolbar->rect().contains(bounds)) {
                qCritical() << "Clipped child" << child->metaObject()->className()
                            << child->objectName() << bounds;
                check(false, "visible toolbar controls must fit inside the window");
            }
        }
        check(bBounds.contains(physicalContent), "all toolbar content must be inside display B");
        // This central selection leaves room below. Placement is right-aligned to the
        // area's outer frame with a four-DIP gap, clamped only at the display's left edge.
        const QRect frame = nativeClient(*area);
        const int expectedLeft = qMax(bBounds.left(), frame.right() + 1 - physicalContent.width());
        check(qAbs(physicalContent.left() - expectedLeft) <= 1 &&
                  qAbs(physicalContent.top() - (frame.bottom() + 1 + 4)) <= 1,
              "toolbar must anchor below the final area with a 4-DIP gap and correct alignment");
    };
    check(starts == 1 && finishes == 1, "drag must have one balanced native interaction lifecycle");
    check(area->screen() == displayB && bBounds.contains(area->physicalRegion()),
          "recording area must finish completely on B");
    verifyToolbar("Immediately after release");
    settleWindows();
    verifyToolbar("After queued DPI/layout events");
    check(renderedContentMatchesDesktop(*toolbar,
                                        startCapture ? "active-destination" : "destination"),
          "destination compositor output must display all toolbar content at the correct scale");
    check(controller.isRecording() == startCapture, "moving must preserve the recording state");
    return failures == 0 ? 0 : 1;
}
