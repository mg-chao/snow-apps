#include "recording_effect_test_source.h"
#include "snow_shot/presentation/globalcanvascontroller.h"
#include "snow_shot/presentation/screenshotfloatingtoolpalettewindow.h"
#include "snow_shot/presentation/screenshotgeometry.h"
#include "snow_shot/presentation/styles/thememanager.h"
#include "snow_shot/storage/settingsadapters.h"
#include "snow_draw_engine_qt/snow_canvas_widget.h"
#include "widgets/button.h"
#include "widgets/color_picker.h"
#include "theme/theme_manager.h"
#include <QApplication>
#include <QMouseEvent>
#include <QPainter>
#include <QScreen>
#include <QScopeGuard>
#include <QTranslator>
#include <algorithm>
#include <thread>
#ifdef Q_OS_WIN
#include <QDebug>
#include <QElapsedTimer>
#include <stdexcept>
#include <QThread>
#include <qt_windows.h>
#endif

void require(bool condition, const char* message);
void mouse(QWidget* widget, QEvent::Type type, QPointF point, Qt::MouseButton button,
           Qt::MouseButtons buttons);
void canvasLaserPointer(QApplication& app);
#ifdef Q_OS_WIN
bool nativeCanvasLaserPointer(QApplication& app);
#endif

namespace {
class LaserTranslator final : public QTranslator {
  public:
    bool isEmpty() const override {
        return false;
    }
    QString translate(const char* context, const char* source, const char*, int) const override {
        if (qstrcmp(context, "ScreenshotToolPalette") == 0 && qstrcmp(source, "Laser pointer") == 0)
            return QStringLiteral("Translated laser pointer");
        return {};
    }
};

QColor buttonBackground(adqt::widgets::AdButton* button) {
    QImage image(button->size(), QImage::Format_ARGB32_Premultiplied);
    image.fill(Qt::transparent);
    QPainter painter(&image);
    button->render(&painter);
    painter.end();
    return image.pixelColor(qMax(1, image.width() / 8), image.height() / 2);
}
} // namespace

