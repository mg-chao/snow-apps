#include "snow_shot/platform/windows/windowchrome.h"
#include "snow_shot/presentation/screenshotcapturecoordinator.h"
#include "snow_shot/storage/applicationstorage.h"
#include "snow_shot/storage/settingsadapters.h"

#include <QApplication>
#include <QCursor>
#include <QDir>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QPainter>
#include <QProcess>
#include <QScreen>
#include <QTemporaryDir>
#include <QTimer>
#include <QWidget>

#include <functional>
#include <iostream>
#include <stdexcept>

#include <qt_windows.h>

namespace {
void require(bool condition, const char* message) {
    if (!condition) {
        throw std::runtime_error(message);
    }
}

QCursor markerCursor(bool editor) {
    QPixmap pixels(24, 24);
    pixels.fill(editor ? QColor(240, 20, 30) : QColor(20, 240, 30));
    if (editor) {
        QPainter painter(&pixels);
        painter.fillRect(12, 0, 12, 24, QColor(20, 30, 240));
    }
    return QCursor(pixels, 0, 0);
}

class Surface final : public QWidget {
  public:
    explicit Surface(QWidget* parent = nullptr) : QWidget(parent) {}
    bool editor = false;
    std::function<void()> entered;

  protected:
    void paintEvent(QPaintEvent*) override {
        QPainter painter(this);
        painter.fillRect(rect(), Qt::white);
    }
    void enterEvent(QEnterEvent* event) override {
        QWidget::enterEvent(event);
        if (entered) {
            QTimer::singleShot(0, this, entered);
        }
        if (editor) {
            // A setup handshake after natural entry, never used by the refresh under test.
            QTimer::singleShot(0, this, [] { std::cout << "ENTER\n" << std::flush; });
        }
    }
    void leaveEvent(QEvent* event) override {
        QWidget::leaveEvent(event);
        if (editor) {
            QTimer::singleShot(0, this, [] { std::cout << "LEAVE\n" << std::flush; });
        }
    }
};

int runEditor(QApplication& app) {
    Surface window;
    window.setWindowFlags(Qt::Tool | Qt::FramelessWindowHint | Qt::WindowStaysOnTopHint);
    const QRect available = app.primaryScreen()->availableGeometry();
    window.setGeometry(QRect(available.center() - QPoint(250, 180), QSize(500, 360)));
    Surface canvas(&window);
    canvas.setGeometry(40, 40, 420, 280);
    canvas.editor = true;
    canvas.setMouseTracking(true);
    canvas.setCursor(markerCursor(true));
    window.show();
    QTimer::singleShot(0, &window, [&] {
        window.repaint();
        const QPoint logical = canvas.mapTo(&window, canvas.rect().center());
        const qreal scale = window.devicePixelRatioF();
        POINT physical{qRound(logical.x() * scale), qRound(logical.y() * scale)};
        ClientToScreen(reinterpret_cast<HWND>(window.winId()), &physical);
        std::cout << "READY " << physical.x << ' ' << physical.y << '\n' << std::flush;
    });
    return app.exec();
}

int runNativeEditor(QApplication& app) {
    BITMAPINFO bitmap{};
    bitmap.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bitmap.bmiHeader.biWidth = 24;
    bitmap.bmiHeader.biHeight = -24;
    bitmap.bmiHeader.biPlanes = 1;
    bitmap.bmiHeader.biBitCount = 32;
    void* pixels = nullptr;
    const HBITMAP color = CreateDIBSection(nullptr, &bitmap, DIB_RGB_COLORS, &pixels, nullptr, 0);
    require(color != nullptr, "native cursor bitmap creation failed");
    for (int y = 0; y < 24; ++y) {
        for (int x = 0; x < 24; ++x) {
            static_cast<DWORD*>(pixels)[y * 24 + x] = x < 12 ? 0xfff0141e : 0xff141ef0;
        }
    }
    const BYTE maskPixels[96]{};
    const HBITMAP mask = CreateBitmap(24, 24, 1, 1, maskPixels);
    ICONINFO icon{};
    icon.hbmColor = color;
    icon.hbmMask = mask;
    const HCURSOR cursor = static_cast<HCURSOR>(CreateIconIndirect(&icon));
    DeleteObject(color);
    DeleteObject(mask);
    require(cursor != nullptr, "native class cursor creation failed");
    WNDCLASSW klass{};
    klass.lpfnWndProc = DefWindowProcW;
    klass.hInstance = GetModuleHandleW(nullptr);
    klass.hCursor = cursor;
    klass.hbrBackground = static_cast<HBRUSH>(GetStockObject(WHITE_BRUSH));
    klass.lpszClassName = L"SnowRecaptureNativeEditor";
    require(RegisterClassW(&klass), "native editor class registration failed");
    const int x = GetSystemMetrics(SM_CXSCREEN) / 2;
    const int y = GetSystemMetrics(SM_CYSCREEN) / 2;
    const HWND window =
        CreateWindowExW(WS_EX_TOPMOST | WS_EX_TOOLWINDOW, klass.lpszClassName,
                        L"Native cursor regression", WS_POPUP | WS_VISIBLE, x - 300, y - 220, 600,
                        440, nullptr, nullptr, klass.hInstance, nullptr);
    require(window != nullptr, "native editor creation failed");
    UpdateWindow(window);
    std::cout << "READY " << x << ' ' << y << '\n' << std::flush;
    QTimer setup;
    QObject::connect(&setup, &QTimer::timeout, &app, [cursor, &setup] {
        CURSORINFO current{sizeof(CURSORINFO)};
        if (GetCursorInfo(&current) && current.hCursor == cursor) {
            setup.stop();
            std::cout << "ENTER\n" << std::flush;
        }
    });
    setup.start(10);
    const int result = app.exec();
    DestroyWindow(window);
    UnregisterClassW(klass.lpszClassName, klass.hInstance);
    DestroyCursor(cursor);
    return result;
}

class EditorProcess final {
  public:
    QProcess process;
    explicit EditorProcess(bool native = false) {
        process.start(QCoreApplication::applicationFilePath(),
                      {native ? QStringLiteral("--native-editor") : QStringLiteral("--editor"),
                       QStringLiteral("--run-native")});
        require(process.waitForStarted(5000), "editor process did not start");
    }
    ~EditorProcess() {
        process.terminate();
        if (!process.waitForFinished(1000)) {
            process.kill();
            process.waitForFinished(1000);
        }
    }
    QByteArray waitLine(const QByteArray& prefix) {
        QElapsedTimer deadline;
        deadline.start();
        while (!deadline.hasExpired(5000)) {
            while (process.canReadLine()) {
                const QByteArray line = process.readLine().trimmed();
                if (line.startsWith(prefix)) {
                    return line;
                }
            }
            process.waitForReadyRead(100);
        }
        throw std::runtime_error("editor setup handshake timed out");
    }
};

struct RestorePointer {
    POINT original{};
    RestorePointer() {
        GetCursorPos(&original);
    }
    ~RestorePointer() {
        SetCursorPos(original.x, original.y);
    }
};

// This regression is specifically about a stationary pointer. Prevent physical mouse
// input on the interactive desktop from changing the fixture while capture is pending.
struct StationaryPointer {
    RECT previous{};
    explicit StationaryPointer(const QPoint& position) {
        require(GetClipCursor(&previous), "could not save pointer confinement");
        confine(position);
    }
    void confine(const QPoint& position) {
        const RECT stationary{position.x(), position.y(), position.x() + 1, position.y() + 1};
        require(ClipCursor(&stationary), "could not keep the regression pointer stationary");
        require(SetCursorPos(position.x(), position.y()), "could not position regression pointer");
    }
    ~StationaryPointer() {
        ClipCursor(&previous);
    }
};

struct ShutdownStorage {
    ~ShutdownStorage() {
        snow_shot::storage::ApplicationStorage::instance().shutdown();
    }
};

ScreenshotCaptureResult capture(ScreenshotCaptureCoordinator& coordinator) {
    QEventLoop loop;
    ScreenshotCaptureResult result;
    bool received = false;
    const auto connection =
        QObject::connect(&coordinator, &ScreenshotCaptureCoordinator::captureFinished, &loop,
                         [&](const ScreenshotCaptureResult& value) {
                             result = value;
                             received = true;
                             loop.quit();
                         });
    ScreenshotCaptureRequest request;
    request.requestId = 1;
    request.refreshLayout = true;
    request.captureCursor = true;
    request.purpose = ScreenshotCapturePurpose::Recapture;
    coordinator.captureAsync(request);
    QTimer::singleShot(15000, &loop, &QEventLoop::quit);
    if (!received) {
        loop.exec();
    }
    QObject::disconnect(connection);
    require(received && result.succeeded, "real desktop capture failed or timed out");
    return result;
}

bool verifyCursor(const ScreenshotCaptureResult& result, const QPoint& position,
                  const QString& artifact, bool editorCursor = true) {
    for (const auto& display : result.displays) {
        if (!display.physicalRect.contains(position)) {
            continue;
        }
        const QPoint local = position - display.physicalRect.topLeft();
        const QImage crop = display.image.copy(QRect(local - QPoint(8, 8), QSize(96, 96)));
        require(crop.save(artifact), "could not save cursor evidence");
        int red = 0;
        int blue = 0;
        int green = 0;
        for (int y = 0; y < crop.height(); ++y) {
            for (int x = 0; x < crop.width(); ++x) {
                const QColor color = crop.pixelColor(x, y);
                red += color.red() > 180 && color.green() < 80 && color.blue() < 80;
                blue += color.blue() > 180 && color.red() < 80 && color.green() < 80;
                green += color.green() > 180 && color.red() < 80 && color.blue() < 80;
            }
        }
        std::cout << artifact.toStdString() << " backend=" << static_cast<int>(display.backend)
                  << " red=" << red << " blue=" << blue << " overlay_green=" << green << '\n';
        return editorCursor ? red >= 100 && blue >= 100 && green == 0
                            : green >= 100 && red == 0 && blue == 0;
    }
    throw std::runtime_error("cursor monitor was absent from capture result");
}

void runRecapture(QApplication& app) {
    using namespace snow_shot::platform::windows;
    QTemporaryDir settings;
    require(settings.isValid(), "could not create isolated settings");
    auto& storage = snow_shot::storage::ApplicationStorage::instance();
    static_cast<void>(storage.initialize({settings.filePath(QStringLiteral("bin")),
                                          settings.filePath(QStringLiteral("data")), 60000}));
    ShutdownStorage shutdownStorage;
    require(snow_shot::storage::ScreenshotSettings().setApiMode(QStringLiteral("dxgi")),
            "could not select DXGI");
    RestorePointer restorePointer;
    const QPoint outside = app.primaryScreen()->availableGeometry().topLeft() + QPoint(5, 5);
    SetCursorPos(outside.x(), outside.y());
    const bool native = app.arguments().contains(QStringLiteral("--native-target"));
    EditorProcess editor(native);
    const auto ready = editor.waitLine("READY ").split(' ');
    require(ready.size() == 3, "invalid editor geometry");
    const QPoint point(ready[1].toInt(), ready[2].toInt());
    StationaryPointer stationaryPointer(point);
    SetCursorPos(point.x(), point.y());
    editor.waitLine("ENTER");

    const QString artifacts =
        QDir::current().absoluteFilePath(QStringLiteral("cursor-e2e-evidence"));
    require(QDir().mkpath(artifacts), "could not create evidence directory");
    ScreenshotCaptureCoordinator coordinator;
    require(verifyCursor(capture(coordinator), point, artifacts + QStringLiteral("/natural.png")),
            "setup failed: natural mouse entry did not capture the editor cursor");
    // Leave the editor's child canvas before covering it. Recapture must resolve the child
    // cursor again, rather than accidentally reusing the target's previously cached shape.
    const int parentOffset = qRound(220 * app.primaryScreen()->devicePixelRatio());
    if (!native) {
        stationaryPointer.confine(point + QPoint(parentOffset, 0));
        editor.waitLine("LEAVE");
    }

    Surface overlay;
    overlay.setWindowFlags(Qt::Tool | Qt::FramelessWindowHint | Qt::WindowStaysOnTopHint);
    overlay.setAttribute(Qt::WA_TranslucentBackground);
    overlay.setGeometry(app.primaryScreen()->geometry());
    overlay.setCursor(markerCursor(false));
    QEventLoop overlayEntry;
    bool enteredOverlay = false;
    overlay.entered = [&] {
        enteredOverlay = true;
        overlayEntry.quit();
    };
    overlay.show();
    overlay.repaint();
    stationaryPointer.confine(point);
    QTimer::singleShot(5000, &overlayEntry, &QEventLoop::quit);
    if (!enteredOverlay) {
        overlayEntry.exec();
    }
    overlay.entered = {};
    require(enteredOverlay && flushWindowComposition(),
            "overlay did not receive native mouse entry");
    // Showing a HWND does not establish input ownership. Verify the stale overlay cursor
    // is actually present before testing the handover to the editor (as in a live session).
    require(verifyCursor(capture(coordinator), point, artifacts + QStringLiteral("/overlay.png"),
                         false),
            "setup failed: overlay cursor was not established before recapture");
    POINT nativePoint{point.x(), point.y()};
    require(WindowFromPoint(nativePoint) == reinterpret_cast<HWND>(overlay.winId()),
            "overlay did not intercept native input before recapture");
    CursorRefresh refresh(&app);
    if (app.arguments().contains(QStringLiteral("--hide-overlay"))) {
        overlay.hide();
        QCoreApplication::processEvents();
        require(flushWindowComposition(), "hidden overlay composition did not complete");
    } else {
        require(setWindowExcludedFromCapture(&overlay, true), "capture exclusion was unavailable");
        require(setWindowInputTransparent(&overlay, true).has_value(),
                "pass-through was unavailable");
    }
    QEventLoop refreshLoop;
    bool refreshed = false;
    bool completed = false;
    QPoint capturePoint = point;
    ScreenshotCaptureResult result;
    bool laterCursorChanged = false;
    refresh.refresh([&](bool ready) {
        refreshed = ready;
        if (ready) {
            POINT capturedPosition{};
            GetCursorPos(&capturedPosition);
            capturePoint = QPoint(capturedPosition.x, capturedPosition.y);
            if (app.arguments().contains(QStringLiteral("--change-after-ready"))) {
                QTimer::singleShot(0, &refreshLoop, [&] {
                    SetCursor(LoadCursorW(nullptr, IDC_WAIT));
                    laterCursorChanged = true;
                });
            }
            // Match the controller: snapshot in the readiness callback, before yielding
            // to later mouse/cursor changes while the capture worker is running.
            result = capture(coordinator);
        }
        completed = true;
        refreshLoop.quit();
    });
    if (app.arguments().contains(QStringLiteral("--move-during-refresh"))) {
        QTimer::singleShot(0, &refreshLoop,
                           [&] { stationaryPointer.confine(point + QPoint(3, 0)); });
    }
    if (!completed) {
        refreshLoop.exec();
    }
    if (!refreshed) {
        const auto diagnostic = capture(coordinator);
        static_cast<void>(
            verifyCursor(diagnostic, point, artifacts + QStringLiteral("/timeout.png")));
    }
    require(refreshed, "native cursor readiness failed");
    require(!app.arguments().contains(QStringLiteral("--change-after-ready")) || laterCursorChanged,
            "snapshot fixture must change the live cursor during capture");
    const bool correct =
        verifyCursor(result, capturePoint, artifacts + QStringLiteral("/recapture.png"));
    require(correct, "REGRESSION: re-capture did not capture the other process's editor cursor");

    QEventLoop restoredEntry;
    bool restored = false;
    overlay.entered = [&] {
        restored = true;
        restoredEntry.quit();
    };
    require(setWindowInputTransparent(&overlay, false).has_value(),
            "overlay input restoration failed");
    require(setWindowExcludedFromCapture(&overlay, false), "capture exclusion restoration failed");
    overlay.show();
    require(refreshCursorUnderPointer(), "restored overlay mouse routing failed");
    QTimer::singleShot(5000, &restoredEntry, &QEventLoop::quit);
    if (!restored) {
        restoredEntry.exec();
    }
    overlay.entered = {};
    require(restored && flushWindowComposition(), "restored overlay did not receive mouse entry");
    require(verifyCursor(capture(coordinator), capturePoint,
                         artifacts + QStringLiteral("/restored.png"), false),
            "restoring input did not restore the overlay cursor");
    overlay.hide();
    coordinator.shutdown();
}
} // namespace

int main(int argc, char** argv) {
    QApplication app(argc, argv);
    try {
        require(app.arguments().contains(QStringLiteral("--run-native")), "pass --run-native");
        if (app.arguments().contains(QStringLiteral("--editor"))) {
            return runEditor(app);
        }
        if (app.arguments().contains(QStringLiteral("--native-editor"))) {
            return runNativeEditor(app);
        }
        runRecapture(app);
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
