#include "snow_shot/presentation/screenshotpinnedwindow.h"
#include "snow_shot/presentation/screenshotcanvascolorsampler.h"
#include "snow_shot/presentation/screenshotocrpresentation.h"
#include "snow_shot/presentation/screenshotocrrecognitionservice.h"
#include "snow_shot/presentation/screenshotpinnededitcontroller.h"
#include "snow_shot/presentation/screenshotfloatingtoolpalettewindow.h"
#include "snow_shot/presentation/screenshotgeometry.h"
#include "snow_shot/presentation/screenshotrecognitionsessioncontroller.h"
#include "snow_shot/presentation/screenshottoolpalette.h"
#include "snow_shot/storage/settingsadapters.h"

#include "snow_draw_engine_qt/snow_canvas_runtime.h"
#include "snow_draw_engine_qt/snow_canvas_widget.h"
#include "theme/theme_manager.h"
#include "widgets/button.h"
#include "widgets/color_picker.h"
#include "widgets/context_menu.h"

#include <QAbstractButton>
#include <QActionGroup>
#include <QApplication>
#include <QClipboard>
#include <QCoreApplication>
#include <QCursor>
#include <QElapsedTimer>
#include <QEnterEvent>
#include <QEvent>
#include <QFrame>
#include <QGraphicsTextItem>
#include <QGraphicsView>
#include <QImage>
#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
#include <QMouseEvent>
#include <QMimeData>
#include <QPainter>
#include <QPointer>
#include <QPushButton>
#include <QRegion>
#include <QScreen>
#include <QThread>
#include <QTimer>
#include <QTextDocument>
#include <QTextEdit>
#include <QVariantAnimation>
#include <QWheelEvent>
#include <QWindow>

#include <algorithm>
#include <functional>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <utility>
#include <vector>

#if defined(Q_OS_WIN) || defined(_WIN32)
#include <qt_windows.h>
#endif

namespace {
#if defined(Q_OS_WIN) || defined(_WIN32)
HWND toNativeHwnd(WId windowId) {
    // Qt transports the native HWND through its integer-valued WId type.
    return reinterpret_cast<HWND>(windowId); // NOLINT(performance-no-int-to-ptr)
}

template <typename T> T* pointerFromLParam(LPARAM value) {
    // Windows transports callback context pointers through LPARAM.
    return reinterpret_cast<T*>(value); // NOLINT(performance-no-int-to-ptr)
}

int nativeChildWindowCount(HWND parent) {
    int count = 0;
    EnumChildWindows(
        parent,
        [](HWND, LPARAM data) -> BOOL {
            ++*pointerFromLParam<int>(data);
            return TRUE;
        },
        reinterpret_cast<LPARAM>(&count));
    return count;
}

RECT nativeRectForQRect(const QRect& rect) {
    return RECT{
        rect.left(),
        rect.top(),
        rect.left() + rect.width(),
        rect.top() + rect.height(),
    };
}

QRect qRectForNativeRect(const RECT& rect) {
    return QRect(rect.left, rect.top, std::max(1L, rect.right - rect.left),
                 std::max(1L, rect.bottom - rect.top));
}

#endif

class FakeOcrRecognition final : public ScreenshotOcrRecognitionPort {
  public:
    struct Pending {
        RequestToken token = 0;
        ScreenshotOcrRequest request;
        QPointer<QObject> receiver;
        Completion completion;
    };

    RequestToken recognize(ScreenshotOcrRequest request, QObject* receiver,
                           Completion completion) override {
        pending = Pending{
            ++nextToken,
            std::move(request),
            receiver,
            std::move(completion),
        };
        return pending.token;
    }

    void cancel(RequestToken token) override {
        cancelledTokens.push_back(token);
        if (pending.token == token) {
            pending = {};
        }
    }

    bool reprioritize(RequestToken token, ScreenshotOcrRequestPriority priority) override {
        if (pending.token != token) {
            return false;
        }
        pending.request.priority = priority;
        return true;
    }

    void complete(ScreenshotOcrRecognitionResult result) {
        Pending request = std::move(pending);
        pending = {};
        if (request.receiver != nullptr && request.completion) {
            request.completion(std::move(result));
        }
    }

    RequestToken nextToken = 0;
    Pending pending;
    QVector<RequestToken> cancelledTokens;
};

class FakeQrRecognition final : public ScreenshotQrRecognitionPort {
  public:
    RequestToken recognize(QImage image, QObject* receiver, Completion completion) override {
        Q_UNUSED(image);
        Q_UNUSED(receiver);
        Q_UNUSED(completion);
        return ++nextToken;
    }

    void cancel(RequestToken token) override {
        cancelledTokens.push_back(token);
    }

    RequestToken nextToken = 0;
    QVector<RequestToken> cancelledTokens;
};

void require(bool condition, const char* message) {
    if (!condition) {
        throw std::runtime_error(message);
    }
}

void recognitionResultsSurviveTargetImageReallocationAndSeedAllModes() {
    FakeOcrRecognition recognition;
    FakeQrRecognition qrRecognition;
    SnowShotApiClient tableRecognition(QString{});
    ScreenshotRecognitionSessionController session(&recognition, &qrRecognition,
                                                    &tableRecognition, {});

    QImage image(QSize(80, 50), QImage::Format_ARGB32_Premultiplied);
    image.fill(QColor(18, 36, 54));
    const QString key = QStringLiteral("capture-1:10,12,80,50");
    const QRectF canvasRect(QPointF(10.0, 12.0), QSizeF(image.size()));
    session.setTarget(ScreenshotRecognitionTarget{key, image, canvasRect});
    session.activate(ScreenshotRecognitionSessionController::Mode::Text);
    require(recognition.pending.token != 0, "the initial OCR request was not started");

    auto presentation = std::make_shared<ScreenshotOcrPresentation>();
    presentation->selection = QRect(10, 12, 80, 50);
    ScreenshotOcrLine line;
    line.text = QStringLiteral("Cached OCR");
    line.quad = QPolygonF({QPointF(20.0, 24.0), QPointF(70.0, 24.0), QPointF(70.0, 38.0),
                           QPointF(20.0, 38.0)});
    presentation->lines.push_back(line);
    recognition.complete({presentation, {}});
    require(session.hasTextResult(), "a completed OCR result was not cached");
    const ScreenshotRecognitionResults cached = session.cachedRecognitionResults();
    require(cached.isValidFor(key) && cached.text.has_value() && cached.text->presentation != nullptr,
            "the cached OCR result did not expose its stable key and raw presentation");

    session.deactivate();
    session.setTarget(ScreenshotRecognitionTarget{key, image.copy(), canvasRect});
    session.activate(ScreenshotRecognitionSessionController::Mode::Text);
    require(recognition.nextToken == 1 && recognition.pending.token == 0 &&
                session.hasTextResult(),
            "reallocating an unchanged selection image should reuse its OCR result");

    ScreenshotRecognitionResults seeded;
    seeded.key = key;
    seeded.text = cached.text;
    seeded.table = SnowShotTableResult{
        QStringLiteral("<table><tr><td>Seeded table</td></tr></table>"), {}, {}, 200};
    seeded.qr = ScreenshotQrRecognitionResult{{QStringLiteral("https://example.test/seeded")}, {}};
    session.seedRecognitionResults(seeded);

    session.activate(ScreenshotRecognitionSessionController::Mode::Table);
    require(session.tableModeActive() && !session.busy(ScreenshotRecognitionSessionController::Mode::Table),
            "a seeded table result should activate without a table request");
    session.activate(ScreenshotRecognitionSessionController::Mode::Qr);
    require(session.qrModeActive() && !session.busy(ScreenshotRecognitionSessionController::Mode::Qr) &&
                qrRecognition.nextToken == 0,
            "a seeded QR result should activate without a QR request");
    session.invalidate();
    require(session.cachedRecognitionResults().isEmpty(),
            "full recognition invalidation should clear retained results");
}

QGraphicsTextItem* formattedTextItem(QGraphicsView* layer) {
    if (layer == nullptr || layer->scene() == nullptr) {
        return nullptr;
    }
    for (QGraphicsItem* item : layer->scene()->items()) {
        auto* textItem = dynamic_cast<QGraphicsTextItem*>(item);
        if (textItem != nullptr &&
            textItem->objectName() == QStringLiteral("screenshotClipboardTextItem")) {
            return textItem;
        }
    }
    return nullptr;
}

void waitForUi(int milliseconds) {
    QElapsedTimer elapsed;
    elapsed.start();
    while (elapsed.elapsed() < milliseconds) {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
        QThread::msleep(1);
    }
}

void sendShortcut(QWidget& receiver, Qt::Key key, Qt::KeyboardModifiers modifiers = Qt::NoModifier,
                  bool autoRepeat = false) {
    QKeyEvent event(QEvent::KeyPress, key, modifiers, QString(), autoRepeat);
    QCoreApplication::sendEvent(&receiver, &event);
}

QPoint systemCursorPosition() {
#if defined(Q_OS_WIN) || defined(_WIN32)
    POINT position{};
    require(GetPhysicalCursorPos(&position) != FALSE,
            "failed to read the physical system cursor position");
    return QPoint(position.x, position.y);
#else
    return QCursor::pos();
#endif
}

void setSystemCursorPosition(const QPoint& position) {
#if defined(Q_OS_WIN) || defined(_WIN32)
    require(SetPhysicalCursorPos(position.x(), position.y()) != FALSE,
            "failed to set the physical system cursor position");
#else
    QCursor::setPos(position);
#endif
}

QRect physicalPinGeometry(QScreen& screen, const QPoint& logicalOffset, const QSize& physicalSize) {
    const qreal dpr = screen.devicePixelRatio();
    return QRect(ScreenshotGeometryMapper::physicalRectForScreen(screen).topLeft() +
                     QPoint(qRound(logicalOffset.x() * dpr), qRound(logicalOffset.y() * dpr)),
                 physicalSize);
}

class CursorPositionRestorer final {
  public:
    CursorPositionRestorer() : m_position(systemCursorPosition()) {}

    ~CursorPositionRestorer() {
        setSystemCursorPosition(m_position);
    }