void canvasLaserPointer(QApplication& app) {
    using namespace snow_shot;
    using adqt::widgets::AdButton;
    auto state = std::make_shared<RecordingEffectTestState>();
    state->autoPublish = false;
    int factories = 0;
    int errors = 0;
    bool failClickThrough = false;
    presentation::GlobalCanvasController controller(
        nullptr,
        {[&]() { return app.primaryScreen(); }, [&](QWidget*, bool) { return !failClickThrough; },
         [&]() {
             ++factories;
             return std::make_unique<RecordingEffectTestSource>(state);
         }});
    QObject::connect(&controller, &presentation::GlobalCanvasController::errorOccurred, &app,
                     [&](const QString& error) {
                         require(!error.isEmpty(), "laser failures report an error");
                         ++errors;
                     });
    const storage::RecordingSettings settings;
    const QColor originalColor = settings.mouseTrailColor();
    const int originalDuration = settings.mouseTrailDurationMs();
    const auto restoreSettings = qScopeGuard([&]() {
        settings.setMouseTrailColor(originalColor);
        settings.setMouseTrailDurationMs(originalDuration);
    });
    require(settings.setMouseTrailColor(QColor(Qt::blue)) && settings.setMouseTrailDurationMs(1400),
            "set unrelated recording appearance");
    controller.activate();
    app.processEvents();
    auto* canvas = controller.canvas();
    auto* window = controller.window();
    auto* palette = controller.toolbar()->palette();
    auto* laser = palette->findChild<AdButton*>(QStringLiteral("globalCanvasLaserPointerButton"));
    auto* clickThrough =
        palette->findChild<AdButton*>(QStringLiteral("globalCanvasClickThroughButton"));
    const auto requirePlacement = [&]() {
        require(laser && clickThrough && laser->isVisible() && clickThrough->isVisible(),
                "both independent canvas toggles are visible");
        require(laser->mapToGlobal(QPoint()).x() + laser->width() <=
                    clickThrough->mapToGlobal(QPoint()).x(),
                "laser is immediately before click-through in the action group");
    };
    requirePlacement();
    require(laser->isCheckable() && !laser->isChecked() && factories == 0,
            "new sessions do not create native observers until the laser is enabled");
    require(laser->toolTip() == QStringLiteral("Laser pointer") &&
                laser->accessibleName() == QStringLiteral("Laser pointer"),
            "laser has a tooltip and accessible name without an invented shortcut");
    LaserTranslator translator;
    app.installTranslator(&translator);
    app.processEvents();
    require(laser->toolTip() == QStringLiteral("Translated laser pointer") &&
                laser->accessibleName() == QStringLiteral("Translated laser pointer"),
            "laser text retranslates live");
    app.removeTranslator(&translator);
    app.processEvents();

    laser->click();
    require(laser->isChecked() && state->active && factories == 1 && state->starts == 1,
            "laser button starts one effects observer");
    require(state->trail == 0xff0000ff && state->trailDurationMs == 500 && state->click == 0 &&
                state->highlight == 0 && !state->recordMouseClicks && !state->showKeyboard &&
                state->webcam == nullptr,
            "canvas enables only the fixed red laser with a 500 ms fade");
    require(settings.mouseTrailColor() == QColor(Qt::blue) &&
                settings.mouseTrailDurationMs() == 1400,
            "canvas laser leaves recording settings unchanged");
#ifdef Q_OS_MACOS
    require(state->capture == window->geometry() &&
                state->canvasScale == canvas->devicePixelRatioF(),
            "macOS laser observes desktop points and renders physical pixels");
#else
    require(state->capture ==
                    ScreenshotGeometryMapper::physicalRectForScreen(*app.primaryScreen()) &&
                state->canvasScale == 1.0,
            "Windows laser observes physical display coordinates at fractional DPI");
#endif
    auto layout = storage::ScreenshotToolbarSettings().layout(
        storage::ScreenshotToolbarLayoutKind::DrawingTools);
    std::reverse(layout.positions.begin(), layout.positions.end());
    palette->setToolbarLayout(layout);
    app.processEvents();
    requirePlacement();
    require(laser->isChecked() && state->starts == 1, "layout rebuilding preserves laser state");

    auto& theme = presentation::styles::ThemeManager::instance();
    const auto originalTheme = theme.themeMode();
    const auto restoreTheme = qScopeGuard([&]() {
        theme.setThemeMode(originalTheme);
        adqt::theme::ThemeManager::instance().applyTo(app);
    });
    for (const auto mode :
         {presentation::styles::ThemeMode::Light, presentation::styles::ThemeMode::Dark}) {
        theme.setThemeMode(mode);
        adqt::theme::ThemeManager::instance().applyTo(app);
        for (bool enabled : {true, false}) {
            if (laser->isChecked() != enabled)
                laser->click();
            if (controller.clickThrough() != enabled)
                clickThrough->click();
            app.processEvents();
            require(laser->buttonStyle() == clickThrough->buttonStyle() &&
                        laser->accentRole() == clickThrough->accentRole(),
                    "laser and click-through use the same toggle style in both themes");
            for (bool hovered : {false, true}) {
                laser->setAttribute(Qt::WA_UnderMouse, hovered);
                clickThrough->setAttribute(Qt::WA_UnderMouse, hovered);
                for (bool pressed : {false, true}) {
                    laser->setDown(pressed);
                    clickThrough->setDown(pressed);
                    require(buttonBackground(laser) == buttonBackground(clickThrough),
                            "laser toggle matches toolbar rest, hover and pressed backgrounds");
                }
            }
            laser->setDown(false);
            clickThrough->setDown(false);
            laser->setAttribute(Qt::WA_UnderMouse, false);
            clickThrough->setAttribute(Qt::WA_UnderMouse, false);
        }
    }
    laser->click();
    const QImage empty = canvas->grab().toImage();
    const auto pixel = [&]() {
        const QImage image = canvas->grab().toImage();
        return image.pixelColor(qRound(30.0 * image.width() / state->output.width()),
                                qRound(30.0 * image.height() / state->output.height()));
    };
    state->publish();
    app.processEvents();
    require(pixel().red() > 240 && pixel().alpha() >= 128,
            "native effect tiles appear above the transparent canvas at the correct DPI");
    require(!canvas->canvasHistoryState().canUndo, "laser frames never enter annotation history");
    canvas->setViewportCamera(320, 240, 2.0);
    require(pixel().red() > 240 && pixel().alpha() >= 128,
            "canvas zoom and pan do not move the laser");
    canvas->setViewportCamera(canvas->width() / 2.0, canvas->height() / 2.0, 1.0);
    adqt::widgets::AdColorPicker picker;
    palette->canvasColorSamplingRequested(&picker);
    const QPointF tileCenter(30.0 * canvas->width() / state->output.width(),
                             30.0 * canvas->height() / state->output.height());
    mouse(canvas, QEvent::MouseButtonPress, tileCenter, Qt::LeftButton, Qt::LeftButton);
    mouse(canvas, QEvent::MouseButtonRelease, tileCenter, Qt::LeftButton, Qt::NoButton);
    require(picker.value().solidColor.alpha() == 0,
            "annotation sampling excludes the laser overlay");
    state->publish(false);
    app.processEvents();
    require(canvas->grab().toImage() == empty, "an empty effects frame clears every expired tile");

    palette->brushRequested();
    mouse(canvas, QEvent::MouseButtonPress, {100, 100}, Qt::LeftButton, Qt::LeftButton);
    mouse(canvas, QEvent::MouseMove, {160, 130}, Qt::NoButton, Qt::LeftButton);
    mouse(canvas, QEvent::MouseButtonRelease, {180, 140}, Qt::LeftButton, Qt::NoButton);
    require(laser->isChecked() && state->active && canvas->canvasHistoryState().canUndo,
            "drawing works while the laser is enabled");
    const int startsBeforeClickThrough = state->starts;
    clickThrough->click();
    require(controller.clickThrough() && laser->isChecked() && state->active &&
                state->starts == startsBeforeClickThrough,
            "click-through retains the existing observer");
    state->publish();
    app.processEvents();
    require(pixel().red() > 240, "laser frames keep rendering during click-through");
    failClickThrough = true;
    clickThrough->click();
    require(controller.clickThrough() && state->active && laser->isChecked() && errors == 1,
            "failed click-through transitions do not stop the laser");
    failClickThrough = false;
    clickThrough->click();
    require(canvas->undo() && !canvas->canvasHistoryState().canUndo,
            "undo removes only the drawn stroke after laser updates");

    const auto staleFrame = state->frame;
    const auto previousGeneration = state->generation;
    window->resize(window->width() - 20, window->height() - 20);
    app.processEvents();
    require(state->generation > previousGeneration && state->starts == startsBeforeClickThrough,
            "geometry changes reconfigure without duplicating observers");
    state->frame = staleFrame;
    state->notify();
    app.processEvents();
    require(pixel().alpha() <= 2, "frames from the previous geometry are rejected");
    state->publish();
    const int acquisitions = state->acquisitions;
    std::thread worker([notify = state->notify]() {
        for (int i = 0; i < 20; ++i)
            notify();
    });
    worker.join();
    app.processEvents();
    require(state->acquisitions == acquisitions + 1,
            "worker notifications coalesce on the GUI thread");
    require(pixel().red() > 240, "the reconfigured generation renders normally");
    const auto latestFrame = state->frame;
    state->publish(false);
    state->frame->revision = latestFrame->revision - 1;
    app.processEvents();
    require(pixel().red() > 240, "older frame revisions cannot replace a newer frame");

    window->hide();
    require(!state->active && laser->isChecked(),
            "hiding suspends observations but retains the toggle");
    window->show();
    app.processEvents();
    require(state->active && laser->isChecked() && pixel().alpha() <= 2,
            "showing resumes with a fresh, empty generation");
    state->publish();
    laser->click();
    app.processEvents();
    require(!state->active && !laser->isChecked() && pixel().alpha() <= 2,
            "disabling clears queued frames and stops observers");

    state->fail = true;
    laser->click();
    require(!laser->isChecked() && !state->active && errors == 2 && controller.active(),
            "start failure rolls back the toggle while retaining the canvas");
    state->fail = false;
    laser->click();
    state->fail = true;
    window->resize(window->width() - 10, window->height() - 10);
    require(!laser->isChecked() && !state->active && errors == 3,
            "reconfiguration failure clears and disables the laser");
    state->fail = false;
    laser->click();
    state->publish();
    state->frame->error = QStringLiteral("native observer failed");
    app.processEvents();
    require(!laser->isChecked() && !state->active && errors == 4 && pixel().alpha() <= 2,
            "runtime failure clears the overlay and reports through the controller");
    laser->click();
    state->publish();
    window->close();
    require(!state->active, "closing stops observers synchronously");
    app.processEvents();
    require(!controller.active(), "queued notifications cannot revive a closed canvas");
    controller.activate();
    app.processEvents();
    require(!controller.toolbar()
                    ->palette()
                    ->findChild<AdButton*>(QStringLiteral("globalCanvasLaserPointerButton"))
                    ->isChecked() &&
                !state->active && factories == 1,
            "reopening starts with the laser disabled and no new observer");
    controller.shutdown();
    app.processEvents();
}

#ifdef Q_OS_WIN
bool nativeCanvasLaserPointer(QApplication& app) {
    try {
        using namespace snow_shot;
        const auto check = [](bool ok, const char* message) {
            if (!ok)
                throw std::runtime_error(message);
        };
        POINT originalCursor{};
        check(GetPhysicalCursorPos(&originalCursor), "read cursor before native laser test");
        const auto restoreCursor =
            qScopeGuard([&]() { SetPhysicalCursorPos(originalCursor.x, originalCursor.y); });
        const auto settle = [&](int milliseconds) {
            QElapsedTimer timer;
            timer.start();
            while (timer.elapsed() < milliseconds) {
                app.processEvents();
                QThread::msleep(5);
            }
        };
        const auto baselineHandles = snow_recording_effects_live_handle_count();
        presentation::GlobalCanvasController controller(
            nullptr, {[&]() { return app.primaryScreen(); }, {}});
        QString nativeError;
        QObject::connect(&controller, &presentation::GlobalCanvasController::errorOccurred, &app,
                         [&](const QString& error) { nativeError = error; });
        controller.activate();
        settle(100);
        auto* canvas = controller.canvas();
        auto* laser = controller.toolbar()->palette()->findChild<adqt::widgets::AdButton*>(
            QStringLiteral("globalCanvasLaserPointerButton"));
        laser->click();
        check(laser->isChecked() &&
                  snow_recording_effects_live_handle_count() == baselineHandles + 1,
              "native canvas owns exactly one effects handle");
        const QRect screen = ScreenshotGeometryMapper::physicalRectForScreen(*app.primaryScreen());
        QRect desktop;
        for (QScreen* display : app.screens())
            desktop |= ScreenshotGeometryMapper::physicalRectForScreen(*display);
        const auto movePointer = [&](const QPoint& point) {
            // Cursor warps need not reach WH_MOUSE_LL. Inject movement in the virtual
            // desktop's physical coordinate space, including negative origins.
            INPUT input{};
            input.type = INPUT_MOUSE;
            input.mi.dx = static_cast<LONG>(
                (static_cast<qint64>(point.x() - desktop.x()) * 65536 + 32768) / desktop.width());
            input.mi.dy = static_cast<LONG>(
                (static_cast<qint64>(point.y() - desktop.y()) * 65536 + 32768) / desktop.height());
            input.mi.dwFlags = MOUSEEVENTF_MOVE | MOUSEEVENTF_ABSOLUTE | MOUSEEVENTF_VIRTUALDESK |
                               MOUSEEVENTF_MOVE_NOCOALESCE;
            check(SendInput(1, &input, sizeof(INPUT)) == 1, "inject native pointer movement");
        };
        const auto redBounds = [&](const QRect& region = QRect()) {
            const QImage image = canvas->grab().toImage().convertToFormat(QImage::Format_ARGB32);
            const QRect scan = region.isEmpty() ? image.rect() : region.intersected(image.rect());
            QRect bounds;
            for (int y = scan.top(); y <= scan.bottom(); ++y) {
                const auto* row = reinterpret_cast<const QRgb*>(image.constScanLine(y));
                for (int x = scan.left(); x <= scan.right(); ++x) {
                    const QRgb pixel = row[x];
                    if (qRed(pixel) > 220 && qGreen(pixel) < 20 && qBlue(pixel) < 20 &&
                        qAlpha(pixel) > 30)
                        bounds |= QRect(x, y, 1, 1);
                }
            }
            return bounds;
        };
        for (bool clickThrough : {false, true}) {
            if (clickThrough)
                controller.activate();
            check(controller.clickThrough() == clickThrough && laser->isChecked(),
                  "native click-through preserves the laser");
            const auto handle = reinterpret_cast<HWND>(controller.window()->winId());
            check(((GetWindowLongPtrW(handle, GWL_EXSTYLE) & WS_EX_TRANSPARENT) != 0) ==
                      clickThrough,
                  "laser does not change native input transparency");
            settle(600);
            const QPoint first(screen.width() / 4, screen.height() / 3);
            for (int step = 0; step < 20; ++step) {
                movePointer(screen.topLeft() + first + QPoint(step * 6, 0));
                settle(25);
            }
            // The global observer can include real user input, so inspect pixels along
            // the injected path rather than requiring all visible trails to lie inside it.
            const QRect trail = redBounds(QRect(first - QPoint(8, 8), QSize(135, 17)));
            if (!nativeError.isEmpty())
                qWarning() << nativeError;
            check(nativeError.isEmpty(), "native laser must not report errors");
            check(!trail.isEmpty(),
                  "native laser follows the physical pointer path in drawing and click-through "
                  "modes");
            settle(650);
            check(redBounds().isEmpty(), "native trail fades after the pointer stops moving");
        }
        controller.shutdown();
        app.processEvents();
        check(snow_recording_effects_live_handle_count() == baselineHandles,
              "native canvas shutdown releases every effects observer");
        return true;
    } catch (const std::exception& error) {
        qWarning() << error.what();
        return false;
    }
}
#endif