  private:
    QPoint m_position;
};

void clickTextEditor(QWidget& editor) {
    const QPointF center(editor.rect().center());
    QMouseEvent press(QEvent::MouseButtonPress, center, editor.mapToGlobal(center.toPoint()),
                      Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
    QCoreApplication::sendEvent(&editor, &press);
    QCoreApplication::processEvents(QEventLoop::AllEvents, 20);

    if (!editor.hasFocus() && editor.window() != nullptr) {
#if defined(Q_OS_WIN) || defined(_WIN32)
        const HWND editorHwnd = toNativeHwnd(editor.window()->winId());
        const HWND foregroundHwnd = GetForegroundWindow();
        const DWORD currentThread = GetCurrentThreadId();
        const DWORD foregroundThread =
            foregroundHwnd != nullptr ? GetWindowThreadProcessId(foregroundHwnd, nullptr) : 0;
        const bool attached = foregroundThread != 0 && foregroundThread != currentThread &&
                              AttachThreadInput(currentThread, foregroundThread, TRUE) != FALSE;
        if (editorHwnd != nullptr) {
            SetActiveWindow(editorHwnd);
            SetForegroundWindow(editorHwnd);
            SetFocus(editorHwnd);
        }
#else
        editor.window()->raise();
        editor.window()->activateWindow();
#endif
        editor.setFocus(Qt::MouseFocusReason);
#if defined(Q_OS_WIN) || defined(_WIN32)
        if (attached) {
            AttachThreadInput(currentThread, foregroundThread, FALSE);
        }
#endif
    }

    QMouseEvent release(QEvent::MouseButtonRelease, center, editor.mapToGlobal(center.toPoint()),
                        Qt::LeftButton, Qt::NoButton, Qt::NoModifier);
    QCoreApplication::sendEvent(&editor, &release);
    waitForUi(100);
}

void typeText(const QString& text, QLineEdit& editor);
QPushButton* buttonNamed(QWidget& window, const QString& accessibleName);
bool processUntilDeleted(QPointer<ScreenshotPinnedWindow>& window, int timeoutMs);

adqt::widgets::AdButton* toolbarButtonNamed(ScreenshotToolPalette& toolbar,
                                             const QString& tooltip) {
    for (adqt::widgets::AdButton* button : toolbar.findChildren<adqt::widgets::AdButton*>()) {
        if (button != nullptr && button->toolTip().startsWith(tooltip)) {
            return button;
        }
    }
    return nullptr;
}

QImage waitForClipboardImage(const std::function<bool(const QImage&)>& predicate,
                             int timeoutMs = 5000) {
    QElapsedTimer elapsed;
    elapsed.start();
    QImage image;
    while (elapsed.elapsed() < timeoutMs) {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
        image = QApplication::clipboard()->image();
        if (!image.isNull() && predicate(image)) {
            return image;
        }
        QThread::msleep(1);
    }
    return image;
}

void setPinnedWindowHovered(ScreenshotPinnedWindow& window, bool hovered) {
    if (hovered) {
        const QPointF center(window.rect().center());
        QEnterEvent enter(center, center, QPointF(window.mapToGlobal(center.toPoint())));
        QCoreApplication::sendEvent(&window, &enter);
        return;
    }
    QEvent leave(QEvent::Leave);
    QCoreApplication::sendEvent(&window, &leave);
}

void pinnedWatermarkEditorAcceptsKeyboardInput(SnowCanvasRuntime&) {
    QScreen* screen = QGuiApplication::primaryScreen();
    require(screen != nullptr, "a primary screen is required");

    auto* pinnedWindow = new ScreenshotPinnedWindow(ScreenshotPinnedWindow::RuntimeMode::NoDocument);
    QPointer<ScreenshotPinnedWindow> guardedWindow(pinnedWindow);
    QImage background(320, 180, QImage::Format_ARGB32_Premultiplied);
    for (int y = 0; y < background.height(); ++y) {
        for (int x = 0; x < background.width(); ++x) {
            background.setPixelColor(
                x, y,
                QColor((x * 17 + y * 3) % 256, (x * 5 + y * 23) % 256, (x * 29 + y * 11) % 256));
        }
    }

    ScreenshotPinnedWindow::Config config;
    config.nativeGeometry = physicalPinGeometry(*screen, QPoint(40, 40), background.size());
    config.canvasSourceRect = QRectF(QPointF(0.0, 0.0), QSizeF(background.size()));
    config.backgroundImage = background;
    config.screen = screen;
    config.enableEditing = true;
    require(pinnedWindow->present(config), "pinned window presentation failed");
    QCoreApplication::processEvents(QEventLoop::AllEvents, 50);
#if defined(Q_OS_WIN) || defined(_WIN32)
    const HWND pinnedHwnd = toNativeHwnd(pinnedWindow->winId());
    require(nativeChildWindowCount(pinnedHwnd) == 0,
            "constructing the hidden edit toolbar must not create native pinned children");
#endif
    QPushButton* editButton = buttonNamed(*pinnedWindow, QStringLiteral("Enable drawing mode"));
    require(editButton != nullptr, "edit button was not found");
    editButton->click();
    QCoreApplication::processEvents(QEventLoop::AllEvents, 50);

    auto* controller = pinnedWindow->findChild<ScreenshotPinnedEditController*>();
    require(controller != nullptr, "pinned edit controller was not found");
    ScreenshotFloatingToolPaletteWindow* toolbarWindow = controller->toolbarWindow();
    ScreenshotToolPalette* toolbar = toolbarWindow != nullptr ? toolbarWindow->palette() : nullptr;
    SnowCanvasWidget* canvas = pinnedWindow->findChild<SnowCanvasWidget*>();
    require(toolbarWindow != nullptr && toolbarWindow->isVisible() && toolbar != nullptr &&
                canvas != nullptr,
            "pinned edit toolbar was not ready");
    require(toolbarWindow->parentWidget() == nullptr && toolbarWindow->isWindow() &&
                toolbarWindow->windowHandle() != nullptr &&
                pinnedWindow->windowHandle() != nullptr &&
                toolbarWindow->windowHandle()->transientParent() == pinnedWindow->windowHandle(),
            "the pinned edit toolbar should remain a transient top-level window");
#if defined(Q_OS_WIN) || defined(_WIN32)
    const HWND toolbarHwnd = toNativeHwnd(toolbarWindow->winId());
    require(nativeChildWindowCount(pinnedHwnd) == 0,
            "showing the edit toolbar must not create native pinned children");
    require(toolbarHwnd != nullptr && toolbarHwnd != pinnedHwnd &&
                GetWindow(toolbarHwnd, GW_OWNER) == pinnedHwnd,
            "the edit toolbar should have the pinned HWND as its native owner");
#endif
    require(canvas->setCanvasTool(SnowCanvasTool::Watermark),
            "pinned canvas should activate the watermark tool");
    QCoreApplication::processEvents(QEventLoop::AllEvents, 50);

    auto* watermarkText =
        toolbar->findChild<QLineEdit*>(QStringLiteral("screenshotWatermarkTextEdit"));
    require(watermarkText != nullptr && watermarkText->isVisible(),
            "pinned watermark text editor should be visible");
    clickTextEditor(*watermarkText);
    require(watermarkText->hasFocus(),
            "clicking the pinned watermark editor should focus it for text input");
    watermarkText->clear();
    typeText(QStringLiteral("PINNED INPUT"), *watermarkText);
    require(watermarkText->text() == QStringLiteral("PINNED INPUT") &&
                canvas->canvasWatermarkConfig().text == QStringLiteral("PINNED INPUT"),
            "keyboard input should update the pinned watermark text");

    QPushButton* confirmButton = buttonNamed(*toolbar, QStringLiteral("Confirm edit"));
    require(confirmButton != nullptr, "pinned confirm button was not found");
    confirmButton->click();
    QCoreApplication::processEvents(QEventLoop::AllEvents, 50);
    QPushButton* closeButton = buttonNamed(*pinnedWindow, QStringLiteral("Close"));
    require(closeButton != nullptr, "close button was not found");
    closeButton->click();
    require(processUntilDeleted(guardedWindow, 2000),
            "pinned window was not deleted after the watermark input test");
}

void pinnedLargeImageRemainsOpenWhenEnteringDrawingMode(SnowCanvasRuntime&) {
    QScreen* screen = QGuiApplication::primaryScreen();
    require(screen != nullptr, "a primary screen is required");

    FakeQrRecognition qrRecognition;
    SnowShotApiClient tableRecognition(QString{});
    auto* pinnedWindow = new ScreenshotPinnedWindow(ScreenshotPinnedWindow::RuntimeMode::NoDocument);
    QPointer<ScreenshotPinnedWindow> guardedWindow(pinnedWindow);
    QImage background(400, 40000, QImage::Format_ARGB32_Premultiplied);
    background.fill(QColor(42, 84, 126));

    ScreenshotPinnedWindow::Config config;
    const QSize displayedSize(background.width() / 10, background.height() / 10);
    config.nativeGeometry = physicalPinGeometry(*screen, QPoint(40, 40), displayedSize);
    config.canvasSourceRect = QRectF(QPointF(0.0, 0.0), QSizeF(background.size()));
    config.backgroundImage = background;
    config.fullResolutionScaleBasis = background.size();
    config.initialScalePercent = 10.0;
    config.screen = screen;
    config.enableEditing = true;
    config.qrRecognition = &qrRecognition;
    config.tableRecognition = &tableRecognition;
    require(pinnedWindow->present(config), "large pinned window presentation failed");
    QCoreApplication::processEvents(QEventLoop::AllEvents, 50);

    QPushButton* editButton = buttonNamed(*pinnedWindow, QStringLiteral("Enable drawing mode"));
    require(editButton != nullptr, "large pinned window edit button was not found");
    editButton->click();
    waitForUi(500);

    require(!guardedWindow.isNull() && guardedWindow->isVisible(),
            "large pinned window closed after entering drawing mode");
    auto* controller = guardedWindow->findChild<ScreenshotPinnedEditController*>();
    require(controller != nullptr && controller->editMode(),
            "large pinned window did not enter drawing mode");
    ScreenshotToolPalette* toolbar =
        controller->toolbarWindow() != nullptr ? controller->toolbarWindow()->palette() : nullptr;
    auto* tableQrButton =
        toolbar != nullptr ? toolbar->findChild<QWidget*>(QStringLiteral("screenshotTableQrButton"))
                           : nullptr;
    require(tableQrButton != nullptr, "large pinned table and QR trigger was not found");
    require(!tableQrButton->isEnabled(),
            "large pinned images should disable the table and QR trigger");

    auto* recognitionSession = guardedWindow->findChild<ScreenshotRecognitionSessionController*>();
    require(recognitionSession != nullptr, "large pinned recognition session was not found");
    toolbar->tableRequested();
    toolbar->qrRequested();
    require(!recognitionSession->active() && qrRecognition.nextToken == 0,
            "large pinned images should reject table and QR activation attempts");
    recognitionSession->activate(ScreenshotRecognitionSessionController::Mode::Qr);
    require(qrRecognition.nextToken == 0 &&
                !recognitionSession->busy(ScreenshotRecognitionSessionController::Mode::Qr),
            "the recognition session should not submit oversized QR images");
    recognitionSession->deactivate();
    require(guardedWindow->currentNativeGeometry() == config.nativeGeometry,
            "large pinned window geometry changed after entering drawing mode");

    guardedWindow->close();
    require(processUntilDeleted(guardedWindow, 2000),
            "large pinned window was not deleted after the drawing-mode test");
}

void pinnedCopyIncludesSourceCanvasDrawing() {
    QScreen* screen = QGuiApplication::primaryScreen();
    require(screen != nullptr, "a primary screen is required");

    const QColor backgroundColor(242, 244, 247);
    QImage background(200, 120, QImage::Format_ARGB32_Premultiplied);
    background.fill(backgroundColor);
    const QRectF sourceRect(640.0, 360.0, 200.0, 120.0);
    const auto containsRedDrawing = [](const QImage& image) {
        for (int y = 0; y < image.height(); ++y) {
            for (int x = 0; x < image.width(); ++x) {
                const QColor pixel = image.pixelColor(x, y);
                if (pixel.alpha() > 0 && pixel.red() > 180 && pixel.red() > pixel.green() * 2 &&
                    pixel.red() > pixel.blue() * 2) {
                    return true;
                }
            }
        }
        return false;
    };

    SnowCanvasRuntime sourceRuntime;
    require(sourceRuntime.isValid(), "pinned copy source runtime creation failed");
    SnowCanvasWidget sourceCanvas(sourceRuntime);
    sourceCanvas.resize(background.size());
    sourceCanvas.show();
    QCoreApplication::processEvents(QEventLoop::AllEvents, 50);
    require(sourceCanvas.setViewportCamera(sourceRect.center().x(), sourceRect.center().y(), 1.0),
            "pinned copy source canvas camera setup failed");
    QCoreApplication::processEvents(QEventLoop::AllEvents, 50);
    require(sourceCanvas.viewRectForCanvasRect(sourceRect) == sourceCanvas.rect(),
            "pinned copy source canvas should display the non-zero canvas rect");
    require(sourceCanvas.setCanvasTool(SnowCanvasTool::Shape),
            "pinned copy source canvas should activate the shape tool");
    SnowCanvasShapeStyle shapeStyle;
    shapeStyle.stroke = QColor(240, 24, 24);
    shapeStyle.strokeWidth = 4.0;
    require(sourceCanvas.setCanvasShapeStylePatch(shapeStyle,
                                                  SnowCanvasShapeStylePropertyStrokeColor |
                                                      SnowCanvasShapeStylePropertyStrokeWidth,
                                                  SnowCanvasShapeKind::Rectangle),
            "pinned copy source canvas should configure a detectable rectangle stroke");
    const auto sendSourcePointerEvent = [&sourceCanvas](QEvent::Type type, const QPointF& position,
                                                        Qt::MouseButton button,
                                                        Qt::MouseButtons buttons) {
        QMouseEvent event(type, position, sourceCanvas.mapToGlobal(position.toPoint()), button,
                          buttons, Qt::NoModifier);
        QCoreApplication::sendEvent(&sourceCanvas, &event);
    };
    sendSourcePointerEvent(QEvent::MouseButtonPress, QPointF(35.0, 30.0), Qt::LeftButton,
                           Qt::LeftButton);
    sendSourcePointerEvent(QEvent::MouseMove, QPointF(165.0, 90.0), Qt::NoButton, Qt::LeftButton);
    sendSourcePointerEvent(QEvent::MouseButtonRelease, QPointF(165.0, 90.0), Qt::LeftButton,
                           Qt::NoButton);
    require(sourceCanvas.canvasHistoryState().canUndo,
            "pinned copy source canvas should commit a rectangle before pinning");
    QCoreApplication::processEvents(QEventLoop::AllEvents, 50);
    require(containsRedDrawing(sourceCanvas.grab().toImage()),
            "source canvas display should contain the rectangle before pinning");
    require(containsRedDrawing(sourceRuntime.renderToImage(
                sourceRect, background.size(), {CanvasExportSource{background, sourceRect}})),
            "source runtime export should contain the rectangle before pinning");

    const QByteArray sourceSessionBeforePin = sourceRuntime.serializeDocumentSession();
    const QImage bakedImage = sourceRuntime.renderToImage(
        sourceRect, background.size(), {CanvasExportSource{background, sourceRect}});
    require(!bakedImage.isNull() && containsRedDrawing(bakedImage),
            "source runtime export should contain the baked rectangle before pinning");

    auto* pinnedWindow = new ScreenshotPinnedWindow(
        ScreenshotPinnedWindow::RuntimeMode::NoDocument);
    QPointer<ScreenshotPinnedWindow> guardedWindow(pinnedWindow);
    ScreenshotPinnedWindow::Config config;
    config.nativeGeometry = physicalPinGeometry(*screen, QPoint(40, 40), background.size());
    config.canvasSourceRect = QRectF(QPointF(0.0, 0.0), QSizeF(bakedImage.size()));
    config.contentCanvasRect = config.canvasSourceRect;
    config.surfaceCanvasRect = config.canvasSourceRect;
    config.imageSource = ScreenshotImageSource::fromImage(bakedImage, config.canvasSourceRect);
    config.fullResolutionScaleBasis = bakedImage.size();
    config.screen = screen;
    config.enableEditing = true;
    require(pinnedWindow->present(config), "pinned copy presentation failed");
    QCoreApplication::processEvents(QEventLoop::AllEvents, 50);
    SnowCanvasWidget* canvas = pinnedWindow->findChild<SnowCanvasWidget*>();
    require(canvas != nullptr && !canvas->canvasHistoryState().canUndo,
            "pinned canvas should start with no inherited canvas history");
    require(containsRedDrawing(canvas->grab().toImage()),
            "pinned canvas display should contain the baked source rectangle");

    require(sourceRuntime.serializeDocumentSession() == sourceSessionBeforePin,
            "presenting a pinned image should not alter the source runtime");

    QPushButton* editButton = buttonNamed(*pinnedWindow, QStringLiteral("Enable drawing mode"));
    require(editButton != nullptr, "pinned edit button was not found before independence check");
    editButton->click();
    QCoreApplication::processEvents(QEventLoop::AllEvents, 50);

    require(canvas->setCanvasTool(SnowCanvasTool::Shape),
            "pinned canvas should activate the shape tool for the independence check");
    SnowCanvasShapeStyle pinnedShapeStyle;
    pinnedShapeStyle.stroke = QColor(24, 80, 240);
    pinnedShapeStyle.strokeWidth = 4.0;
    require(canvas->setCanvasShapeStylePatch(
                pinnedShapeStyle, SnowCanvasShapeStylePropertyStrokeColor |
                                       SnowCanvasShapeStylePropertyStrokeWidth,
                SnowCanvasShapeKind::Rectangle),
            "pinned canvas should configure its independent annotation style");
    const auto sendPinnedPointerEvent = [&canvas](QEvent::Type type, const QPointF& position,
                                                  Qt::MouseButton button,
                                                  Qt::MouseButtons buttons) {
        QMouseEvent event(type, position, canvas->mapToGlobal(position.toPoint()), button,
                          buttons, Qt::NoModifier);
        QCoreApplication::sendEvent(canvas, &event);
    };
    sendPinnedPointerEvent(QEvent::MouseButtonPress, QPointF(48.0, 38.0), Qt::LeftButton,
                           Qt::LeftButton);
    sendPinnedPointerEvent(QEvent::MouseMove, QPointF(152.0, 82.0), Qt::NoButton,
                           Qt::LeftButton);
    sendPinnedPointerEvent(QEvent::MouseButtonRelease, QPointF(152.0, 82.0), Qt::LeftButton,
                           Qt::NoButton);
    require(canvas->canvasHistoryState().canUndo,
            "pinned canvas should accept a new independent annotation");
    require(sourceRuntime.serializeDocumentSession() == sourceSessionBeforePin,
            "pinned annotation should not mutate the source runtime");

    auto* editController = pinnedWindow->findChild<ScreenshotPinnedEditController*>();
    ScreenshotFloatingToolPaletteWindow* toolbarWindow =
        editController != nullptr ? editController->toolbarWindow() : nullptr;
    ScreenshotToolPalette* toolbar = toolbarWindow != nullptr ? toolbarWindow->palette() : nullptr;
    adqt::widgets::AdButton* copyButton = toolbar != nullptr
                                                 ? toolbarButtonNamed(
                                                       *toolbar, QStringLiteral("Copy to clipboard"))
                                                 : nullptr;
    require(editController != nullptr && editController->editMode() && toolbarWindow != nullptr &&
                toolbarWindow->isVisible() && copyButton != nullptr,
            "pinned edit toolbar should expose a live Copy action");

    QApplication::clipboard()->clear();
    copyButton->click();
    const QImage copied =
        waitForClipboardImage([&pinnedWindow, &containsRedDrawing](const QImage& image) {
            return image.size() == pinnedWindow->currentNativeGeometry().size() &&
                   containsRedDrawing(image);
        });
    require(copied.size() == pinnedWindow->currentNativeGeometry().size(),
            "pinned clipboard image should preserve the viewport pixel size");

    require(containsRedDrawing(copied),
            "pinned clipboard image should include canvas-drawn elements");
    require(editController->editMode() && toolbarWindow->isVisible(),
            "copying from the pinned toolbar must keep the editing session open");

    QPushButton* closeButton = buttonNamed(*pinnedWindow, QStringLiteral("Close"));
    require(closeButton != nullptr, "pinned copy close button was not found");
    closeButton->click();
    require(processUntilDeleted(guardedWindow, 2000),
            "pinned window was not deleted after the copy regression test");
}

void typeText(const QString& text, QLineEdit& editor) {
    for (const QChar character : text) {
        QKeyEvent keyPress(QEvent::KeyPress, character.toUpper().unicode(), Qt::NoModifier,
                           QString(character));
        QCoreApplication::sendEvent(&editor, &keyPress);
    }
    waitForUi(100);
}

QPushButton* buttonNamed(QWidget& window, const QString& accessibleName) {
    const QList<QPushButton*> buttons = window.findChildren<QPushButton*>();
    for (QPushButton* button : buttons) {
        if (button != nullptr && button->accessibleName() == accessibleName) {
            return button;
        }
    }
    return nullptr;
}

bool processUntilDeleted(QPointer<ScreenshotPinnedWindow>& window, int timeoutMs) {
    QElapsedTimer elapsed;
    elapsed.start();
    while (!window.isNull() && elapsed.elapsed() < timeoutMs) {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
        QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
        QThread::msleep(1);
    }
    return window.isNull();
}

QImage renderWidget(QWidget& widget) {
    QImage image(widget.size(), QImage::Format_ARGB32_Premultiplied);
    image.fill(Qt::transparent);
    QPainter painter(&image);
    widget.render(&painter, QPoint(), QRegion(), QWidget::DrawChildren);
    return image;
}

void requireColorNear(const QColor& actual, const QColor& expected, int tolerance,
                      const char* message) {
    require(qAbs(actual.red() - expected.red()) <= tolerance &&
                qAbs(actual.green() - expected.green()) <= tolerance &&
                qAbs(actual.blue() - expected.blue()) <= tolerance &&
                qAbs(actual.alpha() - expected.alpha()) <= tolerance,
            message);
}

bool imagesPixelEquivalent(const QImage& first, const QImage& second) {
    if (first.size() != second.size()) {
        return false;
    }
    for (int y = 0; y < first.height(); ++y) {
        for (int x = 0; x < first.width(); ++x) {
            if (first.pixelColor(x, y) != second.pixelColor(x, y)) {
                return false;
            }
        }
    }
    return true;
}

bool imagesPixelAligned(const QImage& actual, const QImage& expected, const QRegion& excluded,
                        int channelTolerance) {
    if (actual.size() != expected.size()) {
        return false;
    }
    for (int y = 0; y < expected.height(); ++y) {
        for (int x = 0; x < expected.width(); ++x) {
            if (excluded.contains(QPoint(x, y))) {
                continue;
            }
            const QColor actualColor = actual.pixelColor(x, y);
            const QColor expectedColor = expected.pixelColor(x, y);
            if (qAbs(actualColor.red() - expectedColor.red()) > channelTolerance ||
                qAbs(actualColor.green() - expectedColor.green()) > channelTolerance ||
                qAbs(actualColor.blue() - expectedColor.blue()) > channelTolerance ||
                qAbs(actualColor.alpha() - expectedColor.alpha()) > channelTolerance) {
                return false;
            }
        }
    }
    return true;
}

class PinnedPresentationObserver final : public QObject {
  public:
    PinnedPresentationObserver(ScreenshotPinnedWindow& window, SnowCanvasWidget& canvas)
        : m_window(window), m_canvas(canvas) {
        m_window.installEventFilter(this);
        m_canvas.installEventFilter(this);
    }

    [[nodiscard]] bool showSeen() const {
        return m_showSeen;
    }

    [[nodiscard]] WId windowIdAtShow() const {
        return m_windowIdAtShow;
    }

    [[nodiscard]] QRect geometryAtShow() const {
        return m_geometryAtShow;
    }

    [[nodiscard]] int windowIdChangesAfterShow() const {
        return m_windowIdChangesAfterShow;
    }

    [[nodiscard]] int geometryChangesAfterShow() const {
        return m_geometryChangesAfterShow;
    }

    [[nodiscard]] int canvasPaintsAfterShow() const {
        return m_canvasPaintsAfterShow;
    }

  protected:
    bool eventFilter(QObject* watched, QEvent* event) override {
        if (event == nullptr) {
            return false;
        }
        if (watched == &m_window) {
            switch (event->type()) {
            case QEvent::WinIdChange:
                if (m_showSeen) {
                    ++m_windowIdChangesAfterShow;
                }
                break;
            case QEvent::Show:
                m_showSeen = true;
                m_windowIdAtShow = m_window.winId();
                m_geometryAtShow = m_window.currentNativeGeometry();
                break;
            case QEvent::Move:
            case QEvent::Resize:
                if (m_showSeen && m_window.currentNativeGeometry() != m_geometryAtShow) {
                    ++m_geometryChangesAfterShow;
                }
                break;
            default:
                break;
            }
        } else if (watched == &m_canvas && m_showSeen && event->type() == QEvent::Paint) {
            ++m_canvasPaintsAfterShow;
        }
        return false;
    }

  private:
    ScreenshotPinnedWindow& m_window;
    SnowCanvasWidget& m_canvas;
    bool m_showSeen = false;
    WId m_windowIdAtShow = 0;
    QRect m_geometryAtShow;
    int m_windowIdChangesAfterShow = 0;
    int m_geometryChangesAfterShow = 0;
    int m_canvasPaintsAfterShow = 0;
};

class PaintEventCounter final : public QObject {
  public:
    explicit PaintEventCounter(QWidget& widget) : m_widget(&widget) {
        m_widget->installEventFilter(this);
    }

    ~PaintEventCounter() override {
        if (m_widget != nullptr) {
            m_widget->removeEventFilter(this);
        }
    }

    [[nodiscard]] int count() const {
        return m_count;
    }

  protected:
    bool eventFilter(QObject* watched, QEvent* event) override {
        if (watched == m_widget && event != nullptr && event->type() == QEvent::Paint) {
            ++m_count;
        }
        return false;
    }

  private:
    QPointer<QWidget> m_widget;
    int m_count = 0;
};

void waitForAnimations(int milliseconds) {
    QElapsedTimer elapsed;
    elapsed.start();
    while (elapsed.elapsed() < milliseconds) {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
        QThread::msleep(1);
    }
}

void pinnedPhysicalPixelsFillClientArea(SnowCanvasRuntime&) {
    QScreen* screen = QGuiApplication::primaryScreen();
    require(screen != nullptr, "a primary screen is required");

    const QRect physicalScreen = ScreenshotGeometryMapper::physicalRectForScreen(*screen);
    const QSize physicalSizes[]{
        QSize(321, 181),
        QSize(323, 183),
        QSize(319, 179),
    };
    for (int iteration = 0; iteration < 3; ++iteration) {
        const QSize physicalSize = physicalSizes[iteration];
        QImage background(physicalSize, QImage::Format_RGBA8888);
        for (int y = 0; y < background.height(); ++y) {
            for (int x = 0; x < background.width(); ++x) {
                background.setPixelColor(x, y,
                                         QColor((x * 37 + y * 17 + iteration * 11 + 1) % 256,
                                                (x * 13 + y * 43 + iteration * 19 + 3) % 256,
                                                (x * 53 + y * 7 + iteration * 23 + 5) % 256, 255));
            }
        }
        background.setDevicePixelRatio(1.25 + iteration * 0.25);

        ScreenshotPinnedWindow::Config config;
        config.nativeGeometry =
            QRect(physicalScreen.topLeft() + QPoint(47 + iteration * 11, 53 + iteration * 13),
                  physicalSize);
        config.canvasSourceRect = QRectF(QPointF(), QSizeF(physicalSize));
        config.backgroundImage = background;
        config.screen = screen;
        config.enableEditing = false;

        auto* pinnedWindow = new ScreenshotPinnedWindow(ScreenshotPinnedWindow::RuntimeMode::NoDocument);
        QPointer<ScreenshotPinnedWindow> guardedWindow(pinnedWindow);
        auto* canvas = pinnedWindow->findChild<SnowCanvasWidget*>();
        auto* controls =
            pinnedWindow->findChild<QFrame*>(QStringLiteral("screenshotPinnedControlsPanel"));
        auto* border = pinnedWindow->findChild<QFrame*>(QStringLiteral("screenshotPinnedBorder"));
        require(canvas != nullptr && controls != nullptr && border != nullptr,
                "physical pin widgets were not found");
        PinnedPresentationObserver observer(*pinnedWindow, *canvas);
        require(pinnedWindow->present(config), "physical-pixel pin presentation failed");

        require(observer.showSeen(), "the pin should be mapped during present()");
        require(observer.windowIdAtShow() == pinnedWindow->winId(),
                "the final native handle must exist before the pin is shown");
        require(observer.geometryAtShow() == config.nativeGeometry &&
                    pinnedWindow->currentNativeGeometry() == config.nativeGeometry,
                "the pinned client geometry must be final when the show event begins");
        require(observer.windowIdChangesAfterShow() == 0 &&
                    observer.geometryChangesAfterShow() == 0,
                "the pin must not recreate or correct its geometry after being shown");
        require(observer.canvasPaintsAfterShow() > 0,
                "present() must synchronously paint the canvas before returning");
#if defined(Q_OS_WIN) || defined(_WIN32)
        const HWND pinnedHwnd = toNativeHwnd(pinnedWindow->winId());
        RECT windowRect{};
        RECT clientRect{};
        POINT clientTopLeft{};
        require(pinnedHwnd != nullptr &&
                    (GetWindowLongPtr(pinnedHwnd, GWL_STYLE) & WS_THICKFRAME) != 0,
                "the pinned HWND should expose the system resize style");
        require(GetWindowRect(pinnedHwnd, &windowRect) != FALSE &&
                    GetClientRect(pinnedHwnd, &clientRect) != FALSE &&
                    ClientToScreen(pinnedHwnd, &clientTopLeft) != FALSE &&
                    qRectForNativeRect(windowRect) == config.nativeGeometry &&
                    QRect(clientTopLeft.x, clientTopLeft.y, clientRect.right - clientRect.left,
                          clientRect.bottom - clientRect.top) == config.nativeGeometry,
                "the frameless resize style must not consume client pixels");
        require(nativeChildWindowCount(pinnedHwnd) == 0,
                "the pinned canvas and controls must remain alien child widgets");
#endif
        QCoreApplication::processEvents(QEventLoop::AllEvents, 50);
        require(observer.windowIdChangesAfterShow() == 0 &&
                    observer.geometryChangesAfterShow() == 0 &&
                    pinnedWindow->currentNativeGeometry() == config.nativeGeometry,
                "the native handle and client geometry must remain stable after event processing");

        QImage rendered(physicalSize, QImage::Format_ARGB32_Premultiplied);
        rendered.setDevicePixelRatio(screen->devicePixelRatio());
        rendered.fill(Qt::transparent);
        {
            QPainter painter(&rendered);
            pinnedWindow->render(&painter, QPoint(), QRegion(), QWidget::DrawChildren);
        }
        rendered.setDevicePixelRatio(1.0);
        require(rendered.size() == physicalSize,
                "the pinned paint surface must contain one pixel per physical screenshot pixel");
        const double physicalScaleX =
            static_cast<double>(physicalSize.width()) / std::max(1, pinnedWindow->width());
        const double physicalScaleY =
            static_cast<double>(physicalSize.height()) / std::max(1, pinnedWindow->height());
        const QRect controlsPhysicalRect =
            QRectF(controls->x() * physicalScaleX, controls->y() * physicalScaleY,
                   controls->width() * physicalScaleX, controls->height() * physicalScaleY)
                .toAlignedRect();
        const int borderPhysicalX = std::max(1, qCeil(2.0 * physicalScaleX));
        const int borderPhysicalY = std::max(1, qCeil(2.0 * physicalScaleY));
        const QRect alignedInterior = rendered.rect().adjusted(borderPhysicalX, borderPhysicalY,
                                                               -borderPhysicalX, -borderPhysicalY);
        const QRegion excludedPixels = QRegion(rendered.rect())
                                           .subtracted(QRegion(alignedInterior))
                                           .united(QRegion(controlsPhysicalRect));
        require(imagesPixelAligned(rendered, background, excludedPixels, 1),
                "the first mapped frame must keep the screenshot interior pixel-aligned");

        pinnedWindow->close();
        require(processUntilDeleted(guardedWindow, 2000), "physical-pixel pin was not deleted");
    }
}

void pinnedContextMenuPreservesNativeGeometry(SnowCanvasRuntime&) {
#if defined(Q_OS_WIN) || defined(_WIN32)
    QScreen* screen = QGuiApplication::primaryScreen();
    require(screen != nullptr, "a primary screen is required");

    const QRect physicalScreen = ScreenshotGeometryMapper::physicalRectForScreen(*screen);
    QImage background(321, 181, QImage::Format_ARGB32_Premultiplied);
    background.fill(QColor(42, 84, 126));

    auto* pinnedWindow = new ScreenshotPinnedWindow(ScreenshotPinnedWindow::RuntimeMode::NoDocument);
    QPointer<ScreenshotPinnedWindow> guardedWindow(pinnedWindow);
    ScreenshotPinnedWindow::Config config;
    // Deliberately avoid a DPI-aligned origin. Qt's integer logical position
    // cannot represent this rectangle exactly on every fractional-DPI screen.
    config.nativeGeometry = QRect(physicalScreen.topLeft() + QPoint(47, 53), background.size());
    config.canvasSourceRect = QRectF(QPointF(), QSizeF(background.size()));
    config.backgroundImage = background;
    config.screen = screen;
    config.enableEditing = false;
    require(pinnedWindow->present(config), "context geometry pin presentation failed");
    waitForUi(50);

    auto* menu = pinnedWindow->findChild<adqt::widgets::AdContextMenu*>(
        QStringLiteral("screenshotPinnedContextMenu"));
    const HWND pinnedHwnd = toNativeHwnd(pinnedWindow->winId());
    require(menu != nullptr && pinnedHwnd != nullptr,
            "context geometry pin should expose its menu and native handle");
    const QRect nativeGeometry = pinnedWindow->currentNativeGeometry();
    const QPoint nativeContextPosition = nativeGeometry.center();
    const QPoint expectedContextPosition = pinnedWindow->mapToGlobal(
        QPoint(qRound((nativeContextPosition.x() - nativeGeometry.left()) *
                      static_cast<double>(pinnedWindow->width()) / nativeGeometry.width()),
               qRound((nativeContextPosition.y() - nativeGeometry.top()) *
                      static_cast<double>(pinnedWindow->height()) / nativeGeometry.height())));
    require(SendMessage(pinnedHwnd, WM_NCHITTEST, 0,
                        MAKELPARAM(static_cast<WORD>(nativeContextPosition.x()),
                                   static_cast<WORD>(nativeContextPosition.y()))) == HTCAPTION,
            "ordinary pinned content should use the native caption path");
    SendMessage(pinnedHwnd, WM_NCRBUTTONDOWN, HTCAPTION,
                MAKELPARAM(static_cast<WORD>(nativeContextPosition.x()),
                           static_cast<WORD>(nativeContextPosition.y())));
    SendMessage(pinnedHwnd, WM_NCRBUTTONUP, HTCAPTION,
                MAKELPARAM(static_cast<WORD>(nativeContextPosition.x()),
                           static_cast<WORD>(nativeContextPosition.y())));

    WINDOWPOS passiveGeometryProposal{};
    passiveGeometryProposal.hwnd = pinnedHwnd;
    passiveGeometryProposal.x = nativeGeometry.x() + 1;
    passiveGeometryProposal.y = nativeGeometry.y() + 1;
    passiveGeometryProposal.cx = nativeGeometry.width();
    passiveGeometryProposal.cy = nativeGeometry.height();
    passiveGeometryProposal.flags = SWP_NOZORDER | SWP_NOACTIVATE;
    SendMessage(pinnedHwnd, WM_WINDOWPOSCHANGING, 0,
                reinterpret_cast<LPARAM>(&passiveGeometryProposal));
    require(passiveGeometryProposal.x == nativeGeometry.x() &&
                passiveGeometryProposal.y == nativeGeometry.y() &&
                passiveGeometryProposal.cx == nativeGeometry.width() &&
                passiveGeometryProposal.cy == nativeGeometry.height(),
            "the context-menu transition must reject rounded native geometry proposals");

    QCoreApplication::processEvents(QEventLoop::AllEvents, 50);
    require(menu->isVisible(), "a native caption right-click should open the pinned context menu");
    require((menu->pos() - expectedContextPosition).manhattanLength() <= 1,
            "the native caption context menu should open at the Qt-global cursor position");
    require(pinnedWindow->currentNativeGeometry() == nativeGeometry,
            "opening the pinned context menu must not round the native window geometry");
    menu->hide();
    waitForUi(50);
    require(pinnedWindow->currentNativeGeometry() == nativeGeometry,
            "closing the pinned context menu must not round the native window geometry");
    WINDOWPOS postMenuGeometryProposal = passiveGeometryProposal;
    postMenuGeometryProposal.x = nativeGeometry.x() + 1;
    postMenuGeometryProposal.y = nativeGeometry.y() + 1;
    SendMessage(pinnedHwnd, WM_WINDOWPOSCHANGING, 0,
                reinterpret_cast<LPARAM>(&postMenuGeometryProposal));
    require(postMenuGeometryProposal.x == nativeGeometry.x() &&
                postMenuGeometryProposal.y == nativeGeometry.y(),
            "closing a menu must not transfer native geometry ownership back to Qt");

    pinnedWindow->close();
    require(processUntilDeleted(guardedWindow, 2000), "context geometry pin was not deleted");
#endif
}

void pinnedContextMenuAndModes(SnowCanvasRuntime&) {
    QScreen* screen = QGuiApplication::primaryScreen();
    require(screen != nullptr, "a primary screen is required");

    FakeOcrRecognition recognition;
    QImage background(320, 180, QImage::Format_ARGB32_Premultiplied);
    background.fill(Qt::transparent);
    for (int y = 0; y < background.height(); ++y) {
        for (int x = 0; x < background.width(); ++x) {
            background.setPixelColor(x, y,
                                     QColor((x * 3) % 256, (y * 5) % 256, (x + y) % 256, 255));
        }
    }
    background.setDevicePixelRatio(1.25);

    auto* pinnedWindow = new ScreenshotPinnedWindow(ScreenshotPinnedWindow::RuntimeMode::NoDocument);
    QPointer<ScreenshotPinnedWindow> guardedWindow(pinnedWindow);
    ScreenshotPinnedWindow::Config config;
    const QRect physicalScreen = ScreenshotGeometryMapper::physicalRectForScreen(*screen);
    const qreal fixtureScaleX = screen->devicePixelRatio();
    const qreal fixtureScaleY = screen->devicePixelRatio();
    config.nativeGeometry = QRect(QPoint(physicalScreen.left() + qRound(80 * fixtureScaleX),
                                         physicalScreen.top() + qRound(80 * fixtureScaleY)),
                                  QSize(qRound(320 * fixtureScaleX), qRound(180 * fixtureScaleY)));
    config.canvasSourceRect = QRectF(QPointF(), QSizeF(320, 180));
    config.backgroundImage = background;
    config.screen = screen;
    config.enableEditing = true;
    config.recognition = &recognition;
    require(pinnedWindow->present(config), "menu test pin presentation failed");
    auto* firstFrameCanvas = pinnedWindow->findChild<SnowCanvasWidget*>();
    require(firstFrameCanvas != nullptr, "pinned canvas should exist on the first frame");
    const QRect firstFrameImageBounds =
        firstFrameCanvas->viewRectForCanvasRect(config.canvasSourceRect);
    require(firstFrameImageBounds == firstFrameCanvas->rect(),
            "pinned image should fit the actual canvas on the first frame");
    require(pinnedWindow->currentNativeGeometry() == config.nativeGeometry,
            "pinned native geometry should be final on the first frame");
    QCoreApplication::processEvents(QEventLoop::AllEvents, 50);
    require(firstFrameCanvas->viewRectForCanvasRect(config.canvasSourceRect) ==
                firstFrameImageBounds,
            "pinned image bounds should not change after the show event loop settles");
    require(pinnedWindow->currentNativeGeometry() == config.nativeGeometry,
            "pinned native geometry should remain stable after the show event loop settles");

    auto* menu = pinnedWindow->findChild<adqt::widgets::AdContextMenu*>(
        QStringLiteral("screenshotPinnedContextMenu"));
    require(menu != nullptr, "pinned window should own an AdContextMenu");
    require(menu->minimumWidth() == 300 && menu->maximumWidth() == 300,
            "pinned context menu should have a fixed width of 300");
    const QList<QAction*> actions = menu->actions();
    require(actions.size() == 14, "pinned menu should have the exact top-level item count");
    require(actions.at(2)->objectName() == QStringLiteral("screenshotPinnedSaveAsFileAction"),
            "Save as File should appear immediately below Copy Original Content");
    require(menu->findChild<QAction*>(QStringLiteral("screenshotPinnedShowMainInterfaceAction")) !=
                    nullptr &&
                !actions.contains(menu->findChild<QAction*>(
                    QStringLiteral("screenshotPinnedShowMainInterfaceAction"))),
            "the show-main-interface fallback should be absent while the tray is enabled");
    const QStringList defaultTrayMenuOptions = snow_shot::storage::TraySettings().menuOptions();
    const QStringList trayMenuWithoutShowMainWindow = {QStringLiteral("quick.screenshot"),
                                                       QStringLiteral("tray.exit")};
    require(snow_shot::storage::TraySettings().setMenuOptions(trayMenuWithoutShowMainWindow),
            "the tray menu option fixture should be writable");
    ScreenshotPinnedWindow::setRuntimeTrayEnabled(true);
    const QList<QAction*> hiddenTrayOptionActions = menu->actions();
    require(hiddenTrayOptionActions.size() == 15 &&
                hiddenTrayOptionActions.at(hiddenTrayOptionActions.size() - 2) ==
                    menu->findChild<QAction*>(
                        QStringLiteral("screenshotPinnedShowMainInterfaceAction")),
            "the pinned menu should retain Show Main Interface when the tray item is hidden");
    require(snow_shot::storage::TraySettings().setMenuOptions(defaultTrayMenuOptions),
            "the tray menu option fixture should be restored");
    ScreenshotPinnedWindow::setRuntimeTrayEnabled(true);
    const QStringList expected{
        QStringLiteral("Copy to Clipboard"),
        QStringLiteral("Copy Original Content"),
        QStringLiteral("Save as File"),
        QStringLiteral("Recognizing text"),
        QString(),
        QStringLiteral("Drawing Mode"),
        QStringLiteral("Process Image"),
        QStringLiteral("Opacity"),
        QStringLiteral("Scale"),
        QString(),
        QStringLiteral("Thumbnail Mode"),
        QStringLiteral("Focus Mode"),
        QString(),
        QStringLiteral("Close"),
    };
    for (int index = 0; index < actions.size(); ++index) {
        require(actions.at(index)->isSeparator() == expected.at(index).isEmpty(),
                "pinned menu separator placement is incorrect");
        if (!actions.at(index)->isSeparator()) {
            require(actions.at(index)->text().section(QLatin1Char('\t'), 0, 0) ==
                        expected.at(index),
                    "pinned menu item order or label is incorrect");
        }
    }
    require(actions.at(0)->text().endsWith(QStringLiteral("\tCtrl+C")) &&
                actions.at(1)->text().endsWith(QStringLiteral("\tCtrl+Shift+C")) &&
                actions.at(2)->text().endsWith(QStringLiteral("\tCtrl+S")) &&
                actions.at(3)->text().endsWith(QStringLiteral("\tCtrl+D")) &&
                actions.at(5)->text().endsWith(QStringLiteral("\tCtrl+E")) &&
                actions.at(10)->text().endsWith(QStringLiteral("\tR")) &&
                actions.constLast()->text().endsWith(QStringLiteral("\tEsc")),
            "pinned menu commands should display every default shortcut");
    require(menu->findChild<adqt::widgets::AdContextMenu*>(
                QStringLiteral("screenshotPinnedMoveCursorMenu")) == nullptr,
            "pinned menu should not expose a Move Cursor item");

    int showMainWindowRequests = 0;
    QObject::connect(pinnedWindow, &ScreenshotPinnedWindow::showMainWindowRequested,
                     [&showMainWindowRequests]() { ++showMainWindowRequests; });
    ScreenshotPinnedWindow::setRuntimeTrayEnabled(false);
    const QList<QAction*> noTrayActions = menu->actions();
    require(noTrayActions.size() == 15 &&
                noTrayActions.at(noTrayActions.size() - 2)->objectName() ==
                    QStringLiteral("screenshotPinnedShowMainInterfaceAction") &&
                noTrayActions.constLast()->objectName() ==
                    QStringLiteral("screenshotPinnedCloseAction"),
            "the show-main-interface fallback should appear immediately before Close");
    noTrayActions.at(noTrayActions.size() - 2)->trigger();
    require(showMainWindowRequests == 1,
            "the show-main-interface fallback should emit its activation request");
    ScreenshotPinnedWindow::setRuntimeTrayEnabled(true);
    require(menu->actions().size() == 14,
            "the show-main-interface fallback should disappear when the tray is enabled");
    require(actions.at(3)->isEnabled() && actions.at(3)->isCheckable(),
            "OCR action should remain available while automatic recognition is pending");
    const auto ocrIconMetadata = adqt::icons::describeIcon(menu->actionIcon(actions.at(3)));
    require(ocrIconMetadata.key.pack == QStringLiteral("snow-shot") &&
                ocrIconMetadata.key.name == QStringLiteral("tool-recognize-text"),
            "OCR action should use the text recognition tool icon");
    require(menu->actionDanger(actions.constLast()),
            "pinned Close action should use danger styling");
    for (int index : {0, 1, 2, 3, 5, 6, 7, 8, 10, 11, 13}) {
        require(menu->actionIcon(actions.at(index)).isValid(),
                "every top-level pinned command should use an Ant Design icon");
    }

    recognition.complete({nullptr, QStringLiteral("deterministic failure")});
    require(actions.at(3)->isEnabled() && actions.at(3)->text().section(QLatin1Char('\t'), 0, 0) ==
                                              QStringLiteral("Display Text Recognition Results"),
            "OCR failure should leave the text recognition command available");
    sendShortcut(*firstFrameCanvas, Qt::Key_D, Qt::ControlModifier);
    auto* canvas = pinnedWindow->findChild<SnowCanvasWidget*>();
    auto* recognitionContent =
        pinnedWindow->findChild<QWidget*>(QStringLiteral("screenshotPinnedRecognitionContent"));
    auto* controlsPanel =
        pinnedWindow->findChild<QFrame*>(QStringLiteral("screenshotPinnedControlsPanel"));
    require(
        canvas != nullptr && controlsPanel != nullptr && recognitionContent != nullptr &&
            recognitionContent->parentWidget() == pinnedWindow && !recognitionContent->isWindow() &&
            !canvas->canvasContentVisible(),
        "OCR mode should use embedded recognition content and hide engine-owned drawing content");
    auto recognizedPresentation = std::make_shared<ScreenshotOcrPresentation>();
    recognizedPresentation->selection = config.canvasSourceRect.toAlignedRect();
    ScreenshotOcrLine recognizedLine;
    recognizedLine.text = QStringLiteral("Pinned OCR text");
    recognizedLine.quad = QPolygonF({QPointF(40.0, 70.0), QPointF(280.0, 70.0),
                                     QPointF(280.0, 110.0), QPointF(40.0, 110.0)});
    recognizedPresentation->lines.push_back(std::move(recognizedLine));
    recognition.complete({std::move(recognizedPresentation), {}});
    QCoreApplication::processEvents(QEventLoop::AllEvents, 50);
    QApplication::clipboard()->clear();
    sendShortcut(*canvas, Qt::Key_A, Qt::ControlModifier);
    sendShortcut(*canvas, Qt::Key_C, Qt::ControlModifier);
    require(QApplication::clipboard()->text() == QStringLiteral("Pinned OCR text"),
            "Copy to Clipboard should copy selected OCR text while recognition is active");
    sendShortcut(*canvas, Qt::Key_E, Qt::ControlModifier);
    require(!actions.at(3)->isChecked() && canvas->canvasContentVisible() &&
                canvas->interactionEnabled(),
            "drawing mode should exit OCR mode and restore canvas interaction");
    auto* editController = pinnedWindow->findChild<ScreenshotPinnedEditController*>();
    ScreenshotToolPalette* editToolbar =
        editController != nullptr && editController->toolbarWindow() != nullptr
            ? editController->toolbarWindow()->palette()
            : nullptr;
    auto* shapeButton = editToolbar != nullptr ? qobject_cast<adqt::widgets::AdButton*>(buttonNamed(
                                                     *editToolbar, QStringLiteral("Shape")))
                                               : nullptr;
    require(shapeButton != nullptr, "drawing mode should expose the shape tool button");
    shapeButton->click();
    require(canvas->canvasTool() == SnowCanvasTool::Shape &&
                shapeButton->buttonStyle() == adqt::widgets::AdButton::ButtonStyle::Solid &&
                shapeButton->accentRole() == adqt::widgets::AdButton::AccentRole::Primary,
            "a drawing tool selected after recognition should retain its highlighted state");
    {
        const CursorPositionRestorer restoreCursor;
        const QPoint start = config.nativeGeometry.center();
        setSystemCursorPosition(start);
        sendShortcut(*canvas, Qt::Key_W);
        require(systemCursorPosition() == start + QPoint(0, -1),
                "W should move the cursor up by one pixel in drawing mode");
        sendShortcut(*canvas, Qt::Key_Down);
        require(systemCursorPosition() == start,
                "Down should move the cursor down by one pixel in drawing mode");
        sendShortcut(*canvas, Qt::Key_A);
        require(systemCursorPosition() == start + QPoint(-1, 0),
                "A should move the cursor left by one pixel in drawing mode");
        sendShortcut(*canvas, Qt::Key_Right);
        require(systemCursorPosition() == start,
                "Right should move the cursor right by one pixel in drawing mode");
    }
    sendShortcut(*canvas, Qt::Key_E, Qt::ControlModifier);

    QApplication::clipboard()->clear();
    sendShortcut(*canvas, Qt::Key_C, Qt::ControlModifier);
    const QImage untransformedCopy = waitForClipboardImage([](const QImage& image) {
        return image.size() == QSize(320, 180) &&
               image.pixelColor(image.width() / 2, image.height() / 2).alpha() == 255;
    });
    require(
        untransformedCopy.pixelColor(untransformedCopy.width() / 2, untransformedCopy.height() / 2)
                .alpha() == 255,
        "untransformed viewport copy should include the pinned background");

    auto* processMenu = qobject_cast<adqt::widgets::AdContextMenu*>(actions.at(6)->menu());
    auto* opacityMenu = qobject_cast<adqt::widgets::AdContextMenu*>(actions.at(7)->menu());
    auto* scaleMenu = qobject_cast<adqt::widgets::AdContextMenu*>(actions.at(8)->menu());
    require(processMenu != nullptr && opacityMenu != nullptr && scaleMenu != nullptr,
            "pinned command submenus should also use AdContextMenu");
    require(opacityMenu->actions().constLast()->text() == QStringLiteral("Current: 100%") &&
                !opacityMenu->actions().constLast()->isEnabled() &&
                scaleMenu->actions().constLast()->text() == QStringLiteral("Current: 100%") &&
                !scaleMenu->actions().constLast()->isEnabled(),
            "opacity and scale submenus should expose disabled current-value readouts");
    const auto flipVerticalIconMetadata =
        adqt::icons::describeIcon(processMenu->actionIcon(processMenu->actions().at(3)));
    require(flipVerticalIconMetadata.key.pack == QStringLiteral("snow-shot") &&
                flipVerticalIconMetadata.key.name == QStringLiteral("flip-vertical"),
            "vertical flip should use the project-owned vertical flip icon");
    const QPoint originalCenter = pinnedWindow->frameGeometry().center();
    processMenu->actions().at(0)->trigger();
    QCoreApplication::processEvents(QEventLoop::AllEvents, 50);
    require(pinnedWindow->size() == QSize(180, 320) &&
                (pinnedWindow->frameGeometry().center() - originalCenter).manhattanLength() <= 2,
            "clockwise rotation should swap size while preserving the window center");
    opacityMenu->actions().at(0)->trigger();
    require(qAbs(pinnedWindow->windowOpacity() - 0.25) <= (1.0 / 255.0),
            "pinned opacity should make the top-level window transparent");
    scaleMenu->actions().at(1)->trigger();
    QCoreApplication::processEvents(QEventLoop::AllEvents, 50);
    require(pinnedWindow->size() == QSize(90, 160),
            "50 percent scale should use the rotation-aware initial pin size");
    actions.at(0)->trigger();
    const QImage scaledCopy = waitForClipboardImage([](const QImage& image) {
        return image.size() == QSize(90, 160) &&
               image.pixelColor(image.width() / 2, image.height() / 2).alpha() == 255;
    });
    require(scaledCopy.pixelColor(scaledCopy.width() / 2, scaledCopy.height() / 2).alpha() == 255,
            "transformed viewport copy should include the transformed background");

    sendShortcut(*canvas, Qt::Key_E, Qt::ControlModifier);
    require(canvas->interactionEnabled(),
            "drawing mode should be active before entering thumbnail mode");

    sendShortcut(*canvas, Qt::Key_R);
    auto* thumbnailAnimation = pinnedWindow->findChild<QVariantAnimation*>(
        QStringLiteral("screenshotPinnedGeometryAnimation"));
    require(thumbnailAnimation != nullptr && thumbnailAnimation->duration() == 150,
            "thumbnail mode should use a 150ms geometry animation");
    require(!actions.at(5)->isChecked() && !canvas->interactionEnabled(),
            "thumbnail mode should exit drawing mode");
    require(controlsPanel->isHidden(), "thumbnail mode should hide the top-right controls");
    waitForAnimations(200);
    require(pinnedWindow->size() == QSize(83, 83),
            "thumbnail mode should finish at an 83dp square");
    actions.at(0)->trigger();
    const QImage thumbnailCopy = waitForClipboardImage([](const QImage& image) {
        return image.size() == QSize(47, 83) &&
               image.pixelColor(2, image.height() / 2).alpha() == 255 &&
               image.pixelColor(image.width() / 2, image.height() / 2).alpha() == 255;
    });
    require(thumbnailCopy.size() == QSize(47, 83) &&
                thumbnailCopy.pixelColor(2, thumbnailCopy.height() / 2).alpha() == 255 &&
                thumbnailCopy.pixelColor(thumbnailCopy.width() / 2, thumbnailCopy.height() / 2)
                        .alpha() == 255,
            "thumbnail copy should contain the complete result without transparent padding");

    QApplication::clipboard()->clear();
    sendShortcut(*canvas, Qt::Key_C, Qt::ControlModifier | Qt::ShiftModifier);
    const QImage originalCopy = waitForClipboardImage(
        [&background](const QImage& image) { return imagesPixelEquivalent(image, background); });
    require(imagesPixelEquivalent(originalCopy, background),
            "Copy Original Content should preserve the immutable pixels and dimensions");

    sendShortcut(*canvas, Qt::Key_E, Qt::ControlModifier);
    require(!actions.at(10)->isChecked() && !actions.at(5)->isChecked() &&
                !canvas->interactionEnabled(),
            "drawing from thumbnail mode should restore the window before editing");
    waitForAnimations(200);
    require(pinnedWindow->size() == QSize(90, 160) && actions.at(5)->isChecked() &&
                canvas->interactionEnabled(),
            "drawing mode should activate after the thumbnail restoration animation");
    require(controlsPanel->isHidden(),
            "the top-right controls should remain hidden after editing starts");

    sendShortcut(*canvas, Qt::Key_Escape);
    require(processUntilDeleted(guardedWindow, 2000),
            "pinned window was not deleted after the context menu test");
}

void pinnedCloseCancelsPendingRecognition(SnowCanvasRuntime&) {
    QScreen* screen = QGuiApplication::primaryScreen();
    require(screen != nullptr, "a primary screen is required");

    FakeOcrRecognition recognition;
    QImage background(160, 90, QImage::Format_ARGB32_Premultiplied);
    background.fill(QColor(24, 72, 120));

    auto* pinnedWindow = new ScreenshotPinnedWindow(ScreenshotPinnedWindow::RuntimeMode::NoDocument);
    QPointer<ScreenshotPinnedWindow> guardedWindow(pinnedWindow);
    ScreenshotPinnedWindow::Config config;
    config.nativeGeometry = physicalPinGeometry(*screen, QPoint(60, 60), background.size());
    config.canvasSourceRect = QRectF(QPointF(), QSizeF(background.size()));
    config.backgroundImage = background;
    config.screen = screen;
    config.recognition = &recognition;
    require(pinnedWindow->present(config), "pending OCR pin presentation failed");
    QCoreApplication::processEvents(QEventLoop::AllEvents, 50);
    const auto requestToken = recognition.pending.token;
    require(requestToken != 0, "pinned window should start text recognition");

    QPushButton* closeButton = buttonNamed(*pinnedWindow, QStringLiteral("Close"));
    require(closeButton != nullptr, "close button was not found");
    closeButton->click();
    require(recognition.cancelledTokens.contains(requestToken) && recognition.pending.token == 0,
            "closing a pinned window must cancel text recognition immediately");
    require(processUntilDeleted(guardedWindow, 2000),
            "pinned window was not deleted after canceling text recognition");
}

void pinnedAsyncPresentationDefersContent(SnowCanvasRuntime&) {
    QScreen* screen = QGuiApplication::primaryScreen();
    require(screen != nullptr, "a primary screen is required");

    QImage expectedImage(QSize(160, 96), QImage::Format_ARGB32_Premultiplied);
    expectedImage.fill(QColor(36, 132, 204));

    auto makeConfig = [screen](const QImage& placeholder, ScreenshotImageLoader loader) {
        ScreenshotPinnedWindow::Config config;
        config.nativeGeometry = physicalPinGeometry(*screen, QPoint(60, 60), placeholder.size());
        config.canvasSourceRect = QRectF(QPointF(), QSizeF(placeholder.size()));
        config.imageSource = ScreenshotImageSource::fromImage(placeholder, config.canvasSourceRect);
        config.fullResolutionScaleBasis = placeholder.size();
        config.screen = screen;
        config.imageLoader = std::move(loader);
        config.enableEditing = false;
        return config;
    };

    ScreenshotImageLoadCallback successLoad;
    auto* successfulWindow =
        new ScreenshotPinnedWindow(ScreenshotPinnedWindow::RuntimeMode::NoDocument);
    QPointer<ScreenshotPinnedWindow> guardedSuccessfulWindow(successfulWindow);
    int successCompletionCount = 0;
    bool successCompletionValue = false;
    const ScreenshotImageLoader successLoader =
        [&successLoad](QObject*, ScreenshotImageLoadCallback callback) {
            successLoad = std::move(callback);
        };
    QImage placeholder(QSize(160, 96), QImage::Format_ARGB32_Premultiplied);
    placeholder.fill(Qt::transparent);
    require(successfulWindow->present(
                makeConfig(placeholder, successLoader),
                [&successCompletionCount, &successCompletionValue](bool succeeded, QImage image) {
                    ++successCompletionCount;
                    successCompletionValue = succeeded && !image.isNull();
                }),
            "asynchronous pinned presentation failed to create its shell");
    QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
    SnowCanvasWidget* successCanvas =
        successfulWindow->findChild<SnowCanvasWidget*>();
    require(successfulWindow->isVisible() && successCanvas != nullptr &&
                !successCanvas->canvasContentVisible() && successCompletionCount == 0,
            "the pinned shell should be visible with hidden content while loading");
    const QImage transparentFrame = renderWidget(*successCanvas);
    require(transparentFrame.pixelColor(transparentFrame.rect().center()).alpha() == 0,
            "the pinned canvas should stay transparent until materialization completes");
    require(static_cast<bool>(successLoad),
            "the pinned image loader should start after the shell is shown");
    successLoad(expectedImage);
    QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
    require(successCanvas->canvasContentVisible() && successCompletionCount == 1 &&
                successCompletionValue,
            "successful pinned materialization should reveal content exactly once");
    const QImage loadedFrame = renderWidget(*successCanvas);
    require(loadedFrame.pixelColor(loadedFrame.rect().center()).alpha() > 0,
            "the pinned canvas should render materialized content");
    successfulWindow->close();
    require(processUntilDeleted(guardedSuccessfulWindow, 2000),
            "successful asynchronous pinned window was not deleted");

    ScreenshotImageLoadCallback failureLoad;
    auto* failedWindow = new ScreenshotPinnedWindow(ScreenshotPinnedWindow::RuntimeMode::NoDocument);
    QPointer<ScreenshotPinnedWindow> guardedFailedWindow(failedWindow);
    int failureCompletionCount = 0;
    bool failureCompletionValue = true;
    const ScreenshotImageLoader failureLoader =
        [&failureLoad](QObject*, ScreenshotImageLoadCallback callback) {
            failureLoad = std::move(callback);
        };
    require(failedWindow->present(
                makeConfig(placeholder, failureLoader),
                [&failureCompletionCount, &failureCompletionValue](bool succeeded, QImage) {
                    ++failureCompletionCount;
                    failureCompletionValue = succeeded;
                }),
            "failed asynchronous pinned presentation could not create its shell");
    require(static_cast<bool>(failureLoad), "the failed pinned loader did not start");
    failureLoad({});
    QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
    require(failureCompletionCount == 1 && !failureCompletionValue,
            "failed pinned materialization should complete exactly once with failure");
    require(processUntilDeleted(guardedFailedWindow, 2000),
            "failed asynchronous pinned window was not closed");

    ScreenshotImageLoadCallback closeLoad;
    auto* closedWindow = new ScreenshotPinnedWindow(ScreenshotPinnedWindow::RuntimeMode::NoDocument);
    QPointer<ScreenshotPinnedWindow> guardedClosedWindow(closedWindow);
    int closeCompletionCount = 0;
    bool closeCompletionValue = true;
    const ScreenshotImageLoader closeLoader =
        [&closeLoad](QObject*, ScreenshotImageLoadCallback callback) {
            closeLoad = std::move(callback);
        };
    require(closedWindow->present(
                makeConfig(placeholder, closeLoader),
                [&closeCompletionCount, &closeCompletionValue](bool succeeded, QImage) {
                    ++closeCompletionCount;
                    closeCompletionValue = succeeded;
                }),
            "close-during-load pinned presentation could not create its shell");
    closedWindow->close();
    closeLoad = {};
    require(closeCompletionCount == 1 && !closeCompletionValue,
            "closing a loading pinned window should resolve presentation exactly once");
    require(processUntilDeleted(guardedClosedWindow, 2000),
            "close-during-load pinned window was not deleted");
}

void pinnedRecognitionPromotesAutomaticPrefetch(SnowCanvasRuntime&) {
    QScreen* screen = QGuiApplication::primaryScreen();
    require(screen != nullptr, "a primary screen is required");

    FakeOcrRecognition recognition;
    QImage background(160, 90, QImage::Format_ARGB32_Premultiplied);
    background.fill(QColor(24, 72, 120));

    auto* pinnedWindow = new ScreenshotPinnedWindow(ScreenshotPinnedWindow::RuntimeMode::NoDocument);
    QPointer<ScreenshotPinnedWindow> guardedWindow(pinnedWindow);
    ScreenshotPinnedWindow::Config config;
    config.nativeGeometry = physicalPinGeometry(*screen, QPoint(60, 60), background.size());
    config.canvasSourceRect = QRectF(QPointF(), QSizeF(background.size()));
    config.imageSource = ScreenshotImageSource::fromLayers(
        {ScreenshotImageLayer{background, config.canvasSourceRect, config.canvasSourceRect}});
    config.screen = screen;
    config.recognition = &recognition;
    require(pinnedWindow->present(config), "priority pin presentation failed");
    QCoreApplication::processEvents(QEventLoop::AllEvents, 50);

    require(recognition.pending.token != 0 &&
                recognition.pending.request.priority == ScreenshotOcrRequestPriority::Prefetch,
            "pinned automatic recognition should be submitted as prefetch work");
    auto* menu = pinnedWindow->findChild<adqt::widgets::AdContextMenu*>(
        QStringLiteral("screenshotPinnedContextMenu"));
    require(menu != nullptr && menu->actions().size() > 2,
            "pinned OCR action should be available during prefetch");
    menu->actions().at(3)->trigger();
    require(recognition.pending.request.priority == ScreenshotOcrRequestPriority::Interactive,
            "opening pending pinned recognition should promote it to interactive work");

    QPushButton* closeButton = buttonNamed(*pinnedWindow, QStringLiteral("Close"));
    require(closeButton != nullptr, "priority pin close button was not found");
    closeButton->click();
    require(processUntilDeleted(guardedWindow, 2000),
            "priority pin was not deleted after the test");
}

void pinnedFormattedClipboardTextSkipsOcrAndSeedsPlainTextEditing(
    SnowCanvasRuntime&) {
    QScreen* screen = QGuiApplication::primaryScreen();
    require(screen != nullptr, "a primary screen is required");

    FakeOcrRecognition recognition;
    const qreal formattedTextDevicePixelRatio = screen->devicePixelRatio();
    const QSize logicalTextSize(320, 120);
    auto document = std::make_shared<QTextDocument>();
    document->setDocumentMargin(0.0);
    document->setHtml(QStringLiteral("<p><b>Formatted</b> clipboard text</p>"));
    document->setTextWidth(logicalTextSize.width());
    QImage renderedText(QSize(qCeil(logicalTextSize.width() * formattedTextDevicePixelRatio),
                              qCeil(logicalTextSize.height() * formattedTextDevicePixelRatio)),
                        QImage::Format_ARGB32_Premultiplied);
    renderedText.setDevicePixelRatio(formattedTextDevicePixelRatio);
    renderedText.fill(Qt::white);
    QPainter textPainter(&renderedText);
    document->drawContents(&textPainter, QRectF(QPointF(), QSizeF(logicalTextSize)));
    textPainter.end();
    const QString plainText = document->toPlainText();

    auto* pinnedWindow = new ScreenshotPinnedWindow(ScreenshotPinnedWindow::RuntimeMode::NoDocument);
    QPointer<ScreenshotPinnedWindow> guardedWindow(pinnedWindow);
    ScreenshotPinnedWindow::Config config;
    config.nativeGeometry = physicalPinGeometry(*screen, QPoint(60, 60), renderedText.size());
    config.canvasSourceRect = QRectF(QPointF(), QSizeF(renderedText.size()));
    config.backgroundImage = renderedText;
    config.screen = screen;
    config.recognition = &recognition;
    config.formattedTextDocument = document;
    config.formattedPlainText = plainText;
    config.formattedTextDevicePixelRatio = formattedTextDevicePixelRatio;
    const QString originalHtml = QStringLiteral(
        "<section data-origin=\"clipboard\"><b>Formatted</b> clipboard text</section>");
    const QString originalText = QStringLiteral("Formatted clipboard text");
    config.originalClipboardContent = {originalHtml, originalText};
    require(pinnedWindow->present(config), "formatted clipboard pin presentation failed");
    QCoreApplication::processEvents(QEventLoop::AllEvents, 50);
    require(recognition.nextToken == 0 && recognition.pending.token == 0,
            "formatted clipboard text must not start automatic OCR");

    auto* copyOriginalAction =
        pinnedWindow->findChild<QAction*>(QStringLiteral("screenshotPinnedCopyOriginalAction"));
    require(copyOriginalAction != nullptr,
            "formatted clipboard pin should expose the original-content copy action");
    copyOriginalAction->trigger();
    const QMimeData* copiedOriginal = QApplication::clipboard()->mimeData();
    require(copiedOriginal != nullptr && copiedOriginal->hasHtml() && copiedOriginal->hasText() &&
                copiedOriginal->html() == originalHtml && copiedOriginal->text() == originalText,
            "Copy Original Content should restore the HTML and text given to the pinned window");

    auto* menu = pinnedWindow->findChild<adqt::widgets::AdContextMenu*>(
        QStringLiteral("screenshotPinnedContextMenu"));
    require(menu != nullptr && menu->actions().size() > 3 && menu->actions().at(3)->isEnabled(),
            "formatted clipboard text should expose text selection without an OCR result");
    menu->actions().at(3)->trigger();
    QCoreApplication::processEvents(QEventLoop::AllEvents, 50);

    auto* textLayer =
        pinnedWindow->findChild<QGraphicsView*>(QStringLiteral("screenshotClipboardText"));
    auto* textItem = formattedTextItem(textLayer);
    require(textLayer != nullptr && textLayer->isVisible() && textItem != nullptr &&
                textItem->document() == document.get() && textItem->toPlainText() == plainText &&
                qFuzzyCompare(textItem->scale(), formattedTextDevicePixelRatio) &&
                recognition.nextToken == 0,
            "text mode should show the original selectable formatted document without OCR");
    const QPointF mappedOrigin = textLayer->transform().map(QPointF());
    const QPointF mappedExtent =
        textLayer->transform().map(QPointF(renderedText.width(), renderedText.height()));
    const QSizeF mappedSize(mappedExtent.x() - mappedOrigin.x(),
                            mappedExtent.y() - mappedOrigin.y());
    require(std::abs(mappedSize.width() - textLayer->viewport()->width()) < 1.0 &&
                std::abs(mappedSize.height() - textLayer->viewport()->height()) < 1.0,
            "formatted clipboard text should use the same physical-pixel canvas basis as the "
            "pinned image on the current DPI");

    auto* session = pinnedWindow->findChild<ScreenshotRecognitionSessionController*>();
    auto* editController = pinnedWindow->findChild<ScreenshotPinnedEditController*>();
    ScreenshotFloatingToolPaletteWindow* toolbarWindow =
        editController != nullptr ? editController->toolbarWindow() : nullptr;
    ScreenshotToolPalette* toolbar = toolbarWindow != nullptr ? toolbarWindow->palette() : nullptr;
    adqt::widgets::AdButton* toolbarCopy = toolbar != nullptr
                                                  ? toolbarButtonNamed(
                                                        *toolbar, QStringLiteral("Copy to clipboard"))
                                                  : nullptr;
    require(session != nullptr && session->originalText() == plainText && toolbarCopy != nullptr,
            "formatted clipboard text should seed the recognition session with plain text");
    session->beginTextEditing();
    QCoreApplication::processEvents(QEventLoop::AllEvents, 50);
    auto* editor = pinnedWindow->findChild<QTextEdit*>(QStringLiteral("screenshotOcrEditor"));
    require(editor != nullptr && editor->toPlainText() == plainText,
            "editing formatted clipboard text should use its plain-text projection");

    const snow_shot::storage::ScreenshotShortcutSettings screenshotShortcuts;
    const QStringList originalTextRecognitionShortcuts =
        screenshotShortcuts.shortcuts(QStringLiteral("text_recognition"));
    require(screenshotShortcuts.setShortcuts(QStringLiteral("text_recognition"),
                                             {QStringLiteral("Alt+D")}),
            "the Text Recognition shortcut fixture should accept a runtime mapping");
    QCoreApplication::processEvents(QEventLoop::AllEvents, 50);
    clickTextEditor(*editor);
    require(editor->hasFocus(), "the pinned recognition editor should own keyboard focus");
    sendShortcut(*editor, Qt::Key_D, Qt::AltModifier);
    require(recognition.nextToken == 0 && recognition.pending.token == 0 &&
                editor->toPlainText() == plainText,
            "configured screenshot shortcuts must not interrupt pinned text editing");
    require(screenshotShortcuts.setShortcuts(QStringLiteral("text_recognition"),
                                             originalTextRecognitionShortcuts),
            "the Text Recognition shortcut fixture should restore its original mapping");

    const QString editedDraft = QStringLiteral("Edited formatted clipboard text");
    session->setTextDraft(editedDraft);
    QCoreApplication::processEvents(QEventLoop::AllEvents, 50);
    toolbarCopy->click();
    require(QApplication::clipboard()->text() == editedDraft && session->editing() &&
                editController->editMode() && toolbarWindow->isVisible(),
            "toolbar Copy should export the current text draft without ending editing");

    session->endTextEditing();
    QCoreApplication::processEvents(QEventLoop::AllEvents, 50);
    toolbarCopy->click();
    require(QApplication::clipboard()->text() == plainText && session->active() &&
                editController->editMode() && toolbarWindow->isVisible(),
            "toolbar Copy should export formatted pins as plain text without ending recognition");
    textLayer = pinnedWindow->findChild<QGraphicsView*>(QStringLiteral("screenshotClipboardText"));
    textItem = formattedTextItem(textLayer);
    require(textLayer != nullptr && textItem != nullptr && textItem->document() == document.get(),
            "ending plain-text editing should restore the formatted clipboard document");

    pinnedWindow->close();
    require(processUntilDeleted(guardedWindow, 2000),
            "formatted clipboard pin was not deleted after the test");
}

void pinnedAutomaticRecognitionCanBeDisabled(SnowCanvasRuntime&) {
    QScreen* screen = QGuiApplication::primaryScreen();
    require(screen != nullptr, "a primary screen is required");

    FakeOcrRecognition recognition;
    QImage background(160, 90, QImage::Format_ARGB32_Premultiplied);
    background.fill(QColor(24, 72, 120));

    auto* pinnedWindow = new ScreenshotPinnedWindow(ScreenshotPinnedWindow::RuntimeMode::NoDocument);
    QPointer<ScreenshotPinnedWindow> guardedWindow(pinnedWindow);
    ScreenshotPinnedWindow::Config config;
    config.nativeGeometry = physicalPinGeometry(*screen, QPoint(60, 60), background.size());
    config.canvasSourceRect = QRectF(QPointF(), QSizeF(background.size()));
    config.backgroundImage = background;
    config.screen = screen;
    config.recognition = &recognition;
    config.automaticTextRecognition = false;
    require(pinnedWindow->present(config), "manual OCR pin presentation failed");
    QCoreApplication::processEvents(QEventLoop::AllEvents, 50);
    require(recognition.pending.token == 0,
            "disabling automatic recognition should suppress pinned OCR prefetch");

    auto* menu = pinnedWindow->findChild<adqt::widgets::AdContextMenu*>(
        QStringLiteral("screenshotPinnedContextMenu"));
    require(menu != nullptr && menu->actions().size() > 2,
            "manual pinned OCR action should remain available without prefetch");
    menu->actions().at(3)->trigger();
    require(recognition.pending.token != 0 &&
                recognition.pending.request.priority == ScreenshotOcrRequestPriority::Interactive,
            "manual pinned OCR should still start interactive recognition");

    pinnedWindow->close();
    require(processUntilDeleted(guardedWindow, 2000),
            "manual OCR pin was not deleted after the test");
}

void pinnedSeededRecognitionSkipsAutomaticRequests(SnowCanvasRuntime&) {
    QScreen* screen = QGuiApplication::primaryScreen();
    require(screen != nullptr, "a primary screen is required");

    FakeOcrRecognition recognition;
    FakeQrRecognition qrRecognition;
    SnowShotApiClient tableRecognition(QString{});
    QImage background(160, 90, QImage::Format_ARGB32_Premultiplied);
    background.fill(QColor(24, 72, 120));

    auto presentation = std::make_shared<ScreenshotOcrPresentation>();
    presentation->selection = QRect(0, 0, background.width(), background.height());
    ScreenshotOcrLine line;
    line.text = QStringLiteral("Seeded OCR");
    line.quad = QPolygonF({QPointF(20.0, 30.0), QPointF(140.0, 30.0), QPointF(140.0, 50.0),
                           QPointF(20.0, 50.0)});
    presentation->lines.push_back(line);

    ScreenshotRecognitionResults results;
    results.key = QStringLiteral("capture-1:0,0,160,90");
    results.text = ScreenshotOcrRecognitionResult{presentation, {}};
    results.table = SnowShotTableResult{
        QStringLiteral("<table><tr><td>Seeded table</td></tr></table>"), {}, {}, 200};
    results.qr = ScreenshotQrRecognitionResult{{QStringLiteral("https://example.test/seeded")}, {}};

    auto* pinnedWindow = new ScreenshotPinnedWindow(ScreenshotPinnedWindow::RuntimeMode::NoDocument);
    QPointer<ScreenshotPinnedWindow> guardedWindow(pinnedWindow);
    ScreenshotPinnedWindow::Config config;
    config.nativeGeometry = physicalPinGeometry(*screen, QPoint(60, 60), background.size());
    config.canvasSourceRect = QRectF(QPointF(), QSizeF(background.size()));
    config.backgroundImage = background;
    config.screen = screen;
    config.recognition = &recognition;
    config.qrRecognition = &qrRecognition;
    config.tableRecognition = &tableRecognition;
    config.recognitionResults = results;
    require(pinnedWindow->present(config), "seeded pin presentation failed");
    QCoreApplication::processEvents(QEventLoop::AllEvents, 50);
    require(recognition.nextToken == 0 && recognition.pending.token == 0,
            "a seeded OCR result should suppress automatic pinned OCR");

    auto* session = pinnedWindow->findChild<ScreenshotRecognitionSessionController*>();
    require(session != nullptr, "seeded pinned recognition session was not found");
    require(session->hasTextResult(),
            "a seeded OCR result should be visible before the recognition tool is activated");
    session->activate(ScreenshotRecognitionSessionController::Mode::Text);
    require(session->hasTextResult() && !session->busy() && recognition.nextToken == 0,
            "a seeded OCR result should activate without a provider request");
    session->activate(ScreenshotRecognitionSessionController::Mode::Table);
    require(session->tableModeActive() && !session->busy(ScreenshotRecognitionSessionController::Mode::Table),
            "a seeded table result should activate without a provider request");
    session->activate(ScreenshotRecognitionSessionController::Mode::Qr);
    require(session->qrModeActive() && !session->busy(ScreenshotRecognitionSessionController::Mode::Qr) &&
                qrRecognition.nextToken == 0,
            "a seeded QR result should activate without a provider request");

    pinnedWindow->close();
    require(processUntilDeleted(guardedWindow, 2000),
            "seeded pinned window was not deleted after the test");
}

void pinnedCloseAfterRecognizedText(SnowCanvasRuntime&) {
    QScreen* screen = QGuiApplication::primaryScreen();
    require(screen != nullptr, "a primary screen is required");

    FakeOcrRecognition recognition;
    QImage background(160, 90, QImage::Format_ARGB32_Premultiplied);
    background.fill(QColor(24, 72, 120));

    auto* pinnedWindow = new ScreenshotPinnedWindow(ScreenshotPinnedWindow::RuntimeMode::NoDocument);
    QPointer<ScreenshotPinnedWindow> guardedWindow(pinnedWindow);
    ScreenshotPinnedWindow::Config config;
    config.nativeGeometry = physicalPinGeometry(*screen, QPoint(60, 60), background.size());
    config.canvasSourceRect = QRectF(QPointF(), QSizeF(background.size()));
    config.backgroundImage = background;
    config.screen = screen;
    config.recognition = &recognition;
    require(pinnedWindow->present(config), "successful OCR pin presentation failed");
    QCoreApplication::processEvents(QEventLoop::AllEvents, 50);
    require(recognition.pending.token != 0, "pinned OCR should start a request");

    auto presentation = std::make_shared<ScreenshotOcrPresentation>();
    presentation->selection = QRect(0, 0, background.width(), background.height());
    ScreenshotOcrLine line;
    line.text = QStringLiteral("Recognized text");
    line.quad = QPolygonF(
        {QPointF(20.0, 30.0), QPointF(140.0, 30.0), QPointF(140.0, 50.0), QPointF(20.0, 50.0)});
    presentation->lines.push_back(line);
    recognition.complete({std::move(presentation), {}});
    QCoreApplication::processEvents(QEventLoop::AllEvents, 50);

    auto* menu = pinnedWindow->findChild<adqt::widgets::AdContextMenu*>(
        QStringLiteral("screenshotPinnedContextMenu"));
    require(menu != nullptr, "pinned context menu was not found");
    menu->actions().at(3)->trigger();
    QCoreApplication::processEvents(QEventLoop::AllEvents, 50);
    auto* recognitionContent =
        pinnedWindow->findChild<QWidget*>(QStringLiteral("screenshotPinnedRecognitionContent"));
    require(recognitionContent != nullptr && recognitionContent->isVisible(),
            "recognized text should be visible before close");

    QPushButton* closeButton = buttonNamed(*pinnedWindow, QStringLiteral("Close"));
    require(closeButton != nullptr, "close button was not found after showing recognized text");
    closeButton->click();
    require(processUntilDeleted(guardedWindow, 2000),
            "pinned window was not deleted after closing recognized text");
}

void pinnedRecognitionProviderLossEndsBusyState(SnowCanvasRuntime&) {
    QScreen* screen = QGuiApplication::primaryScreen();
    require(screen != nullptr, "a primary screen is required");

    auto recognition = std::make_unique<FakeOcrRecognition>();
    QImage background(160, 90, QImage::Format_ARGB32_Premultiplied);
    background.fill(QColor(24, 72, 120));

    auto* pinnedWindow = new ScreenshotPinnedWindow(ScreenshotPinnedWindow::RuntimeMode::NoDocument);
    QPointer<ScreenshotPinnedWindow> guardedWindow(pinnedWindow);
    ScreenshotPinnedWindow::Config config;
    config.nativeGeometry = physicalPinGeometry(*screen, QPoint(60, 60), background.size());
    config.canvasSourceRect = QRectF(QPointF(), QSizeF(background.size()));
    config.backgroundImage = background;
    config.screen = screen;
    config.recognition = recognition.get();
    require(pinnedWindow->present(config), "provider-loss OCR pin presentation failed");
    QCoreApplication::processEvents(QEventLoop::AllEvents, 50);
    require(recognition->pending.token != 0,
            "pinned window should have a pending request before provider loss");

    auto* menu = pinnedWindow->findChild<adqt::widgets::AdContextMenu*>(
        QStringLiteral("screenshotPinnedContextMenu"));
    require(menu != nullptr && menu->actions().at(3)->text().section(QLatin1Char('\t'), 0, 0) ==
                                   QStringLiteral("Recognizing text"),
            "pinned OCR should initially expose its pending state");

    recognition.reset();
    QCoreApplication::processEvents(QEventLoop::AllEvents, 50);
    require(menu->actions().at(3)->text().section(QLatin1Char('\t'), 0, 0) ==
                    QStringLiteral("Display Text Recognition Results") &&
                !menu->actions().at(3)->isEnabled(),
            "provider loss must terminate and disable pinned OCR instead of remaining busy");

    pinnedWindow->close();
    require(processUntilDeleted(guardedWindow, 2000),
            "pinned window was not deleted after the provider-loss test");
}

void pinnedControlsMatchReferenceStyle(SnowCanvasRuntime&) {
    QScreen* screen = QGuiApplication::primaryScreen();
    require(screen != nullptr, "a primary screen is required");

    auto* pinnedWindow = new ScreenshotPinnedWindow(ScreenshotPinnedWindow::RuntimeMode::NoDocument);
    QPointer<ScreenshotPinnedWindow> guardedWindow(pinnedWindow);
    QImage background(400, 400, QImage::Format_ARGB32_Premultiplied);
    background.fill(QColor(42, 84, 126));

    ScreenshotPinnedWindow::Config config;
    config.nativeGeometry = physicalPinGeometry(*screen, QPoint(40, 40), background.size());
    config.canvasSourceRect = QRectF(QPointF(0.0, 0.0), QSizeF(background.size()));
    config.backgroundImage = background;
    config.screen = screen;
    config.enableEditing = true;
    require(pinnedWindow->present(config), "pinned window presentation failed");
    QCoreApplication::processEvents(QEventLoop::AllEvents, 50);

    require(pinnedWindow->testAttribute(Qt::WA_AlwaysShowToolTips),
            "pinned controls should show tooltips while their tool window is inactive");

    auto* panel = pinnedWindow->findChild<QFrame*>(QStringLiteral("screenshotPinnedControlsPanel"));
    auto* border = pinnedWindow->findChild<QFrame*>(QStringLiteral("screenshotPinnedBorder"));
    auto* canvas = pinnedWindow->findChild<SnowCanvasWidget*>();
    auto* editButton = pinnedWindow->findChild<adqt::widgets::AdButton*>(
        QStringLiteral("screenshotPinnedEditButton"));
    auto* closeButton = pinnedWindow->findChild<adqt::widgets::AdButton*>(
        QStringLiteral("screenshotPinnedCloseButton"));
    require(panel != nullptr && border != nullptr && canvas != nullptr && editButton != nullptr &&
                closeButton != nullptr,
            "pinned window should use named border and control widgets");
    setPinnedWindowHovered(*pinnedWindow, false);
    require(panel->isHidden(), "pinned controls should be hidden while the pointer is outside");
    setPinnedWindowHovered(*pinnedWindow, true);
    require(panel->isVisible(), "pinned controls should appear while the window is hovered");
    setPinnedWindowHovered(*pinnedWindow, false);
    require(panel->isHidden(), "pinned controls should hide when the pointer leaves the window");
    setPinnedWindowHovered(*pinnedWindow, true);
    require(pinnedWindow->testAttribute(Qt::WA_TranslucentBackground) &&
                !canvas->testAttribute(Qt::WA_OpaquePaintEvent) &&
                canvas->testAttribute(Qt::WA_NoSystemBackground),
            "pinned result widgets should use per-pixel transparency");
    require(border->geometry() == pinnedWindow->rect() && border->isVisible() &&
                border->testAttribute(Qt::WA_TransparentForMouseEvents) &&
                !border->mask().contains(border->rect().center()),
            "the pinned border should cover the edges without obstructing the canvas");
    require(editButton->shape() == adqt::widgets::AdButton::Shape::Circle &&
                closeButton->shape() == adqt::widgets::AdButton::Shape::Circle &&
                editButton->size() == QSize(32, 32) && closeButton->size() == QSize(32, 32),
            "pinned controls should be 32 pixel circular buttons");
    require(panel->geometry().topRight() == QPoint(pinnedWindow->width() - 17, 16),
            "pinned controls should use the reference 16 pixel top-right inset");

    const QImage pinnedWindowImage = renderWidget(*pinnedWindow);
    const int middleY = pinnedWindowImage.height() / 2;
    const QColor borderColor(QStringLiteral("#DBDBDB"));
    requireColorNear(pinnedWindowImage.pixelColor(0, middleY), borderColor, 0,
                     "pinned window should draw the requested border color");
    requireColorNear(pinnedWindowImage.pixelColor(1, middleY), borderColor, 0,
                     "pinned window border should be two pixels wide");
    requireColorNear(pinnedWindowImage.pixelColor(2, middleY), background.pixelColor(0, middleY), 0,
                     "pinned window border should not extend beyond two pixels");
    const QColor liveBorderColor(QStringLiteral("#276EF1"));
    ScreenshotPinnedWindow::setRuntimeBorderColor(liveBorderColor);
    QCoreApplication::processEvents(QEventLoop::AllEvents, 50);
    require(border->property("borderColor").value<QColor>() == liveBorderColor,
            "a live border-color update should reach an existing pinned window");
    const QImage recoloredPinnedWindow = renderWidget(*pinnedWindow);
    requireColorNear(recoloredPinnedWindow.pixelColor(0, middleY), liveBorderColor, 0,
                     "a live border-color update should repaint the pinned border");
    ScreenshotPinnedWindow::setRuntimeBorderColor(borderColor);

    const QColor mask = adqt::theme::ThemeManager::instance().resolveTheme(editButton).colorBgMask;
    const QImage editNormal = renderWidget(*editButton);
    requireColorNear(editNormal.pixelColor(4, editButton->height() / 2), mask, 2,
                     "pinned edit button should use the semi-transparent black mask color");

    const QImage panelImage = renderWidget(*panel);
    require(panelImage.pixelColor(36, panel->height() / 2).alpha() == 0,
            "pinned controls should have a transparent eight pixel gap without a panel surface");

    const QPointF center(editButton->rect().center());
    QEnterEvent editEnter(center, center, QPointF(editButton->mapToGlobal(center.toPoint())));
    QCoreApplication::sendEvent(editButton, &editEnter);
    const QColor primary =
        adqt::theme::ThemeManager::instance().resolveTheme(editButton).colorPrimary;
    requireColorNear(renderWidget(*editButton).pixelColor(4, editButton->height() / 2), primary, 2,
                     "pinned edit button should use the theme primary color on hover");

    const QPointF closeCenter(closeButton->rect().center());
    QEnterEvent closeEnter(closeCenter, closeCenter,
                           QPointF(closeButton->mapToGlobal(closeCenter.toPoint())));
    QCoreApplication::sendEvent(closeButton, &closeEnter);
    const QColor error = adqt::theme::ThemeManager::instance().resolveTheme(closeButton).colorError;
    requireColorNear(renderWidget(*closeButton).pixelColor(4, closeButton->height() / 2), error, 2,
                     "pinned close button should use the theme error color on hover");

    closeButton->click();
    require(processUntilDeleted(guardedWindow, 2000),
            "pinned window was not deleted after the control style test");
}

void pinnedConfiguredShortcutUpdatesImmediately(SnowCanvasRuntime&) {
    QScreen* screen = QGuiApplication::primaryScreen();
    require(screen != nullptr, "a primary screen is required");

    QImage background(160, 90, QImage::Format_ARGB32_Premultiplied);
    background.fill(Qt::white);
    auto* pinnedWindow = new ScreenshotPinnedWindow(ScreenshotPinnedWindow::RuntimeMode::NoDocument);
    QPointer<ScreenshotPinnedWindow> guardedWindow(pinnedWindow);
    ScreenshotPinnedWindow::Config config;
    config.nativeGeometry = physicalPinGeometry(*screen, QPoint(60, 60), background.size());
    config.canvasSourceRect = QRectF(QPointF(), QSizeF(background.size()));
    config.backgroundImage = background;
    config.screen = screen;
    config.enableEditing = true;
    require(pinnedWindow->present(config), "shortcut test pin presentation failed");
    auto* canvas = pinnedWindow->findChild<SnowCanvasWidget*>();
    auto* drawingAction =
        pinnedWindow->findChild<QAction*>(QStringLiteral("screenshotPinnedDrawingAction"));
    require(canvas != nullptr && drawingAction != nullptr,
            "shortcut test pin controls were not found");

    const snow_shot::storage::PinToScreenShortcutSettings shortcuts;
    require(shortcuts.setShortcuts(QStringLiteral("drawing_mode"), {QStringLiteral("Ctrl+Alt+E")}),
            "the pinned drawing-mode shortcut should be configurable");
    waitForUi(50);
    require(drawingAction->text().endsWith(QStringLiteral("\tCtrl+Alt+E")),
            "an open pinned window should refresh its menu shortcut display immediately");
    sendShortcut(*canvas, Qt::Key_E, Qt::ControlModifier);
    require(!drawingAction->isChecked(),
            "the previous pinned drawing-mode shortcut should stop activating immediately");
    sendShortcut(*canvas, Qt::Key_E, Qt::ControlModifier | Qt::AltModifier);
    require(drawingAction->isChecked(),
            "the configured pinned drawing-mode shortcut should activate immediately");
    drawingAction->setChecked(false);
    require(shortcuts.setShortcuts(QStringLiteral("drawing_mode"), {QStringLiteral("Ctrl+E")}),
            "the pinned drawing-mode shortcut fixture should restore its default");
    waitForUi(50);
    require(drawingAction->text().endsWith(QStringLiteral("\tCtrl+E")),
            "restoring a pinned shortcut should immediately restore the menu display");

    pinnedWindow->close();
    require(processUntilDeleted(guardedWindow, 2000), "shortcut test pin was not deleted");
}

#if defined(Q_OS_WIN) || defined(_WIN32)
void pinnedNativeDragAcceptsCursorMovementShortcuts(SnowCanvasRuntime&) {
    QScreen* screen = QGuiApplication::primaryScreen();
    require(screen != nullptr, "a primary screen is required");
    const CursorPositionRestorer restoreCursor;

    const snow_shot::storage::PinToScreenShortcutSettings shortcuts;
    const QString actionId = QStringLiteral("move_cursor_up");
    const QStringList previousShortcuts = shortcuts.shortcuts(actionId);
    require(shortcuts.setShortcuts(actionId, {QStringLiteral("W")}),
            "the native-drag cursor shortcut fixture could not be configured");

    QImage background(240, 140, QImage::Format_ARGB32_Premultiplied);
    background.fill(QColor(48, 96, 144));
    auto* pinnedWindow = new ScreenshotPinnedWindow(ScreenshotPinnedWindow::RuntimeMode::NoDocument);
    QPointer<ScreenshotPinnedWindow> guardedWindow(pinnedWindow);
    ScreenshotPinnedWindow::Config config;
    config.nativeGeometry = physicalPinGeometry(*screen, QPoint(120, 120), background.size());
    config.canvasSourceRect = QRectF(QPointF(), QSizeF(background.size()));
    config.backgroundImage = background;
    config.screen = screen;
    require(pinnedWindow->present(config), "native-drag shortcut pin presentation failed");
    waitForUi(100);

    const HWND hwnd = toNativeHwnd(pinnedWindow->winId());
    require(hwnd != nullptr, "native-drag shortcut pin did not expose an HWND");

    const QRect startingGeometry = pinnedWindow->currentNativeGeometry();
    const QPoint startingCursor = startingGeometry.center();
    setSystemCursorPosition(startingCursor);
    waitForUi(50);

    static_cast<void>(SendMessageW(
        hwnd, WM_NCLBUTTONDOWN, HTCAPTION,
        MAKELPARAM(static_cast<WORD>(startingCursor.x()), static_cast<WORD>(startingCursor.y()))));
    const bool capturedForMove = GetCapture() == hwnd;
    const QPoint cursorBeforeShortcut = systemCursorPosition();
    const QPoint windowPositionBeforeShortcut = pinnedWindow->currentNativeGeometry().topLeft();
    sendShortcut(*pinnedWindow, Qt::Key_W);
    const QPoint cursorAfterShortcuts = systemCursorPosition();
    const QPoint windowPositionAfterShortcuts = pinnedWindow->currentNativeGeometry().topLeft();
    const QPoint shortcutCursorDelta = cursorAfterShortcuts - cursorBeforeShortcut;
    const QPoint shortcutWindowDelta = windowPositionAfterShortcuts - windowPositionBeforeShortcut;
    const bool cursorShortcutsMoved = shortcutCursorDelta == QPoint(0, -1);
    const bool windowFollowedShortcuts = shortcutWindowDelta == shortcutCursorDelta;

    const QPoint pointerDelta(7, 3);
    const QPoint cursorBeforePointerMove = systemCursorPosition();
    setSystemCursorPosition(cursorAfterShortcuts + pointerDelta);
    static_cast<void>(SendMessageW(hwnd, WM_MOUSEMOVE, MK_LBUTTON, 0));
    const QPoint actualPointerDelta = systemCursorPosition() - cursorBeforePointerMove;
    const bool windowFollowedPointer =
        pinnedWindow->currentNativeGeometry().topLeft() - windowPositionAfterShortcuts ==
        actualPointerDelta;
    static_cast<void>(SendMessageW(hwnd, WM_LBUTTONUP, 0, 0));
    waitForUi(50);

    require(shortcuts.setShortcuts(actionId, previousShortcuts),
            "the native-drag cursor shortcut fixture could not restore its configuration");
    pinnedWindow->close();
    require(processUntilDeleted(guardedWindow, 2000), "native-drag shortcut pin was not deleted");
    require(capturedForMove, "a native caption press must start pinned-window pointer capture");
    require(cursorShortcutsMoved,
            "configured cursor shortcuts must remain active throughout a pinned-window drag");
    require(windowFollowedShortcuts,
            "a pinned window must follow cursor shortcuts before its native drag is released");
    require(windowFollowedPointer,
            "application-managed pinned-window dragging must continue to follow pointer input");
}
#endif

void pinnedThumbnailUsesOpaqueThemeBackground(SnowCanvasRuntime&) {
    QScreen* screen = QGuiApplication::primaryScreen();
    require(screen != nullptr, "a primary screen is required");

    QImage transparentImage(400, 400, QImage::Format_ARGB32_Premultiplied);
    transparentImage.fill(Qt::transparent);

    auto* pinnedWindow = new ScreenshotPinnedWindow(ScreenshotPinnedWindow::RuntimeMode::NoDocument);
    QPointer<ScreenshotPinnedWindow> guardedWindow(pinnedWindow);
    ScreenshotPinnedWindow::Config config;
    config.nativeGeometry = physicalPinGeometry(*screen, QPoint(40, 40), transparentImage.size());
    config.canvasSourceRect = QRectF(QPointF(), QSizeF(transparentImage.size()));
    config.backgroundImage = transparentImage;
    config.screen = screen;
    require(pinnedWindow->present(config), "transparent pinned window presentation failed");
    QCoreApplication::processEvents(QEventLoop::AllEvents, 50);

    auto* thumbnailAction =
        pinnedWindow->findChild<QAction*>(QStringLiteral("screenshotPinnedThumbnailAction"));
    require(thumbnailAction != nullptr, "pinned thumbnail action was not found");
    thumbnailAction->setChecked(true);
    waitForUi(200);
    require(thumbnailAction->isChecked(), "pinned window should enter thumbnail mode");

    QColor expectedBackground =
        adqt::theme::ThemeManager::instance().resolveTheme(pinnedWindow).colorBgContainer;
    if (!expectedBackground.isValid()) {
        expectedBackground = pinnedWindow->palette().color(QPalette::Window);
    }
    expectedBackground.setAlpha(255);
    const QImage thumbnail = renderWidget(*pinnedWindow);
    requireColorNear(thumbnail.pixelColor(thumbnail.rect().center()), expectedBackground, 0,
                     "transparent thumbnail regions should use the opaque theme background");

    pinnedWindow->close();
    require(processUntilDeleted(guardedWindow, 2000),
            "pinned window was not deleted after the thumbnail background test");
}

void pinnedControlsHideBelowMinimumNativeSize(SnowCanvasRuntime&) {
    QScreen* screen = QGuiApplication::primaryScreen();
    require(screen != nullptr, "a primary screen is required");

    const auto verifyControls = [screen](const QSize& nativeSize,
                                                         bool expectedVisible) {
        QImage background(400, 400, QImage::Format_ARGB32_Premultiplied);
        background.fill(QColor(42, 84, 126));

        auto* pinnedWindow = new ScreenshotPinnedWindow(ScreenshotPinnedWindow::RuntimeMode::NoDocument);
        QPointer<ScreenshotPinnedWindow> guardedWindow(pinnedWindow);
        ScreenshotPinnedWindow::Config config;
        config.nativeGeometry = physicalPinGeometry(*screen, QPoint(40, 40), nativeSize);
        config.canvasSourceRect = QRectF(QPointF(0.0, 0.0), QSizeF(background.size()));
        config.backgroundImage = background;
        config.screen = screen;
        config.enableEditing = true;
        require(pinnedWindow->present(config),
                "minimum-size controls test pin presentation failed");
        QCoreApplication::processEvents(QEventLoop::AllEvents, 50);

        auto* controlsPanel =
            pinnedWindow->findChild<QFrame*>(QStringLiteral("screenshotPinnedControlsPanel"));
        setPinnedWindowHovered(*pinnedWindow, true);
        require(controlsPanel != nullptr && controlsPanel->isVisible() == expectedVisible,
                expectedVisible ? "pinned controls should be visible at the minimum size"
                                : "pinned controls should be hidden below the minimum size");

        pinnedWindow->close();
        require(processUntilDeleted(guardedWindow, 2000),
                "minimum-size controls test pin was not deleted");
    };

    verifyControls(QSize(383, 383), true);
    verifyControls(QSize(382, 383), false);
    verifyControls(QSize(383, 382), false);
}

void closePinnedWindow(SnowCanvasRuntime&, bool enableEditing, bool enterEditMode,
                       int iteration) {
    std::cerr << "iteration=" << iteration << " editing=" << enableEditing
              << " editMode=" << enterEditMode << " start\n";
    QScreen* screen = QGuiApplication::primaryScreen();
    require(screen != nullptr, "a primary screen is required");

    auto* pinnedWindow = new ScreenshotPinnedWindow(ScreenshotPinnedWindow::RuntimeMode::NoDocument);
    QPointer<ScreenshotPinnedWindow> guardedWindow(pinnedWindow);

    QImage background(400, 400, QImage::Format_ARGB32_Premultiplied);
    background.fill(QColor(42, 84, 126));

    ScreenshotPinnedWindow::Config config;
    config.nativeGeometry = physicalPinGeometry(*screen, QPoint(40, 40), QSize(400, 400));
    config.canvasSourceRect = QRectF(QPointF(0.0, 0.0), QSizeF(background.size()));
    config.backgroundImage = background;
    config.screen = screen;
    config.enableEditing = enableEditing;
    require(pinnedWindow->present(config), "pinned window presentation failed");
    QCoreApplication::processEvents(QEventLoop::AllEvents, 50);

    if (enterEditMode) {
        QPushButton* editButton = buttonNamed(*pinnedWindow, QStringLiteral("Enable drawing mode"));
        require(editButton != nullptr, "edit button was not found");
        editButton->click();
        QCoreApplication::processEvents(QEventLoop::AllEvents, 50);
    }

    int heartbeatCount = 0;
    QTimer heartbeat;
    heartbeat.setInterval(0);
    QObject::connect(&heartbeat, &QTimer::timeout, [&heartbeatCount]() { ++heartbeatCount; });
    heartbeat.start();

    QPushButton* closeButton = buttonNamed(*pinnedWindow, QStringLiteral("Close"));
    require(closeButton != nullptr, "close button was not found");
    closeButton->click();

    require(processUntilDeleted(guardedWindow, 2000),
            "pinned window was not deleted after clicking close");
    require(heartbeatCount > 0, "event loop stopped responding while closing pinned window");
    std::cerr << "iteration=" << iteration << " editing=" << enableEditing
              << " editMode=" << enterEditMode << " closed\n";
}

void pinnedScalingAndAspectLockedResizing(SnowCanvasRuntime&) {
    QScreen* screen = QGuiApplication::primaryScreen();
    require(screen != nullptr, "a primary screen is required");

    FakeOcrRecognition recognition;
    QImage background(240, 120, QImage::Format_ARGB32_Premultiplied);
    background.fill(QColor(37, 91, 143));
    const qreal dpr = screen->devicePixelRatio();
    const QRect physicalScreen = ScreenshotGeometryMapper::physicalRectForScreen(*screen);

    auto* pinnedWindow = new ScreenshotPinnedWindow(ScreenshotPinnedWindow::RuntimeMode::NoDocument);
    QPointer<ScreenshotPinnedWindow> guardedWindow(pinnedWindow);
    ScreenshotPinnedWindow::Config config;
    const QSize logicalFixtureSize(1000, 500);
    config.nativeGeometry = QRect(
        physicalScreen.topLeft() + QPoint(qRound(120 * dpr), qRound(100 * dpr)),
        QSize(qRound(logicalFixtureSize.width() * dpr), qRound(logicalFixtureSize.height() * dpr)));
    config.canvasSourceRect = QRectF(QPointF(), QSizeF(background.size()));
    config.backgroundImage = background;
    config.screen = screen;
    config.enableEditing = true;
    config.recognition = &recognition;
    require(pinnedWindow->present(config), "scaling test pin presentation failed");
    waitForUi(60);

    auto* canvas = pinnedWindow->findChild<SnowCanvasWidget*>();
    auto* scaleLabel =
        pinnedWindow->findChild<QLabel*>(QStringLiteral("screenshotPinnedScaleLabel"));
    auto* menu = pinnedWindow->findChild<adqt::widgets::AdContextMenu*>(
        QStringLiteral("screenshotPinnedContextMenu"));
    auto* scaleMenu = pinnedWindow->findChild<adqt::widgets::AdContextMenu*>(
        QStringLiteral("screenshotPinnedScaleMenu"));
    auto* opacityMenu = pinnedWindow->findChild<adqt::widgets::AdContextMenu*>(
        QStringLiteral("screenshotPinnedOpacityMenu"));
    auto* controlsPanel =
        pinnedWindow->findChild<QFrame*>(QStringLiteral("screenshotPinnedControlsPanel"));
    require(canvas != nullptr && scaleLabel != nullptr && menu != nullptr &&
                opacityMenu != nullptr && scaleMenu != nullptr && controlsPanel != nullptr,
            "scaling test controls were not found");
    require(pinnedWindow->currentNativeGeometry().size() == config.nativeGeometry.size() &&
                scaleMenu->actions().at(3)->isChecked() && scaleLabel->isHidden() &&
                opacityMenu->actions().constLast()->text() == QStringLiteral("Current: 100%") &&
                scaleMenu->actions().constLast()->text() == QStringLiteral("Current: 100%"),
            "the initial native size and current-value readouts should be 100 percent");

    const auto sendWheel = [canvas](const QPoint& localPosition, const QPoint& pixelDelta,
                                    const QPoint& angleDelta,
                                    Qt::KeyboardModifiers modifiers = Qt::NoModifier) {
        QWheelEvent wheel(QPointF(localPosition), QPointF(canvas->mapToGlobal(localPosition)),
                          pixelDelta, angleDelta, Qt::NoButton, modifiers, Qt::NoScrollPhase,
                          false);
        QCoreApplication::sendEvent(canvas, &wheel);
        require(wheel.isAccepted(), "vertical pinned wheel input should be consumed");
        waitForUi(30);
    };
    const auto expectedSize = [&config](int percent, bool transposed = false) {
        QSize baseline = config.nativeGeometry.size();
        if (transposed) {
            baseline.transpose();
        }
        return QSize(qRound(baseline.width() * percent / 100.0),
                     qRound(baseline.height() * percent / 100.0));
    };

    const QPoint center = canvas->rect().center();
    const QRect beforeOpacityWheel = pinnedWindow->currentNativeGeometry();
    sendWheel(center, QPoint(), QPoint(0, -480), Qt::ControlModifier);
    require(pinnedWindow->currentNativeGeometry() == beforeOpacityWheel &&
                qAbs(pinnedWindow->windowOpacity() - 0.80) <= (1.0 / 255.0) &&
                scaleLabel->isVisible() && scaleLabel->text() == QStringLiteral("Opacity: 80%") &&
                opacityMenu->actions().constLast()->text() == QStringLiteral("Current: 80%") &&
                scaleMenu->actions().constLast()->text() == QStringLiteral("Current: 100%"),
            "Ctrl-wheel should adjust opacity without changing the pinned scale");
    require(std::none_of(
                opacityMenu->actions().cbegin(), opacityMenu->actions().cend(),
                [](const QAction* action) { return action->isCheckable() && action->isChecked(); }),
            "non-preset Ctrl-wheel opacity should leave every preset unchecked");
    sendWheel(center, QPoint(), QPoint(0, 120), Qt::ControlModifier);
    require(qAbs(pinnedWindow->windowOpacity() - 0.85) <= (1.0 / 255.0) &&
                scaleLabel->isVisible() && scaleLabel->text() == QStringLiteral("Opacity: 85%") &&
                opacityMenu->actions().constLast()->text() == QStringLiteral("Current: 85%"),
            "Ctrl-wheel should increase opacity in five percentage point steps");
    opacityMenu->actions().at(3)->trigger();
    require(qAbs(pinnedWindow->windowOpacity() - 1.0) <= (1.0 / 255.0) && scaleLabel->isVisible() &&
                scaleLabel->text() == QStringLiteral("Opacity: 100%") &&
                opacityMenu->actions().constLast()->text() == QStringLiteral("Current: 100%"),
            "the opacity preset should restore the current opacity readout");

    const QRect beforeCenterScale = pinnedWindow->currentNativeGeometry();
    const QPoint nativeCenterAnchor = beforeCenterScale.center();
    sendWheel(center, QPoint(), QPoint(0, 120));
    const QRect afterCenterScale = pinnedWindow->currentNativeGeometry();
    require(afterCenterScale.size() == expectedSize(110) &&
                (afterCenterScale.center() - nativeCenterAnchor).manhattanLength() <= 2 &&
                scaleLabel->isVisible() && scaleLabel->text() == QStringLiteral("Scale: 110%") &&
                scaleMenu->actions().constLast()->text() == QStringLiteral("Current: 110%"),
            "one wheel notch should scale ten points around the cursor");

    sendWheel(center, QPoint(), QPoint(0, 60));
    require(pinnedWindow->currentNativeGeometry().size() == expectedSize(110),
            "a partial wheel delta should not scale before reaching 120 units");
    sendWheel(center, QPoint(), QPoint(0, 60));
    require(pinnedWindow->currentNativeGeometry().size() == expectedSize(120),
            "partial wheel deltas should accumulate into one ten-point step");
    sendWheel(center, QPoint(), QPoint(0, -240));
    require(pinnedWindow->currentNativeGeometry().size() == expectedSize(100),
            "a multi-notch wheel delta should apply every ten-point step");

    const QPoint arbitraryPoint(canvas->width() / 4, canvas->height() * 2 / 3);
    const QRect beforeArbitraryScale = pinnedWindow->currentNativeGeometry();
    const double normalizedX = static_cast<double>(arbitraryPoint.x()) / canvas->width();
    const double normalizedY = static_cast<double>(arbitraryPoint.y()) / canvas->height();
    const QPoint arbitraryAnchor(
        qRound(beforeArbitraryScale.left() + normalizedX * beforeArbitraryScale.width()),
        qRound(beforeArbitraryScale.top() + normalizedY * beforeArbitraryScale.height()));
    sendWheel(arbitraryPoint, QPoint(0, 1), QPoint());
    const QRect afterArbitraryScale = pinnedWindow->currentNativeGeometry();
    const QPoint preservedAnchor(
        qRound(afterArbitraryScale.left() + normalizedX * afterArbitraryScale.width()),
        qRound(afterArbitraryScale.top() + normalizedY * afterArbitraryScale.height()));
    require(afterArbitraryScale.size() == expectedSize(110) &&
                (preservedAnchor - arbitraryAnchor).manhattanLength() <= 3,
            "pixel-delta scaling should preserve an arbitrary cursor anchor");
    require(std::none_of(scaleMenu->actions().cbegin(), scaleMenu->actions().cend(),
                         [](const QAction* action) { return action->isChecked(); }),
            "non-preset wheel scales should leave every preset unchecked");
    waitForUi(600);
    sendWheel(arbitraryPoint, QPoint(0, 1), QPoint());
    waitForUi(600);
    require(scaleLabel->isVisible() && scaleLabel->text() == QStringLiteral("Scale: 120%"),
            "continued scaling should restart the one-second readout timer");

    sendWheel(center, QPoint(), QPoint(0, 120 * 100));
    require(pinnedWindow->currentNativeGeometry().size() == expectedSize(500),
            "wheel scaling should clamp at 500 percent");
    sendWheel(center, QPoint(), QPoint(0, -120 * 100));
    require(pinnedWindow->currentNativeGeometry().size() == expectedSize(10),
            "wheel scaling should clamp at 10 percent");

    scaleMenu->actions().at(3)->trigger();
    waitForUi(1100);
    require(scaleLabel->isHidden(), "the scale readout should hide after one second");
    const QRect beforeRotation = pinnedWindow->currentNativeGeometry();
    auto* processMenu = qobject_cast<adqt::widgets::AdContextMenu*>(menu->actions().at(6)->menu());
    require(processMenu != nullptr, "the process-image menu was not found");
    processMenu->actions().at(0)->trigger();
    waitForUi(40);
    require(pinnedWindow->currentNativeGeometry().size() == expectedSize(100, true) &&
                (pinnedWindow->currentNativeGeometry().center() - beforeRotation.center())
                        .manhattanLength() <= 2 &&
                scaleLabel->isHidden(),
            "rotation should transpose the baseline without changing or showing scale");
    const QPoint presetTopLeft = pinnedWindow->currentNativeGeometry().topLeft();
    scaleMenu->actions().at(1)->trigger();
    waitForUi(40);
    require(pinnedWindow->currentNativeGeometry().size() == expectedSize(50, true),
            "context presets should use the oriented native baseline");
    require(pinnedWindow->currentNativeGeometry().topLeft() == presetTopLeft,
            "context presets should preserve the native top-left anchor");
    require(scaleLabel->text() == QStringLiteral("Scale: 50%"),
            "context presets should update the scale readout");

    scaleMenu->actions().at(3)->trigger();
    waitForUi(40);
    QRect arbitraryNative = pinnedWindow->currentNativeGeometry();
    arbitraryNative.setSize(expectedSize(83, true));
#if defined(Q_OS_WIN) || defined(_WIN32)
    const HWND arbitraryResizeHwnd = toNativeHwnd(pinnedWindow->winId());
    RECT arbitraryProposal = nativeRectForQRect(arbitraryNative);
    SendMessage(arbitraryResizeHwnd, WM_ENTERSIZEMOVE, 0, 0);
    require(SendMessage(arbitraryResizeHwnd, WM_SIZING, WMSZ_BOTTOMRIGHT,
                        reinterpret_cast<LPARAM>(&arbitraryProposal)) == TRUE,
            "the arbitrary native resize proposal was not accepted");
    const QRect acceptedArbitraryResize = qRectForNativeRect(arbitraryProposal);
    SetWindowPos(arbitraryResizeHwnd, nullptr, acceptedArbitraryResize.x(),
                 acceptedArbitraryResize.y(), acceptedArbitraryResize.width(),
                 acceptedArbitraryResize.height(), SWP_NOZORDER | SWP_NOACTIVATE);
    SendMessage(arbitraryResizeHwnd, WM_EXITSIZEMOVE, 0, 0);
#else
    pinnedWindow->resize(arbitraryNative.size());
#endif
    waitForUi(80);
    require(scaleLabel->text() == QStringLiteral("Scale: 83%") && scaleLabel->isVisible() &&
                std::none_of(scaleMenu->actions().cbegin(), scaleMenu->actions().cend(),
                             [](const QAction* action) { return action->isChecked(); }),
            "an operating-system resize should adopt and display an arbitrary scale");

    sendWheel(canvas->rect().center(), QPoint(), QPoint(0, 120));
    require(pinnedWindow->currentNativeGeometry().size() == expectedSize(90, true) &&
                scaleLabel->text() == QStringLiteral("Scale: 90%"),
            "wheel scaling should advance an arbitrary 83 percent scale to 90 percent");
    sendWheel(canvas->rect().center(), QPoint(), QPoint(0, -120));
    require(pinnedWindow->currentNativeGeometry().size() == expectedSize(80, true) &&
                scaleLabel->text() == QStringLiteral("Scale: 80%"),
            "wheel scaling should move an arbitrary 83 percent scale down to 80 percent");

    scaleMenu->actions().at(3)->trigger();
    waitForUi(30);

#if defined(Q_OS_WIN) || defined(_WIN32)
    const HWND pinnedHwnd = toNativeHwnd(pinnedWindow->winId());
    require(pinnedHwnd != nullptr, "pinned window should have a native handle");
    require((GetWindowLongPtr(pinnedHwnd, GWL_STYLE) & WS_THICKFRAME) != 0,
            "system resizing requires WS_THICKFRAME on the pinned HWND");
    require(nativeChildWindowCount(pinnedHwnd) == 0,
            "the scaling pin should contain no native child windows");
    PaintEventCounter canvasPaints(*canvas);

    const auto nativeHitTest = [pinnedHwnd](const QPoint& position) {
        return SendMessage(
            pinnedHwnd, WM_NCHITTEST, 0,
            MAKELPARAM(static_cast<WORD>(position.x()), static_cast<WORD>(position.y())));
    };

    const QRect hitTestGeometry = pinnedWindow->currentNativeGeometry();
    const int hitTestRight = hitTestGeometry.left() + hitTestGeometry.width() - 1;
    const int hitTestBottom = hitTestGeometry.top() + hitTestGeometry.height() - 1;
    const QPoint hitTestCenter = hitTestGeometry.center();
    struct NativeHitTestCase {
        QPoint position;
        LRESULT expected;
    };
    const std::vector<NativeHitTestCase> hitTestCases{
        {{hitTestGeometry.left(), hitTestGeometry.top()}, HTTOPLEFT},
        {{hitTestRight, hitTestGeometry.top()}, HTTOPRIGHT},
        {{hitTestRight, hitTestBottom}, HTBOTTOMRIGHT},
        {{hitTestGeometry.left(), hitTestBottom}, HTBOTTOMLEFT},
        {{hitTestCenter.x(), hitTestGeometry.top()}, HTTOP},
        {{hitTestRight, hitTestCenter.y()}, HTRIGHT},
        {{hitTestCenter.x(), hitTestBottom}, HTBOTTOM},
        {{hitTestGeometry.left(), hitTestCenter.y()}, HTLEFT},
    };
    for (const NativeHitTestCase& testCase : hitTestCases) {
        require(nativeHitTest(testCase.position) == testCase.expected,
                "WM_NCHITTEST should expose every expected resize border");
    }
    require(WindowFromPoint(POINT{hitTestCenter.x(), hitTestCenter.y()}) == pinnedHwnd &&
                nativeHitTest(hitTestCenter) == HTCAPTION,
            "ordinary image content should hit the single pinned surface as a caption");
    {
        const CursorPositionRestorer restoreCursorPosition;
        QCursor::setPos(pinnedWindow->mapToGlobal(pinnedWindow->rect().center()));
        waitForUi(20);

        setPinnedWindowHovered(*pinnedWindow, false);
        require(controlsPanel->isHidden(),
                "the native caption hover test should start with hidden controls");
        SendMessage(
            pinnedHwnd, WM_NCMOUSEMOVE, HTCAPTION,
            MAKELPARAM(static_cast<WORD>(hitTestCenter.x()), static_cast<WORD>(hitTestCenter.y())));
        require(controlsPanel->isVisible(),
                "a native caption hover should reveal the pinned controls");

        const HCURSOR resizeCursor = LoadCursor(nullptr, IDC_SIZEWE);
        require(resizeCursor != nullptr, "the horizontal resize cursor should load");
        const HCURSOR previousCursor = SetCursor(resizeCursor);
        const LRESULT cursorHandled =
            SendMessage(pinnedHwnd, WM_SETCURSOR, reinterpret_cast<WPARAM>(pinnedHwnd),
                        MAKELPARAM(HTCAPTION, WM_MOUSEMOVE));
        const HCURSOR appliedCursor = GetCursor();
        SetCursor(previousCursor);
        require(
            cursorHandled == TRUE &&
                pinnedWindow->windowHandle()->cursor().shape() == Qt::OpenHandCursor &&
                appliedCursor != resizeCursor,
            "the pinned caption should replace a stale resize cursor with its open-hand cursor");
    }
    setPinnedWindowHovered(*pinnedWindow, true);
    const QPoint controlsCenter = controlsPanel->geometry().center();
    const QPoint nativeControlsCenter(
        hitTestGeometry.left() +
            qRound(controlsCenter.x() * static_cast<double>(hitTestGeometry.width()) /
                   std::max(1, pinnedWindow->width())),
        hitTestGeometry.top() +
            qRound(controlsCenter.y() * static_cast<double>(hitTestGeometry.height()) /
                   std::max(1, pinnedWindow->height())));
    require(nativeHitTest(nativeControlsCenter) == HTCLIENT,
            "the pinned controls should remain client-interactive");

    struct NativeResizeResult {
        QRect before;
        QRect requested;
        QRect after;
    };
    const auto sendNativeResize = [pinnedWindow, pinnedHwnd,
                                   &canvasPaints](const QPoint& direction, WPARAM sizingEdge,
                                                  bool expand, int magnitude = 24) {
        const QRect before = pinnedWindow->currentNativeGeometry();
        const int distance = expand ? magnitude : -magnitude;
        QRect requested = before;
        if (direction.x() < 0) {
            requested.setLeft(before.left() + direction.x() * distance);
        } else if (direction.x() > 0) {
            requested.setRight(before.right() + direction.x() * distance);
        }
        if (direction.y() < 0) {
            requested.setTop(before.top() + direction.y() * distance);
        } else if (direction.y() > 0) {
            requested.setBottom(before.bottom() + direction.y() * distance);
        }

        RECT proposedNative = nativeRectForQRect(requested);
        const HWND captureBefore = GetCapture();
        require(SendMessage(pinnedHwnd, WM_SIZING, sizingEdge,
                            reinterpret_cast<LPARAM>(&proposedNative)) == TRUE,
                "WM_SIZING should accept an enabled native resize proposal");
        require(GetCapture() == captureBefore,
                "WM_SIZING must not use application-managed mouse capture");

        WINDOWPOS acceptedPosition{};
        acceptedPosition.hwnd = pinnedHwnd;
        acceptedPosition.x = proposedNative.left;
        acceptedPosition.y = proposedNative.top;
        acceptedPosition.cx = proposedNative.right - proposedNative.left;
        acceptedPosition.cy = proposedNative.bottom - proposedNative.top;
        acceptedPosition.flags = SWP_NOZORDER | SWP_NOACTIVATE;
        SendMessage(pinnedHwnd, WM_WINDOWPOSCHANGING, 0,
                    reinterpret_cast<LPARAM>(&acceptedPosition));
        require((acceptedPosition.flags & SWP_NOCOPYBITS) != 0,
                "live resizing a translucent pin must discard stale client pixels");
        const int paintsBeforeResize = canvasPaints.count();
        require(SetWindowPos(pinnedHwnd, nullptr, acceptedPosition.x, acceptedPosition.y,
                             acceptedPosition.cx, acceptedPosition.cy,
                             SWP_NOZORDER | SWP_NOACTIVATE) != FALSE,
                "the accepted native resize should be applied");
        require(canvasPaints.count() > paintsBeforeResize,
                "each live resize step must synchronously publish a complete canvas frame");
        SendMessage(pinnedHwnd, WM_EXITSIZEMOVE, 0, 0);
        return NativeResizeResult{
            before,
            requested,
            qRectForNativeRect(proposedNative),
        };
    };

    const auto fixedCornerForDirection = [](const QRect& geometry, const QPoint& direction) {
        if (direction == QPoint(-1, 0)) {
            return geometry.topRight();
        }
        if (direction == QPoint(1, 0) || direction == QPoint(0, 1)) {
            return geometry.topLeft();
        }
        if (direction == QPoint(0, -1)) {
            return geometry.bottomLeft();
        }
        if (direction == QPoint(-1, -1)) {
            return geometry.bottomRight();
        }
        if (direction == QPoint(1, -1)) {
            return geometry.bottomLeft();
        }
        if (direction == QPoint(-1, 1)) {
            return geometry.topRight();
        }
        return geometry.topLeft();
    };

    struct NativeResizeCase {
        QPoint direction;
        WPARAM sizingEdge;
    };
    const std::vector<NativeResizeCase> resizeCases{
        {QPoint(-1, 0), WMSZ_LEFT},       {QPoint(1, 0), WMSZ_RIGHT},
        {QPoint(0, -1), WMSZ_TOP},        {QPoint(0, 1), WMSZ_BOTTOM},
        {QPoint(-1, -1), WMSZ_TOPLEFT},   {QPoint(1, -1), WMSZ_TOPRIGHT},
        {QPoint(-1, 1), WMSZ_BOTTOMLEFT}, {QPoint(1, 1), WMSZ_BOTTOMRIGHT},
    };
    for (const NativeResizeCase& resizeCase : resizeCases) {
        for (bool expand : {true, false}) {
            scaleMenu->actions().at(3)->trigger();
            waitForUi(20);
            const NativeResizeResult resize =
                sendNativeResize(resizeCase.direction, resizeCase.sizingEdge, expand);
            const QSize orientedBaseline = expectedSize(100, true);
            require(
                qAbs(resize.after.height() -
                     qRound(resize.after.width() * static_cast<double>(orientedBaseline.height()) /
                            orientedBaseline.width())) <= 1,
                "native edge and corner sizing should preserve the oriented aspect ratio");
            require(fixedCornerForDirection(resize.after, resizeCase.direction) ==
                        fixedCornerForDirection(resize.before, resizeCase.direction),
                    "native sizing should preserve the fixed opposite anchor");
            require(resize.requested != resize.after,
                    "native sizing should correct the proposal to the aspect ratio");
        }
    }

    scaleMenu->actions().at(3)->trigger();
    waitForUi(20);
    const QRect maximumStart = pinnedWindow->currentNativeGeometry();
    const NativeResizeResult maximumResize =
        sendNativeResize(QPoint(1, 0), WMSZ_RIGHT, true, std::max(1, maximumStart.width() * 6));
    require(maximumResize.after.size() == expectedSize(500, true),
            "native resizing should clamp at 500 percent");
    scaleMenu->actions().at(3)->trigger();
    waitForUi(20);
    const QRect minimumStart = pinnedWindow->currentNativeGeometry();
    const int belowMinimumWidth = std::max(1, qRound(minimumStart.width() * 0.05));
    const NativeResizeResult minimumResize = sendNativeResize(
        QPoint(1, 0), WMSZ_RIGHT, false, std::max(1, minimumStart.width() - belowMinimumWidth));
    require(minimumResize.after.size() == expectedSize(10, true),
            "native resizing should clamp at 10 percent");

    MINMAXINFO trackingLimits{};
    SendMessage(pinnedHwnd, WM_GETMINMAXINFO, 0, reinterpret_cast<LPARAM>(&trackingLimits));
    require(QSize(trackingLimits.ptMinTrackSize.x, trackingLimits.ptMinTrackSize.y) ==
                    expectedSize(10, true) &&
                QSize(trackingLimits.ptMaxTrackSize.x, trackingLimits.ptMaxTrackSize.y) ==
                    expectedSize(500, true),
            "Windows tracking limits should match the oriented 10-to-500-percent bounds");

    scaleMenu->actions().at(3)->trigger();
    waitForUi(20);
    menu->actions().at(5)->setChecked(true);
    waitForUi(30);
    const QRect editModeGeometry = pinnedWindow->currentNativeGeometry();
    require(nativeHitTest(QPoint(editModeGeometry.right(), editModeGeometry.center().y())) ==
                HTCLIENT,
            "native resize borders should be disabled in drawing mode");
    QRect disabledDrawingProposal = editModeGeometry;
    disabledDrawingProposal.setRight(disabledDrawingProposal.right() + 40);
    RECT disabledDrawingNative = nativeRectForQRect(disabledDrawingProposal);
    SendMessage(pinnedHwnd, WM_SIZING, WMSZ_RIGHT,
                reinterpret_cast<LPARAM>(&disabledDrawingNative));
    require(qRectForNativeRect(disabledDrawingNative) == disabledDrawingProposal,
            "drawing mode should leave WM_SIZING proposals unchanged");
    menu->actions().at(5)->setChecked(false);

    menu->actions().at(10)->setChecked(true);
    waitForUi(200);
    const QRect thumbnailGeometry = pinnedWindow->currentNativeGeometry();
    require(nativeHitTest(QPoint(thumbnailGeometry.right(), thumbnailGeometry.center().y())) ==
                HTCAPTION,
            "thumbnail mode should keep the native border draggable without resizing");
    QRect disabledThumbnailProposal = thumbnailGeometry;
    disabledThumbnailProposal.setBottom(disabledThumbnailProposal.bottom() + 24);
    RECT disabledThumbnailNative = nativeRectForQRect(disabledThumbnailProposal);
    SendMessage(pinnedHwnd, WM_SIZING, WMSZ_BOTTOM,
                reinterpret_cast<LPARAM>(&disabledThumbnailNative));
    require(qRectForNativeRect(disabledThumbnailNative) == disabledThumbnailProposal,
            "thumbnail mode should leave WM_SIZING proposals unchanged");
#endif
    sendWheel(canvas->rect().center(), QPoint(), QPoint(0, 120));
    require(!menu->actions().at(10)->isChecked() &&
                pinnedWindow->currentNativeGeometry().size() == expectedSize(110, true),
            "thumbnail wheel input should restore the pin and apply cursor scaling");

    recognition.complete({nullptr, QStringLiteral("deterministic failure")});
    menu->actions().at(3)->setChecked(true);
    auto* recognitionSession = pinnedWindow->findChild<ScreenshotRecognitionSessionController*>();
    auto* editController = pinnedWindow->findChild<ScreenshotPinnedEditController*>();
    ScreenshotToolPalette* recognitionToolbar =
        editController != nullptr && editController->toolbarWindow() != nullptr
            ? editController->toolbarWindow()->palette()
            : nullptr;
    require(recognitionSession != nullptr && recognitionToolbar != nullptr,
            "recognition controls should exist after activating text recognition");

    const auto requireRecognitionDisablesZoom =
        [&](ScreenshotRecognitionSessionController::Mode mode, const char* modeName) {
            require(recognitionSession->active() && recognitionSession->mode() == mode, modeName);
            require(!scaleMenu->menuAction()->isEnabled(),
                    "recognition should disable the scale menu");
#if defined(Q_OS_WIN) || defined(_WIN32)
            const QRect recognitionGeometry = pinnedWindow->currentNativeGeometry();
            require(nativeHitTest(QPoint(recognitionGeometry.right(),
                                         recognitionGeometry.center().y())) == HTCLIENT &&
                        nativeHitTest(recognitionGeometry.center()) == HTCLIENT,
                    "recognition should disable native resize zoom and keep the interior "
                    "interactive");
#endif
            const QRect beforeWheel = pinnedWindow->currentNativeGeometry();
            sendWheel(canvas->rect().center(), QPoint(), QPoint(0, 120));
            require(pinnedWindow->currentNativeGeometry() == beforeWheel,
                    "recognition should ignore wheel zoom");
            scaleMenu->actions().at(1)->trigger();
            require(pinnedWindow->currentNativeGeometry() == beforeWheel,
                    "recognition should ignore scale preset activation");
        };

    requireRecognitionDisablesZoom(ScreenshotRecognitionSessionController::Mode::Text,
                                   "text recognition should be active");
    recognitionToolbar->tableRequested();
    requireRecognitionDisablesZoom(ScreenshotRecognitionSessionController::Mode::Table,
                                   "table recognition should be active");
    recognitionToolbar->qrRequested();
    requireRecognitionDisablesZoom(ScreenshotRecognitionSessionController::Mode::Qr,
                                   "QR recognition should be active");

    menu->actions().constLast()->trigger();
    require(processUntilDeleted(guardedWindow, 2000),
            "pinned window was not deleted after scaling and resizing tests");
}

void pinnedSettledWheelScalingAdvancesPastRoundedLevel(SnowCanvasRuntime&) {
    QScreen* screen = QGuiApplication::primaryScreen();
    require(screen != nullptr, "a primary screen is required");

    QImage background(199, 101, QImage::Format_ARGB32_Premultiplied);
    background.fill(QColor(46, 97, 149));
    const QRect physicalScreen = ScreenshotGeometryMapper::physicalRectForScreen(*screen);
    const QSize baseline(993, 497);

    auto* pinnedWindow = new ScreenshotPinnedWindow(ScreenshotPinnedWindow::RuntimeMode::NoDocument);
    QPointer<ScreenshotPinnedWindow> guardedWindow(pinnedWindow);
    ScreenshotPinnedWindow::Config config;
    config.nativeGeometry = QRect(physicalScreen.topLeft() + QPoint(160, 140), baseline);
    config.canvasSourceRect = QRectF(QPointF(), QSizeF(background.size()));
    config.backgroundImage = background;
    config.screen = screen;
    config.enableEditing = false;
    require(pinnedWindow->present(config), "settled wheel scaling test pin presentation failed");
    waitForUi(50);

    auto* canvas = pinnedWindow->findChild<SnowCanvasWidget*>();
    auto* scaleLabel =
        pinnedWindow->findChild<QLabel*>(QStringLiteral("screenshotPinnedScaleLabel"));
    require(canvas != nullptr && scaleLabel != nullptr,
            "settled wheel scaling test controls were not found");

    const auto sendNotch = [canvas](int angleDelta) {
        const QPoint position = canvas->rect().center();
        QWheelEvent wheel(QPointF(position), QPointF(canvas->mapToGlobal(position)), QPoint(),
                          QPoint(0, angleDelta), Qt::NoButton, Qt::NoModifier, Qt::NoScrollPhase,
                          false);
        QCoreApplication::sendEvent(canvas, &wheel);
        require(wheel.isAccepted(), "settled wheel notch should be consumed");
        waitForUi(10);
    };
    const auto expectedSize = [&baseline](int percent) {
        return QSize(qRound(baseline.width() * percent / 100.0),
                     qRound(baseline.height() * percent / 100.0));
    };

    sendNotch(120);
    sendNotch(120);
    const QSize settledSize = pinnedWindow->currentNativeGeometry().size();
    const QSize expectedSettledSize = expectedSize(120);
    require(qAbs(settledSize.width() - expectedSettledSize.width()) <= 1 &&
                qAbs(settledSize.height() - expectedSettledSize.height()) <= 1 &&
                scaleLabel->text() == QStringLiteral("Scale: 120%"),
            "separate settled wheel notches should advance beyond the first rounded level");

    pinnedWindow->close();
    require(processUntilDeleted(guardedWindow, 2000),
            "settled wheel scaling test pin was not deleted");
}

void pinnedWheelScalingUsesConfiguredAnchor(SnowCanvasRuntime&) {
    QScreen* screen = QGuiApplication::primaryScreen();
    require(screen != nullptr, "a primary screen is required");

    QImage background(320, 180, QImage::Format_ARGB32_Premultiplied);
    background.fill(QColor(46, 97, 149));

    auto* pinnedWindow = new ScreenshotPinnedWindow(ScreenshotPinnedWindow::RuntimeMode::NoDocument);
    QPointer<ScreenshotPinnedWindow> guardedWindow(pinnedWindow);
    ScreenshotPinnedWindow::Config config;
    config.nativeGeometry = physicalPinGeometry(*screen, QPoint(120, 100), background.size());
    config.canvasSourceRect = QRectF(QPointF(), QSizeF(background.size()));
    config.backgroundImage = background;
    config.screen = screen;
    config.enableEditing = false;
    config.mouseWheelZoomMode = QStringLiteral("top_left");
    require(pinnedWindow->present(config), "configured wheel anchor pin presentation failed");
    waitForUi(50);

    auto* canvas = pinnedWindow->findChild<SnowCanvasWidget*>();
    require(canvas != nullptr, "configured wheel anchor canvas was not found");
    const QRect before = pinnedWindow->currentNativeGeometry();
    const QPoint position = canvas->rect().bottomRight() - QPoint(8, 8);
    QWheelEvent wheel(QPointF(position), QPointF(canvas->mapToGlobal(position)), QPoint(),
                      QPoint(0, 120), Qt::NoButton, Qt::NoModifier, Qt::NoScrollPhase, false);
    QCoreApplication::sendEvent(canvas, &wheel);
    waitForUi(20);

    const QRect after = pinnedWindow->currentNativeGeometry();
    require(wheel.isAccepted() && after.topLeft() == before.topLeft() &&
                after.size() == QSize(qRound(before.width() * 1.1), qRound(before.height() * 1.1)),
            "configured top-left wheel scaling should preserve the native top-left anchor");

    pinnedWindow->close();
    require(processUntilDeleted(guardedWindow, 2000),
            "configured wheel anchor pin was not deleted");
}

void pinnedFollowsPerMonitorDpiScaling(SnowCanvasRuntime&) {
#if defined(Q_OS_WIN) || defined(_WIN32)
    QScreen* sourceScreen = nullptr;
    QScreen* destinationScreen = nullptr;
    const QList<QScreen*> screens = QGuiApplication::screens();
    for (QScreen* candidateSource : screens) {
        for (QScreen* candidateDestination : screens) {
            if (candidateSource != nullptr && candidateDestination != nullptr &&
                candidateSource != candidateDestination &&
                qAbs(candidateSource->devicePixelRatio() -
                     candidateDestination->devicePixelRatio()) > 0.01) {
                sourceScreen = candidateSource;
                destinationScreen = candidateDestination;
                break;
            }
        }
        if (sourceScreen != nullptr) {
            break;
        }
    }
    if (sourceScreen == nullptr || destinationScreen == nullptr) {
        return;
    }

    const QSize logicalSize(300, 150);
    const qreal sourceDpr = sourceScreen->devicePixelRatio();
    const qreal destinationDpr = destinationScreen->devicePixelRatio();
    const QRect sourcePhysical = ScreenshotGeometryMapper::physicalRectForScreen(*sourceScreen);
    const QRect destinationPhysical =
        ScreenshotGeometryMapper::physicalRectForScreen(*destinationScreen);
    QImage background(logicalSize, QImage::Format_ARGB32_Premultiplied);
    background.fill(QColor(54, 105, 157));

    auto* pinnedWindow = new ScreenshotPinnedWindow(ScreenshotPinnedWindow::RuntimeMode::NoDocument);
    QPointer<ScreenshotPinnedWindow> guardedWindow(pinnedWindow);
    ScreenshotPinnedWindow::Config config;
    config.nativeGeometry = QRect(
        sourcePhysical.topLeft() + QPoint(qRound(60 * sourceDpr), qRound(60 * sourceDpr)),
        QSize(qRound(logicalSize.width() * sourceDpr), qRound(logicalSize.height() * sourceDpr)));
    config.canvasSourceRect = QRectF(QPointF(), QSizeF(background.size()));
    config.backgroundImage = background;
    const QSize initialPhysicalSize = config.nativeGeometry.size();
    config.screen = sourceScreen;
    config.enableEditing = false;
    require(pinnedWindow->present(config), "multi-monitor DPI pin presentation failed");
    waitForUi(100);
    auto* scaleLabel =
        pinnedWindow->findChild<QLabel*>(QStringLiteral("screenshotPinnedScaleLabel"));
    require(scaleLabel != nullptr && scaleLabel->isHidden(),
            "initial multi-monitor placement should not show a scale readout");

    const auto moveToPhysicalScreen = [pinnedWindow](const QRect& physicalScreen) {
        const QRect current = pinnedWindow->currentNativeGeometry();
        const QPoint target =
            physicalScreen.center() - QPoint(current.width() / 2, current.height() / 2);
        const HWND hwnd = toNativeHwnd(pinnedWindow->winId());
        RECT movingProposal = nativeRectForQRect(QRect(target, current.size()));
        SendMessage(hwnd, WM_ENTERSIZEMOVE, 0, 0);
        require(SendMessage(hwnd, WM_MOVING, 0, reinterpret_cast<LPARAM>(&movingProposal)) == TRUE,
                "the cross-screen native move proposal was not accepted");
        const QRect acceptedMove = qRectForNativeRect(movingProposal);
        SetWindowPos(hwnd, nullptr, acceptedMove.x(), acceptedMove.y(), 0, 0,
                     SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
        SendMessage(hwnd, WM_EXITSIZEMOVE, 0, 0);
        waitForUi(300);
    };
    moveToPhysicalScreen(destinationPhysical);
    const QSize destinationSize = pinnedWindow->currentNativeGeometry().size();
    const QSize expectedDestinationSize(
        qRound(initialPhysicalSize.width() * destinationDpr / sourceDpr),
        qRound(initialPhysicalSize.height() * destinationDpr / sourceDpr));
    require(qAbs(destinationSize.width() - expectedDestinationSize.width()) <= 3 &&
                qAbs(destinationSize.height() - expectedDestinationSize.height()) <= 3 &&
                scaleLabel->isVisible() &&
                scaleLabel->text() ==
                    QStringLiteral("Scale: %1%").arg(qRound(100.0 * destinationDpr / sourceDpr)),
            "a differing-DPI monitor transition should adopt Qt's native resize");

    moveToPhysicalScreen(sourcePhysical);
    const QSize returnedSize = pinnedWindow->currentNativeGeometry().size();
    require(qAbs(returnedSize.width() - initialPhysicalSize.width()) <= 3 &&
                qAbs(returnedSize.height() - initialPhysicalSize.height()) <= 3,
            "returning across the DPI boundary should restore scale without drift");
    pinnedWindow->close();
    require(processUntilDeleted(guardedWindow, 2000), "multi-monitor DPI test pin was not deleted");
#endif
}

void pinnedDrawingToolbarMatchesCaptureInteractions(SnowCanvasRuntime&) {
    QScreen* screen = QGuiApplication::primaryScreen();
    require(screen != nullptr, "a primary screen is required");

    FakeOcrRecognition recognition;
    FakeQrRecognition qrRecognition;
    auto* pinnedWindow = new ScreenshotPinnedWindow(ScreenshotPinnedWindow::RuntimeMode::NoDocument);
    QPointer<ScreenshotPinnedWindow> guardedWindow(pinnedWindow);
    QImage background(320, 180, QImage::Format_ARGB32_Premultiplied);
    background.fill(QColor(42, 84, 126));

    ScreenshotPinnedWindow::Config config;
    config.nativeGeometry = physicalPinGeometry(*screen, QPoint(40, 40), QSize(400, 400));
    config.canvasSourceRect = QRectF(QPointF(0.0, 0.0), QSizeF(background.size()));
    config.backgroundImage = background;
    config.screen = screen;
    config.enableEditing = true;
    config.automaticTextRecognition = false;
    config.recognition = &recognition;
    config.qrRecognition = &qrRecognition;
    require(pinnedWindow->present(config), "pinned window presentation failed");
    QCoreApplication::processEvents(QEventLoop::AllEvents, 50);
    QPushButton* editButton = buttonNamed(*pinnedWindow, QStringLiteral("Enable drawing mode"));
    require(editButton != nullptr, "edit button was not found");
    auto* controlsPanel =
        pinnedWindow->findChild<QFrame*>(QStringLiteral("screenshotPinnedControlsPanel"));
    setPinnedWindowHovered(*pinnedWindow, true);
    require(controlsPanel != nullptr && controlsPanel->isVisible(),
            "pinned controls should be visible before editing");
    editButton->click();
    QCoreApplication::processEvents(QEventLoop::AllEvents, 50);
    require(controlsPanel->isHidden(), "pinned controls should be hidden while editing");

    SnowCanvasWidget* canvas = pinnedWindow->findChild<SnowCanvasWidget*>();
    require(canvas != nullptr, "pinned screenshot canvas was not found");

    auto* controller = pinnedWindow->findChild<ScreenshotPinnedEditController*>();
    ScreenshotToolPalette* toolbar = controller != nullptr && controller->toolbarWindow() != nullptr
                                         ? controller->toolbarWindow()->palette()
                                         : nullptr;
    require(toolbar != nullptr, "pinned drawing toolbar was not found");

    auto* translationButton = toolbar->findChild<adqt::widgets::AdButton*>(
        QStringLiteral("screenshotTextTranslationButton"));
    require(translationButton != nullptr &&
                translationButton->toolTip().contains(QStringLiteral("Text Translation")) &&
                translationButton->toolTip().contains(QStringLiteral("Ctrl+T")),
            "pinned drawing toolbar should expose Text Translation with its configured shortcut");

    const snow_shot::storage::ScreenshotShortcutSettings screenshotShortcuts;
    const QStringList originalTextRecognitionShortcuts =
        screenshotShortcuts.shortcuts(QStringLiteral("text_recognition"));
    require(screenshotShortcuts.setShortcuts(QStringLiteral("text_recognition"),
                                             {QStringLiteral("Alt+D")}),
            "the Text Recognition shortcut fixture should accept a runtime mapping");
    QCoreApplication::processEvents(QEventLoop::AllEvents, 50);

    sendShortcut(*canvas, Qt::Key_X, Qt::ControlModifier);
    require(toolbar->activeToolForTests() == ScreenshotToolPalette::Tool::Table,
            "the Table Recognition shortcut should activate from the pinned toolbar");
    sendShortcut(*canvas, Qt::Key_Q, Qt::ControlModifier);
    require(toolbar->activeToolForTests() == ScreenshotToolPalette::Tool::Qr,
            "the QR Code Recognition shortcut should activate from the pinned toolbar");
    sendShortcut(*canvas, Qt::Key_D, Qt::AltModifier);
    require(toolbar->activeToolForTests() == ScreenshotToolPalette::Tool::Ocr &&
                recognition.pending.token != 0,
            "the configured Text Recognition shortcut should activate from the pinned toolbar");
    require(screenshotShortcuts.setShortcuts(QStringLiteral("text_recognition"),
                                             originalTextRecognitionShortcuts),
            "the Text Recognition shortcut fixture should restore its original mapping");
    sendShortcut(*canvas, Qt::Key_T, Qt::ControlModifier);
    require(toolbar->activeToolForTests() == ScreenshotToolPalette::Tool::TextTranslation &&
                translationButton->busy(),
            "the Text Translation shortcut should activate its toolbar control while recognizing");

    auto translatedPresentation = std::make_shared<ScreenshotOcrPresentation>();
    translatedPresentation->selection = config.canvasSourceRect.toAlignedRect();
    ScreenshotOcrLine translatedLine;
    translatedLine.text = QStringLiteral("Text to translate");
    translatedLine.quad = QPolygonF(config.canvasSourceRect);
    translatedPresentation->lines.push_back(std::move(translatedLine));
    recognition.complete({std::move(translatedPresentation), {}});
    QCoreApplication::processEvents(QEventLoop::AllEvents, 50);
    require(toolbar->activeToolForTests() == ScreenshotToolPalette::Tool::TextTranslation &&
                translationButton->buttonStyle() == adqt::widgets::AdButton::ButtonStyle::Solid &&
                !translationButton->busy(),
            "Text Translation should stay active and stop loading after recognition completes");
    sendShortcut(*canvas, Qt::Key_1);
    require(canvas->canvasContentVisible() && canvas->interactionEnabled(),
            "a drawing shortcut should leave pinned text translation mode");

    const QPoint localPosition = canvas->rect().center();
    const auto sendWheel = [canvas, localPosition](int angleDelta) {
        QWheelEvent wheel(QPointF(localPosition), QPointF(canvas->mapToGlobal(localPosition)),
                          QPoint(), QPoint(0, angleDelta), Qt::NoButton, Qt::NoModifier,
                          Qt::NoScrollPhase, false);
        QCoreApplication::sendEvent(canvas, &wheel);
        return wheel.isAccepted();
    };

    require(canvas->setCanvasTool(SnowCanvasTool::Shape), "Shape tool could not be activated");
    QCoreApplication::processEvents(QEventLoop::AllEvents, 50);
    const double shapeStrokeWidth = canvas->canvasStyleToolbarState().shapeStyle.strokeWidth;
    require(sendWheel(120) &&
                canvas->canvasStyleToolbarState().shapeStyle.strokeWidth == shapeStrokeWidth + 1.0,
            "Shape wheel input should increase pinned stroke width by one pixel");

    require(canvas->setCanvasTool(SnowCanvasTool::Text), "Text tool could not be activated");
    QCoreApplication::processEvents(QEventLoop::AllEvents, 50);
    const double textFontSize = canvas->canvasStyleToolbarState().textStyle.fontSize;
    require(sendWheel(120) && canvas->canvasStyleToolbarState().textStyle.fontSize > textFontSize,
            "Text wheel input should increase pinned font size");

    require(canvas->setCanvasTool(SnowCanvasTool::Shape), "Shape tool could not be restored");
    QCoreApplication::processEvents(QEventLoop::AllEvents, 50);
    adqt::widgets::AdColorPicker* strokeColorPicker = nullptr;
    for (adqt::widgets::AdColorPicker* picker :
         toolbar->findChildren<adqt::widgets::AdColorPicker*>()) {
        if (picker != nullptr && picker->accessibleName() == QStringLiteral("Stroke color")) {
            strokeColorPicker = picker;
            break;
        }
    }
    auto* sampler = strokeColorPicker != nullptr
                        ? qobject_cast<QAbstractButton*>(strokeColorPicker->previewContent())
                        : nullptr;
    require(strokeColorPicker != nullptr && sampler != nullptr,
            "pinned stroke color picker should expose canvas sampling");
    QImage physicalCanvasRaster = canvas->grab().toImage();
    physicalCanvasRaster.setDevicePixelRatio(1.0);
    const QRect canvasPhysicalBounds = pinnedWindow->currentNativeGeometry();
    const auto localPositionForPhysical = [canvas,
                                           canvasPhysicalBounds](const QPoint& physicalPosition) {
        return QPointF((physicalPosition.x() - canvasPhysicalBounds.left()) *
                           static_cast<qreal>(canvas->width()) / canvasPhysicalBounds.width(),
                       (physicalPosition.y() - canvasPhysicalBounds.top()) *
                           static_cast<qreal>(canvas->height()) / canvasPhysicalBounds.height());
    };
    const auto expectedColorAtPhysical = [&physicalCanvasRaster,
                                          canvasPhysicalBounds](const QPoint& physicalPosition) {
        const QImage preview = ScreenshotCanvasColorSampler::previewFromPhysicalRaster(
            physicalCanvasRaster, canvasPhysicalBounds, physicalPosition);
        return preview.pixelColor(preview.width() / 2, preview.height() / 2).toRgb();
    };
    QPoint firstPhysicalPosition;
    QColor firstExpectedColor;
    QColor secondExpectedColor;
    const int searchTop = canvasPhysicalBounds.top() + canvasPhysicalBounds.height() / 4;
    const int searchBottom = canvasPhysicalBounds.top() + canvasPhysicalBounds.height() * 3 / 4;
    for (int y = searchTop; y < searchBottom && !firstExpectedColor.isValid(); ++y) {
        for (int x = canvasPhysicalBounds.left(); x < canvasPhysicalBounds.right(); ++x) {
            const QPoint candidate(x, y);
            const QColor candidateColor = expectedColorAtPhysical(candidate);
            const QColor adjacentColor = expectedColorAtPhysical(candidate + QPoint(1, 0));
            if (candidateColor != adjacentColor) {
                firstPhysicalPosition = candidate;
                firstExpectedColor = candidateColor;
                secondExpectedColor = adjacentColor;
                break;
            }
        }
    }
    const QPoint secondPhysicalPosition = firstPhysicalPosition + QPoint(1, 0);
    require(firstExpectedColor.isValid() && secondExpectedColor.isValid() &&
                firstExpectedColor != secondExpectedColor,
            "the pinned sampling fixture must distinguish adjacent physical pixels");
    sampler->click();
    QWidget colorPickerToolWindow;

    {
        const CursorPositionRestorer restoreCursor;
        const QPoint start = config.nativeGeometry.center();
        setSystemCursorPosition(start);
        sendShortcut(colorPickerToolWindow, Qt::Key_W);
#if defined(Q_OS_WIN) || defined(_WIN32)
        require(systemCursorPosition() == start + QPoint(0, -1),
                "pinned canvas color sampling must route keyboard cursor movement from a "
                "transient color picker window");
        sendShortcut(colorPickerToolWindow, Qt::Key_W, Qt::NoModifier, true);
        require(systemCursorPosition() == start + QPoint(0, -2),
                "an auto-repeated pinned cursor shortcut must move by one more physical pixel");
#else
        require(systemCursorPosition() == start,
                "pinned cursor movement must remain disabled without a native physical backend");
#endif
    }

    const QPointF firstLocalPosition = localPositionForPhysical(firstPhysicalPosition);
    const QPointF firstGlobalPosition(canvas->mapToGlobal(firstLocalPosition.toPoint()));
    QMouseEvent samplingMove(QEvent::MouseMove, firstLocalPosition, firstGlobalPosition,
                             Qt::NoButton, Qt::NoButton, Qt::NoModifier);
    QCoreApplication::sendEvent(canvas, &samplingMove);
    QMouseEvent samplingPress(QEvent::MouseButtonPress, firstLocalPosition, firstGlobalPosition,
                              Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
    QCoreApplication::sendEvent(canvas, &samplingPress);
    require(samplingMove.isAccepted() && samplingPress.isAccepted() &&
                strokeColorPicker->value().isSolid() &&
                strokeColorPicker->value().solidColor.toRgb() == firstExpectedColor,
            "pinned canvas sampling should commit the exact physical color beneath the cursor");

    sampler->click();
    const QPointF secondLocalPosition = localPositionForPhysical(secondPhysicalPosition);
    const QPointF secondGlobalPosition(canvas->mapToGlobal(secondLocalPosition.toPoint()));
    QMouseEvent adjacentSamplingMove(QEvent::MouseMove, secondLocalPosition, secondGlobalPosition,
                                     Qt::NoButton, Qt::NoButton, Qt::NoModifier);
    QCoreApplication::sendEvent(canvas, &adjacentSamplingMove);
    QMouseEvent adjacentSamplingPress(QEvent::MouseButtonPress, secondLocalPosition,
                                      secondGlobalPosition, Qt::LeftButton, Qt::LeftButton,
                                      Qt::NoModifier);
    QCoreApplication::sendEvent(canvas, &adjacentSamplingPress);
    require(adjacentSamplingMove.isAccepted() && adjacentSamplingPress.isAccepted() &&
                strokeColorPicker->value().isSolid() &&
                strokeColorPicker->value().solidColor.toRgb() == secondExpectedColor &&
                strokeColorPicker->value().solidColor.toRgb() != firstExpectedColor,
            "adjacent pinned physical pixels must commit distinct rendered colors");

    {
        const CursorPositionRestorer restoreCursor;
        const QPoint start = config.nativeGeometry.center();
        setSystemCursorPosition(start);
        sendShortcut(colorPickerToolWindow, Qt::Key_W);
        require(systemCursorPosition() == start,
                "the transient color picker window must lose cursor shortcuts after sampling");
    }

    require(canvas->setCanvasTool(SnowCanvasTool::Spotlight),
            "Spotlight tool could not be activated");
    QCoreApplication::processEvents(QEventLoop::AllEvents, 50);

    const QRect geometryBeforeWheel = pinnedWindow->currentNativeGeometry();
    require(sendWheel(120) && qFuzzyCompare(canvas->canvasSpotlightConfig().opacity + 1.0, 1.69) &&
                pinnedWindow->currentNativeGeometry() == geometryBeforeWheel,
            "Spotlight wheel input should increase pinned mask opacity by five percent");

    QPushButton* closeButton = buttonNamed(*pinnedWindow, QStringLiteral("Close"));
    require(closeButton != nullptr, "close button was not found");
    closeButton->click();
    require(processUntilDeleted(guardedWindow, 2000),
            "pinned window was not deleted after the Spotlight wheel test");
}

void pinnedEditToolbarControlsCanvasHistory(SnowCanvasRuntime&) {
    QScreen* screen = QGuiApplication::primaryScreen();
    require(screen != nullptr, "a primary screen is required");

    auto* pinnedWindow = new ScreenshotPinnedWindow(ScreenshotPinnedWindow::RuntimeMode::NoDocument);
    QPointer<ScreenshotPinnedWindow> guardedWindow(pinnedWindow);
    QImage background(320, 180, QImage::Format_ARGB32_Premultiplied);
    background.fill(QColor(42, 84, 126));

    ScreenshotPinnedWindow::Config config;
    config.nativeGeometry = physicalPinGeometry(*screen, QPoint(40, 40), QSize(400, 400));
    config.canvasSourceRect = QRectF(QPointF(0.0, 0.0), QSizeF(background.size()));
    config.backgroundImage = background;
    config.screen = screen;
    config.enableEditing = true;
    require(pinnedWindow->present(config), "pinned window presentation failed");
    QCoreApplication::processEvents(QEventLoop::AllEvents, 50);
    QPushButton* editButton = buttonNamed(*pinnedWindow, QStringLiteral("Enable drawing mode"));
    require(editButton != nullptr, "edit button was not found");
    auto* controlsPanel =
        pinnedWindow->findChild<QFrame*>(QStringLiteral("screenshotPinnedControlsPanel"));
    require(controlsPanel != nullptr, "pinned controls panel was not found");
    setPinnedWindowHovered(*pinnedWindow, true);
    editButton->click();
    QCoreApplication::processEvents(QEventLoop::AllEvents, 50);
    require(controlsPanel->isHidden(), "pinned controls should be hidden while editing");

    auto* controller = pinnedWindow->findChild<ScreenshotPinnedEditController*>();
    require(controller != nullptr, "pinned edit controller was not found");
    ScreenshotFloatingToolPaletteWindow* toolbarWindow = controller->toolbarWindow();
    require(toolbarWindow != nullptr && toolbarWindow->isVisible(),
            "pinned edit toolbar should be visible in edit mode");
    require(toolbarWindow->testAttribute(Qt::WA_AlwaysShowToolTips),
            "pinned edit toolbar should show tooltips while its tool window is inactive");
    ScreenshotToolPalette* toolbar = toolbarWindow->palette();
    require(toolbar != nullptr, "pinned edit palette was not found");

    const QPoint manualToolbarPosition = toolbarWindow->contentPosition() + QPoint(24, 16);
    toolbarWindow->moveContentTo(manualToolbarPosition);
    toolbarWindow->dragFinished();
    QCoreApplication::processEvents(QEventLoop::AllEvents, 50);
    const QPoint toolbarPositionBeforeRotation = toolbarWindow->contentPosition();
    const QPoint pinnedPositionBeforeRotation = pinnedWindow->pos();

    auto* contextMenu = pinnedWindow->findChild<adqt::widgets::AdContextMenu*>(
        QStringLiteral("screenshotPinnedContextMenu"));
    require(contextMenu != nullptr, "pinned context menu was not found");
    auto* processMenu =
        qobject_cast<adqt::widgets::AdContextMenu*>(contextMenu->actions().at(6)->menu());
    require(processMenu != nullptr, "pinned process-image menu was not found");
    processMenu->actions().at(0)->trigger();
    QCoreApplication::processEvents(QEventLoop::AllEvents, 50);
    require(toolbarWindow->contentPosition() ==
                toolbarPositionBeforeRotation + pinnedWindow->pos() - pinnedPositionBeforeRotation,
            "a manually placed toolbar should follow the pin's rotation-time move");

    auto* undoButton =
        toolbar->findChild<adqt::widgets::AdButton*>(QStringLiteral("screenshotUndoButton"));
    auto* redoButton =
        toolbar->findChild<adqt::widgets::AdButton*>(QStringLiteral("screenshotRedoButton"));
    require(undoButton != nullptr && redoButton != nullptr,
            "pinned edit toolbar should expose undo and redo buttons");
    require(!undoButton->isEnabled() && !redoButton->isEnabled(),
            "pinned history buttons should start disabled");

    SnowCanvasWidget* canvas = pinnedWindow->findChild<SnowCanvasWidget*>();
    require(canvas != nullptr, "pinned screenshot canvas was not found");
    QKeyEvent brushShortcut(QEvent::KeyPress, Qt::Key_P, Qt::NoModifier);
    QCoreApplication::sendEvent(canvas, &brushShortcut);
    require(canvas->canvasTool() == SnowCanvasTool::FreeDraw,
            "the configured Brush shortcut should activate in pinned drawing mode");
    QKeyEvent shapeShortcut(QEvent::KeyPress, Qt::Key_1, Qt::NoModifier);
    QCoreApplication::sendEvent(canvas, &shapeShortcut);
    require(canvas->canvasTool() == SnowCanvasTool::Shape,
            "the configured Shape shortcut should activate in pinned drawing mode");
    const QPoint canvasHitPosition(60, 60);
    require(QApplication::widgetAt(canvas->mapToGlobal(canvasHitPosition)) == canvas,
            "drawing mode should expose the canvas to native pointer hit testing");
    const SnowCanvasWatermarkConfig initialConfig = canvas->canvasWatermarkConfig();
    SnowCanvasWatermarkConfig editedConfig = initialConfig;
    editedConfig.text = QStringLiteral("PINNED HISTORY TEST");
    require(canvas->setCanvasWatermarkConfig(editedConfig),
            "pinned canvas edit should commit to history");
    QCoreApplication::processEvents(QEventLoop::AllEvents, 50);
    require(undoButton->isEnabled() && !redoButton->isEnabled(),
            "a pinned canvas edit should enable only undo");

    undoButton->click();
    QCoreApplication::processEvents(QEventLoop::AllEvents, 50);
    require(canvas->canvasWatermarkConfig().text == initialConfig.text,
            "pinned undo button should restore the previous canvas state");
    require(!undoButton->isEnabled() && redoButton->isEnabled(),
            "undoing the pinned edit should enable only redo");

    redoButton->click();
    QCoreApplication::processEvents(QEventLoop::AllEvents, 50);
    require(canvas->canvasWatermarkConfig().text == editedConfig.text,
            "pinned redo button should restore the edited canvas state");
    require(undoButton->isEnabled() && !redoButton->isEnabled(),
            "redoing the pinned edit should enable only undo");

    const auto sendCanvasPointerEvent = [canvas](QEvent::Type type, const QPointF& position,
                                                 Qt::MouseButton button, Qt::MouseButtons buttons) {
        QMouseEvent event(type, position, canvas->mapToGlobal(position.toPoint()), button, buttons,
                          Qt::NoModifier);
        QCoreApplication::sendEvent(canvas, &event);
    };
    require(canvas->setCanvasTool(SnowCanvasTool::Shape),
            "pinned canvas should activate the shape tool");
    sendCanvasPointerEvent(QEvent::MouseButtonPress, QPointF(60.0, 60.0), Qt::LeftButton,
                           Qt::LeftButton);
    sendCanvasPointerEvent(QEvent::MouseMove, QPointF(150.0, 120.0), Qt::NoButton, Qt::LeftButton);
    sendCanvasPointerEvent(QEvent::MouseButtonRelease, QPointF(150.0, 120.0), Qt::LeftButton,
                           Qt::NoButton);
    QCoreApplication::processEvents(QEventLoop::AllEvents, 50);
    undoButton->click();
    QCoreApplication::processEvents(QEventLoop::AllEvents, 50);
    require(canvas->canvasWatermarkConfig().text == editedConfig.text,
            "a pointer-drawn shape should be the latest pinned canvas history entry");
    redoButton->click();
    QCoreApplication::processEvents(QEventLoop::AllEvents, 50);
    require(canvas->setCanvasTool(SnowCanvasTool::Select),
            "pinned canvas should activate the select tool");
    sendCanvasPointerEvent(QEvent::MouseButtonPress, QPointF(60.0, 90.0), Qt::LeftButton,
                           Qt::LeftButton);
    sendCanvasPointerEvent(QEvent::MouseButtonRelease, QPointF(60.0, 90.0), Qt::LeftButton,
                           Qt::NoButton);
    QCoreApplication::processEvents(QEventLoop::AllEvents, 50);
    require(canvas->canvasStyleToolbarState().source ==
                SnowCanvasStyleToolbarSource::SelectedRectangle,
            "the pinned shape should be selected before confirmation");

    QPushButton* confirmButton = buttonNamed(*toolbar, QStringLiteral("Confirm edit"));
    require(confirmButton != nullptr, "pinned edit confirm button was not found");
    confirmButton->click();
    QCoreApplication::processEvents(QEventLoop::AllEvents, 50);
    require(controlsPanel->isVisible(), "pinned controls should return after confirming the edit");
    require(toolbarWindow->isHidden(), "pinned edit toolbar should hide after confirming the edit");
    require(canvas->canvasTool() == SnowCanvasTool::Select,
            "confirming a pinned edit should restore the select tool");
    require(canvas->canvasStyleToolbarState().source ==
                SnowCanvasStyleToolbarSource::DefaultRectangle,
            "confirming a pinned edit should clear the canvas selection");

    const QPoint nativeMoveDelta(24, 18);
    const QRect nativeGeometryBeforeMove = pinnedWindow->currentNativeGeometry();
    const QPoint pinnedPositionBeforeMove = pinnedWindow->pos();
    const QPoint toolbarPositionBeforeMove = toolbarWindow->contentPosition();
#if defined(Q_OS_WIN) || defined(_WIN32)
    const HWND moveHwnd = toNativeHwnd(pinnedWindow->winId());
    RECT movingProposal = nativeRectForQRect(nativeGeometryBeforeMove.translated(nativeMoveDelta));
    SendMessage(moveHwnd, WM_ENTERSIZEMOVE, 0, 0);
    require(SendMessage(moveHwnd, WM_MOVING, 0, reinterpret_cast<LPARAM>(&movingProposal)) == TRUE,
            "the edit-toolbar native move proposal was not accepted");
    const QRect acceptedMove = qRectForNativeRect(movingProposal);
    SetWindowPos(moveHwnd, nullptr, acceptedMove.x(), acceptedMove.y(), 0, 0,
                 SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
    SendMessage(moveHwnd, WM_EXITSIZEMOVE, 0, 0);
#else
    pinnedWindow->move(pinnedPositionBeforeMove + nativeMoveDelta);
#endif
    QCoreApplication::processEvents(QEventLoop::AllEvents, 50);
    const QPoint logicalMoveDelta = pinnedWindow->pos() - pinnedPositionBeforeMove;
    require(pinnedWindow->currentNativeGeometry().topLeft() ==
                    nativeGeometryBeforeMove.topLeft() + nativeMoveDelta &&
                toolbarWindow->contentPosition() == toolbarPositionBeforeMove + logicalMoveDelta,
            "a manually placed toolbar should follow every committed native pin move");

    QPushButton* closeButton = buttonNamed(*pinnedWindow, QStringLiteral("Close"));
    require(closeButton != nullptr, "close button was not found");
    closeButton->click();
    require(processUntilDeleted(guardedWindow, 2000),
            "pinned window was not deleted after the history test");
}
} // namespace

int main(int argc, char* argv[]) {
    QApplication app(argc, argv);
    QApplication::setQuitOnLastWindowClosed(false);

    try {
        SnowCanvasRuntime sourceRuntime;
        require(sourceRuntime.isValid(), "source runtime creation failed");
        recognitionResultsSurviveTargetImageReallocationAndSeedAllModes();
        pinnedSeededRecognitionSkipsAutomaticRequests(sourceRuntime);
        if (app.arguments().contains(QStringLiteral("--large-edit-only"))) {
            pinnedLargeImageRemainsOpenWhenEnteringDrawingMode(sourceRuntime);
            return 0;
        }
        if (app.arguments().contains(QStringLiteral("--recognition-provider-loss-only"))) {
            pinnedRecognitionProviderLossEndsBusyState(sourceRuntime);
            return 0;
        }
        if (app.arguments().contains(QStringLiteral("--recognition-priority-only"))) {
            pinnedRecognitionPromotesAutomaticPrefetch(sourceRuntime);
            return 0;
        }
        if (app.arguments().contains(QStringLiteral("--clipboard-content-only"))) {
            pinnedFormattedClipboardTextSkipsOcrAndSeedsPlainTextEditing(sourceRuntime);
            return 0;
        }
        if (app.arguments().contains(QStringLiteral("--close-after-recognized-text"))) {
            pinnedCloseAfterRecognizedText(sourceRuntime);
            return 0;
        }
        if (app.arguments().contains(QStringLiteral("--async-presentation-only"))) {
            pinnedAsyncPresentationDefersContent(sourceRuntime);
            return 0;
        }
        if (app.arguments().contains(QStringLiteral("--tray-pin-runtime-only"))) {
            pinnedContextMenuAndModes(sourceRuntime);
            pinnedControlsMatchReferenceStyle(sourceRuntime);
            return 0;
        }
        if (app.arguments().contains(QStringLiteral("--pinned-shortcut-only"))) {
            pinnedConfiguredShortcutUpdatesImmediately(sourceRuntime);
            return 0;
        }
#if defined(Q_OS_WIN) || defined(_WIN32)
        if (app.arguments().contains(QStringLiteral("--native-drag-shortcut-only"))) {
            pinnedNativeDragAcceptsCursorMovementShortcuts(sourceRuntime);
            return 0;
        }
#endif
        if (app.arguments().contains(QStringLiteral("--toolbar-parity-only"))) {
            pinnedDrawingToolbarMatchesCaptureInteractions(sourceRuntime);
            return 0;
        }
        if (app.arguments().contains(QStringLiteral("--thumbnail-background-only"))) {
            pinnedThumbnailUsesOpaqueThemeBackground(sourceRuntime);
            return 0;
        }
        pinnedContextMenuPreservesNativeGeometry(sourceRuntime);
        pinnedPhysicalPixelsFillClientArea(sourceRuntime);
        pinnedScalingAndAspectLockedResizing(sourceRuntime);
        pinnedSettledWheelScalingAdvancesPastRoundedLevel(sourceRuntime);
        pinnedWheelScalingUsesConfiguredAnchor(sourceRuntime);
        pinnedFollowsPerMonitorDpiScaling(sourceRuntime);
        pinnedCopyIncludesSourceCanvasDrawing();
        pinnedContextMenuAndModes(sourceRuntime);
        pinnedCloseCancelsPendingRecognition(sourceRuntime);
        pinnedAutomaticRecognitionCanBeDisabled(sourceRuntime);
        pinnedCloseAfterRecognizedText(sourceRuntime);
        pinnedAsyncPresentationDefersContent(sourceRuntime);
        pinnedRecognitionProviderLossEndsBusyState(sourceRuntime);
        pinnedFormattedClipboardTextSkipsOcrAndSeedsPlainTextEditing(sourceRuntime);
        pinnedControlsMatchReferenceStyle(sourceRuntime);
        pinnedThumbnailUsesOpaqueThemeBackground(sourceRuntime);
        pinnedControlsHideBelowMinimumNativeSize(sourceRuntime);
        pinnedLargeImageRemainsOpenWhenEnteringDrawingMode(sourceRuntime);
        pinnedWatermarkEditorAcceptsKeyboardInput(sourceRuntime);
        pinnedEditToolbarControlsCanvasHistory(sourceRuntime);
        pinnedDrawingToolbarMatchesCaptureInteractions(sourceRuntime);

        for (int iteration = 0; iteration < 8; ++iteration) {
            closePinnedWindow(sourceRuntime, false, false, iteration);
            closePinnedWindow(sourceRuntime, true, false, iteration);
            closePinnedWindow(sourceRuntime, true, true, iteration);
        }
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
    return 0;
}
