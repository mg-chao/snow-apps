#include "close_release_native_test_support.h"
#include "snow_shot/presentation/screenshotpinnedwindow.h"
#include "snow_shot/presentation/canvasstatusreadout.h"
#include "../src/presentation/pinned/screenshotpinnedwindownative.h"
#include "../src/presentation/pinned/screenshotpinnednativegeometrycontroller.h"
#include "snow_shot/presentation/screenshotcanvasrenderer.h"
#include "snow_shot/presentation/screenshotexportartifact.h"
#include "snow_shot/presentation/pinnedwindowgroupmanager.h"
#include "snow_shot/presentation/screenshotpinnededitcontroller.h"
#include "snow_shot/presentation/screenshotfloatingtoolpalettewindow.h"
#include "snow_shot/presentation/screenshotgeometry.h"
#include "snow_shot/presentation/screenshotocrpresentation.h"
#include "snow_shot/presentation/screenshotocrrecognitionservice.h"
#include "snow_shot/presentation/screenshotqrrecognitionservice.h"
#include "snow_shot/presentation/screenshotrecognitionsessioncontroller.h"
#include "snow_shot/presentation/screenshotrecognitionwindow.h"
#include "snow_shot/presentation/screenshotselectionexportuiservices.h"
#include "snow_shot/presentation/screenshottoolpalette.h"
#include "snow_shot/presentation/windowshortcutmanager.h"
#include "snow_shot/storage/applicationstorage.h"
#include "snow_shot/storage/pinnedwindowrepository.h"
#include "snow_shot/storage/pinnedwindowtypes.h"
#include "snow_shot/storage/settingsadapters.h"

#include "snow_draw_engine_qt/snow_canvas_runtime.h"
#include "snow_draw_engine_qt/snow_canvas_widget.h"
#include "theme/theme_manager.h"
#include "widgets/button.h"
#include "widgets/color_picker.h"
#include "widgets/context_menu.h"
#include "widgets/modal.h"
#include "widgets/input_line_edit.h"

#include <QAbstractButton>
#include <QActionGroup>
#include <QApplication>
#include <QClipboard>
#include <QCoreApplication>
#include <QCursor>
#include <QDataStream>
#include <QDir>
#include <QElapsedTimer>
#include <QEnterEvent>
#include <QEvent>
#include <QFrame>
#include <QFileDialog>
#include <QFileInfo>
#include <QFontDatabase>
#include <QGraphicsItem>
#include <QGraphicsScene>
#include <QGraphicsView>
#include <QImage>
#include <QKeyEvent>
#include <QLabel>
#include <QLineF>
#include <QLineEdit>
#include <QMouseEvent>
#include <QMimeData>
#include <QPainter>
#include <QPointer>
#include <QPushButton>
#include <QRegion>
#include <QScreen>
#include <QScopeGuard>
#include <QTemporaryDir>
#include <QTableView>
#include <QThread>
#include <QTimer>
#include <QTextBrowser>
#include <QUuid>
#include <QVariantAnimation>
#include <QWheelEvent>
#include <QWindow>

#include <algorithm>
#include <functional>
#include <initializer_list>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <utility>
#include <vector>

void runPinnedOriginalImageTranslationTests();

// Offscreen tests exercise restored state and queued DPI notifications without
// installing the Windows HWND hooks required by present().
class ScreenshotPinnedWindowTestAccess {
  public:
    static QTimer* showReadout(ScreenshotPinnedWindow& window, bool opacity) {
        window.m_scalePercent = 125;
        window.m_opacityPercent = 80;
        if (opacity) {
            window.showOpacityReadout();
        } else {
            window.showScaleReadout();
        }
        return window.m_scaleLabelTimer;
    }
    static void restoreOffscreen(ScreenshotPinnedWindow& window,
                                 const ScreenshotPinnedWindow::Config& config) {
        window.setAttribute(Qt::WA_DeleteOnClose, false);
        const qreal dpr = window.devicePixelRatioF();
        window.resize(qRound(config.nativeGeometry.width() / dpr),
                      qRound(config.nativeGeometry.height() / dpr));
        window.m_canvas->setGeometry(window.rect());
        window.m_canvasSourceRect = config.canvasSourceRect;
        window.m_backgroundCanvasRect = config.canvasSourceRect;
        window.m_resultSurfaceCanvasRect = config.canvasSourceRect;
        window.m_initialPhysicalSize = config.fullResolutionScaleBasis;
        window.m_originalImage = config.imageSource.materializedImage;
        window.m_transformedImage = window.m_originalImage;
        window.m_screenshotRenderer->setImageSource(config.imageSource);
        window.m_screenshotRenderer->setPinnedResultSurface(
            config.canvasSourceRect, config.canvasSourceRect, config.resultStyle);
        window.restorePersistentState(config);
        static_cast<void>(window.m_nativeGeometryController->initialize(config.nativeGeometry));
        window.m_presented = true;
        window.updateCanvasViewport();
    }

    static ScreenshotRecognitionSessionController*
    recognitionOffscreen(ScreenshotPinnedWindow& window, ScreenshotPinnedWindow::Config config) {
        window.m_recognitionContent = new ScreenshotRecognitionWindow(
            {}, &window, ScreenshotRecognitionWindow::PresentationMode::EmbeddedChild);
        ScreenshotRecognitionSessionActions actions;
        actions.ensureContent = [&window]() { return window.m_recognitionContent; };
        actions.applyOcrPresentation =
            [&window](std::shared_ptr<ScreenshotOcrPresentation> presentation) {
                window.m_ocrReady = true;
                window.m_ocrMode = true;
                window.m_originalOcrPresentation = std::move(presentation);
                window.updateOcrPresentation();
            };
        window.m_recognitionSession = std::make_unique<ScreenshotRecognitionSessionController>(
            nullptr, nullptr, nullptr, std::move(actions), &window);
        auto* session = window.m_recognitionSession.get();
        session->setTarget(
            {config.recognitionResults.key, window.m_originalImage, window.m_canvasSourceRect});
        session->seedRecognitionResults(config.recognitionResults);
        session->activate(ScreenshotRecognitionSessionController::Mode::Text);
        return session;
    }
    static std::shared_ptr<ScreenshotExportArtifact> fileSave(ScreenshotPinnedWindow& window) {
        return window.fileSaveArtifact();
    }
    static void rotateRecognitionOffscreen(ScreenshotPinnedWindow& window) {
        window.m_imageTransform =
            QImage::trueMatrix(QTransform().rotate(90), window.m_originalImage.width(),
                               window.m_originalImage.height());
        window.m_transformedImage = window.m_originalImage.transformed(window.m_imageTransform);
        window.m_backgroundCanvasRect.setSize(QSizeF(window.m_transformedImage.size()));
        window.updateOcrPresentation();
    }
    static const ScreenshotOcrPresentation&
    displayedRecognition(const ScreenshotPinnedWindow& window) {
        return *window.m_displayOcrPresentation;
    }
    static void quickSave(ScreenshotPinnedWindow& window) {
        window.quickSave();
    }
    static void saveAsFile(ScreenshotPinnedWindow& window) {
        window.saveAsFile();
    }

    static void leaveViewportAtPreviousDpi(ScreenshotPinnedWindow& window, qreal ratio) {
        window.m_viewportZoom *= ratio;
        window.m_canvas->setViewportCamera(window.m_viewportCenter.x(), window.m_viewportCenter.y(),
                                           window.m_viewportZoom);
    }

    static double viewportZoom(const ScreenshotPinnedWindow& window) {
        return window.m_viewportZoom;
    }

    static std::shared_ptr<ScreenshotExportArtifact>
    exportArtifact(ScreenshotPinnedWindow& window) {
        return window.m_exportArtifact;
    }

    static void setAnimationSnapshot(ScreenshotPinnedWindow& window, const QRect& target) {
        window.m_geometryAnimation = new QVariantAnimation(&window);
        window.m_geometryAnimation->setStartValue(window.currentNativeGeometry());
        window.m_geometryAnimation->setEndValue(target);
        window.m_geometryAnimation->start();
        window.m_geometryAnimation->pause();
        window.m_geometryAnimating = true;
    }

    static void finishExpansionOffscreen(ScreenshotPinnedWindow& window) {
        window.m_thumbnailMode = false;
        // Geometry is verified by the native recreation test. Suppress the
        // HWND mutation here so the transition state can be tested offscreen.
        window.m_nativeGeometryController.reset();
        window.restoreFromThumbnailImmediately();
    }

    static bool isGeometryAnimating(const ScreenshotPinnedWindow& window) {
        return window.m_geometryAnimating ||
               window.m_geometryAnimation->state() != QAbstractAnimation::Stopped;
    }
};

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

void require(bool condition, const char* message) {
    if (!condition) {
        throw std::runtime_error(message);
    }
}

void waitForUi(int milliseconds) {
    QElapsedTimer elapsed;
    elapsed.start();
    while (elapsed.elapsed() < milliseconds) {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
        QThread::msleep(1);
    }
}

void releaseCloseGesture(QWidget& receiver, Qt::MouseButton button) {
    const QPoint point = receiver.rect().center();
    QMouseEvent release(QEvent::MouseButtonRelease, QPointF(point),
                        QPointF(receiver.mapToGlobal(point)), button, Qt::NoButton, Qt::NoModifier);
    QCoreApplication::sendEvent(&receiver, &release);
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
    const bool positioned = SetPhysicalCursorPos(position.x(), position.y()) != FALSE;
    if (!positioned) {
        std::cerr << "SetPhysicalCursorPos failed: error=" << GetLastError() << '\n';
    }
    require(positioned, "failed to set the physical system cursor position");
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

class PinnedWindowTestApplication final : public QApplication {
  public:
    using QApplication::QApplication;
    QPointer<ScreenshotPinnedWindow> movementProbe;
    std::function<void(QPoint, QPoint)> afterMovementKey;

    bool notify(QObject* receiver, QEvent* event) override {
        if (movementProbe == nullptr || receiver != movementProbe ||
            event->type() != QEvent::KeyPress || !afterMovementKey) {
            return QApplication::notify(receiver, event);
        }
        const QPoint cursorBefore = systemCursorPosition();
        const QPoint windowBefore = movementProbe->currentNativeGeometry().topLeft();
        const bool handled = QApplication::notify(receiver, event);
        afterMovementKey(systemCursorPosition() - cursorBefore,
                         movementProbe->currentNativeGeometry().topLeft() - windowBefore);
        return handled;
    }
};

QPushButton* buttonNamed(QWidget& window, const QString& accessibleName);
bool processUntilDeleted(QPointer<ScreenshotPinnedWindow>& window, int timeoutMs);
adqt::widgets::AdButton* toolbarButtonNamed(ScreenshotToolPalette& toolbar, const QString& tooltip);

class ImmediateQrRecognition final : public ScreenshotQrRecognitionPort {
  public:
    explicit ImmediateQrRecognition(QStringList contents) : m_contents(std::move(contents)) {}

    RequestToken recognize(QImage, QObject*, Completion completion) override {
        if (completion) {
            completion(ScreenshotQrRecognitionResult{m_contents, {}});
        }
        return 1;
    }

    void cancel(RequestToken) override {}

  private:
    QStringList m_contents;
};

void pinnedQrResultCopiesWithKeyboardShortcut() {
    QScreen* screen = QGuiApplication::primaryScreen();
    require(screen != nullptr, "a primary screen is required");

    ImmediateQrRecognition qrRecognition(
        {QStringLiteral("https://example.com/pinned-qr"), QStringLiteral("second payload")});
    QImage background(320, 180, QImage::Format_ARGB32_Premultiplied);
    background.fill(QColor(42, 84, 126, 255));
    auto* pinnedWindow = new ScreenshotPinnedWindow();
    QPointer<ScreenshotPinnedWindow> guardedWindow(pinnedWindow);
    ScreenshotPinnedWindow::Config config;
    config.nativeGeometry = physicalPinGeometry(*screen, QPoint(40, 40), background.size());
    config.canvasSourceRect = QRectF(QPointF(), QSizeF(background.size()));
    config.imageSource = ScreenshotImageSource::fromImage(background, config.canvasSourceRect);
    config.screen = screen;
    config.enableEditing = true;
    config.automaticTextRecognition = false;
    config.qrRecognition = &qrRecognition;
    require(pinnedWindow->present(config), "pinned QR copy presentation failed");
    waitForUi(50);

    auto* editButton = pinnedWindow->findChild<adqt::widgets::AdButton*>(
        QStringLiteral("screenshotPinnedEditButton"));
    require(editButton != nullptr, "pinned QR copy edit button was not found");
    editButton->click();
    waitForUi(50);

    auto* editController = pinnedWindow->findChild<ScreenshotPinnedEditController*>();
    auto* toolbarWindow = editController != nullptr ? editController->toolbarWindow() : nullptr;
    auto* toolbar = toolbarWindow != nullptr ? toolbarWindow->palette() : nullptr;
    require(toolbar != nullptr, "pinned QR copy toolbar was not created");
    toolbar->setQrEnabled(true);
    require(QMetaObject::invokeMethod(toolbar, "qrRequested", Qt::DirectConnection),
            "pinned QR copy should activate barcode recognition");
    QElapsedTimer qrReady;
    qrReady.start();
    QTextBrowser* browser = nullptr;
    while (qrReady.elapsed() < 10000) {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
        browser = pinnedWindow->findChild<QTextBrowser*>(QStringLiteral("screenshotQrContents"));
        if (browser != nullptr && browser->isVisible()) {
            break;
        }
        QThread::msleep(1);
    }

    require(browser != nullptr && browser->isVisible(),
            "pinned QR recognition should create a visible result surface");
    browser->setFocus(Qt::OtherFocusReason);
    const QString expected = QStringLiteral("https://example.com/pinned-qr\nsecond payload");
    require(browser->toPlainText() == expected,
            "pinned QR recognition should render all decoded payloads");

    QApplication::clipboard()->setText(QStringLiteral("stale clipboard text"));
    QKeyEvent copy(QEvent::KeyPress, Qt::Key_C, Qt::ControlModifier);
    QApplication::sendEvent(browser, &copy);
    require(copy.isAccepted() && QApplication::clipboard()->text() == expected,
            "Ctrl+C should copy all pinned QR result text");

    QKeyEvent selectAll(QEvent::KeyPress, Qt::Key_A, Qt::ControlModifier);
    QApplication::sendEvent(browser, &selectAll);
    require(selectAll.isAccepted() && browser->textCursor().hasSelection(),
            "Ctrl+A should select the pinned QR result text");

    pinnedWindow->close();
    require(processUntilDeleted(guardedWindow, 2000), "pinned QR copy window was not deleted");
}

void groupedPinnedWindowSignalConnectionsDoNotAssert() {
    QScreen* screen = QGuiApplication::primaryScreen();
    require(screen != nullptr, "a primary screen is required");

    snow_shot::presentation::PinnedWindowGroupManager groupManager;
    QImage image(120, 80, QImage::Format_ARGB32_Premultiplied);
    image.fill(QColor(42, 84, 126, 255));
    auto* pinnedWindow = new ScreenshotPinnedWindow();
    QPointer<ScreenshotPinnedWindow> guardedWindow(pinnedWindow);
    ScreenshotPinnedWindow::Config config;
    config.nativeGeometry = physicalPinGeometry(*screen, QPoint(40, 40), image.size());
    config.canvasSourceRect = QRectF(QPointF(), QSizeF(image.size()));
    config.imageSource = ScreenshotImageSource::fromImage(image, config.canvasSourceRect);
    config.screen = screen;
    config.enableEditing = true;
    config.automaticTextRecognition = false;
    config.groupManager = &groupManager;
    config.groupId = groupManager.activeGroupId();
    require(pinnedWindow->present(config),
            "a grouped pinned window should present without a Qt connection assertion");

    require(groupManager.createGroup(QStringLiteral("Signal test")).has_value(),
            "group creation should still work after grouped presentation");
    QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
    pinnedWindow->close();
    require(processUntilDeleted(guardedWindow, 2000),
            "the grouped pinned window should close after the connection test");
    // The manager is bound to the process-wide storage in the full run; the
    // group must not survive into sections that assert on initial group state.
    require(groupManager.deleteEmptyGroups(),
            "the signal-test group should not leak into later sections");
}

void groupMenuActionsExposeIconsAndCleanupState() {
    QScreen* screen = QGuiApplication::primaryScreen();
    require(screen != nullptr, "a primary screen is required");

    // The menu's startup state is only "just the built-in group" when this
    // section owns its repository: a bare manager binds to the process-wide
    // ApplicationStorage, which earlier sections of the full run have already
    // extended with their own groups.
    QTemporaryDir directory;
    require(directory.isValid(), "temporary storage directory is unavailable");
    snow_shot::storage::PinnedWindowRepository repository(directory.path());
    snow_shot::presentation::PinnedWindowGroupManager groupManager(&repository);
    QImage image(120, 80, QImage::Format_ARGB32_Premultiplied);
    image.fill(QColor(42, 84, 126, 255));
    auto* pinnedWindow = new ScreenshotPinnedWindow();
    QPointer<ScreenshotPinnedWindow> guardedWindow(pinnedWindow);
    ScreenshotPinnedWindow::Config config;
    config.nativeGeometry = physicalPinGeometry(*screen, QPoint(40, 40), image.size());
    config.canvasSourceRect = QRectF(QPointF(), QSizeF(image.size()));
    config.imageSource = ScreenshotImageSource::fromImage(image, config.canvasSourceRect);
    config.screen = screen;
    config.enableEditing = true;
    config.automaticTextRecognition = false;
    config.groupManager = &groupManager;
    config.groupId = groupManager.activeGroupId();
    require(pinnedWindow->present(config),
            "a grouped pinned window should present for the group menu checks");

    auto* groupMenu = pinnedWindow->findChild<adqt::widgets::AdContextMenu*>(
        QStringLiteral("screenshotPinnedGroupMenu"));
    require(groupMenu != nullptr, "the pinned context menu should own a group submenu");
    const auto groupMenuActionNamed = [groupMenu](const QString& name) {
        for (QAction* action : groupMenu->actions()) {
            if (action != nullptr && action->objectName() == name) {
                return action;
            }
        }
        return static_cast<QAction*>(nullptr);
    };
    // The submenu clears and recreates its actions on rebuild, so every state
    // check must resolve its QAction again after the refresh.
    const auto refreshGroupMenu = [groupMenu, &groupMenuActionNamed](const QString& name) {
        require(QMetaObject::invokeMethod(groupMenu, "aboutToShow", Qt::DirectConnection),
                "the group submenu rebuild should be triggerable");
        return groupMenuActionNamed(name);
    };

    QAction* groupHeader = groupMenu->menuAction();
    require(groupHeader != nullptr &&
                groupHeader->objectName() == QStringLiteral("screenshotPinnedGroupAction") &&
                !groupHeader->icon().isNull(),
            "the group submenu header should carry an icon");

    auto* contextMenu = pinnedWindow->findChild<adqt::widgets::AdContextMenu*>(
        QStringLiteral("screenshotPinnedContextMenu"));
    require(contextMenu != nullptr, "the pinned window should own its context menu");
    const QList<QAction*> contextActions = contextMenu->actions();
    const int groupIndex = contextActions.indexOf(groupHeader);
    require(groupIndex >= 0 && groupIndex + 1 < contextActions.size() &&
                contextActions.at(groupIndex + 1)->objectName() ==
                    QStringLiteral("screenshotPinnedThumbnailAction"),
            "the group submenu should sit directly above Thumbnail mode");

    QAction* newGroup = groupMenuActionNamed(QStringLiteral("screenshotPinnedNewGroupAction"));
    require(newGroup != nullptr && !newGroup->icon().isNull() && newGroup->isEnabled(),
            "New Group should expose an icon and stay actionable");
    QAction* deleteEmpty =
        groupMenuActionNamed(QStringLiteral("screenshotPinnedDeleteEmptyGroupsAction"));
    require(deleteEmpty != nullptr && !deleteEmpty->icon().isNull(),
            "Delete Empty Groups should expose an icon");
    require(!deleteEmpty->isEnabled(),
            "Delete Empty Groups should start disabled while only the built-in group exists");

    require(groupManager.createGroup(QStringLiteral("Cleanup")).has_value(),
            "an empty custom group should be created for the cleanup state");
    deleteEmpty = refreshGroupMenu(QStringLiteral("screenshotPinnedDeleteEmptyGroupsAction"));
    require(deleteEmpty != nullptr && deleteEmpty->isEnabled(),
            "Delete Empty Groups should enable once an empty custom group exists");

    require(groupManager.deleteEmptyGroups(), "the empty custom group should be deleted");
    deleteEmpty = refreshGroupMenu(QStringLiteral("screenshotPinnedDeleteEmptyGroupsAction"));
    require(deleteEmpty != nullptr && !deleteEmpty->isEnabled(),
            "Delete Empty Groups should disable again after the cleanup");

    pinnedWindow->close();
    require(processUntilDeleted(guardedWindow, 2000),
            "the group menu pinned window was not deleted after the checks");
}

adqt::widgets::AdButton* toolbarButtonNamed(ScreenshotToolPalette& toolbar,
                                            const QString& tooltip) {
    for (adqt::widgets::AdButton* button : toolbar.findChildren<adqt::widgets::AdButton*>()) {
        if (button != nullptr && button->toolTip().startsWith(tooltip)) {
            return button;
        }
    }
    return nullptr;
}

class IdleOcrRecognition final : public ScreenshotOcrRecognitionPort {
  public:
    RequestToken recognize(ScreenshotOcrRequest, QObject*, Completion) override {
        ++requests;
        return 1;
    }

    void cancel(RequestToken) override {}

    bool reprioritize(RequestToken, ScreenshotOcrRequestPriority) override {
        return false;
    }

    int requests = 0;
};

QAction* pinnedMenuActionNamed(ScreenshotPinnedWindow& window, const QString& name) {
    auto* menu = window.findChild<adqt::widgets::AdContextMenu*>(
        QStringLiteral("screenshotPinnedContextMenu"));
    if (menu == nullptr) {
        return nullptr;
    }
    for (QAction* action : menu->actions()) {
        if (action != nullptr && action->objectName() == name) {
            return action;
        }
    }
    return nullptr;
}

QVector<ScreenshotPinnedWindow*> topLevelPinnedWindows() {
    QVector<ScreenshotPinnedWindow*> windows;
    for (QWidget* widget : QApplication::topLevelWidgets()) {
        if (auto* window = qobject_cast<ScreenshotPinnedWindow*>(widget)) {
            windows.push_back(window);
        }
    }
    return windows;
}

ScreenshotPinnedWindow*
hiddenPinnedWindowExcept(std::initializer_list<ScreenshotPinnedWindow*> excluded) {
    const QVector<ScreenshotPinnedWindow*> windows = topLevelPinnedWindows();
    for (ScreenshotPinnedWindow* window : windows) {
        if (window != nullptr && !window->isVisible() &&
            std::find(excluded.begin(), excluded.end(), window) == excluded.end()) {
            return window;
        }
    }
    return nullptr;
}

ScreenshotPinnedWindow* onlyVisiblePinnedWindow() {
    ScreenshotPinnedWindow* visibleWindow = nullptr;
    for (ScreenshotPinnedWindow* candidate : topLevelPinnedWindows()) {
        if (candidate->isVisible()) {
            require(visibleWindow == nullptr, "only the current test pin should be visible");
            visibleWindow = candidate;
        }
    }
    return visibleWindow;
}

void setPinnedWindowHovered(ScreenshotPinnedWindow& window, bool hovered);

void pinnedSelectionRendersCachedOcrInCanvasCoordinates(bool restoreFromStorage = false,
                                                        bool includeOtherResults = false,
                                                        bool initiallyVisible = false) {
    QScreen* screen = QGuiApplication::primaryScreen();
    require(screen != nullptr, "a primary screen is required");

    IdleOcrRecognition recognition;
    auto services = std::make_unique<ScreenshotSelectionExportUiServices>(&recognition);
    const QString text = QStringLiteral("Cached screenshot text");
    for (const int padding : {0, 12}) {
        for (const QPoint& origin : {QPoint(), QPoint(640, 360), QPoint(-640, -360)}) {
            ScreenshotPinnedSelectionRequest request;
            request.selection = QRect(origin, QSize(320, 180));
            request.contentCanvasRect = QRectF(request.selection);
            request.surfaceCanvasRect =
                request.contentCanvasRect.adjusted(-padding, -padding, padding, padding);
            request.resultStyle.shadowWidth = padding;
            const auto layout = ScreenshotResultCompositor::layoutForContent(
                request.selection.size(), request.resultStyle);
            request.fullResolutionScaleBasis = layout.outputRect.size();
            const bool testInteractions =
                origin.isNull() && padding == 0 && !restoreFromStorage && !initiallyVisible;
            const QSize pinSize =
                testInteractions ? QSize(800, 450) : request.fullResolutionScaleBasis;
            request.geometry.nativeGeometry = physicalPinGeometry(*screen, QPoint(40, 40), pinSize);
            request.geometry.canvasSourceRect = request.surfaceCanvasRect;
            request.geometry.initialPhysicalSize = request.fullResolutionScaleBasis;
            request.screen = screen;

            auto presentation = std::make_shared<ScreenshotOcrPresentation>();
            presentation->selection = request.selection;
            ScreenshotOcrLine line;
            line.text = text;
            line.confidence = 0.93;
            const QRectF textRect(QPointF(origin) + QPointF(40, 60), QSizeF(240, 32));
            line.quad = QPolygonF({textRect.topLeft(), textRect.topRight(), textRect.bottomRight(),
                                   textRect.bottomLeft()});
            presentation->lines.push_back(line);
            presentation->prepareForRendering();
            request.recognitionResults.key = QStringLiteral("cached-pinned-selection");
            request.recognitionVisible = initiallyVisible;
            ScreenshotOcrRecognitionResult result;
            result.presentation = presentation;
            request.recognitionResults.text = result;
            if (includeOtherResults) {
                SnowShotTableResult table;
                table.html = QStringLiteral("<table><tr><td>Saved table</td></tr></table>");
                request.recognitionResults.table = table;
                request.recognitionResults.qr =
                    ScreenshotQrRecognitionResult{{QStringLiteral("Saved barcode")}, {}};
            }

            QImage content(request.selection.size(), QImage::Format_ARGB32_Premultiplied);
            content.fill(QColor(42, 84, 126));
            const QImage image = ScreenshotResultCompositor::compose(content, request.resultStyle);
            auto artifact = std::make_shared<ScreenshotExportArtifact>(
                ScreenshotExportSource::fromImage(image));
            bool completed = false;
            bool succeeded = false;
            require(services->presentPinnedArtifact(request, std::move(artifact),
                                                    [&completed, &succeeded](bool success, QImage) {
                                                        completed = true;
                                                        succeeded = success;
                                                    }),
                    "a screenshot with cached OCR should be pinnable");
            QElapsedTimer elapsed;
            elapsed.start();
            while (!completed && elapsed.elapsed() < 5000) {
                QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
                QThread::msleep(1);
            }
            require(completed && succeeded, "the cached OCR pin should finish loading");
            QPointer<ScreenshotPinnedWindow> window(onlyVisiblePinnedWindow());
            require(window != nullptr, "the cached OCR pin should be visible");
            const QString persistenceId = window->persistenceId();
            const auto closeWindow = qScopeGuard([&window, persistenceId]() {
                if (window != nullptr) {
                    window->close();
                    static_cast<void>(processUntilDeleted(window, 2000));
                }
                static_cast<void>(
                    snow_shot::storage::ApplicationStorage::instance().pinnedWindows().remove(
                        persistenceId));
            });
            QAction* action =
                pinnedMenuActionNamed(*window, QStringLiteral("screenshotPinnedOcrAction"));
            require(action != nullptr && action->isEnabled(),
                    "cached OCR should be available from the pinned context menu");
#if defined(Q_OS_WIN) || defined(_WIN32)
            if (testInteractions) {
                const QPoint corner = window->currentNativeGeometry().topLeft();
                require(
                    SendMessageW(toNativeHwnd(window->winId()), WM_NCHITTEST, 0,
                                 MAKELPARAM(static_cast<WORD>(corner.x()),
                                            static_cast<WORD>(corner.y()))) == HTTOPLEFT,
                    "ordinary pinned resize borders must retain priority over caption dragging");
            }
#endif
            require(action->isChecked() == initiallyVisible,
                    "a pin must inherit visibility independently of its OCR cache");
            if (!initiallyVisible) {
                action->trigger();
            }
            QCoreApplication::processEvents(QEventLoop::AllEvents, 20);

            auto* viewingController = window->findChild<ScreenshotPinnedEditController*>();
            require(viewingController == nullptr || viewingController->toolbarWindow() == nullptr,
                    "context-menu OCR must show its result without opening a toolbar");

            if (restoreFromStorage) {
                const auto record = window->persistenceSnapshot();
                require(record.recognitionVisible, "a visible OCR overlay must be persisted");
                require(!record.recognitionResults.isEmpty(),
                        "the pinned snapshot should serialize its cached recognition results");
                QDataStream payload(record.recognitionResults);
                QString savedKey;
                quint8 hasText = 0, hasTable = 0, hasQr = 0;
                QString savedError;
                QRect savedSelection;
                qint64 savedLineCount = 0;
                payload >> savedKey >> hasText >> hasTable >> hasQr >> savedError >>
                    savedSelection >> savedLineCount;
                require(payload.status() == QDataStream::Ok &&
                            savedKey == request.recognitionResults.key && hasText == 1 &&
                            savedSelection == presentation->selection &&
                            savedLineCount == presentation->lines.size(),
                        "persisted OCR must keep the existing 64-bit line-count format");
                window->close();
                require(processUntilDeleted(window, 2000), "the original OCR pin should close");
                services.reset();

                auto& storage = snow_shot::storage::ApplicationStorage::instance();
                const QString directory = storage.configurationDirectory();
                require(storage.pinnedWindows().upsert(record).success &&
                            storage.pinnedWindows().flush().success,
                        "the pinned screenshot and OCR should be saved to disk");
                storage.shutdown();
                const snow_shot::storage::StorageInitializationOptions options{
                    QDir(directory).absoluteFilePath(QStringLiteral("../bin")), directory, 0};
                require(storage.initialize(options).success,
                        "pinned storage should reopen after a restart");
                const auto loaded = storage.pinnedWindows().loadRecord(record.id);
                require(loaded.has_value() &&
                            loaded->recognitionResults == record.recognitionResults,
                        "reopened storage should preserve the serialized OCR bytes");

                services = std::make_unique<ScreenshotSelectionExportUiServices>(&recognition);
                services->restorePersistedWindows();
                QCoreApplication::processEvents(QEventLoop::AllEvents, 50);
                window = onlyVisiblePinnedWindow();
                require(window != nullptr, "the saved OCR pin should be recreated after restart");
                action =
                    pinnedMenuActionNamed(*window, QStringLiteral("screenshotPinnedOcrAction"));
                require(action != nullptr && action->isEnabled(),
                        "the restored pin should offer text recognition results");
                require(action->isChecked(), "restored OCR must already be visible");
                QCoreApplication::processEvents(QEventLoop::AllEvents, 20);

                auto* session = window->findChild<ScreenshotRecognitionSessionController*>();
                require(session != nullptr, "the restored pin should have a recognition session");
                const auto cached = session->cachedRecognitionResults();
                require(cached.text.has_value() && cached.text->presentation != nullptr &&
                            cached.text->presentation->lines.size() == presentation->lines.size(),
                        "restoring saved OCR must retain every recognized line");
                require(cached.text->presentation->selection == presentation->selection &&
                            cached.text->presentation->lines.front().text == line.text &&
                            cached.text->presentation->lines.front().quad == line.quad &&
                            window->persistenceSnapshot().recognitionResults ==
                                record.recognitionResults,
                        "restoring saved OCR must preserve its text and canvas coordinates");
                if (includeOtherResults) {
                    require(cached.table.has_value() &&
                                cached.table->html == request.recognitionResults.table->html &&
                                cached.qr.has_value() &&
                                cached.qr->contents == request.recognitionResults.qr->contents,
                            "restoring OCR must preserve subsequent table and barcode results");
                }
            }

            auto* recognitionContent = window->findChild<ScreenshotRecognitionWindow*>(
                QStringLiteral("screenshotPinnedRecognitionContent"));
            if (recognitionContent == nullptr || !recognitionContent->isVisible()) {
                auto* session = window->findChild<ScreenshotRecognitionSessionController*>();
                std::cerr << "origin=" << origin.x() << ',' << origin.y() << " padding=" << padding
                          << " restored=" << restoreFromStorage << " action=" << action->isEnabled()
                          << '/' << action->isChecked()
                          << " target=" << (session != nullptr && session->hasTarget())
                          << " cached=" << (session != nullptr && session->hasTextResult())
                          << " active=" << (session != nullptr && session->active()) << '\n';
            }
            require(recognitionContent != nullptr && recognitionContent->isVisible(),
                    "showing cached OCR should display the embedded recognition surface");
            auto* restoredController = window->findChild<ScreenshotPinnedEditController*>();
            require(restoredController == nullptr || restoredController->toolbarWindow() == nullptr,
                    "restored OCR must not open the toolbar");
            auto* textLayer = recognitionContent->findChild<QGraphicsView*>(
                QStringLiteral("snowShotOcrTextLayer"));
            require(textLayer != nullptr && textLayer->isVisible(),
                    "cached OCR should have a visible text layer");
            const QList<QGraphicsItem*> items = textLayer->scene()->items();
            require(items.size() == 1, "the pinned text layer should contain the cached line");
            QGraphicsItem* textItem = items.front();
            const QRectF renderedRect =
                textLayer->viewportTransform().mapRect(textItem->sceneBoundingRect());
            const QPointF expectedCenter(
                (textRect.center().x() - request.surfaceCanvasRect.left()) /
                    request.surfaceCanvasRect.width() * textLayer->viewport()->width(),
                (textRect.center().y() - request.surfaceCanvasRect.top()) /
                    request.surfaceCanvasRect.height() * textLayer->viewport()->height());
            require(textItem->isVisible() &&
                        textLayer->viewport()->rect().contains(renderedRect.center().toPoint()) &&
                        QLineF(renderedRect.center(), expectedCenter).length() < 3.0,
                    qPrintable(QStringLiteral("cached OCR must render over its screenshot text "
                                              "at canvas origin (%1, %2) with padding %3")
                                   .arg(origin.x())
                                   .arg(origin.y())
                                   .arg(padding)));
            QImage renderedText(textLayer->viewport()->size(), QImage::Format_ARGB32_Premultiplied);
            renderedText.fill(Qt::transparent);
            QPainter painter(&renderedText);
            textLayer->scene()->render(&painter);
            painter.end();
            bool hasTextPixels = false;
            for (int y = 0; y < renderedText.height() && !hasTextPixels; ++y) {
                for (int x = 0; x < renderedText.width(); ++x) {
                    if (qAlpha(renderedText.pixel(x, y)) != 0) {
                        hasTextPixels = true;
                        break;
                    }
                }
            }
            require(hasTextPixels, "cached OCR should paint text pixels in the pinned viewport");
            require(recognitionContent->copyVisibleContentToClipboard() &&
                        QApplication::clipboard()->text() == text,
                    "the rendered cached OCR text should remain copyable");
            require(recognition.requests == 0,
                    "pinning cached OCR should not recognize the screenshot again");
            require(presentation->selection == request.selection &&
                        presentation->lines.front().quad == line.quad,
                    "pinning must preserve the source screenshot's cached OCR coordinates");
            if (testInteractions) {
                for (const bool toolbarOpen : {false, true}) {
                    if (toolbarOpen) {
                        action->trigger();
                        auto* editButton = window->findChild<adqt::widgets::AdButton*>(
                            QStringLiteral("screenshotPinnedEditButton"));
                        require(editButton != nullptr, "find the drawing button");
                        editButton->click();
                        action->trigger();
                        auto* editor = window->findChild<ScreenshotPinnedEditController*>();
                        require(editor != nullptr && editor->toolbarWindow() != nullptr &&
                                    editor->toolbarWindow()->isVisible(),
                                "context-menu OCR must preserve an already open toolbar");
                    }
                    const QPoint backgroundPoint(10, recognitionContent->height() - 20);
                    QMouseEvent hover(QEvent::MouseMove, QPointF(backgroundPoint),
                                      QPointF(recognitionContent->mapToGlobal(backgroundPoint)),
                                      Qt::NoButton, Qt::NoButton, Qt::NoModifier);
                    QApplication::sendEvent(recognitionContent, &hover);
                    require(recognitionContent->cursor().shape() == Qt::OpenHandCursor,
                            "OCR background must show a drag cursor with or without a toolbar");
                    const QPoint textPoint = renderedRect.center().toPoint();
                    QMouseEvent textHover(QEvent::MouseMove, QPointF(textPoint),
                                          QPointF(recognitionContent->mapToGlobal(textPoint)),
                                          Qt::NoButton, Qt::NoButton, Qt::NoModifier);
                    QApplication::sendEvent(recognitionContent, &textHover);
                    require(
                        recognitionContent->cursor().shape() == Qt::IBeamCursor,
                        "moving from OCR background onto text must restore the selection cursor");
#if defined(Q_OS_WIN) || defined(_WIN32)
                    const HWND hwnd = toNativeHwnd(window->winId());
                    const QRect nativeGeometry = window->currentNativeGeometry();
                    const auto hitTest = [&](const QPoint& point) {
                        const QPoint local = recognitionContent->mapTo(window, point);
                        const QPoint native =
                            nativeGeometry.topLeft() +
                            QPoint(qRound(local.x() * double(nativeGeometry.width()) /
                                          window->width()),
                                   qRound(local.y() * double(nativeGeometry.height()) /
                                          window->height()));
                        return SendMessageW(hwnd, WM_NCHITTEST, 0,
                                            MAKELPARAM(static_cast<WORD>(native.x()),
                                                       static_cast<WORD>(native.y())));
                    };
                    require(hitTest(backgroundPoint) == HTCAPTION &&
                                hitTest(renderedRect.center().toPoint()) == HTCLIENT,
                            "only OCR background must expose native caption dragging");
                    auto* controls =
                        window->findChild<QFrame*>(QStringLiteral("screenshotPinnedControlsPanel"));
                    if (!toolbarOpen) {
                        setPinnedWindowHovered(*window, true);
                        require(
                            controls != nullptr && controls->isVisible() &&
                                hitTest(recognitionContent->mapFrom(
                                    window, controls->mapTo(window, controls->rect().center()))) ==
                                    HTCLIENT,
                            "pinned controls must retain client hit testing over OCR background");
                    } else {
                        require(controls != nullptr && !controls->isVisible(),
                                "the drawing toolbar must retain its existing control visibility");
                    }
                    QMouseEvent press(QEvent::MouseButtonPress, QPointF(backgroundPoint),
                                      QPointF(recognitionContent->mapToGlobal(backgroundPoint)),
                                      Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
                    QApplication::sendEvent(recognitionContent, &press);
                    // Avoid entering USER32's modal loop when testing the move handoff.
                    MSG message{};
                    while (PeekMessageW(&message, hwnd, WM_SYSCOMMAND, WM_SYSCOMMAND, PM_REMOVE) !=
                           0) {
                    }
                    require(window->cursor().shape() == Qt::ClosedHandCursor,
                            "pressing OCR background must start the system move");
                    SendMessageW(hwnd, WM_EXITSIZEMOVE, 0, 0);
#endif
                }
                action->trigger();
                require(!window->persistenceSnapshot().recognitionVisible,
                        "hiding OCR must update persistence immediately");
            }
            window->close();
            require(processUntilDeleted(window, 2000), "the cached OCR pin should close");
        }
    }
}

ScreenshotPinnedWindow::Config cachedOcrPinConfig(ScreenshotOcrRecognitionPort* recognition,
                                                  int shadowWidth = 0) {
    QScreen* screen = QGuiApplication::primaryScreen();
    require(screen != nullptr, "a primary screen is required");
    QImage image(320, 180, QImage::Format_ARGB32_Premultiplied);
    image.fill(QColor(42, 84, 126));
    ScreenshotPinnedWindow::Config config;
    config.canvasSourceRect = QRectF(640, 360, 320, 180);
    config.imageSource = ScreenshotImageSource::fromImage(image, config.canvasSourceRect);
    config.contentCanvasRect = config.canvasSourceRect;
    config.surfaceCanvasRect =
        config.canvasSourceRect.adjusted(-shadowWidth, -shadowWidth, shadowWidth, shadowWidth);
    config.resultStyle.shadowWidth = shadowWidth;
    config.fullResolutionScaleBasis = config.surfaceCanvasRect.size().toSize();
    config.nativeGeometry =
        physicalPinGeometry(*screen, QPoint(40, 40), config.fullResolutionScaleBasis);
    config.screen = screen;
    config.recognition = recognition;
    config.automaticTextRecognition = false;
    auto presentation = std::make_shared<ScreenshotOcrPresentation>();
    presentation->selection = config.canvasSourceRect.toRect();
    ScreenshotOcrLine line;
    line.text = QStringLiteral("Saved OCR");
    line.confidence = 0.95;
    const QRectF textRect(680, 405, 180, 30);
    line.quad = QPolygonF(
        {textRect.topLeft(), textRect.topRight(), textRect.bottomRight(), textRect.bottomLeft()});
    presentation->lines.push_back(line);
    presentation->prepareForRendering();
    config.recognitionResults.key = QStringLiteral("cached-ocr-audit");
    config.recognitionResults.text = ScreenshotOcrRecognitionResult{presentation, {}, {}, {}};
    return config;
}

void pinnedEditingRecognitionShortcutsUsePaletteCommands() {
    ScreenshotPinnedWindow window;
    SnowCanvasWidget canvas;
    snow_shot::presentation::WindowShortcutManager manager;
    manager.addScopeWindow(&canvas);
    ScreenshotPinnedEditController controller(window, canvas, manager);
    canvas.show();
    controller.setEditMode(true);
    auto* palette = controller.toolbarWindow()->palette();
    require(palette != nullptr, "pinned editing must expose a palette");
    int requests = 0;
    QObject::connect(&controller, &ScreenshotPinnedEditController::textRecognitionRequested,
                     &controller, [&]() { ++requests; });
    QObject::connect(&controller, &ScreenshotPinnedEditController::textTranslationRequested,
                     &controller, [&]() { ++requests; });
    QObject::connect(&controller, &ScreenshotPinnedEditController::tableRecognitionRequested,
                     &controller, [&]() { ++requests; });
    QObject::connect(&controller, &ScreenshotPinnedEditController::qrRecognitionRequested,
                     &controller, [&]() { ++requests; });
    const snow_shot::storage::ScreenshotShortcutSettings settings;
    const auto original = settings.allShortcuts();
    for (const auto& [id, tool] :
         {std::pair{"text_recognition", ScreenshotToolPalette::Tool::Ocr},
          std::pair{"text_translation", ScreenshotToolPalette::Tool::TextTranslation},
          std::pair{"table_recognition", ScreenshotToolPalette::Tool::Table},
          std::pair{"qr_code_recognition", ScreenshotToolPalette::Tool::Qr}}) {
        auto bindings = original;
        for (auto& keys : bindings) {
            keys.clear();
        }
        bindings[QString::fromLatin1(id)] = {QStringLiteral("Ctrl+Alt+F12")};
        require(settings.setAllShortcutsAtomic(bindings), "pinned shortcut setup failed");
        palette->setActiveTool(ScreenshotToolPalette::Tool::Select);
        const int before = requests;
        sendShortcut(canvas, Qt::Key_F12, Qt::ControlModifier | Qt::AltModifier);
        require(requests == before + 1 && palette->activeToolForTests() == tool,
                "pinned recognition shortcut must activate and synchronize the toolbar item");
        sendShortcut(canvas, Qt::Key_F12, Qt::ControlModifier | Qt::AltModifier);
        require(requests == before + 1 &&
                    palette->activeToolForTests() == ScreenshotToolPalette::Tool::Select,
                "repeating pinned recognition must use the button's toggle-to-selection command");
    }
    require(settings.setAllShortcutsAtomic(original), "pinned shortcut restoration failed");
    controller.setEditMode(false);
}

void pinnedRecognitionShortcutTogglesResults() {
    const bool offscreen = QGuiApplication::platformName() == QStringLiteral("offscreen");
    auto config = cachedOcrPinConfig(nullptr);
    QPointer<ScreenshotPinnedWindow> window(new ScreenshotPinnedWindow());
    const auto cleanup = qScopeGuard([&]() {
        if (window != nullptr) {
            window->close();
            static_cast<void>(processUntilDeleted(window, 2000));
        }
    });
    if (offscreen) {
        window->show();
        window->activateWindow();
    } else {
        require(window->present(config), "the recognition shortcut pin should present");
    }
    waitForUi(50);
    auto* canvas = window->findChild<SnowCanvasWidget*>();
    QAction* action = pinnedMenuActionNamed(*window, QStringLiteral("screenshotPinnedOcrAction"));
    if (offscreen && action != nullptr) {
        // Native image presentation requires an HWND. Exercise shortcut/action parity here;
        // the native run additionally verifies the actual recognition visibility.
        QObject::disconnect(action, nullptr, window, nullptr);
        action->setEnabled(true);
    }
    require(canvas != nullptr && action != nullptr && action->isEnabled(),
            "the recognition shortcut fixture should expose cached OCR");
    require(!window->persistenceSnapshot().recognitionVisible,
            "recognition should initially be hidden");
    for (const bool visible : {true, false, true, false}) {
        sendShortcut(*canvas, Qt::Key_D, Qt::ControlModifier);
        require(action->isChecked() == visible &&
                    (offscreen || window->persistenceSnapshot().recognitionVisible == visible),
                "each recognition shortcut press must toggle result visibility");
    }
}

void pinnedSnapshotRetainsRecognitionBeforeDeferredSetup() {
    IdleOcrRecognition recognition;
    auto config = cachedOcrPinConfig(&recognition);
    config.recognitionVisible = true;
    QPointer<ScreenshotPinnedWindow> window(new ScreenshotPinnedWindow());
    const auto cleanup = qScopeGuard([&]() {
        if (window != nullptr) {
            window->close();
            static_cast<void>(processUntilDeleted(window, 2000));
        }
    });
    require(window->present(config), "the early snapshot pin should present");
    auto* session = window->findChild<ScreenshotRecognitionSessionController*>();
    require(session != nullptr && !session->hasTarget(),
            "the snapshot fixture should precede deferred recognition setup");
    const auto earlyRecord = window->persistenceSnapshot();
    require(!earlyRecord.recognitionResults.isEmpty() && earlyRecord.recognitionVisible,
            "a snapshot before deferred setup must retain OCR results and requested visibility");
    QCoreApplication::processEvents(QEventLoop::AllEvents, 50);
    require(session->hasTarget() && session->active() &&
                window->persistenceSnapshot().recognitionResults ==
                    earlyRecord.recognitionResults &&
                window->persistenceSnapshot().recognitionVisible && recognition.requests == 0,
            "recognition setup must restore the overlay without changing or re-requesting OCR");
    session->invalidate();
    require(window->persistenceSnapshot().recognitionResults.isEmpty(),
            "invalidating initialized recognition must not revive the original cached result");
}

void pinnedImageConversionsSurviveRestartWithoutProvider() {
    using Format = SnowShotImageConversionFormat;
    using Mode = ScreenshotRecognitionSessionController::Mode;
    snow_shot::storage::ScreenshotImageConversionSettings().setVisionModel(
        QStringLiteral("vision-saved"));
    for (const Format format : {Format::Markdown, Format::Html}) {
        IdleOcrRecognition recognition;
        auto config = cachedOcrPinConfig(&recognition);
        config.automaticTextRecognition = true;
        if (format == Format::Markdown) {
            config.recognitionResults.text.reset();
        }
        const QString source = format == Format::Markdown
                                   ? QStringLiteral("# Saved\n\n**Document**")
                                   : QStringLiteral("<h1>Saved</h1><p>Document</p>");
        config.recognitionResults.conversions = {{format, QStringLiteral("vision-saved"), source}};
        config.recognitionResults.visibleConversion = format;
        QByteArray payload;
        for (const bool restore : {false, true}) {
            config.restorePersistentState = restore;
            if (restore) {
                config.persistedRecognitionResults = payload;
                config.recognitionResults = {};
            }
            QPointer<ScreenshotPinnedWindow> window(new ScreenshotPinnedWindow);
            const auto cleanup = qScopeGuard([&]() {
                if (window) {
                    window->close();
                    static_cast<void>(processUntilDeleted(window, 2000));
                }
            });
            require(window->present(config), "conversion pin should present");
            auto* session = window->findChild<ScreenshotRecognitionSessionController*>();
            waitForUi(100);
            require(session && session->conversionModeActive() &&
                        session->mode() ==
                            (format == Format::Markdown ? Mode::Markdown : Mode::Html) &&
                        !session->busy(session->mode()),
                    "the last completed conversion is restored without a model provider");
            const auto mime = session->recognitionClipboardMimeData();
            require(mime && mime->text() == source,
                    "restored toolbar Copy preserves exact format source");
            const auto results = session->recognitionResultsSnapshot();
            require(results.text.has_value() == (format == Format::Html) &&
                        results.conversions.size() == 1 && results.visibleConversion == format,
                    "conversion persistence retains the existing OCR payload and visible format");
            require(recognition.requests == 0, "restoring a conversion never requires an OCR pass");
            const auto record = window->persistenceSnapshot();
            require(!record.recognitionResults.isEmpty(),
                    "conversion is included in the actual pin record");
            if (restore) {
                require(record.recognitionResults == payload,
                        "pin recognition payload round-trips exactly");
                session->deactivate();
                require(!session->recognitionResultsSnapshot().visibleConversion.has_value() &&
                            session->cachedRecognitionResults().conversions.size() == 1,
                        "hiding the preview retains the completed cache but not its visible state");
            }
            payload = record.recognitionResults;
        }
    }
}

void restoredInvalidOcrDoesNotSuppressRecognition() {
    for (const bool trailingBytes : {false, true}) {
        IdleOcrRecognition recognition;
        auto config = cachedOcrPinConfig(&recognition);
        const auto presentation = config.recognitionResults.text->presentation;
        QDataStream stream(&config.persistedRecognitionResults, QIODevice::WriteOnly);
        stream << config.recognitionResults.key << quint8(1) << quint8(0) << quint8(0)
               << (trailingBytes ? QString() : QStringLiteral("Recognition failed"))
               << presentation->selection << qint64(trailingBytes ? 0 : 1);
        const auto& line = presentation->lines.front();
        stream << line.text << line.confidence << line.quad << quint8(0);
        config.recognitionResults = {};
        config.restorePersistentState = true;
        config.persistedRecognitionVisible = true;
        config.automaticTextRecognition = true;
        QPointer<ScreenshotPinnedWindow> window(new ScreenshotPinnedWindow());
        const auto cleanup = qScopeGuard([&]() {
            if (window != nullptr) {
                window->close();
                static_cast<void>(processUntilDeleted(window, 2000));
            }
        });
        require(window->present(config), "the invalid OCR fixture should present");
        QCoreApplication::processEvents(QEventLoop::AllEvents, 50);
        auto* session = window->findChild<ScreenshotRecognitionSessionController*>();
        require(session != nullptr && !session->hasTextResult(),
                trailingBytes ? "an OCR payload with trailing data must not become an empty result"
                              : "a persisted OCR error must not become a successful cached result");
        require(!session->active() && !window->persistenceSnapshot().recognitionVisible,
                "invalid persisted recognition must leave the overlay hidden");
        QElapsedTimer elapsed;
        elapsed.start();
        while (recognition.requests == 0 && elapsed.elapsed() < 2000) {
            QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
        }
        require(recognition.requests == 1,
                "failed or malformed saved OCR must allow automatic recognition to retry");
    }
}

void cachedPinnedOcrAvailableWithoutRecognitionProvider() {
    auto config = cachedOcrPinConfig(nullptr);
    SnowShotTableResult table;
    table.html = QStringLiteral("<table><tr><td>Saved table</td></tr></table>");
    config.recognitionResults.table = table;
    config.recognitionResults.qr =
        ScreenshotQrRecognitionResult{{QStringLiteral("Saved barcode")}, {}};
    QPointer<ScreenshotPinnedWindow> window(new ScreenshotPinnedWindow());
    const auto cleanup = qScopeGuard([&]() {
        if (window != nullptr) {
            window->close();
            static_cast<void>(processUntilDeleted(window, 2000));
        }
    });
    require(window->present(config), "the provider-free cached pin should present");
    QCoreApplication::processEvents(QEventLoop::AllEvents, 50);
    QAction* action = pinnedMenuActionNamed(*window, QStringLiteral("screenshotPinnedOcrAction"));
    require(action != nullptr && action->isEnabled(),
            "cached text must remain available without a recognition provider");
    action->trigger();
    auto* content = window->findChild<ScreenshotRecognitionWindow*>(
        QStringLiteral("screenshotPinnedRecognitionContent"));
    auto* session = window->findChild<ScreenshotRecognitionSessionController*>();
    require(session != nullptr, "cached recognition should create a session");
    auto mimeData = session->recognitionClipboardMimeData();
    require(content != nullptr && content->isVisible() && mimeData != nullptr &&
                mimeData->text() == QStringLiteral("Saved OCR"),
            "cached text must render and copy without a recognition provider");
    require(window->findChild<ScreenshotPinnedEditController*>() == nullptr,
            "provider-free cached OCR must not create a toolbar for context-menu activation");
    auto* editButton =
        window->findChild<adqt::widgets::AdButton*>(QStringLiteral("screenshotPinnedEditButton"));
    require(editButton != nullptr, "find the drawing button for explicit toolbar activation");
    editButton->click();
    auto* controller = window->findChild<ScreenshotPinnedEditController*>();
    auto* toolbarWindow = controller != nullptr ? controller->toolbarWindow() : nullptr;
    auto* toolbar = toolbarWindow != nullptr ? toolbarWindow->palette() : nullptr;
    require(toolbar != nullptr && toolbarWindow->isVisible(),
            "explicit drawing activation should open the toolbar");
    auto* ocrButton = toolbarButtonNamed(*toolbar, QStringLiteral("Text recognition"));
    auto* tableQrButton = toolbar->findChild<QWidget*>(QStringLiteral("screenshotTableQrButton"));
    require(ocrButton != nullptr && ocrButton->isEnabled() && tableQrButton != nullptr &&
                tableQrButton->isEnabled(),
            "cached text, table and barcode controls must be enabled without providers");
    require(QMetaObject::invokeMethod(toolbar, "tableRequested", Qt::DirectConnection),
            "the table toolbar action should activate");
    auto* tableView = content->findChild<QTableView*>();
    mimeData = session->recognitionClipboardMimeData();
    require(tableView != nullptr && tableView->isVisible() &&
                tableView->model()->index(0, 0).data().toString() ==
                    QStringLiteral("Saved table") &&
                mimeData != nullptr && mimeData->text() == QStringLiteral("Saved table"),
            "cached tables must display and copy without a provider");
    require(QMetaObject::invokeMethod(toolbar, "qrRequested", Qt::DirectConnection),
            "the barcode toolbar action should activate");
    auto* browser = content->findChild<QTextBrowser*>(QStringLiteral("screenshotQrContents"));
    mimeData = session->recognitionClipboardMimeData();
    require(browser != nullptr && browser->isVisible() &&
                browser->toPlainText() == QStringLiteral("Saved barcode") && mimeData != nullptr &&
                mimeData->text() == QStringLiteral("Saved barcode"),
            "cached barcodes must display and copy without a provider");
    session->invalidate();
    require(!action->isEnabled() && !ocrButton->isEnabled() && !tableQrButton->isEnabled(),
            "invalidated results must not leave provider-free recognition controls enabled");
}

void pinnedTransformResetPersistsWithoutResize() {
    auto config = cachedOcrPinConfig(nullptr);
    snow_shot::storage::PinnedWindowRecord lastWritten;
    int writeCount = 0;
    config.persistenceWriter = [&](const snow_shot::storage::PinnedWindowRecord& record) {
        lastWritten = record;
        ++writeCount;
    };
    QPointer<ScreenshotPinnedWindow> window(new ScreenshotPinnedWindow());
    const auto cleanup = qScopeGuard([&]() {
        if (window != nullptr) {
            window->close();
            static_cast<void>(processUntilDeleted(window, 2000));
        }
    });
    require(window->present(config), "the transform reset fixture should present");
    waitForUi(400);
    auto* menu = window->findChild<adqt::widgets::AdContextMenu*>(
        QStringLiteral("screenshotPinnedProcessImageMenu"));
    require(menu != nullptr && menu->actions().size() == 6,
            "the transform reset fixture should expose its image actions");
    for (const int operation : {2, 0}) {
        const QRect geometry = window->currentNativeGeometry();
        menu->actions().at(operation)->trigger();
        if (operation == 0) {
            menu->actions().at(operation)->trigger();
        }
        waitForUi(400);
        require(!lastWritten.imageTransform.isIdentity() && lastWritten.nativeGeometry == geometry,
                "the flip or half-turn must persist without changing the window size");
        const int previousWrites = writeCount;
        menu->actions().back()->trigger();
        QElapsedTimer elapsed;
        elapsed.start();
        while (writeCount == previousWrites && elapsed.elapsed() < 2000) {
            QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
            QThread::msleep(1);
        }
        require(writeCount > previousWrites && lastWritten.imageTransform.isIdentity() &&
                    lastWritten.quarterTurns == 0 && lastWritten.nativeGeometry == geometry &&
                    !lastWritten.recognitionResults.isEmpty(),
                "resetting a flip or half-turn must persist the reset and retain OCR");
    }
}

void transformedPinnedOcrTracksCanvasViewport() {
    for (const int shadowWidth : {0, 12}) {
        IdleOcrRecognition recognition;
        auto config = cachedOcrPinConfig(&recognition, shadowWidth);
        QPointer<ScreenshotPinnedWindow> window(new ScreenshotPinnedWindow());
        const auto cleanup = qScopeGuard([&]() {
            if (window != nullptr) {
                window->close();
                static_cast<void>(processUntilDeleted(window, 2000));
            }
        });
        require(window->present(config), "the transformed OCR fixture should present");
        QCoreApplication::processEvents(QEventLoop::AllEvents, 50);
        QAction* action =
            pinnedMenuActionNamed(*window, QStringLiteral("screenshotPinnedOcrAction"));
        action->trigger();
        const auto verifyAlignment = [&]() {
            QCoreApplication::processEvents(QEventLoop::AllEvents, 50);
            auto* canvas = window->findChild<SnowCanvasWidget*>();
            auto* content = window->findChild<ScreenshotRecognitionWindow*>();
            auto* layer =
                content != nullptr
                    ? content->findChild<QGraphicsView*>(QStringLiteral("snowShotOcrTextLayer"))
                    : nullptr;
            if (canvas == nullptr || layer == nullptr || layer->scene()->items().size() != 1) {
                auto* session = window->findChild<ScreenshotRecognitionSessionController*>();
                std::cerr << "shadow=" << shadowWidth
                          << " turns=" << window->persistenceSnapshot().quarterTurns
                          << " restored=" << config.restorePersistentState
                          << " action=" << action->isEnabled() << '/' << action->isChecked()
                          << " target=" << (session != nullptr && session->hasTarget())
                          << " cached=" << (session != nullptr && session->hasTextResult())
                          << " active=" << (session != nullptr && session->active()) << '\n';
            }
            require(canvas != nullptr && layer != nullptr && layer->scene()->items().size() == 1,
                    "the transformed pin should retain its OCR text layer");
            const auto record = window->persistenceSnapshot();
            const QPointF sourcePoint = config.recognitionResults.text->presentation->lines.front()
                                            .quad.boundingRect()
                                            .center();
            const QPointF transformedPoint =
                record.contentCanvasRect.topLeft() +
                record.imageTransform.map(sourcePoint - config.canvasSourceRect.topLeft());
            const QPointF expected = canvas->canvasToViewTransform().map(transformedPoint);
            auto* item = layer->scene()->items().front();
            const QPointF actual =
                layer->viewportTransform().map(item->sceneBoundingRect().center());
            require(item->isVisible() && QLineF(actual, expected).length() < 3.0,
                    "OCR text must follow the image viewport after transforms and restoration");
        };
        verifyAlignment();
        for (int rotation = 0; rotation < 4; ++rotation) {
            auto* menu = window->findChild<adqt::widgets::AdContextMenu*>(
                QStringLiteral("screenshotPinnedProcessImageMenu"));
            require(menu != nullptr, "the image transform menu should exist");
            menu->actions().front()->trigger();
            verifyAlignment();
            const auto record = window->persistenceSnapshot();
            window->close();
            require(processUntilDeleted(window, 2000), "the transformed pin should close");
            config.restorePersistentState = true;
            config.nativeGeometry = record.nativeGeometry;
            config.persistedImageTransform = record.imageTransform;
            config.persistedQuarterTurns = record.quarterTurns;
            config.persistedRecognitionResults = record.recognitionResults;
            window = new ScreenshotPinnedWindow();
            require(window->present(config), "the transformed pin should restore");
            QCoreApplication::processEvents(QEventLoop::AllEvents, 50);
            action = pinnedMenuActionNamed(*window, QStringLiteral("screenshotPinnedOcrAction"));
            action->trigger();
            verifyAlignment();
        }
    }
}

void pinnedWindowPoolReusesAndReplenishesPreparedShell() {
    QScreen* screen = QGuiApplication::primaryScreen();
    require(screen != nullptr, "a primary screen is required");
    require(topLevelPinnedWindows().isEmpty(),
            "the pooling test must start without pinned windows");

    ScreenshotSelectionExportUiServices services;
    services.prewarmPinnedWindow(screen);
    QVector<ScreenshotPinnedWindow*> windows = topLevelPinnedWindows();
    require(windows.size() == 1 && !windows.front()->isVisible() && windows.front()->winId() != 0,
            "prewarming should create one hidden native pinned shell");
    QPointer<ScreenshotPinnedWindow> firstPrepared(windows.front());

    services.prewarmPinnedWindow(screen);
    windows = topLevelPinnedWindows();
    require(windows.size() == 1 && windows.front() == firstPrepared,
            "repeated prewarming should preserve the single prepared shell");

    QImage firstImage(QSize(160, 96), QImage::Format_ARGB32_Premultiplied);
    firstImage.fill(QColor(36, 132, 204));
    const QRect firstGeometry = physicalPinGeometry(*screen, QPoint(60, 60), firstImage.size());
    int firstCompletionCount = 0;
    bool firstCompletionSucceeded = false;
    require(services.presentPinnedImage(
                firstImage, screen, firstGeometry, firstImage.size(), {}, {}, 1.0, {}, {},
                [&firstCompletionCount, &firstCompletionSucceeded](bool succeeded, QImage image) {
                    ++firstCompletionCount;
                    firstCompletionSucceeded = succeeded && !image.isNull();
                }),
            "the first pooled pinned image could not be presented");
    require(firstPrepared != nullptr && firstPrepared->isVisible() &&
                firstPrepared->currentNativeGeometry() == firstGeometry,
            "the first presentation should consume the prepared shell");
    waitForUi(100);
    require(firstCompletionCount == 1 && firstCompletionSucceeded,
            "the first pooled presentation should complete exactly once");

    QPointer<ScreenshotPinnedWindow> secondPrepared(hiddenPinnedWindowExcept({firstPrepared}));
    require(secondPrepared != nullptr && secondPrepared->winId() != 0 &&
                topLevelPinnedWindows().size() == 2,
            "the pool should replenish one hidden native shell after the first frame");
    services.prewarmPinnedWindow(screen);
    require(hiddenPinnedWindowExcept({firstPrepared}) == secondPrepared &&
                topLevelPinnedWindows().size() == 2,
            "prewarming a replenished pool should not exceed one spare");

    auto* firstFocusMenu = firstPrepared->findChild<adqt::widgets::AdContextMenu*>(
        QStringLiteral("screenshotPinnedFocusMenu"));
    require(firstFocusMenu != nullptr && firstFocusMenu->actions().size() == 4,
            "the first pinned focus menu was not found");
    firstFocusMenu->actions().front()->trigger();
    QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
    require(secondPrepared != nullptr && !secondPrepared->isVisible(),
            "showing all pinned windows must ignore the hidden pool spare");

    QImage secondImage(QSize(120, 80), QImage::Format_ARGB32_Premultiplied);
    secondImage.fill(QColor(84, 168, 112));
    const QRect secondGeometry = physicalPinGeometry(*screen, QPoint(260, 60), secondImage.size());
    int secondCompletionCount = 0;
    require(services.presentPinnedImage(secondImage, screen, secondGeometry, secondImage.size(), {},
                                        {}, 1.0, {}, {},
                                        [&secondCompletionCount](bool succeeded, QImage image) {
                                            if (succeeded && !image.isNull()) {
                                                ++secondCompletionCount;
                                            }
                                        }),
            "the second pooled pinned image could not be presented");
    require(secondPrepared != nullptr && secondPrepared->isVisible() &&
                secondPrepared->currentNativeGeometry() == secondGeometry,
            "the second presentation should consume the replenished shell");
    waitForUi(100);
    require(secondCompletionCount == 1,
            "the second pooled presentation should complete exactly once");

    QPointer<ScreenshotPinnedWindow> finalPrepared(
        hiddenPinnedWindowExcept({firstPrepared, secondPrepared}));
    require(finalPrepared != nullptr && topLevelPinnedWindows().size() == 3,
            "the pool should replenish after every successful presentation");
    auto* secondFocusMenu = secondPrepared->findChild<adqt::widgets::AdContextMenu*>(
        QStringLiteral("screenshotPinnedFocusMenu"));
    require(secondFocusMenu != nullptr && secondFocusMenu->actions().size() == 4,
            "the second pinned focus menu was not found");
    secondFocusMenu->actions().back()->trigger();
    require(processUntilDeleted(firstPrepared, 2000) && processUntilDeleted(secondPrepared, 2000),
            "closing all presented pins should delete both visible windows");
    require(finalPrepared != nullptr && !finalPrepared->isVisible(),
            "closing all presented pins must preserve the hidden pool spare");

    ScreenshotImageLoadCallback failedLoad;
    const ScreenshotImageLoader failingLoader =
        [&failedLoad](QObject*, ScreenshotImageLoadCallback callback) {
            failedLoad = std::move(callback);
        };
    int failureCompletionCount = 0;
    bool failureCompletionSucceeded = true;
    const QSize failedImageSize(96, 64);
    const QRect failedGeometry = physicalPinGeometry(*screen, QPoint(60, 200), failedImageSize);
    require(services.presentPinnedImage(
                {}, screen, failedGeometry, failedImageSize, {}, {}, 1.0, {}, failingLoader,
                [&failureCompletionCount, &failureCompletionSucceeded](bool succeeded, QImage) {
                    ++failureCompletionCount;
                    failureCompletionSucceeded = succeeded;
                }),
            "the failing pooled pinned image could not create its shell");
    QPointer<ScreenshotPinnedWindow> failedWindow(finalPrepared);
    require(failedWindow != nullptr && failedWindow->isVisible() && static_cast<bool>(failedLoad),
            "the failing presentation should consume the final prepared shell");
    failedLoad({});
    require(processUntilDeleted(failedWindow, 2000),
            "a failed pooled presentation should close its consumed shell");
    waitForUi(100);
    require(failureCompletionCount == 1 && !failureCompletionSucceeded,
            "a failed pooled presentation should complete exactly once");
    ScreenshotPinnedWindow* recoveredSpare = hiddenPinnedWindowExcept({});
    require(recoveredSpare != nullptr && topLevelPinnedWindows().size() == 1 &&
                recoveredSpare->winId() != 0,
            "the pool should recover one prepared shell after presentation failure");
}

// A pin that is presented while the capture-side recognition feature has never
// been activated only carries the lazy recognition provider. Resolving it must
// not depend on the recognition actions already being usable.
void pinnedRecognitionAvailableThroughLazyProvider() {
    QScreen* screen = QGuiApplication::primaryScreen();
    require(screen != nullptr, "a primary screen is required");

    IdleOcrRecognition ocrRecognition;
    ImmediateQrRecognition qrRecognition({QStringLiteral("https://example.com/lazy-provider")});
    int providerConsultations = 0;
    ScreenshotPinnedWindow::Config config;
    auto provider = [&]() {
        ++providerConsultations;
        ScreenshotPinnedRecognitionProviders providers;
        providers.recognition = &ocrRecognition;
        providers.qrRecognition = &qrRecognition;
        return providers;
    };

    QImage background(320, 180, QImage::Format_ARGB32_Premultiplied);
    background.fill(QColor(42, 84, 126, 255));
    auto* pinnedWindow = new ScreenshotPinnedWindow();
    QPointer<ScreenshotPinnedWindow> guardedWindow(pinnedWindow);
    config.nativeGeometry = physicalPinGeometry(*screen, QPoint(40, 40), background.size());
    config.canvasSourceRect = QRectF(QPointF(), QSizeF(background.size()));
    config.imageSource = ScreenshotImageSource::fromImage(background, config.canvasSourceRect);
    config.screen = screen;
    config.enableEditing = true;
    config.automaticTextRecognition = false;
    config.recognitionProvider = provider;
    require(pinnedWindow->present(config), "lazy provider pin presentation failed");
    waitForUi(50);

    require(providerConsultations > 0, "presenting a pin must resolve its recognition provider");
    QAction* ocrAction =
        pinnedMenuActionNamed(*pinnedWindow, QStringLiteral("screenshotPinnedOcrAction"));
    require(ocrAction != nullptr, "lazy provider pin should expose its recognition action");
    require(ocrAction->isEnabled(),
            "pinned text recognition must be usable without prior capture-toolbar recognition");

    auto* editButton = pinnedWindow->findChild<adqt::widgets::AdButton*>(
        QStringLiteral("screenshotPinnedEditButton"));
    require(editButton != nullptr, "lazy provider edit button was not found");
    editButton->click();
    waitForUi(50);
    auto* editController = pinnedWindow->findChild<ScreenshotPinnedEditController*>();
    auto* toolbarWindow = editController != nullptr ? editController->toolbarWindow() : nullptr;
    auto* toolbar = toolbarWindow != nullptr ? toolbarWindow->palette() : nullptr;
    require(toolbar != nullptr, "lazy provider toolbar was not created");
    adqt::widgets::AdButton* ocrButton =
        toolbarButtonNamed(*toolbar, QStringLiteral("Text recognition"));
    require(ocrButton != nullptr, "lazy provider toolbar should expose a recognition button");
    require(ocrButton->isEnabled(),
            "pinned toolbar text recognition must be usable without prior capture-toolbar "
            "recognition");
    require(ocrRecognition.requests == 0,
            "a pin without automatic recognition must not recognize on its own");
    require(QMetaObject::invokeMethod(toolbar, "ocrRequested", Qt::DirectConnection),
            "pinned text recognition should activate from the toolbar trigger");
    QElapsedTimer manualRecognitionWait;
    manualRecognitionWait.start();
    while (ocrRecognition.requests == 0 && manualRecognitionWait.elapsed() < 5000) {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
        QThread::msleep(1);
    }
    require(ocrRecognition.requests == 1,
            "manually triggered text recognition must start through the lazy provider");

    pinnedWindow->close();
    require(processUntilDeleted(guardedWindow, 2000), "lazy provider pin was not deleted");

    // Automatic text recognition on pin derives from the same lazily resolved
    // pointers and must prefetch without prior user activation.
    int automaticProviderConsultations = 0;
    auto automaticProvider = [&]() {
        ++automaticProviderConsultations;
        ScreenshotPinnedRecognitionProviders providers;
        providers.recognition = &ocrRecognition;
        return providers;
    };
    auto* automaticWindow = new ScreenshotPinnedWindow();
    QPointer<ScreenshotPinnedWindow> guardedAutomaticWindow(automaticWindow);
    config.nativeGeometry = physicalPinGeometry(*screen, QPoint(80, 80), background.size());
    config.automaticTextRecognition = true;
    config.recognitionProvider = automaticProvider;
    require(automaticWindow->present(config), "automatic recognition pin presentation failed");
    const int requestsBeforeAutomaticRecognition = ocrRecognition.requests;
    QElapsedTimer prefetchWait;
    prefetchWait.start();
    while (ocrRecognition.requests == requestsBeforeAutomaticRecognition &&
           prefetchWait.elapsed() < 5000) {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
        QThread::msleep(1);
    }
    require(automaticProviderConsultations > 0,
            "an automatically recognizing pin must resolve its recognition provider");
    require(ocrRecognition.requests > requestsBeforeAutomaticRecognition,
            "automatic text recognition must prefetch through the lazy provider");

    automaticWindow->close();
    require(processUntilDeleted(guardedAutomaticWindow, 2000),
            "automatic recognition pin was not deleted");
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

void pinnedLargeImageRemainsOpenWhenEnteringDrawingMode(SnowCanvasRuntime&) {
    QScreen* screen = QGuiApplication::primaryScreen();
    require(screen != nullptr, "a primary screen is required");

    auto* pinnedWindow = new ScreenshotPinnedWindow();
    QPointer<ScreenshotPinnedWindow> guardedWindow(pinnedWindow);
    QImage background(400, 40000, QImage::Format_ARGB32_Premultiplied);
    background.fill(QColor(42, 84, 126));

    ScreenshotPinnedWindow::Config config;
    const QSize displayedSize(background.width() / 10, background.height() / 10);
    config.nativeGeometry = physicalPinGeometry(*screen, QPoint(40, 40), displayedSize);
    config.canvasSourceRect = QRectF(QPointF(0.0, 0.0), QSizeF(background.size()));
    config.imageSource = ScreenshotImageSource::fromImage(background, config.canvasSourceRect);
    config.fullResolutionScaleBasis = background.size();
    config.screen = screen;
    config.enableEditing = true;
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

    auto* pinnedWindow = new ScreenshotPinnedWindow();
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
    require(canvas->setCanvasShapeStylePatch(pinnedShapeStyle,
                                             SnowCanvasShapeStylePropertyStrokeColor |
                                                 SnowCanvasShapeStylePropertyStrokeWidth,
                                             SnowCanvasShapeKind::Rectangle),
            "pinned canvas should configure its independent annotation style");
    const auto sendPinnedPointerEvent = [&canvas](QEvent::Type type, const QPointF& position,
                                                  Qt::MouseButton button,
                                                  Qt::MouseButtons buttons) {
        QMouseEvent event(type, position, canvas->mapToGlobal(position.toPoint()), button, buttons,
                          Qt::NoModifier);
        QCoreApplication::sendEvent(canvas, &event);
    };
    sendPinnedPointerEvent(QEvent::MouseButtonPress, QPointF(48.0, 38.0), Qt::LeftButton,
                           Qt::LeftButton);
    sendPinnedPointerEvent(QEvent::MouseMove, QPointF(152.0, 82.0), Qt::NoButton, Qt::LeftButton);
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
    adqt::widgets::AdButton* copyButton =
        toolbar != nullptr ? toolbarButtonNamed(*toolbar, QStringLiteral("Copy to clipboard"))
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
    require(canvas->canvasTool() == SnowCanvasTool::Shape,
            "copying from the pinned toolbar must preserve the active drawing tool");
    require(editController->editMode() && toolbarWindow->isVisible(),
            "copying from the pinned toolbar must keep the editing session open");

    QPushButton* closeButton = buttonNamed(*pinnedWindow, QStringLiteral("Close"));
    require(closeButton != nullptr, "pinned copy close button was not found");
    closeButton->click();
    require(processUntilDeleted(guardedWindow, 2000),
            "pinned window was not deleted after the copy regression test");
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
    explicit PinnedPresentationObserver(ScreenshotPinnedWindow& window) : m_window(window) {
        m_window.installEventFilter(this);
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
        }
        return false;
    }

  private:
    ScreenshotPinnedWindow& m_window;
    bool m_showSeen = false;
    WId m_windowIdAtShow = 0;
    QRect m_geometryAtShow;
    int m_windowIdChangesAfterShow = 0;
    int m_geometryChangesAfterShow = 0;
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
        config.imageSource = ScreenshotImageSource::fromImage(background, config.canvasSourceRect);
        config.screen = screen;
        config.enableEditing = false;

        auto* pinnedWindow = new ScreenshotPinnedWindow();
        QPointer<ScreenshotPinnedWindow> guardedWindow(pinnedWindow);
        auto* canvas = pinnedWindow->findChild<SnowCanvasWidget*>();
        auto* controls =
            pinnedWindow->findChild<QFrame*>(QStringLiteral("screenshotPinnedControlsPanel"));
        auto* border = pinnedWindow->findChild<QFrame*>(QStringLiteral("screenshotPinnedBorder"));
        require(canvas != nullptr && controls != nullptr && border != nullptr,
                "physical pin widgets were not found");
        PinnedPresentationObserver observer(*pinnedWindow);
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

    auto* pinnedWindow = new ScreenshotPinnedWindow();
    QPointer<ScreenshotPinnedWindow> guardedWindow(pinnedWindow);
    ScreenshotPinnedWindow::Config config;
    // Deliberately avoid a DPI-aligned origin. Qt's integer logical position
    // cannot represent this rectangle exactly on every fractional-DPI screen.
    config.nativeGeometry = QRect(physicalScreen.topLeft() + QPoint(47, 53), background.size());
    config.canvasSourceRect = QRectF(QPointF(), QSizeF(background.size()));
    config.imageSource = ScreenshotImageSource::fromImage(background, config.canvasSourceRect);
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
    auto* successfulWindow = new ScreenshotPinnedWindow();
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
    SnowCanvasWidget* successCanvas = successfulWindow->findChild<SnowCanvasWidget*>();
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
    auto* failedWindow = new ScreenshotPinnedWindow();
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
    auto* closedWindow = new ScreenshotPinnedWindow();
    QPointer<ScreenshotPinnedWindow> guardedClosedWindow(closedWindow);
    int closeCompletionCount = 0;
    bool closeCompletionValue = true;
    const ScreenshotImageLoader closeLoader = [&closeLoad](QObject*,
                                                           ScreenshotImageLoadCallback callback) {
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

void pinnedDeferredPresentationSurvivesGroupSwitch(SnowCanvasRuntime&) {
    QScreen* screen = QGuiApplication::primaryScreen();
    require(screen != nullptr, "a primary screen is required");

    QTemporaryDir directory;
    require(directory.isValid(), "temporary storage directory is unavailable");
    snow_shot::storage::PinnedWindowRepository repository(directory.path());
    snow_shot::presentation::PinnedWindowGroupManager groupManager(&repository);
    const auto inactiveGroup = groupManager.createGroup(QStringLiteral("Inactive"));
    require(inactiveGroup.has_value(), "the inactive test group should be created");

    const QString persistenceId = QUuid::createUuid().toString(QUuid::WithoutBraces);
    QImage materializedImage(QSize(160, 96), QImage::Format_ARGB32_Premultiplied);
    materializedImage.fill(QColor(84, 168, 112));
    ScreenshotImageLoadCallback deferredLoad;
    ScreenshotPinnedWindow::Config config;
    config.nativeGeometry = physicalPinGeometry(*screen, QPoint(60, 60), materializedImage.size());
    config.canvasSourceRect = QRectF(QPointF(), QSizeF(materializedImage.size()));
    config.contentCanvasRect = config.canvasSourceRect;
    config.surfaceCanvasRect = config.canvasSourceRect;
    config.fullResolutionScaleBasis = materializedImage.size();
    config.screen = screen;
    config.enableEditing = true;
    config.groupManager = &groupManager;
    config.groupId = groupManager.activeGroupId();
    config.persistenceId = persistenceId;
    config.imageLoader = [&deferredLoad](QObject*, ScreenshotImageLoadCallback callback) {
        deferredLoad = std::move(callback);
    };
    config.persistenceWriter = [&repository](const snow_shot::storage::PinnedWindowRecord& record) {
        static_cast<void>(repository.upsert(record));
    };
    config.persistenceRemover = [&repository](const QString& id) {
        static_cast<void>(repository.remove(id));
    };

    auto* window = new ScreenshotPinnedWindow();
    QPointer<ScreenshotPinnedWindow> guardedWindow(window);
    require(window->present(config), "deferred pinned presentation failed to create its shell");
    QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
    require(static_cast<bool>(deferredLoad),
            "the deferred pinned loader should start after the shell is shown");

    require(groupManager.setActiveGroup(*inactiveGroup),
            "switching away from a loading pinned window should succeed");
    QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
    require(guardedWindow != nullptr,
            "switching groups must retain a loading pinned window until its image is materialized");
    require(repository.summaries().isEmpty(),
            "a loading pinned window must not be persisted with a null image");

    require(groupManager.setActiveGroup(QStringLiteral("default")),
            "switching back to the loading pinned window's group should succeed");
    require(guardedWindow != nullptr,
            "switching back should cancel a deferred inactive-group close");
    deferredLoad(materializedImage);
    QCoreApplication::processEvents(QEventLoop::AllEvents, 50);
    const auto persisted = repository.loadRecord(persistenceId);
    require(persisted.has_value() && persisted->id == persistenceId && !persisted->image.isNull(),
            "materializing a retained loading pin should persist its image");

    window->close();
    require(processUntilDeleted(guardedWindow, 2000),
            "the retained loading pinned window was not deleted after the test");

    const QString inactivePersistenceId = QUuid::createUuid().toString(QUuid::WithoutBraces);
    config.persistenceId = inactivePersistenceId;
    ScreenshotImageLoadCallback inactiveLoad;
    config.imageLoader = [&inactiveLoad](QObject*, ScreenshotImageLoadCallback callback) {
        inactiveLoad = std::move(callback);
    };
    auto* inactiveWindow = new ScreenshotPinnedWindow();
    QPointer<ScreenshotPinnedWindow> guardedInactiveWindow(inactiveWindow);
    require(inactiveWindow->present(config),
            "the inactive deferred pinned presentation failed to create its shell");
    QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
    require(static_cast<bool>(inactiveLoad),
            "the inactive deferred pinned loader should start after the shell is shown");
    require(groupManager.setActiveGroup(*inactiveGroup),
            "switching away from the second loading pinned window should succeed");
    inactiveLoad(materializedImage);
    QCoreApplication::processEvents(QEventLoop::AllEvents, 50);
    require(processUntilDeleted(guardedInactiveWindow, 2000),
            "an inactive loading pinned window should close after materialization");
    const auto inactiveRecord = repository.loadRecord(inactivePersistenceId);
    require(inactiveRecord.has_value() && !inactiveRecord->image.isNull(),
            "an inactive loading pinned window should persist after materialization");
}

void deferredPinUserCloseCancelsLateMaterialization() {
    QScreen* screen = QGuiApplication::primaryScreen();
    require(screen != nullptr, "a primary screen is required");

    QImage deliveredImage(QSize(160, 96), QImage::Format_ARGB32_Premultiplied);
    deliveredImage.fill(QColor(84, 168, 112));
    ScreenshotImageLoadCallback deferredLoad;
    ScreenshotImageLoader loader = [&deferredLoad](QObject*, ScreenshotImageLoadCallback callback) {
        deferredLoad = std::move(callback);
    };

    ScreenshotSelectionExportUiServices services;
    const QRect geometry = physicalPinGeometry(*screen, QPoint(60, 60), deliveredImage.size());
    require(services.presentPinnedImage({}, screen, geometry, deliveredImage.size(), {}, {}, 1.0,
                                        {}, std::move(loader)),
            "a loader-backed pinned presentation failed to create its shell");
    QCoreApplication::processEvents(QEventLoop::AllEvents, 50);
    require(static_cast<bool>(deferredLoad), "the pending pinned loader was not started");

    ScreenshotPinnedWindow* window = nullptr;
    for (QWidget* widget : QApplication::topLevelWidgets()) {
        auto* candidate = qobject_cast<ScreenshotPinnedWindow*>(widget);
        if (candidate != nullptr && candidate->findChild<QAction*>(
                                        QStringLiteral("screenshotPinnedCloseAction")) != nullptr) {
            require(window == nullptr, "the pending close test found multiple pinned windows");
            window = candidate;
        }
    }
    require(window != nullptr, "the pending pinned shell was not discoverable");
    const QString persistenceId = window->persistenceId();
    QPointer<ScreenshotPinnedWindow> guardedWindow(window);
    auto* closeAction = window->findChild<QAction*>(QStringLiteral("screenshotPinnedCloseAction"));
    require(closeAction != nullptr, "the pending pinned close action was not found");
    closeAction->trigger();
    require(processUntilDeleted(guardedWindow, 2000),
            "the user-closed pending pinned shell was not deleted");

    deferredLoad(std::move(deliveredImage));
    QCoreApplication::processEvents(QEventLoop::AllEvents, 100);
    const auto summaries =
        snow_shot::storage::ApplicationStorage::instance().pinnedWindows().summaries();
    require(
        std::none_of(summaries.cbegin(), summaries.cend(),
                     [&persistenceId](const auto& summary) { return summary.id == persistenceId; }),
        "a late materialization callback must not resurrect a user-closed pin");
}

void pinnedControlsMatchReferenceStyle(SnowCanvasRuntime&) {
    QScreen* screen = QGuiApplication::primaryScreen();
    require(screen != nullptr, "a primary screen is required");

    auto* pinnedWindow = new ScreenshotPinnedWindow();
    QPointer<ScreenshotPinnedWindow> guardedWindow(pinnedWindow);
    QImage background(400, 400, QImage::Format_ARGB32_Premultiplied);
    background.fill(QColor(42, 84, 126));

    ScreenshotPinnedWindow::Config config;
    config.nativeGeometry = physicalPinGeometry(*screen, QPoint(40, 40), background.size());
    config.canvasSourceRect = QRectF(QPointF(0.0, 0.0), QSizeF(background.size()));
    config.imageSource = ScreenshotImageSource::fromImage(background, config.canvasSourceRect);
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

void pinnedShortcutDisplayUsesSettingsFormat() {
    ScreenshotPinnedWindow window;
    auto* action = window.findChild<QAction*>(QStringLiteral("screenshotPinnedDrawingAction"));
    require(action != nullptr, "the pinned menu should be available without showing a window");
    const snow_shot::storage::PinToScreenShortcutSettings shortcuts;
    const QStringList original = shortcuts.shortcuts(QStringLiteral("drawing_mode"));
    require(shortcuts.setShortcuts(QStringLiteral("drawing_mode"),
                                   {QStringLiteral("Ctrl++"), QStringLiteral("Num+1")}),
            "the pinned shortcut should accept plus and keypad keys");
    require(action->text().endsWith(QStringLiteral("\tCtrl+Plus / Num 1")),
            "pinned menus must use the settings key names and alternative separator");
    QEvent languageChange(QEvent::LanguageChange);
    QCoreApplication::sendEvent(&window, &languageChange);
    require(action->text().endsWith(QStringLiteral("\tCtrl+Plus / Num 1")),
            "pinned key display must retain the settings format after retranslation");
    require(shortcuts.setShortcuts(QStringLiteral("drawing_mode"), original),
            "the pinned shortcut fixture should restore its original shortcuts");
}

void pinnedConfiguredShortcutUpdatesImmediately(SnowCanvasRuntime&) {
    QScreen* screen = QGuiApplication::primaryScreen();
    require(screen != nullptr, "a primary screen is required");

    QImage background(160, 90, QImage::Format_ARGB32_Premultiplied);
    background.fill(Qt::white);
    auto* pinnedWindow = new ScreenshotPinnedWindow();
    QPointer<ScreenshotPinnedWindow> guardedWindow(pinnedWindow);
    ScreenshotPinnedWindow::Config config;
    config.nativeGeometry = physicalPinGeometry(*screen, QPoint(60, 60), background.size());
    config.canvasSourceRect = QRectF(QPointF(), QSizeF(background.size()));
    config.imageSource = ScreenshotImageSource::fromImage(background, config.canvasSourceRect);
    config.screen = screen;
    config.enableEditing = true;
    require(pinnedWindow->present(config), "shortcut test pin presentation failed");
    auto* canvas = pinnedWindow->findChild<SnowCanvasWidget*>();
    auto* drawingAction =
        pinnedWindow->findChild<QAction*>(QStringLiteral("screenshotPinnedDrawingAction"));
    require(canvas != nullptr && drawingAction != nullptr,
            "shortcut test pin controls were not found");
    waitForUi(50);
#if defined(Q_OS_WIN) || defined(_WIN32)
    require(GetForegroundWindow() == toNativeHwnd(pinnedWindow->winId()),
            "a newly created pinned window should take foreground focus");
#else
    require(pinnedWindow->isActiveWindow(),
            "a newly created pinned window should take window focus");
#endif
    require(canvas->hasFocus(),
            "a newly created pinned window should focus its keyboard interaction surface");
    drawingAction->setChecked(true);
    drawingAction->setChecked(false);
    auto* editController = pinnedWindow->findChild<ScreenshotPinnedEditController*>();
    auto* toolbarWindow = editController != nullptr ? editController->toolbarWindow() : nullptr;
    auto* confirmButton =
        toolbarWindow != nullptr && toolbarWindow->palette() != nullptr
            ? buttonNamed(*toolbarWindow->palette(), QStringLiteral("Confirm edit"))
            : nullptr;
    require(confirmButton != nullptr &&
                confirmButton->toolTip() == QStringLiteral("Confirm edit (Space)"),
            "pinned Confirm Edit should show the default drawing-mode shortcut");

    const snow_shot::storage::PinToScreenShortcutSettings shortcuts;
    require(shortcuts.setShortcuts(QStringLiteral("drawing_mode"), {QStringLiteral("Ctrl+Alt+E")}),
            "the pinned drawing-mode shortcut should be configurable");
    waitForUi(50);
    require(drawingAction->text().endsWith(QStringLiteral("\tCtrl+Alt+E")),
            "an open pinned window should refresh its menu shortcut display immediately");
    require(confirmButton->toolTip() == QStringLiteral("Confirm edit (Ctrl+Alt+E)"),
            "pinned Confirm Edit should refresh its drawing-mode shortcut hint immediately");
    sendShortcut(*canvas, Qt::Key_E, Qt::ControlModifier);
    require(!drawingAction->isChecked(),
            "the previous pinned drawing-mode shortcut should stop activating immediately");
    sendShortcut(*canvas, Qt::Key_E, Qt::ControlModifier | Qt::AltModifier);
    require(drawingAction->isChecked(),
            "the configured pinned drawing-mode shortcut should activate immediately");
    drawingAction->setChecked(false);
    require(shortcuts.setShortcuts(QStringLiteral("drawing_mode"), {QStringLiteral("Space")}),
            "the pinned drawing-mode shortcut fixture should restore its default");
    waitForUi(50);
    require(drawingAction->text().endsWith(QStringLiteral("\tSpace")),
            "restoring a pinned shortcut should immediately restore the menu display");

    pinnedWindow->close();
    require(processUntilDeleted(guardedWindow, 2000), "shortcut test pin was not deleted");
}

void pinnedMovementShortcutsMoveIdleWindow() {
    QScreen* screen = QGuiApplication::primaryScreen();
    require(screen != nullptr, "a primary screen is required");
    QImage background(240, 140, QImage::Format_ARGB32_Premultiplied);
    background.fill(Qt::white);
    auto* pinnedWindow = new ScreenshotPinnedWindow();
    pinnedWindow->setAttribute(Qt::WA_DontShowOnScreen);
    QPointer<ScreenshotPinnedWindow> guardedWindow(pinnedWindow);
    ScreenshotPinnedWindow::Config config;
    config.nativeGeometry = physicalPinGeometry(*screen, QPoint(120, 120), background.size());
    config.canvasSourceRect = QRectF(QPointF(), QSizeF(background.size()));
    config.imageSource = ScreenshotImageSource::fromImage(background, config.canvasSourceRect);
    config.screen = screen;
    config.automaticTextRecognition = false;
    require(pinnedWindow->present(config), "keyboard movement pin presentation failed");
    waitForUi(50);
    pinnedWindow->activateWindow();
    auto* canvas = pinnedWindow->findChild<SnowCanvasWidget*>();
    require(canvas != nullptr, "keyboard movement pin canvas was not found");
    canvas->setFocus();
    const QPoint cursorBefore = systemCursorPosition();
    const struct {
        Qt::Key key;
        QPoint delta;
    } movements[] = {
        {Qt::Key_W, QPoint(0, -1)},   {Qt::Key_Up, QPoint(0, -1)},   {Qt::Key_S, QPoint(0, 1)},
        {Qt::Key_Down, QPoint(0, 1)}, {Qt::Key_A, QPoint(-1, 0)},    {Qt::Key_Left, QPoint(-1, 0)},
        {Qt::Key_D, QPoint(1, 0)},    {Qt::Key_Right, QPoint(1, 0)},
    };
    for (const auto& movement : movements) {
        for (const bool repeat : {false, true}) {
            const QRect before = pinnedWindow->currentNativeGeometry();
            sendShortcut(*canvas, movement.key, Qt::NoModifier, repeat);
            QCoreApplication::processEvents();
            require(pinnedWindow->currentNativeGeometry() == before.translated(movement.delta),
                    "movement shortcuts must move an idle pin by one physical pixel, including "
                    "repeats");
        }
    }
    require(systemCursorPosition() == cursorBefore,
            "moving an idle pin with the keyboard must not move the cursor");

    const snow_shot::storage::PinToScreenShortcutSettings shortcuts;
    const QString actionId = QStringLiteral("move_cursor_up");
    const QStringList previousShortcuts = shortcuts.shortcuts(actionId);
    require(shortcuts.setShortcuts(actionId, {QStringLiteral("Ctrl+Alt+U")}),
            "the window movement shortcut could not be configured");
    const QRect beforeCustomShortcut = pinnedWindow->currentNativeGeometry();
    sendShortcut(*canvas, Qt::Key_W);
    require(pinnedWindow->currentNativeGeometry() == beforeCustomShortcut,
            "a replaced movement shortcut must stop moving the window");
    sendShortcut(*canvas, Qt::Key_U, Qt::ControlModifier | Qt::AltModifier);
    require(pinnedWindow->currentNativeGeometry() == beforeCustomShortcut.translated(0, -1),
            "a configured movement shortcut must immediately move an idle pin");
    require(shortcuts.setShortcuts(actionId, previousShortcuts),
            "the window movement shortcuts could not be restored");
    require(pinnedWindow->persistenceSnapshot().nativeGeometry ==
                beforeCustomShortcut.translated(0, -1),
            "the persisted pin geometry must include keyboard movement");

    auto* textInput = new QLineEdit(pinnedWindow);
    textInput->show();
    textInput->setFocus();
    require(textInput->hasFocus(), "movement text-input fixture must have focus");
    const QRect beforeTextInput = pinnedWindow->currentNativeGeometry();
    sendShortcut(*textInput, Qt::Key_Left);
    require(pinnedWindow->currentNativeGeometry() == beforeTextInput,
            "movement shortcuts must not move a pin while a text field has focus");
    pinnedWindow->close();
    require(processUntilDeleted(guardedWindow, 2000), "keyboard movement pin was not deleted");
}

#if defined(Q_OS_WIN) || defined(_WIN32)
// Starting a window move posts the system drag command (SC_DRAGMOVE), which
// would enter USER32's modal move loop as soon as events are pumped. Tests
// simulate that loop with explicit messages instead, so the posted command is
// discarded right after the drag begins.
void discardPostedSystemDrag(HWND hwnd) {
    MSG message{};
    while (PeekMessageW(&message, hwnd, WM_SYSCOMMAND, WM_SYSCOMMAND, PM_REMOVE) != 0) {
    }
}

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
    auto* pinnedWindow = new ScreenshotPinnedWindow();
    QPointer<ScreenshotPinnedWindow> guardedWindow(pinnedWindow);
    ScreenshotPinnedWindow::Config config;
    config.nativeGeometry = physicalPinGeometry(*screen, QPoint(120, 120), background.size());
    config.canvasSourceRect = QRectF(QPointF(), QSizeF(background.size()));
    config.imageSource = ScreenshotImageSource::fromImage(background, config.canvasSourceRect);
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
    discardPostedSystemDrag(hwnd);
    SendMessageW(hwnd, WM_CAPTURECHANGED, 0, 0);
    static_cast<void>(SendMessageW(hwnd, WM_ENTERSIZEMOVE, 0, 0));

    const QPoint cursorBeforeShortcut = systemCursorPosition();
    const QPoint windowPositionBeforeShortcut = pinnedWindow->currentNativeGeometry().topLeft();
    sendShortcut(*pinnedWindow, Qt::Key_W);
    const QPoint cursorAfterShortcuts = systemCursorPosition();
    // USER32 reacts to cursor movement with WM_MOVING on its next iteration.
    RECT shortcutProposal =
        nativeRectForQRect(startingGeometry.translated(cursorAfterShortcuts - startingCursor));
    require(SendMessageW(hwnd, WM_MOVING, 0, reinterpret_cast<LPARAM>(&shortcutProposal)) == TRUE,
            "the shortcut's system move proposal was not accepted");
    SetWindowPos(hwnd, nullptr, shortcutProposal.left, shortcutProposal.top, 0, 0,
                 SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
    const QPoint windowPositionAfterShortcuts = pinnedWindow->currentNativeGeometry().topLeft();
    const QPoint shortcutCursorDelta = cursorAfterShortcuts - cursorBeforeShortcut;
    const QPoint shortcutWindowDelta = windowPositionAfterShortcuts - windowPositionBeforeShortcut;
    const bool cursorShortcutsMoved = shortcutCursorDelta == QPoint(0, -1);
    const bool windowFollowedShortcuts = shortcutWindowDelta == shortcutCursorDelta;

    // A system drag tracks the pointer inside USER32's move loop and hands
    // the application the proposed rectangle through WM_MOVING; emulate that
    // proposal for the follow-up pointer movement.
    const QPoint pointerDelta(7, 3);
    const QPoint cursorBeforePointerMove = systemCursorPosition();
    setSystemCursorPosition(cursorAfterShortcuts + pointerDelta);
    RECT movingProposal =
        nativeRectForQRect(pinnedWindow->currentNativeGeometry().translated(pointerDelta));
    require(SendMessageW(hwnd, WM_MOVING, 0, reinterpret_cast<LPARAM>(&movingProposal)) == TRUE,
            "the system move proposal was not accepted");
    const RECT acceptedMove = movingProposal;
    SetWindowPos(hwnd, nullptr, acceptedMove.left, acceptedMove.top, 0, 0,
                 SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
    const QPoint actualPointerDelta = systemCursorPosition() - cursorBeforePointerMove;
    const bool windowFollowedPointer =
        pinnedWindow->currentNativeGeometry().topLeft() - windowPositionAfterShortcuts ==
        actualPointerDelta;
    static_cast<void>(SendMessageW(hwnd, WM_EXITSIZEMOVE, 0, 0));
    static_cast<void>(SendMessageW(hwnd, WM_LBUTTONUP, 0, 0));
    waitForUi(50);

    require(shortcuts.setShortcuts(actionId, previousShortcuts),
            "the native-drag cursor shortcut fixture could not restore its configuration");
    pinnedWindow->close();
    require(processUntilDeleted(guardedWindow, 2000), "native-drag shortcut pin was not deleted");
    require(cursorShortcutsMoved,
            "configured cursor shortcuts must remain active throughout a pinned-window drag");
    require(windowFollowedShortcuts,
            "a pinned window must follow cursor shortcuts before its native drag is released");
    require(windowFollowedPointer,
            "a system-driven pinned-window drag must follow accepted move proposals");
}

void pinnedSystemMoveLoopAcceptsMovementShortcuts() {
    QScreen* screen = QGuiApplication::primaryScreen();
    require(screen != nullptr, "a primary screen is required");
    const CursorPositionRestorer restoreCursor;
    QImage background(240, 140, QImage::Format_ARGB32_Premultiplied);
    background.fill(Qt::white);
    auto* pinnedWindow = new ScreenshotPinnedWindow();
    QPointer<ScreenshotPinnedWindow> guardedWindow(pinnedWindow);
    ScreenshotPinnedWindow::Config config;
    config.nativeGeometry = physicalPinGeometry(*screen, QPoint(120, 120), background.size());
    config.canvasSourceRect = QRectF(QPointF(), QSizeF(background.size()));
    config.imageSource = ScreenshotImageSource::fromImage(background, config.canvasSourceRect);
    config.screen = screen;
    config.automaticTextRecognition = false;
    require(pinnedWindow->present(config), "system move loop pin presentation failed");
    waitForUi(50);
    const HWND hwnd = toNativeHwnd(pinnedWindow->winId());
    const QPoint startingCursor = pinnedWindow->currentNativeGeometry().center();
    setSystemCursorPosition(startingCursor);

    const snow_shot::storage::PinToScreenShortcutSettings shortcuts;
    const QString actionId = QStringLiteral("move_cursor_up");
    const QStringList previousShortcuts = shortcuts.shortcuts(actionId);
    require(shortcuts.setShortcuts(actionId, {QStringLiteral("W"), QStringLiteral("Up")}),
            "system move loop custom shortcut could not be configured");
    struct Movement {
        UINT key;
        QPoint delta;
        bool modified = false;
    };
    const std::vector<Movement> movements{
        {'W', QPoint(0, -1)},    {VK_UP, QPoint(0, -1)},   {'S', QPoint(0, 1)},
        {VK_DOWN, QPoint(0, 1)}, {'A', QPoint(-1, 0)},     {VK_LEFT, QPoint(-1, 0)},
        {'D', QPoint(1, 0)},     {VK_RIGHT, QPoint(1, 0)}, {VK_F6, QPoint(0, -1), true},
        {VK_SPACE, QPoint()},
    };
    struct Probe {
        const std::vector<Movement>* movements;
        QPoint cursorWindowOffset;
        size_t next = 0;
        bool observedMoveLoop = true;
        bool cursorMoved = true;
        bool windowMoved = true;
        size_t deliveredKeys = 0;
        BYTE keyboardState[256]{};
    } probe{&movements, startingCursor - pinnedWindow->currentNativeGeometry().topLeft()};
    auto* testApp = static_cast<PinnedWindowTestApplication*>(QCoreApplication::instance());
    testApp->movementProbe = pinnedWindow;
    const auto clearProbe = qScopeGuard([testApp]() {
        testApp->afterMovementKey = {};
        testApp->movementProbe = nullptr;
    });
    testApp->afterMovementKey = [&probe](QPoint cursorDelta, QPoint) {
        const QPoint expected = probe.movements->at(probe.next - 1).delta;
        probe.cursorMoved &= cursorDelta == expected;
        ++probe.deliveredKeys;
        if (cursorDelta != expected) {
            std::cerr << "native drag key " << probe.movements->at(probe.next - 1).key
                      << ": cursor delta " << cursorDelta.x() << ',' << cursorDelta.y()
                      << ", expected " << expected.x() << ',' << expected.y() << '\n';
        }
    };
    require(GetKeyboardState(probe.keyboardState) != FALSE,
            "system move loop keyboard state could not be read");
    require(SetPropW(hwnd, L"SnowPinnedMoveLoopProbe", &probe) != FALSE,
            "system move loop probe could not be installed");
    const UINT_PTR timer =
        SetTimer(hwnd, 0x53534D50, 100, [](HWND timerWindow, UINT, UINT_PTR timerId, DWORD) {
            auto* state = static_cast<Probe*>(GetPropW(timerWindow, L"SnowPinnedMoveLoopProbe"));
            RECT windowRect{};
            GetWindowRect(timerWindow, &windowRect);
            const QPoint offset = systemCursorPosition() - QPoint(windowRect.left, windowRect.top);
            if (state->next > 0) {
                if (offset != state->cursorWindowOffset) {
                    std::cerr << "drag step " << state->next << " cursor/window offset "
                              << offset.x() << ',' << offset.y() << " expected "
                              << state->cursorWindowOffset.x() << ','
                              << state->cursorWindowOffset.y() << '\n';
                }
                state->windowMoved &= offset == state->cursorWindowOffset;
            }
            if (state->next > 0) {
                SetKeyboardState(state->keyboardState);
            }
            if (state->next < state->movements->size()) {
                GUITHREADINFO info{};
                info.cbSize = sizeof(info);
                state->observedMoveLoop &= GetGUIThreadInfo(GetCurrentThreadId(), &info) != FALSE &&
                                           (info.flags & GUI_INMOVESIZE) != 0;
                const Movement& movement = state->movements->at(state->next++);
                if (movement.modified) {
                    const snow_shot::storage::PinToScreenShortcutSettings settings;
                    state->cursorMoved &= settings.setShortcuts(QStringLiteral("move_cursor_up"),
                                                                {QStringLiteral("Ctrl+Alt+F6")});
                    BYTE modifiedState[256]{};
                    GetKeyboardState(modifiedState);
                    modifiedState[VK_CONTROL] = 0x80;
                    modifiedState[VK_LCONTROL] = 0x80;
                    modifiedState[VK_MENU] = 0x80;
                    modifiedState[VK_LMENU] = 0x80;
                    SetKeyboardState(modifiedState);
                }
                const UINT scan = MapVirtualKeyW(movement.key, MAPVK_VK_TO_VSC_EX);
                const bool extended = movement.key >= VK_LEFT && movement.key <= VK_DOWN;
                const LPARAM keyData =
                    1 | (static_cast<LPARAM>(scan & 0xFF) << 16) | (extended ? (1LL << 24) : 0);
                PostMessageW(timerWindow, WM_KEYDOWN, movement.key, keyData);
                PostMessageW(timerWindow, WM_KEYDOWN, movement.key, keyData | (1LL << 30));
                PostMessageW(timerWindow, WM_KEYUP, movement.key,
                             keyData | (1LL << 31) | (1LL << 30));
                return;
            }
            KillTimer(timerWindow, timerId);
            INPUT release{};
            release.type = INPUT_MOUSE;
            release.mi.dwFlags = MOUSEEVENTF_LEFTUP;
            SendInput(1, &release, sizeof(release));
            PostMessageW(timerWindow, WM_LBUTTONUP, 0, 0);
        });
    require(timer != 0, "system move loop probe timer could not be installed");
    const auto removeProbe = qScopeGuard([hwnd, timer]() {
        KillTimer(hwnd, timer);
        RemovePropW(hwnd, L"SnowPinnedMoveLoopProbe");
    });
    require(WindowFromPoint(POINT{startingCursor.x(), startingCursor.y()}) == hwnd,
            "system move loop mouse input must target the test pin");
    INPUT press{};
    press.type = INPUT_MOUSE;
    press.mi.dwFlags = MOUSEEVENTF_LEFTDOWN;
    require(SendInput(1, &press, sizeof(press)) == 1, "system move loop mouse press failed");
    // Hold the physical button, but dispatch its caption press explicitly so
    // native hit testing cannot produce a second drag during the handoff.
    Sleep(20);
    MSG mouseMessage{};
    while (PeekMessageW(&mouseMessage, nullptr, WM_MOUSEFIRST, WM_MOUSELAST, PM_REMOVE) != FALSE) {
    }
    while (PeekMessageW(&mouseMessage, nullptr, WM_NCMOUSEMOVE, WM_NCMBUTTONDBLCLK, PM_REMOVE) !=
           FALSE) {
    }
    SendMessageW(
        hwnd, WM_NCLBUTTONDOWN, HTCAPTION,
        MAKELPARAM(static_cast<WORD>(startingCursor.x()), static_cast<WORD>(startingCursor.y())));
    waitForUi(350);
    KillTimer(hwnd, timer);
    RemovePropW(hwnd, L"SnowPinnedMoveLoopProbe");
    testApp->afterMovementKey = {};
    testApp->movementProbe = nullptr;
    SetKeyboardState(probe.keyboardState);
    require(shortcuts.setShortcuts(actionId, previousShortcuts),
            "system move loop custom shortcuts could not be restored");
    const auto* drawingAction =
        pinnedWindow->findChild<QAction*>(QStringLiteral("screenshotPinnedDrawingAction"));
    const bool drawingInactive = drawingAction != nullptr && !drawingAction->isChecked();
    pinnedWindow->close();
    require(processUntilDeleted(guardedWindow, 2000), "system move loop pin was not deleted");
    require(probe.observedMoveLoop, "the shortcut probe must run inside USER32's real move loop");
    require(probe.next == movements.size(), "all system move loop shortcuts must be exercised");
    require(probe.deliveredKeys == 18,
            "each configured native drag key press and repeat must be delivered exactly once");
    require(probe.cursorMoved,
            "native keyboard messages must move the cursor inside USER32's move loop");
    require(probe.windowMoved,
            "the dragged window must follow native keyboard movement before mouse release");
    require(drawingInactive, "unrelated drawing shortcuts must not activate inside a native drag");
}

void pinnedNativeDragCrossingDpiBoundaryPreservesDestination(SnowCanvasRuntime&) {
    const CursorPositionRestorer restoreCursor;
    QScreen* sourceScreen = nullptr;
    QScreen* destinationScreen = nullptr;
    for (QScreen* candidate : QGuiApplication::screens()) {
        if (candidate == nullptr) {
            continue;
        }
        for (QScreen* other : QGuiApplication::screens()) {
            if (other != nullptr && other != candidate &&
                candidate->devicePixelRatio() > other->devicePixelRatio() + 0.01 &&
                candidate->geometry().left() > other->geometry().left()) {
                sourceScreen = candidate;
                destinationScreen = other;
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

    auto* pinnedWindow = new ScreenshotPinnedWindow();
    QPointer<ScreenshotPinnedWindow> guardedWindow(pinnedWindow);
    ScreenshotPinnedWindow::Config config;
    config.nativeGeometry = QRect(
        sourcePhysical.center() - QPoint(qRound(logicalSize.width() * sourceDpr / 2.0),
                                         qRound(logicalSize.height() * sourceDpr / 2.0)),
        QSize(qRound(logicalSize.width() * sourceDpr), qRound(logicalSize.height() * sourceDpr)));
    config.canvasSourceRect = QRectF(QPointF(), QSizeF(background.size()));
    config.imageSource = ScreenshotImageSource::fromImage(background, config.canvasSourceRect);
    config.screen = sourceScreen;
    config.enableEditing = false;
    require(pinnedWindow->present(config), "cross-DPI native drag pin presentation failed");
    waitForUi(100);

    const HWND hwnd = toNativeHwnd(pinnedWindow->winId());
    require(hwnd != nullptr, "cross-DPI native drag pin did not expose an HWND");
    const QRect startingGeometry = pinnedWindow->currentNativeGeometry();
    const QPoint startingCursor = startingGeometry.center();
    setSystemCursorPosition(startingCursor);
    waitForUi(50);
    static_cast<void>(SendMessageW(
        hwnd, WM_NCLBUTTONDOWN, HTCAPTION,
        MAKELPARAM(static_cast<WORD>(startingCursor.x()), static_cast<WORD>(startingCursor.y()))));
    discardPostedSystemDrag(hwnd);
    static_cast<void>(SendMessageW(hwnd, WM_ENTERSIZEMOVE, 0, 0));

    // A system drag hands the application its proposed rectangle through
    // WM_MOVING; applying it moves the window onto the destination monitor,
    // where Windows performs the native DPI transition and the window adopts
    // the suggested geometry verbatim.
    const auto proposeSystemMove = [hwnd, pinnedWindow](const QPoint& cursor) {
        setSystemCursorPosition(cursor);
        waitForUi(30);
        const QRect current = pinnedWindow->currentNativeGeometry();
        const QPoint target = cursor - QPoint(current.width() / 2, current.height() / 2);
        RECT movingProposal = nativeRectForQRect(QRect(target, current.size()));
        require(SendMessageW(hwnd, WM_MOVING, 0, reinterpret_cast<LPARAM>(&movingProposal)) == TRUE,
                "the cross-screen system move proposal was not accepted");
        const QRect acceptedMove = qRectForNativeRect(movingProposal);
        SetWindowPos(hwnd, nullptr, acceptedMove.x(), acceptedMove.y(), 0, 0,
                     SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
        waitForUi(300);
    };

    proposeSystemMove(destinationPhysical.center());
    const QPoint continuedCursor = destinationPhysical.center() + QPoint(120, 40);
    proposeSystemMove(continuedCursor);

    const QRect destinationGeometry = pinnedWindow->currentNativeGeometry();
    const QSize expectedSize(qRound(startingGeometry.width() * destinationDpr / sourceDpr),
                             qRound(startingGeometry.height() * destinationDpr / sourceDpr));
    auto* scaleLabel =
        pinnedWindow->findChild<QLabel*>(QStringLiteral("screenshotPinnedScaleLabel"));
    require(destinationGeometry != startingGeometry &&
                destinationGeometry.contains(continuedCursor) &&
                qAbs(destinationGeometry.width() - expectedSize.width()) <= 3 &&
                qAbs(destinationGeometry.height() - expectedSize.height()) <= 3 &&
                scaleLabel != nullptr && scaleLabel->isVisible() &&
                scaleLabel->text() ==
                    QStringLiteral("Scale: %1%").arg(qRound(100.0 * destinationDpr / sourceDpr)),
            "cross-DPI native dragging must preserve destination position and native DPI size "
            "while updating the zoom readout");

    static_cast<void>(SendMessageW(hwnd, WM_EXITSIZEMOVE, 0, 0));
    static_cast<void>(SendMessageW(hwnd, WM_LBUTTONUP, 0, 0));
    pinnedWindow->close();
    require(processUntilDeleted(guardedWindow, 2000), "cross-DPI native drag pin was not deleted");
}
#endif

void pinnedMiddleClickActions() {
    const snow_shot::storage::PinToScreenSettings settings;
    const QString previousAction = settings.middleMouseButtonAction();
    const auto restore = qScopeGuard([&] { settings.setMiddleMouseButtonAction(previousAction); });
    const bool offscreen = QGuiApplication::platformName() == QStringLiteral("offscreen");
    auto* window = new ScreenshotPinnedWindow();
    QPointer<ScreenshotPinnedWindow> guarded(window);
    if (offscreen) {
        // Offscreen Windows widgets cannot present an HWND-backed pin. Check command
        // state and Qt routing here; the native run additionally checks physical geometry.
        window->resize(600, 400);
        window->show();
    } else {
        QScreen* screen = QGuiApplication::primaryScreen();
        require(screen != nullptr, "middle-click test needs a screen");
        QImage image(600, 400, QImage::Format_ARGB32_Premultiplied);
        image.fill(Qt::white);
        ScreenshotPinnedWindow::Config config;
        config.nativeGeometry = physicalPinGeometry(*screen, QPoint(40, 40), image.size());
        config.canvasSourceRect = image.rect();
        config.imageSource = ScreenshotImageSource::fromImage(image, config.canvasSourceRect);
        config.screen = screen;
        config.automaticTextRecognition = false;
        require(window->present(config), "middle-click pin presentation failed");
    }
    waitForUi(200);
    auto* canvas = window->findChild<SnowCanvasWidget*>();
    auto* thumbnail =
        window->findChild<QAction*>(QStringLiteral("screenshotPinnedThumbnailAction"));
    auto* scale = window->findChild<adqt::widgets::AdContextMenu*>(
        QStringLiteral("screenshotPinnedScaleMenu"));
    require(canvas != nullptr && thumbnail != nullptr && scale != nullptr,
            "middle-click controls missing");
    const QRect original = window->currentNativeGeometry();
    const auto send = [](QWidget* receiver, QEvent::Type type, const QPoint& point) {
        QMouseEvent event(
            type, QPointF(point), QPointF(receiver->mapToGlobal(point)), Qt::MiddleButton,
            type == QEvent::MouseButtonRelease ? Qt::NoButton : Qt::MiddleButton, Qt::NoModifier);
        QCoreApplication::sendEvent(receiver, &event);
    };
    const auto press = [&](QWidget* receiver) {
        send(receiver, QEvent::MouseButtonPress, receiver->rect().center());
    };
    require(settings.setMiddleMouseButtonAction(QStringLiteral("reset_zoom")), "configure reset");
    scale->actions().at(1)->trigger();
    press(canvas);
    require(scale->actions().at(3)->isChecked(), "middle-click must select 100 percent zoom");
    if (!offscreen) {
        require(window->currentNativeGeometry() == original, "reset must restore baseline size");
    }
    require(settings.setMiddleMouseButtonAction(QStringLiteral("thumbnail_mode")),
            "configure middle-click thumbnail");
    press(window);
    require(thumbnail->isChecked(), "window middle press must toggle exactly once");
    send(window, QEvent::MouseButtonRelease, window->rect().center());
    require(thumbnail->isChecked(), "middle release must not execute another action");
    if (!offscreen) {
        press(canvas);
        require(thumbnail->isChecked(), "geometry animation must block middle-click");
    }
    waitForUi(300);
    send(canvas, QEvent::MouseButtonDblClick, canvas->rect().center());
    waitForUi(300);
    require(!thumbnail->isChecked(), "second physical middle press must dispatch exactly once");
    press(canvas);
    waitForUi(300);
    require(settings.setMiddleMouseButtonAction(QStringLiteral("none")), "configure None");
    const QRect thumbnailGeometry = window->currentNativeGeometry();
    press(canvas);
    require(thumbnail->isChecked() && window->currentNativeGeometry() == thumbnailGeometry,
            "None must preserve thumbnail state and geometry");
    require(settings.setMiddleMouseButtonAction(QStringLiteral("reset_zoom")), "configure reset");
    press(canvas);
    require(!thumbnail->isChecked() && scale->actions().at(3)->isChecked(),
            "reset zoom must leave thumbnail mode and select 100 percent");
    if (!offscreen) {
        require(window->currentNativeGeometry() == original,
                "reset from thumbnail must restore 100 percent physical geometry");
        require(settings.setMiddleMouseButtonAction(QStringLiteral("thumbnail_mode")),
                "configure exclusion tests");
        setPinnedWindowHovered(*window, true);
        auto* controls =
            window->findChild<QFrame*>(QStringLiteral("screenshotPinnedControlsPanel"));
        require(controls != nullptr && controls->isVisible(), "middle-click controls missing");
        send(window, QEvent::MouseButtonPress, controls->mapTo(window, controls->rect().center()));
        press(controls);
        require(!thumbnail->isChecked(), "controls must not dispatch middle-click actions");
        auto* drawing =
            window->findChild<QAction*>(QStringLiteral("screenshotPinnedDrawingAction"));
        require(drawing != nullptr, "drawing action missing");
        drawing->setChecked(true);
        waitForUi(50);
        press(canvas);
        require(!thumbnail->isChecked(), "drawing mode must not dispatch middle-click actions");
        drawing->setChecked(false);
        waitForUi(50);
#if defined(Q_OS_WIN) || defined(_WIN32)
        const HWND hwnd = toNativeHwnd(window->winId());
        const auto native = [&](UINT message, WPARAM hit) {
            const QPoint point = window->currentNativeGeometry().center();
            SendMessageW(hwnd, message, hit,
                         MAKELPARAM(static_cast<WORD>(point.x()), static_cast<WORD>(point.y())));
        };
        native(WM_NCMBUTTONDOWN, HTLEFT);
        require(!thumbnail->isChecked(), "native resize regions must not dispatch middle-click");
        native(WM_NCMBUTTONDOWN, HTCAPTION);
        native(WM_NCMBUTTONUP, HTCAPTION);
        waitForUi(300);
        require(thumbnail->isChecked(), "native middle press/release must dispatch exactly once");
        native(WM_NCMBUTTONDBLCLK, HTCAPTION);
        waitForUi(300);
        require(!thumbnail->isChecked(), "native second middle press must dispatch exactly once");
        require(settings.setMiddleMouseButtonAction(QStringLiteral("none")),
                "configure native None");
        native(WM_NCMBUTTONDOWN, HTCAPTION);
        require(!thumbnail->isChecked(), "native None must preserve state");
        scale->actions().at(1)->trigger();
        require(settings.setMiddleMouseButtonAction(QStringLiteral("reset_zoom")),
                "configure native reset");
        native(WM_NCMBUTTONDOWN, HTCAPTION);
        require(window->currentNativeGeometry() == original, "native middle-click must reset zoom");
#endif
    }
    bool removed = false;
    QObject::connect(window, &ScreenshotPinnedWindow::closingForPersistence,
                     [&removed](const auto&, bool remove) { removed = remove; });
    require(settings.setMiddleMouseButtonAction(QStringLiteral("close")), "configure Close");
#if defined(Q_OS_WIN) || defined(_WIN32)
    if (!offscreen) {
        const QPoint point = window->currentNativeGeometry().center();
        SendMessageW(toNativeHwnd(window->winId()), WM_NCMBUTTONDOWN, HTCAPTION,
                     MAKELPARAM(static_cast<WORD>(point.x()), static_cast<WORD>(point.y())));
    } else
#endif
    {
        press(canvas);
    }
    require(guarded && guarded->isVisible(), "middle-click press must keep the pin open");
    releaseCloseGesture(*window, Qt::MiddleButton);
    require(processUntilDeleted(guarded, 2000), "middle-click Close must delete the pin");
    require(offscreen || removed, "middle-click Close must use the persistence removal lifecycle");
}

void pinnedOffscreenDoubleClickActions() {
    // A fully presented Windows pin requires an HWND. Exercise Qt event routing
    // and command state on an offscreen widget; native tests cover its geometry.
    const snow_shot::storage::PinToScreenSettings settings;
    const QString previousAction = settings.doubleClickAction();
    const auto restore = qScopeGuard([&] { settings.setDoubleClickAction(previousAction); });
    auto* window = new ScreenshotPinnedWindow();
    QPointer<ScreenshotPinnedWindow> guarded(window);
    window->resize(600, 400);
    window->show();
    waitForUi(30);
    auto* canvas = window->findChild<SnowCanvasWidget*>();
    auto* thumbnail =
        window->findChild<QAction*>(QStringLiteral("screenshotPinnedThumbnailAction"));
    require(canvas != nullptr && thumbnail != nullptr, "offscreen double-click controls missing");
    const auto send = [](QWidget* receiver, Qt::MouseButton button = Qt::LeftButton) {
        const QPoint point = receiver->rect().center();
        QMouseEvent event(QEvent::MouseButtonDblClick, QPointF(point),
                          QPointF(receiver->mapToGlobal(point)), button, button, Qt::NoModifier);
        QCoreApplication::sendEvent(receiver, &event);
    };
    require(settings.setDoubleClickAction(QStringLiteral("thumbnail_mode")), "configure toggle");
    send(window, Qt::MiddleButton);
    require(!thumbnail->isChecked(), "offscreen non-left double-click must not toggle");
    send(window);
    require(thumbnail->isChecked(), "offscreen window double-click must enter thumbnail mode");
    waitForUi(300);
    send(canvas);
    waitForUi(300);
    require(!thumbnail->isChecked(), "offscreen canvas double-click must leave thumbnail mode");
    require(settings.setDoubleClickAction(QStringLiteral("none")), "configure offscreen None");
    send(canvas);
    require(guarded && !thumbnail->isChecked(), "offscreen None must leave the window unchanged");
    require(settings.setDoubleClickAction(QStringLiteral("close")), "configure offscreen Close");
    send(canvas);
    require(guarded && guarded->isVisible(), "double-click must wait for its second release");
    releaseCloseGesture(*window, Qt::LeftButton);
    require(processUntilDeleted(guarded, 2000), "offscreen Close must delete the clicked window");
}

void pinnedOcrDoubleClickUsesDragRegion(bool middleClick = false) {
    const snow_shot::storage::PinToScreenSettings settings;
    const QString previousAction = settings.doubleClickAction();
    const QString previousMiddleAction = settings.middleMouseButtonAction();
    const auto restoreMiddle =
        qScopeGuard([&] { settings.setMiddleMouseButtonAction(previousMiddleAction); });
    require(settings.setMiddleMouseButtonAction(QStringLiteral("close")),
            "configure OCR middle-click");
    const auto eventType = middleClick ? QEvent::MouseButtonPress : QEvent::MouseButtonDblClick;
    const auto button = middleClick ? Qt::MiddleButton : Qt::LeftButton;
    const auto restore = qScopeGuard([&] { settings.setDoubleClickAction(previousAction); });
    require(settings.setDoubleClickAction(QStringLiteral("close")), "configure OCR double-click");
    QScreen* screen = QGuiApplication::primaryScreen();
    require(screen != nullptr, "a primary screen is required");
    QImage image(600, 360, QImage::Format_ARGB32_Premultiplied);
    image.fill(Qt::white);
    auto presentation = std::make_shared<ScreenshotOcrPresentation>();
    presentation->selection = image.rect();
    const QRectF textRect(100, 100, 300, 40);
    ScreenshotOcrLine line;
    line.text = QStringLiteral("Double click selectable text");
    line.confidence = 0.99;
    line.quad = QPolygonF(
        {textRect.topLeft(), textRect.topRight(), textRect.bottomRight(), textRect.bottomLeft()});
    presentation->lines.push_back(line);
    presentation->prepareForRendering();
    ScreenshotPinnedWindow::Config config;
    config.nativeGeometry = physicalPinGeometry(*screen, QPoint(40, 40), image.size());
    config.canvasSourceRect = image.rect();
    config.contentCanvasRect = image.rect();
    config.surfaceCanvasRect = image.rect();
    config.imageSource = ScreenshotImageSource::fromImage(image, config.canvasSourceRect);
    config.screen = screen;
    config.automaticTextRecognition = false;
    config.recognitionVisible = true;
    config.recognitionResults.key = QStringLiteral("double-click-ocr");
    ScreenshotOcrRecognitionResult recognition;
    recognition.presentation = presentation;
    config.recognitionResults.text = recognition;
    auto* window = new ScreenshotPinnedWindow();
    QPointer<ScreenshotPinnedWindow> guarded(window);
    require(window->present(config), "OCR double-click pin presentation failed");
    waitForUi(200);
    auto* content = window->findChild<ScreenshotRecognitionWindow*>(
        QStringLiteral("screenshotPinnedRecognitionContent"));
    require(content != nullptr && content->isVisible(), "OCR double-click surface missing");
    auto* textLayer = content->findChild<QGraphicsView*>(QStringLiteral("snowShotOcrTextLayer"));
    require(textLayer != nullptr && !textLayer->scene()->items().isEmpty(), "OCR text missing");
    const auto* item = textLayer->scene()->items().front();
    const QPoint textPoint = textLayer->viewport()->mapTo(
        content, textLayer->mapFromScene(item->sceneBoundingRect().center()));
    const QPoint background(20, content->height() - 30);
    require(!content->isOcrBackgroundAt(textPoint) && content->isOcrBackgroundAt(background),
            "OCR fixture must distinguish text and draggable background");
    QMouseEvent textDoubleClick(eventType, QPointF(textPoint),
                                QPointF(content->mapToGlobal(textPoint)), button, button,
                                Qt::NoModifier);
    QCoreApplication::sendEvent(content, &textDoubleClick);
    QMouseEvent textRelease(QEvent::MouseButtonRelease, QPointF(textPoint),
                            QPointF(content->mapToGlobal(textPoint)), button, Qt::NoButton,
                            Qt::NoModifier);
    QCoreApplication::sendEvent(content, &textRelease);
    waitForUi(30);
    require(guarded && window->isVisible(), "double-clicking OCR text must not close the pin");
    if (middleClick) {
        require(settings.setMiddleMouseButtonAction(QStringLiteral("reset_zoom")),
                "configure OCR reset zoom");
        const QRect before = window->currentNativeGeometry();
        QMouseEvent reset(QEvent::MouseButtonPress, QPointF(background),
                          QPointF(content->mapToGlobal(background)), button, button,
                          Qt::NoModifier);
        QCoreApplication::sendEvent(content, &reset);
        require(window->currentNativeGeometry() == before && content->isVisible(),
                "reset zoom must leave text-only OCR geometry unchanged");
        require(settings.setMiddleMouseButtonAction(QStringLiteral("close")), "restore OCR Close");
    }
    QMouseEvent backgroundDoubleClick(eventType, QPointF(background),
                                      QPointF(content->mapToGlobal(background)), button, button,
                                      Qt::NoModifier);
    QCoreApplication::sendEvent(content, &backgroundDoubleClick);
    require(guarded && window->isVisible(), "OCR background close must wait for button release");
    releaseCloseGesture(*window, button);
    require(processUntilDeleted(guarded, 2000), "double-clicking OCR drag background must close");
}

void enlargedPinnedThumbnailRemainsVisible() {
    const CursorPositionRestorer cursorRestorer;
    QScreen* screen = QGuiApplication::primaryScreen();
    require(screen != nullptr, "enlarged thumbnail requires a screen");
    const QRect screenRect = ScreenshotGeometryMapper::physicalRectForScreen(*screen);
    QImage image(600, 400, QImage::Format_ARGB32_Premultiplied);
    image.fill(Qt::white);
    ScreenshotPinnedWindow::Config config;
    config.nativeGeometry =
        QRect(screenRect.topLeft(), QSize(screenRect.width() + 500, screenRect.height() + 500));
    config.canvasSourceRect = QRectF(QPointF(), QSizeF(image.size()));
    config.imageSource = ScreenshotImageSource::fromImage(image, config.canvasSourceRect);
    config.screen = screen;
    config.automaticTextRecognition = false;
    auto* window = new ScreenshotPinnedWindow();
    QPointer<ScreenshotPinnedWindow> guarded(window);
    require(window->present(config), "enlarged thumbnail pin presentation failed");
    waitForUi(200);
    require(guarded && window->isVisible(), "enlarged pin must start visible");
    const QRect original = window->currentNativeGeometry();
    setSystemCursorPosition(screenRect.center());
    auto* thumbnail =
        window->findChild<QAction*>(QStringLiteral("screenshotPinnedThumbnailAction"));
    require(thumbnail != nullptr, "enlarged thumbnail action missing");
    thumbnail->setChecked(true);
    waitForUi(300);
    require(guarded && window->isVisible(),
            "shrinking an enlarged pin to a thumbnail must not close the window");
    require(screenRect.intersects(window->currentNativeGeometry()),
            "enlarged pin thumbnail must remain on screen");
    thumbnail->setChecked(false);
    waitForUi(300);
    require(guarded && window->isVisible() && window->currentNativeGeometry() == original,
            "leaving thumbnail mode must restore the enlarged pin without closing it");
    window->close();
    require(processUntilDeleted(guarded, 2000), "enlarged thumbnail fixture did not close");
}

void pinnedThumbnailTracksCurrentMousePosition() {
    const CursorPositionRestorer cursorRestorer;
    QScreen* screen = QGuiApplication::primaryScreen();
    require(screen != nullptr, "thumbnail anchoring requires a screen");
    QImage image(600, 400, QImage::Format_ARGB32_Premultiplied);
    image.fill(Qt::white);
    ScreenshotPinnedWindow::Config config;
    config.nativeGeometry = physicalPinGeometry(*screen, QPoint(100, 100), image.size());
    config.canvasSourceRect = QRectF(QPointF(), QSizeF(image.size()));
    config.imageSource = ScreenshotImageSource::fromImage(image, config.canvasSourceRect);
    config.screen = screen;
    config.automaticTextRecognition = false;
    ScreenshotPinnedWindow window;
    require(window.present(config), "thumbnail anchoring pin presentation failed");
    waitForUi(200);
    auto* thumbnail = window.findChild<QAction*>(QStringLiteral("screenshotPinnedThumbnailAction"));
    require(thumbnail != nullptr, "thumbnail anchoring action missing");
    const auto requireAnchor = [](const QRect& before, const QRect& after, const QPoint& cursor) {
        const QPointF fraction((cursor.x() - before.x()) / double(before.width()),
                               (cursor.y() - before.y()) / double(before.height()));
        const QPointF mapped(after.x() + fraction.x() * after.width(),
                             after.y() + fraction.y() * after.height());
        require((mapped - cursor).manhattanLength() <= 1.0,
                "thumbnail transitions must preserve the current mouse anchor");
    };
    const QRect original = window.currentNativeGeometry();
    const QPoint shrinkCursor = original.topLeft() + QPoint(200, 100);
    setSystemCursorPosition(shrinkCursor);
    thumbnail->setChecked(true);
    waitForUi(300);
    const QRect thumbnailGeometry = window.currentNativeGeometry();
    requireAnchor(original, thumbnailGeometry, shrinkCursor);
    const QPoint expandCursor =
        thumbnailGeometry.topLeft() +
        QPoint(thumbnailGeometry.width() / 2, thumbnailGeometry.height() / 2);
    setSystemCursorPosition(expandCursor);
    thumbnail->setChecked(false);
    waitForUi(300);
    require(window.currentNativeGeometry() == original,
            "thumbnail exit must restore the saved position and size despite mouse movement");
}

void pinnedDoubleClickActions() {
    const snow_shot::storage::PinToScreenSettings settings;
    const QString previousAction = settings.doubleClickAction();
    const auto restore = qScopeGuard([&] { settings.setDoubleClickAction(previousAction); });
    require(settings.setDoubleClickAction(QStringLiteral("thumbnail_mode")),
            "configure pinned double-click default");
    QScreen* screen = QGuiApplication::primaryScreen();
    require(screen != nullptr, "a primary screen is required");
    QImage image(600, 400, QImage::Format_ARGB32_Premultiplied);
    image.fill(Qt::white);
    ScreenshotPinnedWindow::Config config;
    config.nativeGeometry = physicalPinGeometry(*screen, QPoint(40, 40), image.size());
    config.canvasSourceRect = QRectF(QPointF(), QSizeF(image.size()));
    config.imageSource = ScreenshotImageSource::fromImage(image, config.canvasSourceRect);
    config.screen = screen;
    config.automaticTextRecognition = false;
    auto* window = new ScreenshotPinnedWindow();
    QPointer<ScreenshotPinnedWindow> guarded(window);
    require(window->present(config), "double-click pin presentation failed");
    waitForUi(200);
    auto* canvas = window->findChild<SnowCanvasWidget*>();
    auto* thumbnail =
        window->findChild<QAction*>(QStringLiteral("screenshotPinnedThumbnailAction"));
    require(canvas != nullptr && thumbnail != nullptr, "double-click pin controls missing");
    const QRect original = window->currentNativeGeometry();
    const auto send = [](QWidget* receiver, const QPoint& point,
                         Qt::MouseButton button = Qt::LeftButton) {
        QMouseEvent event(QEvent::MouseButtonDblClick, QPointF(point),
                          QPointF(receiver->mapToGlobal(point)), button, button, Qt::NoModifier);
        QCoreApplication::sendEvent(receiver, &event);
    };
    for (const auto button : {Qt::RightButton, Qt::MiddleButton}) {
        send(window, window->rect().center(), button);
        require(!thumbnail->isChecked(), "non-left double-click must not toggle thumbnail mode");
    }
    send(window, window->rect().center());
    require(thumbnail->isChecked(), "default double-click must enter thumbnail mode exactly once");
    // The second gesture during the transition cannot restart or reverse the animation.
    send(window, window->rect().center());
    require(thumbnail->isChecked(), "animation must block another double-click action");
    waitForUi(300);
    send(canvas, canvas->rect().center());
    waitForUi(300);
    require(!thumbnail->isChecked() && window->currentNativeGeometry() == original,
            "canvas double-click must restore the pre-thumbnail geometry exactly once");

    require(settings.setDoubleClickAction(QStringLiteral("none")), "configure None on open pin");
    send(canvas, canvas->rect().center());
    waitForUi(30);
    require(guarded && !thumbnail->isChecked() && window->currentNativeGeometry() == original,
            "None must leave the existing pin unchanged");
    require(settings.setDoubleClickAction(QStringLiteral("thumbnail_mode")), "restore toggle");
    setPinnedWindowHovered(*window, true);
    auto* controls = window->findChild<QFrame*>(QStringLiteral("screenshotPinnedControlsPanel"));
    require(controls != nullptr && controls->isVisible(), "double-click controls must be visible");
    send(window, controls->mapTo(window, controls->rect().center()));
    require(!thumbnail->isChecked(), "control panel must not trigger the double-click action");
    auto* drawing = window->findChild<QAction*>(QStringLiteral("screenshotPinnedDrawingAction"));
    require(drawing != nullptr, "drawing action missing");
    drawing->setChecked(true);
    waitForUi(50);
    send(canvas, canvas->rect().center());
    require(!thumbnail->isChecked(), "drawing input must not trigger the double-click action");
    drawing->setChecked(false);
    waitForUi(50);

#if defined(Q_OS_WIN) || defined(_WIN32)
    if (QGuiApplication::platformName() != QStringLiteral("offscreen")) {
        const HWND hwnd = toNativeHwnd(window->winId());
        const auto nativeDoubleClick = [&](const QPoint& point, WPARAM hit) {
            SendMessageW(hwnd, WM_NCLBUTTONDBLCLK, hit,
                         MAKELPARAM(static_cast<WORD>(point.x()), static_cast<WORD>(point.y())));
        };
        for (const QString& action : {QStringLiteral("none"), QStringLiteral("thumbnail_mode")}) {
            require(settings.setDoubleClickAction(action), "configure native double-click");
            nativeDoubleClick(window->currentNativeGeometry().center(), HTCAPTION);
            waitForUi(300);
            require(thumbnail->isChecked() == (action == QStringLiteral("thumbnail_mode")) &&
                        !window->isMaximized(),
                    "native caption double-click must dispatch once without maximizing");
        }
        nativeDoubleClick(window->currentNativeGeometry().center(), HTCAPTION);
        waitForUi(300);
        require(!thumbnail->isChecked() && window->currentNativeGeometry() == original,
                "native thumbnail double-click must restore original geometry");
        const QPoint border(original.left(), original.center().y());
        require(SendMessageW(hwnd, WM_NCHITTEST, 0,
                             MAKELPARAM(static_cast<WORD>(border.x()),
                                        static_cast<WORD>(border.y()))) == HTLEFT,
                "resize border must not become a double-click caption");
    }
#endif
    require(settings.setDoubleClickAction(QStringLiteral("thumbnail_mode")), "configure toggle");
    send(window, window->rect().center());
    waitForUi(300);
    require(thumbnail->isChecked(), "enter thumbnail before changing the action");
    require(settings.setDoubleClickAction(QStringLiteral("none")), "configure None on thumbnail");
    send(canvas, canvas->rect().center());
    require(thumbnail->isChecked(), "None must also apply to an already open thumbnail");
    auto* other = new ScreenshotPinnedWindow();
    QPointer<ScreenshotPinnedWindow> otherGuard(other);
    require(other->present(config), "second pin presentation failed");
    bool removed = false;
    QObject::connect(window, &ScreenshotPinnedWindow::closingForPersistence,
                     [&removed](const auto&, bool remove) { removed = remove; });
    require(settings.setDoubleClickAction(QStringLiteral("close")), "configure Close on thumbnail");
    send(canvas, canvas->rect().center());
    require(guarded && guarded->isVisible(), "close double-click must leave the thumbnail visible");
    releaseCloseGesture(*window, Qt::LeftButton);
    require(processUntilDeleted(guarded, 2000) && removed && otherGuard && other->isVisible(),
            "Close must remove only the clicked thumbnail through the user-close lifecycle");
#if defined(Q_OS_WIN) || defined(_WIN32)
    if (QGuiApplication::platformName() != QStringLiteral("offscreen")) {
        const QPoint center = other->currentNativeGeometry().center();
        SendMessageW(toNativeHwnd(other->winId()), WM_NCLBUTTONDBLCLK, HTCAPTION,
                     MAKELPARAM(static_cast<WORD>(center.x()), static_cast<WORD>(center.y())));
    } else
#endif
    {
        send(other, other->rect().center());
    }
    require(otherGuard && otherGuard->isVisible(), "native double-click must wait for release");
#if defined(Q_OS_WIN) || defined(_WIN32)
    if (QGuiApplication::platformName() != QStringLiteral("offscreen")) {
        SendMessageW(toNativeHwnd(other->winId()), WM_NCLBUTTONUP, HTCAPTION, 0);
    } else
#endif
    {
        releaseCloseGesture(*other, Qt::LeftButton);
    }
    require(processUntilDeleted(otherGuard, 2000), "Close must also close a normal-sized pin");
}

void pinnedThumbnailUsesOpaqueThemeBackground(SnowCanvasRuntime&) {
    QScreen* screen = QGuiApplication::primaryScreen();
    require(screen != nullptr, "a primary screen is required");

    // Thumbnail mode uses a square viewport. Keep the source non-square so
    // the scaled result leaves transparent letterbox pixels around the image.
    QImage transparentImage(400, 200, QImage::Format_ARGB32_Premultiplied);
    transparentImage.fill(Qt::transparent);

    auto* pinnedWindow = new ScreenshotPinnedWindow();
    QPointer<ScreenshotPinnedWindow> guardedWindow(pinnedWindow);
    ScreenshotPinnedWindow::Config config;
    config.nativeGeometry = physicalPinGeometry(*screen, QPoint(40, 40), transparentImage.size());
    config.canvasSourceRect = QRectF(QPointF(), QSizeF(transparentImage.size()));
    config.imageSource =
        ScreenshotImageSource::fromImage(transparentImage, config.canvasSourceRect);
    config.screen = screen;
    require(pinnedWindow->present(config), "transparent pinned window presentation failed");
    QCoreApplication::processEvents(QEventLoop::AllEvents, 50);
    require(renderWidget(*pinnedWindow).pixelColor(pinnedWindow->rect().center()).alpha() == 0,
            "transparent pinned content should preserve alpha outside thumbnail mode");

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
    requireColorNear(thumbnail.pixelColor(thumbnail.width() / 2, 3), expectedBackground, 0,
                     "thumbnail letterbox regions should use the opaque theme background");

    thumbnailAction->setChecked(false);
    waitForUi(200);
    require(!thumbnailAction->isChecked(), "pinned window should leave thumbnail mode");
    require(renderWidget(*pinnedWindow).pixelColor(pinnedWindow->rect().center()).alpha() == 0,
            "leaving thumbnail mode should restore transparent pinned content");

    pinnedWindow->close();
    require(processUntilDeleted(guardedWindow, 2000),
            "pinned window was not deleted after the thumbnail background test");
}

void pinnedControlsHideBelowMinimumNativeSize(SnowCanvasRuntime&) {
    QScreen* screen = QGuiApplication::primaryScreen();
    require(screen != nullptr, "a primary screen is required");

    const auto verifyControls = [screen](const QSize& nativeSize, bool expectedVisible) {
        QImage background(400, 400, QImage::Format_ARGB32_Premultiplied);
        background.fill(QColor(42, 84, 126));

        auto* pinnedWindow = new ScreenshotPinnedWindow();
        QPointer<ScreenshotPinnedWindow> guardedWindow(pinnedWindow);
        ScreenshotPinnedWindow::Config config;
        config.nativeGeometry = physicalPinGeometry(*screen, QPoint(40, 40), nativeSize);
        config.canvasSourceRect = QRectF(QPointF(0.0, 0.0), QSizeF(background.size()));
        config.imageSource = ScreenshotImageSource::fromImage(background, config.canvasSourceRect);
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

void pinnedGeometryQueriesDoNotCreateNativeWindows() {
    ScreenshotPinnedWindow window;
    window.setAttribute(Qt::WA_DeleteOnClose, false);
    require(window.internalWinId() == 0, "the geometry fixture must start without a native window");
    static_cast<void>(window.currentNativeGeometry());
    require(window.internalWinId() == 0, "reading pinned geometry must not create a native window");
    QEnterEvent enter(QPointF(10, 10), QPointF(10, 10), QPointF(10, 10));
    QCoreApplication::sendEvent(&window, &enter);
    require(window.internalWinId() == 0,
            "hover delivery must not create an unpresented native window");

    window.show();
    window.close();
    if (window.windowHandle() != nullptr) {
        window.windowHandle()->destroy();
    }
    require(window.internalWinId() == 0, "the closed fixture must have no native window");
    QCoreApplication::sendEvent(&window, &enter);
    QEvent leave(QEvent::Leave);
    QCoreApplication::sendEvent(&window, &leave);
    static_cast<void>(window.currentNativeGeometry());
    require(window.internalWinId() == 0,
            "late hover and geometry queries must not recreate a closed native window");

    // Full pin presentation installs HWND hooks and cannot run with the offscreen backend.
    // The Windows registration also exercises passive reconciliation after native destruction.
    if (QGuiApplication::platformName() != QStringLiteral("windows")) {
        return;
    }
    ScreenshotPinnedWindow presentedWindow;
    presentedWindow.setAttribute(Qt::WA_DeleteOnClose, false);
    QImage background(400, 400, QImage::Format_ARGB32_Premultiplied);
    background.fill(Qt::white);
    ScreenshotPinnedWindow::Config config;
    config.screen = QGuiApplication::primaryScreen();
    require(config.screen != nullptr, "the presented geometry fixture needs a screen");
    config.nativeGeometry = physicalPinGeometry(*config.screen, QPoint(40, 40), background.size());
    config.canvasSourceRect = QRectF(QPointF(), QSizeF(background.size()));
    config.imageSource = ScreenshotImageSource::fromImage(background, config.canvasSourceRect);
    config.automaticTextRecognition = false;
    require(presentedWindow.present(config), "the native geometry fixture must present");
    presentedWindow.windowHandle()->destroy();
    require(presentedWindow.internalWinId() == 0,
            "the presented fixture must lose its native window");
    QEvent update(QEvent::UpdateRequest);
    QCoreApplication::sendEvent(&presentedWindow, &update);
    require(presentedWindow.internalWinId() == 0,
            "passive geometry reconciliation must not recreate a native window");
    presentedWindow.close();
}

void pinnedEscapeBurst(bool nativeKeys = false) {
    QScreen* screen = QGuiApplication::primaryScreen();
    require(screen != nullptr, "a primary screen is required");
    QImage background(400, 400, QImage::Format_ARGB32_Premultiplied);
    background.fill(QColor(42, 84, 126));
    snow_shot::presentation::PinnedWindowGroupManager groupManager;
    ScreenshotSelectionExportUiServices services(nullptr, nullptr, nullptr, {}, {}, &groupManager);
    for (int batch = 0; batch < 4; ++batch) {
        std::vector<QPointer<ScreenshotPinnedWindow>> windows;
        for (int index = 0; index < 16; ++index) {
            if (batch >= 2) {
                require(
                    services.presentPinnedImage(
                        background, screen,
                        physicalPinGeometry(*screen, QPoint(40 + index, 40), background.size())),
                    "pooled burst pin presentation failed");
                for (auto* window : topLevelPinnedWindows()) {
                    if (window->isVisible() && std::none_of(windows.begin(), windows.end(),
                                                            [window](const auto& existing) {
                                                                return existing == window;
                                                            })) {
                        windows.emplace_back(window);
                    }
                }
                require(windows.size() == static_cast<std::size_t>(index + 1),
                        "each pooled presentation must create one visible pin");
                continue;
            }
            auto* window = new ScreenshotPinnedWindow();
            windows.emplace_back(window);
            ScreenshotPinnedWindow::Config config;
            config.nativeGeometry =
                physicalPinGeometry(*screen, QPoint(40 + index, 40), background.size());
            config.canvasSourceRect = QRectF(QPointF(), QSizeF(background.size()));
            config.imageSource =
                ScreenshotImageSource::fromImage(background, config.canvasSourceRect);
            config.screen = screen;
            config.enableEditing = true;
            config.automaticTextRecognition = false;
            config.groupManager = &groupManager;
            config.groupId = groupManager.activeGroupId();
            require(window->present(config), "burst pin presentation failed");
        }
        QCoreApplication::processEvents();
        if (nativeKeys) {
            std::reverse(windows.begin(), windows.end());
        }
        for (auto it = windows.rbegin(); it != windows.rend(); ++it) {
            auto* canvas = (*it)->findChild<SnowCanvasWidget*>();
            require(canvas != nullptr, "each burst pin must have a keyboard surface");
            if (nativeKeys) {
#if defined(Q_OS_WIN) || defined(_WIN32)
                QElapsedTimer activationTimeout;
                activationTimeout.start();
                QElapsedTimer stableActivation;
                stableActivation.start();
                while (activationTimeout.elapsed() < 2000 && stableActivation.elapsed() < 100) {
                    if (!(*it)->isActiveWindow() ||
                        GetForegroundWindow() != toNativeHwnd((*it)->winId())) {
                        static_cast<void>(
                            screenshot_pinned_window_native::activateWindow((*it)->winId()));
                        (*it)->activateWindow();
                        canvas->setFocus();
                        stableActivation.restart();
                    }
                    waitForUi(10);
                }
                require((*it)->isActiveWindow() &&
                            GetForegroundWindow() == toNativeHwnd((*it)->winId()),
                        "Escape target must be active");
                canvas->setFocus();
                require(QApplication::focusWidget() == canvas, "Escape canvas must own focus");
                // Exercise the Windows key mapper without global input injection.
                // The pin must retain input ownership until the native key-up.
                const LPARAM keyData =
                    1 | (static_cast<LPARAM>(MapVirtualKeyW(VK_ESCAPE, MAPVK_VK_TO_VSC)) << 16);
                require(PostMessageW(toNativeHwnd((*it)->winId()), WM_KEYDOWN, VK_ESCAPE,
                                     keyData) != FALSE,
                        "native Escape press failed");
                waitForUi(30);
                require(*it && (*it)->isVisible(), "native Escape down must not close the pin");
                require(PostMessageW(toNativeHwnd((*it)->winId()), WM_KEYUP, VK_ESCAPE,
                                     keyData | (LPARAM(3) << 30)) != FALSE,
                        "native Escape release failed");
                require(processUntilDeleted(*it, 2000),
                        "native Escape release must close the active pin");
#endif
            } else {
                sendShortcut(*canvas, Qt::Key_Escape);
                sendShortcut(*canvas, Qt::Key_Escape, Qt::NoModifier, true);
                require(*it && (*it)->isVisible(), "Escape repeats must keep the pin open");
                QKeyEvent release(QEvent::KeyRelease, Qt::Key_Escape, Qt::NoModifier);
                QCoreApplication::sendEvent(canvas, &release);
            }
            if (batch % 2 != 0) {
                require(processUntilDeleted(*it, 2000), "sequential Escape must delete its pin");
            }
        }
        for (auto& window : windows) {
            require(processUntilDeleted(window, 2000), "Escape must delete every pin in the burst");
        }
        QCoreApplication::processEvents();
    }
}

void closePinnedWindow(SnowCanvasRuntime&, bool enableEditing, bool enterEditMode, int iteration) {
    std::cerr << "iteration=" << iteration << " editing=" << enableEditing
              << " editMode=" << enterEditMode << " start\n";
    QScreen* screen = QGuiApplication::primaryScreen();
    require(screen != nullptr, "a primary screen is required");

    auto* pinnedWindow = new ScreenshotPinnedWindow();
    QPointer<ScreenshotPinnedWindow> guardedWindow(pinnedWindow);

    QImage background(400, 400, QImage::Format_ARGB32_Premultiplied);
    background.fill(QColor(42, 84, 126));

    ScreenshotPinnedWindow::Config config;
    config.nativeGeometry = physicalPinGeometry(*screen, QPoint(40, 40), QSize(400, 400));
    config.canvasSourceRect = QRectF(QPointF(0.0, 0.0), QSizeF(background.size()));
    config.imageSource = ScreenshotImageSource::fromImage(background, config.canvasSourceRect);
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

void pinnedReadoutOffscreen() {
    ScreenshotPinnedWindow window;
    window.setAttribute(Qt::WA_DeleteOnClose, false);
    window.resize(640, 480);
    window.show();
    auto* label = window.findChild<QLabel*>(QStringLiteral("screenshotPinnedScaleLabel"));
    require(label != nullptr && dynamic_cast<CanvasStatusReadout*>(label) != nullptr,
            "pinned scale label must use the shared canvas readout and retain its object name");
    for (const bool opacity : {false, true}) {
        auto* timer = ScreenshotPinnedWindowTestAccess::showReadout(window, opacity);
        const QString text =
            opacity ? QStringLiteral("Opacity: 80%") : QStringLiteral("Scale: 125%");
        require(label->isVisible() && label->text() == text && label->toolTip() == text &&
                    label->accessibleName() == text && label->x() == 8 &&
                    label->y() + label->height() == window.height() - 8,
                "scale and opacity readouts must retain their copy and eight-pixel inset");
        require(label->testAttribute(Qt::WA_TransparentForMouseEvents) &&
                    label->focusPolicy() == Qt::NoFocus && timer->isSingleShot() &&
                    timer->interval() == 1000 && timer->isActive(),
                "readout must be passive and retain its one-second timeout");
        require(QMetaObject::invokeMethod(timer, "timeout", Qt::DirectConnection),
                "controlled scheduler must deliver the readout timeout");
        require(label->isHidden(), "readout timeout must hide the shared label");
        timer->stop();
    }
}

void pinnedScalingAndAspectLockedResizing(SnowCanvasRuntime&) {
    QScreen* screen = QGuiApplication::primaryScreen();
    require(screen != nullptr, "a primary screen is required");

    QImage background(240, 120, QImage::Format_ARGB32_Premultiplied);
    background.fill(QColor(37, 91, 143));
    const qreal dpr = screen->devicePixelRatio();
    const QRect physicalScreen = ScreenshotGeometryMapper::physicalRectForScreen(*screen);

    auto* pinnedWindow = new ScreenshotPinnedWindow();
    QPointer<ScreenshotPinnedWindow> guardedWindow(pinnedWindow);
    ScreenshotPinnedWindow::Config config;
    const QSize logicalFixtureSize(1000, 500);
    config.nativeGeometry = QRect(
        physicalScreen.topLeft() + QPoint(qRound(120 * dpr), qRound(100 * dpr)),
        QSize(qRound(logicalFixtureSize.width() * dpr), qRound(logicalFixtureSize.height() * dpr)));
    config.canvasSourceRect = QRectF(QPointF(), QSizeF(background.size()));
    config.imageSource = ScreenshotImageSource::fromImage(background, config.canvasSourceRect);
    config.screen = screen;
    config.enableEditing = true;
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
    auto* processAction =
        pinnedMenuActionNamed(*pinnedWindow, QStringLiteral("screenshotPinnedProcessImageMenu"));
    auto* processMenu = qobject_cast<adqt::widgets::AdContextMenu*>(
        processAction != nullptr ? processAction->menu() : nullptr);
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
    auto* drawingAction =
        pinnedMenuActionNamed(*pinnedWindow, QStringLiteral("screenshotPinnedDrawingAction"));
    require(drawingAction != nullptr, "pinned drawing action was not found");
    drawingAction->setChecked(true);
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
    drawingAction->setChecked(false);

    auto* thumbnailAction =
        pinnedMenuActionNamed(*pinnedWindow, QStringLiteral("screenshotPinnedThumbnailAction"));
    require(thumbnailAction != nullptr, "pinned thumbnail action was not found");
    thumbnailAction->setChecked(true);
    // Entering thumbnail mode runs a geometry animation whose shrinking frames
    // re-render with linear filtering; wait for the window to settle instead of
    // assuming a fixed budget shorter than the animation can take.
    QRect thumbnailGeometry;
    bool thumbnailBorderDraggable = false;
    QElapsedTimer thumbnailSettle;
    thumbnailSettle.start();
    while (thumbnailSettle.elapsed() < 2000) {
        waitForUi(50);
        thumbnailGeometry = pinnedWindow->currentNativeGeometry();
        if (nativeHitTest(QPoint(thumbnailGeometry.right(), thumbnailGeometry.center().y())) ==
            HTCAPTION) {
            thumbnailBorderDraggable = true;
            break;
        }
    }
    require(thumbnailBorderDraggable,
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
    require(!thumbnailAction->isChecked() &&
                pinnedWindow->currentNativeGeometry().size() == expectedSize(110, true),
            "thumbnail wheel input should restore the pin and apply cursor scaling");

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

    auto* pinnedWindow = new ScreenshotPinnedWindow();
    QPointer<ScreenshotPinnedWindow> guardedWindow(pinnedWindow);
    ScreenshotPinnedWindow::Config config;
    config.nativeGeometry = QRect(physicalScreen.topLeft() + QPoint(160, 140), baseline);
    config.canvasSourceRect = QRectF(QPointF(), QSizeF(background.size()));
    config.imageSource = ScreenshotImageSource::fromImage(background, config.canvasSourceRect);
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

    auto* pinnedWindow = new ScreenshotPinnedWindow();
    QPointer<ScreenshotPinnedWindow> guardedWindow(pinnedWindow);
    ScreenshotPinnedWindow::Config config;
    config.nativeGeometry = physicalPinGeometry(*screen, QPoint(120, 100), background.size());
    config.canvasSourceRect = QRectF(QPointF(), QSizeF(background.size()));
    config.imageSource = ScreenshotImageSource::fromImage(background, config.canvasSourceRect);
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

    auto* pinnedWindow = new ScreenshotPinnedWindow();
    QPointer<ScreenshotPinnedWindow> guardedWindow(pinnedWindow);
    ScreenshotPinnedWindow::Config config;
    config.nativeGeometry = QRect(
        sourcePhysical.topLeft() + QPoint(qRound(60 * sourceDpr), qRound(60 * sourceDpr)),
        QSize(qRound(logicalSize.width() * sourceDpr), qRound(logicalSize.height() * sourceDpr)));
    config.canvasSourceRect = QRectF(QPointF(), QSizeF(background.size()));
    config.imageSource = ScreenshotImageSource::fromImage(background, config.canvasSourceRect);
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

// Process-lifetime fallback storage handed back to when a scoped
// IsolatedPinnedStorage guard shuts down, so the remaining sections never
// run without isolated storage.
QTemporaryDir& hermeticPinnedStorageDir() {
    static QTemporaryDir directory;
    return directory;
}

void initializeIsolatedPinnedStorage(const QString& root) {
    auto& storage = snow_shot::storage::ApplicationStorage::instance();
    storage.shutdown();
    const snow_shot::storage::StorageInitializationOptions options{
        QDir(root).filePath(QStringLiteral("bin")),
        QDir(root).filePath(QStringLiteral("settings")),
        0,
    };
    require(storage.initialize(options).success,
            "failed to initialize isolated pinned window storage");
}

// Isolated storage keeps seeded records out of the developer's real
// configuration and out of the other tests in this binary.
class IsolatedPinnedStorage final {
  public:
    IsolatedPinnedStorage() {
        require(m_temporary.isValid(), "temporary directory unavailable");
        initializeIsolatedPinnedStorage(m_temporary.path());
    }

    // Product code lazily opens the developer's real AppData configuration
    // whenever it finds storage uninitialized, and the drawing toolbar
    // persists its wheel-driven style steps through that store. Leaving
    // storage shut down here would let that happen for the remaining
    // sections, leaking one stroke-width bump per run until the wheel tests
    // saturate at their clamp, so hand back to hermetic storage instead.
    ~IsolatedPinnedStorage() {
        auto& storage = snow_shot::storage::ApplicationStorage::instance();
        storage.shutdown();
        QTemporaryDir& hermetic = hermeticPinnedStorageDir();
        if (!hermetic.isValid()) {
            return;
        }
        const snow_shot::storage::StorageInitializationOptions options{
            QDir(hermetic.path()).filePath(QStringLiteral("bin")),
            QDir(hermetic.path()).filePath(QStringLiteral("settings")),
            0,
        };
        static_cast<void>(storage.initialize(options));
    }

    IsolatedPinnedStorage(const IsolatedPinnedStorage&) = delete;
    IsolatedPinnedStorage& operator=(const IsolatedPinnedStorage&) = delete;

  private:
    QTemporaryDir m_temporary;
};

void restoredPinnedSelectionRendersCachedOcrAfterStorageRestart() {
    IsolatedPinnedStorage storage;
    pinnedSelectionRendersCachedOcrInCanvasCoordinates(true);
    pinnedSelectionRendersCachedOcrInCanvasCoordinates(true, true);
}

// Describes a pinned window that was saved on a monitor whose recorded DPI
// is `savedDpiFactor` times the current DPI. The recorded DPI and percent are
// informational; the native geometry is what that percent produced in the
// saved monitor's pixels, and a restore recreates exactly those pixels.
snow_shot::storage::PinnedWindowRecord savedPinnedRecord(QScreen& screen, qreal savedDpiFactor,
                                                         const QSize& basis, double scalePercent,
                                                         const QPoint& savedOffset) {
    const QRect physical = ScreenshotGeometryMapper::physicalRectForScreen(screen);
    const qreal dpr = screen.devicePixelRatio() > 0.0 ? screen.devicePixelRatio() : 1.0;
    snow_shot::storage::PinnedWindowRecord record;
    record.id = QUuid::createUuid().toString(QUuid::WithoutBraces);
    record.image = QImage(basis, QImage::Format_ARGB32_Premultiplied);
    record.image.fill(QColor(54, 105, 157));
    record.canvasSourceRect = QRectF(QPointF(), QSizeF(basis));
    record.contentCanvasRect = record.canvasSourceRect;
    record.surfaceCanvasRect = record.canvasSourceRect;
    record.initialPhysicalSize = basis;
    record.scalePercent = scalePercent;
    record.screenName = screen.name();
    record.screenDpi = savedDpiFactor * dpr;
    record.screenPhysicalGeometry =
        QRect(physical.topLeft(), QSize(qRound(physical.width() * savedDpiFactor),
                                        qRound(physical.height() * savedDpiFactor)));
    record.nativeGeometry = QRect(physical.topLeft() + savedOffset,
                                  QSize(qRound(basis.width() * scalePercent / 100.0),
                                        qRound(basis.height() * scalePercent / 100.0)));
    return record;
}

ScreenshotPinnedWindow*
restoreSeededPinnedWindow(ScreenshotSelectionExportUiServices& services,
                          const snow_shot::storage::PinnedWindowRecord& record) {
    auto& storage = snow_shot::storage::ApplicationStorage::instance();
    const snow_shot::storage::StorageResult seeded = storage.pinnedWindows().upsert(record);
    require(seeded.success, qPrintable(seeded.error));

    services.restorePersistedWindows();
    QCoreApplication::processEvents(QEventLoop::AllEvents, 50);

    ScreenshotPinnedWindow* restoredWindow = nullptr;
    ScreenshotPinnedWindow* preparedWindow = nullptr;
    for (ScreenshotPinnedWindow* window : topLevelPinnedWindows()) {
        if (window->isVisible()) {
            require(restoredWindow == nullptr,
                    "restore should have created exactly one presented window");
            restoredWindow = window;
        } else {
            require(preparedWindow == nullptr,
                    "restore should have replenished exactly one hidden shell");
            preparedWindow = window;
        }
    }
    require(restoredWindow != nullptr, "restore should have created the seeded pinned window");
    require(preparedWindow != nullptr && preparedWindow->winId() != 0,
            "restore should have replenished one hidden native shell");
    return restoredWindow;
}

// Reads the "Current: N%" entry the way a user sees it: opening the context
// menu is what refreshes the readout from the window state.
QString scaleMenuReadout(ScreenshotPinnedWindow& window) {
    auto* contextMenu = window.findChild<adqt::widgets::AdContextMenu*>(
        QStringLiteral("screenshotPinnedContextMenu"));
    auto* scaleMenu = window.findChild<adqt::widgets::AdContextMenu*>(
        QStringLiteral("screenshotPinnedScaleMenu"));
    require(contextMenu != nullptr && scaleMenu != nullptr,
            "restored pinned scale menu was not found");
    contextMenu->aboutToShow();
    return scaleMenu->actions().constLast()->text();
}

void closeRestoredPinnedWindow(ScreenshotPinnedWindow* window, const QString& recordId) {
    static_cast<void>(
        snow_shot::storage::ApplicationStorage::instance().pinnedWindows().remove(recordId));
    QPointer<ScreenshotPinnedWindow> guardedWindow(window);
    window->close();
    require(processUntilDeleted(guardedWindow, 2000), "restored pinned window was not deleted");
}

void restoredPinnedWindowIgnoresMonitorDpiChange(SnowCanvasRuntime&) {
    QScreen* screen = QGuiApplication::primaryScreen();
    require(screen != nullptr, "a primary screen is required");
    const QRect physical = ScreenshotGeometryMapper::physicalRectForScreen(*screen);

    IsolatedPinnedStorage storage;
    // Saved at 50% on a monitor whose recorded DPI is twice the current one.
    // Physical pixels are the only unit, so the window comes back at its
    // saved pixel size, which is still 50% of the unchanged basis.
    const snow_shot::storage::PinnedWindowRecord record =
        savedPinnedRecord(*screen, 2.0, QSize(800, 400), 50.0, QPoint(200, 120));

    ScreenshotSelectionExportUiServices services;
    ScreenshotPinnedWindow* restoredWindow = restoreSeededPinnedWindow(services, record);
    const QRect expectedGeometry(physical.topLeft() + QPoint(200, 120), QSize(400, 200));
    require(restoredWindow->currentNativeGeometry() == expectedGeometry,
            "restored pinned window should present at the saved physical geometry");
    require(scaleMenuReadout(*restoredWindow) == QStringLiteral("Current: 50%"),
            "restored pinned scale menu should derive from the saved physical pixels");

    closeRestoredPinnedWindow(restoredWindow, record.id);
}

void restoredThumbnailStateOffscreen(const QString& scenario) {
    ScreenshotPinnedWindow::Config config;
    config.nativeGeometry = QRect(40, 30, 120, 120);
    config.canvasSourceRect = QRectF(0, 0, 400, 200);
    config.fullResolutionScaleBasis = QSize(800, 400);
    QImage transparentImage(400, 200, QImage::Format_ARGB32_Premultiplied);
    transparentImage.fill(Qt::transparent);
    config.imageSource =
        ScreenshotImageSource::fromImage(transparentImage, config.canvasSourceRect);
    config.restorePersistentState = true;
    config.persistedThumbnailMode = true;
    config.persistedPreThumbnailNativeGeometry = QRect(200, 120, 400, 200);
    ScreenshotPinnedWindow window;
    ScreenshotPinnedWindowTestAccess::restoreOffscreen(window, config);
    if (scenario == QStringLiteral("appearance")) {
        auto* thumbnail =
            window.findChild<QAction*>(QStringLiteral("screenshotPinnedThumbnailAction"));
        auto* canvas = window.findChild<SnowCanvasWidget*>();
        const QImage rendered = renderWidget(*canvas);
        require(rendered.pixelColor(rendered.rect().center()).alpha() == 255,
                "a recreated thumbnail must paint an opaque background from its first frame");
        require(thumbnail != nullptr && thumbnail->isChecked(),
                "restored thumbnail action must reflect the mode before opening a menu");
    } else if (scenario == QStringLiteral("dpi")) {
        const double expectedZoom = ScreenshotPinnedWindowTestAccess::viewportZoom(window);
        // A native resize may arrive before Qt updates the DPR. Leave the camera
        // at that earlier scale, then deliver the final DPR notification alone.
        for (const qreal ratio : {0.5, 1.25, 2.0}) {
            ScreenshotPinnedWindowTestAccess::leaveViewportAtPreviousDpi(window, ratio);
            QEvent dpiChanged(QEvent::DevicePixelRatioChange);
            QCoreApplication::sendEvent(&window, &dpiChanged);
            waitForUi(30);
            require(
                qFuzzyCompare(ScreenshotPinnedWindowTestAccess::viewportZoom(window), expectedZoom),
                "DPI settlement must refresh a thumbnail camera even without another resize");
        }
        require(window.persistenceSnapshot().preThumbnailNativeGeometry ==
                    config.persistedPreThumbnailNativeGeometry,
                "thumbnail DPI changes must preserve the saved expansion rectangle");
    } else if (scenario == QStringLiteral("snapshot")) {
        const QRect target(70, 60, 83, 83);
        ScreenshotPinnedWindowTestAccess::setAnimationSnapshot(window, target);
        const auto snapshot = window.persistenceSnapshot();
        require(snapshot.thumbnailMode && snapshot.nativeGeometry == target,
                "a persisted mode must be paired with the animation destination, never a frame");
        ScreenshotPinnedWindowTestAccess::finishExpansionOffscreen(window);
        require(!ScreenshotPinnedWindowTestAccess::isGeometryAnimating(window),
                "an immediate command must finish expansion even after the mode flag is cleared");
    } else if (scenario == QStringLiteral("copy")) {
        auto* copy = window.findChild<QAction*>(QStringLiteral("screenshotPinnedCopyAction"));
        require(copy != nullptr, "restored thumbnail copy action missing");
        static_cast<void>(renderWidget(*window.findChild<SnowCanvasWidget*>()));
        waitForUi(30);
        QApplication::clipboard()->clear();
        copy->trigger();
        // Windows publishes to the native clipboard even with offscreen QPA.
        // Inspect the same artifact the Copy command sends to that adapter.
        const auto artifact = ScreenshotPinnedWindowTestAccess::exportArtifact(window);
        require(artifact != nullptr, "thumbnail Copy must create an export artifact");
        QImage copied;
        require(artifact->requestImage(&window,
                                       [&copied](ScreenshotExportImageResult result) {
                                           copied = std::move(result.image);
                                       }),
                "thumbnail copy artifact must provide an image");
        QElapsedTimer deadline;
        deadline.start();
        while (copied.isNull() && deadline.elapsed() < 5000) {
            waitForUi(10);
        }
        if (copied.size() != QSize(120, 60)) {
            std::cerr << "thumbnail copy size=" << copied.width() << 'x' << copied.height()
                      << " widget=" << window.width() << 'x' << window.height()
                      << " dpr=" << window.devicePixelRatioF() << '\n';
        }
        require(copied.size() == QSize(120, 60),
                "thumbnail copy must use the displayed physical pixels at the current DPI");
    }
}

void thumbnailAnimationSurvivesRecreation(bool entering) {
    IsolatedPinnedStorage storage;
    QScreen* screen = QGuiApplication::primaryScreen();
    require(screen != nullptr, "thumbnail recreation requires a screen");
    auto record = savedPinnedRecord(*screen, 1.0, QSize(800, 400), 50.0, QPoint(200, 120));
    const QRect expanded = record.nativeGeometry;
    record.image.fill(Qt::transparent);
    ScreenshotSelectionExportUiServices services;
    auto* window = restoreSeededPinnedWindow(services, record);
    auto* thumbnail =
        window->findChild<QAction*>(QStringLiteral("screenshotPinnedThumbnailAction"));
    require(thumbnail != nullptr, "thumbnail recreation action missing");
    thumbnail->setChecked(true);
    if (!entering) {
        waitForUi(250);
        thumbnail->setChecked(false);
    }
    auto* animation =
        window->findChild<QVariantAnimation*>(QStringLiteral("screenshotPinnedGeometryAnimation"));
    require(animation != nullptr, "thumbnail recreation animation missing");
    animation->pause();
    animation->setCurrentTime(animation->duration() / 2);
    const QRect target = animation->endValue().toRect();
    auto snapshot = window->persistenceSnapshot();
    require(snapshot.thumbnailMode == entering && snapshot.nativeGeometry == target &&
                snapshot.preThumbnailNativeGeometry == expanded,
            "closing during a thumbnail transition must save the destination and expansion state");
    QPointer<ScreenshotPinnedWindow> guarded(window);
    window->closeForInactiveGroup();
    require(processUntilDeleted(guarded, 2000), "thumbnail transition fixture did not close");
    window = restoreSeededPinnedWindow(services, snapshot);
    require(window->currentNativeGeometry() == target &&
                window->persistenceSnapshot().thumbnailMode == entering,
            "recreation must finish the saved transition at its destination");
    const QImage rendered = renderWidget(*window);
    require(rendered.pixelColor(rendered.rect().center()).alpha() == (entering ? 255 : 0),
            "recreated pins must immediately render the background appropriate to their mode");
    closeRestoredPinnedWindow(window, record.id);
}

void thumbnailReentryPreservesExpandedGeometry(bool scaleDuringExpansion = false) {
    IsolatedPinnedStorage storage;
    QScreen* screen = QGuiApplication::primaryScreen();
    require(screen != nullptr, "thumbnail reentry requires a screen");
    auto record = savedPinnedRecord(*screen, 1.0, QSize(800, 400), 50.0, QPoint(200, 120));
    const QRect expanded = record.nativeGeometry;
    record.thumbnailMode = true;
    record.preThumbnailNativeGeometry = expanded;
    record.nativeGeometry.setSize(QSize(120, 120));
    ScreenshotSelectionExportUiServices services;
    auto* window = restoreSeededPinnedWindow(services, record);
    static_cast<void>(scaleMenuReadout(*window));
    auto* thumbnail =
        window->findChild<QAction*>(QStringLiteral("screenshotPinnedThumbnailAction"));
    thumbnail->setChecked(false);
    auto* animation =
        window->findChild<QVariantAnimation*>(QStringLiteral("screenshotPinnedGeometryAnimation"));
    require(animation != nullptr, "thumbnail expansion animation missing");
    animation->pause();
    animation->setCurrentTime(animation->duration() / 2);
    if (scaleDuringExpansion) {
        auto* scale = window->findChild<adqt::widgets::AdContextMenu*>(
            QStringLiteral("screenshotPinnedScaleMenu"));
        require(scale != nullptr, "thumbnail scale menu missing");
        scale->actions().at(3)->trigger();
        require(animation->state() == QAbstractAnimation::Stopped,
                "scaling during thumbnail expansion must cancel the pending animation");
        waitForUi(250);
        const QRect expected(expanded.topLeft(), record.initialPhysicalSize);
        require(window->currentNativeGeometry() == expected &&
                    window->persistenceSnapshot().nativeGeometry == expected,
                "a new scale command must replace the pending expansion in live and saved state");
        closeRestoredPinnedWindow(window, record.id);
        return;
    }
    thumbnail->setChecked(true);
    require(window->persistenceSnapshot().preThumbnailNativeGeometry == expanded,
            "reentering during expansion must not replace the saved rectangle with a frame");
    waitForUi(250);
    thumbnail->setChecked(false);
    waitForUi(250);
    require(window->currentNativeGeometry() == expanded,
            "rapid thumbnail toggles must still expand to the original physical rectangle");
    closeRestoredPinnedWindow(window, record.id);
}

void restoredThumbnailScaleMenuStaysConsistentThroughExit(SnowCanvasRuntime&) {
    QScreen* screen = QGuiApplication::primaryScreen();
    require(screen != nullptr, "a primary screen is required");
    const QRect physical = ScreenshotGeometryMapper::physicalRectForScreen(*screen);

    IsolatedPinnedStorage storage;
    // The record describes a window saved in thumbnail mode on a monitor whose
    // recorded DPI is twice the current one. Every persisted physical
    // geometry comes back at its saved pixels: the thumbnail rectangle, the
    // pre-thumbnail rectangle and the scale they encode.
    snow_shot::storage::PinnedWindowRecord record =
        savedPinnedRecord(*screen, 2.0, QSize(800, 400), 50.0, QPoint(200, 120));
    record.thumbnailMode = true;
    record.preThumbnailNativeGeometry = record.nativeGeometry;
    record.nativeGeometry = QRect(physical.topLeft() + QPoint(40, 30), QSize(120, 120));

    ScreenshotSelectionExportUiServices services;
    ScreenshotPinnedWindow* restoredWindow = restoreSeededPinnedWindow(services, record);

    auto* thumbnailAction =
        restoredWindow->findChild<QAction*>(QStringLiteral("screenshotPinnedThumbnailAction"));
    require(thumbnailAction != nullptr, "pinned thumbnail action was not found");
    const QRect expectedThumbnailGeometry(physical.topLeft() + QPoint(40, 30), QSize(120, 120));
    require(restoredWindow->currentNativeGeometry() == expectedThumbnailGeometry,
            "restored thumbnail pinned window should present at the saved thumbnail geometry");
    // The scale menu is reachable while the thumbnail is showing, and it
    // describes the geometry the window will return to.
    require(scaleMenuReadout(*restoredWindow) == QStringLiteral("Current: 50%"),
            "the scale menu should describe the saved scale in thumbnail mode");

    // The thumbnail action mirrors its checked state when the context menu
    // opens, so synchronize it before unchecking to leave thumbnail mode the
    // same way a user does.
    thumbnailAction->setChecked(true);
    thumbnailAction->setChecked(false);

    const QRect expectedGeometry(physical.topLeft() + QPoint(200, 120), QSize(400, 200));
    QElapsedTimer settled;
    settled.start();
    while (restoredWindow->currentNativeGeometry() != expectedGeometry &&
           settled.elapsed() < 2000) {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
    }
    QCoreApplication::processEvents(QEventLoop::AllEvents, 100);

    require(restoredWindow->currentNativeGeometry() == expectedGeometry,
            "leaving thumbnail mode should apply the saved pre-thumbnail geometry");
    require(scaleMenuReadout(*restoredWindow) == QStringLiteral("Current: 50%"),
            "the scale menu should still describe the saved scale after leaving thumbnail mode");

    closeRestoredPinnedWindow(restoredWindow, record.id);
}

void restoredFractionalScaleCopiesTheDisplayedViewport(SnowCanvasRuntime&) {
    QScreen* screen = QGuiApplication::primaryScreen();
    require(screen != nullptr, "a primary screen is required");

    IsolatedPinnedStorage storage;
    const QSize basis(250, 125);
    const snow_shot::storage::PinnedWindowRecord record =
        savedPinnedRecord(*screen, 1.0, basis, 56.8, QPoint(160, 140));

    ScreenshotSelectionExportUiServices services;
    ScreenshotPinnedWindow* restoredWindow = restoreSeededPinnedWindow(services, record);
    const QSize displayedSize = record.nativeGeometry.size();
    require(restoredWindow->currentNativeGeometry().size() == displayedSize,
            "fractional-scale restore should present at the saved physical size");
    require(scaleMenuReadout(*restoredWindow) == QStringLiteral("Current: 57%"),
            "fractional scale should only round in the user-facing readout");

    QAction* copyAction =
        pinnedMenuActionNamed(*restoredWindow, QStringLiteral("screenshotPinnedCopyAction"));
    require(copyAction != nullptr, "restored pinned copy action was not found");
    QApplication::clipboard()->clear();
    copyAction->trigger();
    const QImage copied =
        waitForClipboardImage([](const QImage& image) { return !image.isNull(); });
    require(copied.size() == displayedSize,
            "copying a fractional-scale pin must preserve its displayed physical size");

    closeRestoredPinnedWindow(restoredWindow, record.id);
}

void restoredPinnedWindowKeepsExactWheelLevelAtSameDpi(SnowCanvasRuntime&) {
    QScreen* screen = QGuiApplication::primaryScreen();
    require(screen != nullptr, "a primary screen is required");

    IsolatedPinnedStorage storage;
    // 110% of a 993 px basis is stored as 1092 px, which derives back to
    // 109.97%. The derivation snaps to the displayed whole percent, so the
    // restored window reports the exact 110% level and the wheel notch that
    // targets 120% advances (see pinnedSettledWheelScalingAdvancesPastRoundedLevel
    // for the live case).
    const QSize basis(993, 497);
    const snow_shot::storage::PinnedWindowRecord record =
        savedPinnedRecord(*screen, 1.0, basis, 110.0, QPoint(160, 140));

    ScreenshotSelectionExportUiServices services;
    ScreenshotPinnedWindow* restoredWindow = restoreSeededPinnedWindow(services, record);
    require(restoredWindow->currentNativeGeometry().size() == QSize(1092, 547),
            "same-DPI restore should present at the saved geometry");
    require(scaleMenuReadout(*restoredWindow) == QStringLiteral("Current: 110%"),
            "same-DPI restore should report the saved scale");

    auto* canvas = restoredWindow->findChild<SnowCanvasWidget*>();
    auto* scaleLabel =
        restoredWindow->findChild<QLabel*>(QStringLiteral("screenshotPinnedScaleLabel"));
    require(canvas != nullptr && scaleLabel != nullptr,
            "restored pinned wheel controls were not found");
    const QPoint position = canvas->rect().center();
    QWheelEvent wheel(QPointF(position), QPointF(canvas->mapToGlobal(position)), QPoint(),
                      QPoint(0, 120), Qt::NoButton, Qt::NoModifier, Qt::NoScrollPhase, false);
    QCoreApplication::sendEvent(canvas, &wheel);
    require(wheel.isAccepted(), "restored pinned wheel notch should be consumed");
    waitForUi(50);

    const QSize expectedSize(qRound(basis.width() * 1.2), qRound(basis.height() * 1.2));
    const QSize actualSize = restoredWindow->currentNativeGeometry().size();
    require(qAbs(actualSize.width() - expectedSize.width()) <= 1 &&
                qAbs(actualSize.height() - expectedSize.height()) <= 1 &&
                scaleLabel->text() == QStringLiteral("Scale: 120%"),
            "one wheel notch after a same-DPI restore should advance from 110% to 120%");

    closeRestoredPinnedWindow(restoredWindow, record.id);
}

void pinnedDrawingToolbarMatchesCaptureInteractions(SnowCanvasRuntime&, bool rotateTools = false) {
    QScreen* screen = QGuiApplication::primaryScreen();
    require(screen != nullptr, "a primary screen is required");

    auto* pinnedWindow = new ScreenshotPinnedWindow();
    QPointer<ScreenshotPinnedWindow> guardedWindow(pinnedWindow);
    QImage background(320, 180, QImage::Format_ARGB32_Premultiplied);
    background.fill(QColor(42, 84, 126));

    ScreenshotPinnedWindow::Config config;
    config.nativeGeometry = physicalPinGeometry(*screen, QPoint(40, 40), QSize(400, 400));
    config.canvasSourceRect = QRectF(QPointF(0.0, 0.0), QSizeF(background.size()));
    config.imageSource = ScreenshotImageSource::fromImage(background, config.canvasSourceRect);
    config.screen = screen;
    config.enableEditing = true;
    config.automaticTextRecognition = false;
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

    if (rotateTools) {
        for (int iteration = 0; iteration < 8; ++iteration) {
            for (SnowCanvasTool tool : {SnowCanvasTool::Arrow, SnowCanvasTool::Line,
                                        SnowCanvasTool::Highlight, SnowCanvasTool::Spotlight}) {
                require(canvas->setCanvasTool(tool), "rotating drawing tool must activate");
                QCoreApplication::processEvents();
                const QPointF start(80, 80);
                const QPointF end(180, 140);
                QMouseEvent press(QEvent::MouseButtonPress, start,
                                  canvas->mapToGlobal(start.toPoint()), Qt::LeftButton,
                                  Qt::LeftButton, Qt::NoModifier);
                QCoreApplication::sendEvent(canvas, &press);
                QMouseEvent move(QEvent::MouseMove, end, canvas->mapToGlobal(end.toPoint()),
                                 Qt::NoButton, Qt::LeftButton, Qt::NoModifier);
                QCoreApplication::sendEvent(canvas, &move);
                QMouseEvent release(QEvent::MouseButtonRelease, end,
                                    canvas->mapToGlobal(end.toPoint()), Qt::LeftButton,
                                    Qt::NoButton, Qt::NoModifier);
                QCoreApplication::sendEvent(canvas, &release);
                QCoreApplication::processEvents();
                require(!canvas->grab().isNull(), "rotated drawing must remain renderable");
            }
        }
        pinnedWindow->close();
        require(processUntilDeleted(guardedWindow, 2000), "rotated drawing pin must close");
        return;
    }

    auto* translationButton = toolbar->findChild<adqt::widgets::AdButton*>(
        QStringLiteral("screenshotTextTranslationButton"));
    require(translationButton != nullptr &&
                translationButton->toolTip().contains(QStringLiteral("Text translation")) &&
                translationButton->toolTip().contains(QStringLiteral("Ctrl+T")),
            "pinned drawing toolbar should expose Text translation with its configured shortcut");

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

void pinnedDrawingShortcutsToggleActiveTool() {
    QScreen* screen = QGuiApplication::primaryScreen();
    require(screen != nullptr, "a primary screen is required");
    auto* window = new ScreenshotPinnedWindow();
    QPointer<ScreenshotPinnedWindow> guardedWindow(window);
    QImage background(320, 180, QImage::Format_ARGB32_Premultiplied);
    background.fill(Qt::white);
    ScreenshotPinnedWindow::Config config;
    config.nativeGeometry = physicalPinGeometry(*screen, QPoint(40, 40), background.size());
    config.canvasSourceRect = QRectF(QPointF(), QSizeF(background.size()));
    config.imageSource = ScreenshotImageSource::fromImage(background, config.canvasSourceRect);
    config.screen = screen;
    config.enableEditing = true;
    require(window->present(config), "shortcut test pin presentation failed");
    auto* editButton = buttonNamed(*window, QStringLiteral("Enable drawing mode"));
    require(editButton != nullptr, "drawing mode button was not found");
    editButton->click();
    QCoreApplication::processEvents();

    auto* canvas = window->findChild<SnowCanvasWidget*>();
    auto* controller = window->findChild<ScreenshotPinnedEditController*>();
    require(canvas != nullptr && controller != nullptr && controller->toolbarWindow() != nullptr,
            "drawing shortcut fixture should expose a canvas and toolbar");
    auto* palette = controller->toolbarWindow()->palette();
    require(palette != nullptr, "drawing shortcut fixture should expose its palette");
    const auto pressKey = [canvas](Qt::Key key) {
        QKeyEvent press(QEvent::KeyPress, key, Qt::NoModifier);
        QCoreApplication::sendEvent(canvas, &press);
        QKeyEvent release(QEvent::KeyRelease, key, Qt::NoModifier);
        QCoreApplication::sendEvent(canvas, &release);
    };
    for (const auto& [key, tool] : {std::pair{Qt::Key_P, SnowCanvasTool::FreeDraw},
                                    std::pair{Qt::Key_1, SnowCanvasTool::Shape}}) {
        pressKey(key);
        require(canvas->canvasTool() == tool,
                "the first shortcut press should activate the pinned canvas tool");
        pressKey(key);
        require(canvas->canvasTool() == SnowCanvasTool::Select &&
                    palette->activeToolForTests() == ScreenshotToolPalette::Tool::Select,
                "the second shortcut press should return both canvas and toolbar to Select");
        pressKey(key);
        require(canvas->canvasTool() == tool,
                "the third shortcut press should reactivate the pinned canvas tool");
    }
    require(canvas->setCanvasTool(SnowCanvasTool::Shape), "reset fixture should activate Shape");
    QMouseEvent down(QEvent::MouseButtonPress, QPointF(30, 30), QPointF(30, 30), Qt::LeftButton,
                     Qt::LeftButton, Qt::NoModifier);
    QMouseEvent move(QEvent::MouseMove, QPointF(90, 90), QPointF(90, 90), Qt::NoButton,
                     Qt::LeftButton, Qt::NoModifier);
    QMouseEvent up(QEvent::MouseButtonRelease, QPointF(90, 90), QPointF(90, 90), Qt::LeftButton,
                   Qt::NoButton, Qt::NoModifier);
    QCoreApplication::sendEvent(canvas, &down);
    QCoreApplication::sendEvent(canvas, &move);
    QCoreApplication::sendEvent(canvas, &up);
    require(canvas->canvasHistoryState().canUndo, "pinned reset fixture should contain an edit");
    require(canvas->resetEditingState(), "pinned reset fixture should clear selection");
    palette->setActiveTool(ScreenshotToolPalette::Tool::Select);
    auto* reset =
        palette->findChild<adqt::widgets::AdButton*>(QStringLiteral("screenshotResetCanvasButton"));
    require(reset != nullptr && reset->isEnabled(),
            "pinned canvas reset should be enabled without selection");
    reset->click();
    require(!canvas->canvasHistoryState().canUndo && !canvas->canvasHistoryState().canRedo,
            "pinned reset should clear the canvas document and history");
    window->close();
    require(processUntilDeleted(guardedWindow, 2000), "shortcut test pin should close");
}

void pinnedEditToolbarControlsCanvasHistory(SnowCanvasRuntime&) {
    QScreen* screen = QGuiApplication::primaryScreen();
    require(screen != nullptr, "a primary screen is required");

    auto* pinnedWindow = new ScreenshotPinnedWindow();
    QPointer<ScreenshotPinnedWindow> guardedWindow(pinnedWindow);
    QImage background(320, 180, QImage::Format_ARGB32_Premultiplied);
    background.fill(QColor(42, 84, 126));

    ScreenshotPinnedWindow::Config config;
    config.nativeGeometry = physicalPinGeometry(*screen, QPoint(40, 40), QSize(400, 400));
    config.canvasSourceRect = QRectF(QPointF(0.0, 0.0), QSizeF(background.size()));
    config.imageSource = ScreenshotImageSource::fromImage(background, config.canvasSourceRect);
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
    QPointer<ScreenshotFloatingToolPaletteWindow> guardedToolbar(toolbarWindow);
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
    auto* processAction =
        pinnedMenuActionNamed(*pinnedWindow, QStringLiteral("screenshotPinnedProcessImageMenu"));
    auto* processMenu = qobject_cast<adqt::widgets::AdContextMenu*>(
        processAction != nullptr ? processAction->menu() : nullptr);
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
    require(guardedToolbar == nullptr && controller->toolbarWindow() == nullptr,
            "pinned edit toolbar should be destroyed after confirming the edit");
    require(canvas->canvasTool() == SnowCanvasTool::Select,
            "confirming a pinned edit should restore the select tool");
    require(canvas->canvasStyleToolbarState().source ==
                SnowCanvasStyleToolbarSource::DefaultRectangle,
            "confirming a pinned edit should clear the canvas selection");

    const QPoint nativeMoveDelta(24, 18);
    const QRect nativeGeometryBeforeMove = pinnedWindow->currentNativeGeometry();
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
    const QPoint pinnedPositionBeforeMove = pinnedWindow->pos();
    pinnedWindow->move(pinnedPositionBeforeMove + nativeMoveDelta);
#endif
    QCoreApplication::processEvents(QEventLoop::AllEvents, 50);
    require(pinnedWindow->currentNativeGeometry().topLeft() ==
                nativeGeometryBeforeMove.topLeft() + nativeMoveDelta,
            "a pin should remain movable after its drawing toolbar is destroyed");

    editButton->click();
    QCoreApplication::processEvents(QEventLoop::AllEvents, 50);
    ScreenshotFloatingToolPaletteWindow* recreatedToolbar = controller->toolbarWindow();
    QPointer<ScreenshotFloatingToolPaletteWindow> guardedRecreatedToolbar(recreatedToolbar);
    require(recreatedToolbar != nullptr && recreatedToolbar->isVisible(),
            "re-entering drawing mode should create and show a fresh toolbar");
    require(recreatedToolbar->windowHandle() != nullptr &&
                pinnedWindow->windowHandle() != nullptr &&
                recreatedToolbar->windowHandle()->transientParent() == pinnedWindow->windowHandle(),
            "a recreated drawing toolbar should restore pinned-window ownership");
    ScreenshotToolPalette* recreatedPalette = recreatedToolbar->palette();
    require(recreatedPalette != nullptr && recreatedPalette->findChild<adqt::widgets::AdButton*>(
                                               QStringLiteral("screenshotUndoButton")) != nullptr,
            "a recreated drawing toolbar should restore its command controls");

    auto* thumbnailAction =
        pinnedWindow->findChild<QAction*>(QStringLiteral("screenshotPinnedThumbnailAction"));
    require(thumbnailAction != nullptr, "pinned thumbnail action was not found");
    thumbnailAction->setChecked(true);
    QCoreApplication::processEvents(QEventLoop::AllEvents, 50);
    require(guardedRecreatedToolbar == nullptr && controller->toolbarWindow() == nullptr,
            "leaving drawing mode for thumbnail mode should destroy the toolbar immediately");

    QPushButton* closeButton = buttonNamed(*pinnedWindow, QStringLiteral("Close"));
    require(closeButton != nullptr, "close button was not found");
    closeButton->click();
    require(processUntilDeleted(guardedWindow, 2000),
            "pinned window was not deleted after the history test");
}
void pinnedRecognitionSaveSnapshotsAndRoutesOffscreen() {
#if defined(Q_OS_WIN)
    require(QFontDatabase::addApplicationFont(QStringLiteral("C:/Windows/Fonts/segoeui.ttf")) >= 0,
            "load offscreen recognition font");
    QApplication::setFont(QFont(QStringLiteral("Segoe UI")));
#endif

    QTemporaryDir directory;
    const snow_shot::storage::ScreenshotSettings output;
    const snow_shot::storage::TextRecognitionSettings settings;
    require(directory.isValid() && output.setImageSaveDirectory(directory.path()) &&
                output.setLastManualSaveDirectory(directory.path()) &&
                output.setLastManualSaveFormat(QStringLiteral("png")) &&
                output.setImageFormat(QStringLiteral("png")) &&
                output.setAutoSaveFilenameFormat(QStringLiteral("Recognized")) &&
                settings.setSaveRecognitionResultAsImage(true),
            "recognition save settings");
    ScreenshotPinnedWindow window;
    auto config = cachedOcrPinConfig(nullptr);
    config.recognitionResults.text->presentation->lines[0].sourceLineQuads = {
        config.recognitionResults.text->presentation->lines[0].quad};
    config.recognitionResults.translatedText =
        std::make_shared<ScreenshotOcrPresentation>(*config.recognitionResults.text->presentation);
    config.recognitionResults.translatedText->setLineText(0, QStringLiteral("Translated result"));
    ScreenshotPinnedWindowTestAccess::restoreOffscreen(window, config);
    auto* session = ScreenshotPinnedWindowTestAccess::recognitionOffscreen(window, config);
    const auto pixels = [&](std::shared_ptr<ScreenshotExportArtifact> artifact) {
        QImage result;
        bool complete = false;
        require(artifact &&
                    artifact->requestImage(&window,
                                           [&](ScreenshotExportImageResult rendered) {
                                               require(rendered.succeeded(),
                                                       "recognition artifact renders successfully");
                                               result = rendered.image;
                                               complete = true;
                                           }),
                "recognition artifact schedules");
        QElapsedTimer timer;
        timer.start();
        while (!complete && timer.elapsed() < 10000)
            waitForUi(5);
        require(complete, "recognition artifact completes");
        return result;
    };
    const QImage recognized = pixels(ScreenshotPinnedWindowTestAccess::fileSave(window));
    require(recognized != config.imageSource.materializedImage, "OCR text appears in saved image");
    require(settings.setSaveRecognitionResultAsImage(false),
            "disable recognition saving on open pin");
    require(pixels(ScreenshotPinnedWindowTestAccess::fileSave(window)) ==
                config.imageSource.materializedImage,
            "disabled option preserves ordinary image export");
    require(settings.setSaveRecognitionResultAsImage(true), "reenable recognition saving");
    require(session->activateCachedTextTranslation(), "activate cached original-image translation");
    auto frozen = ScreenshotPinnedWindowTestAccess::fileSave(window);
    const QImage translated = pixels(frozen);
    require(translated != recognized, "current translation replaces source OCR text");
    session->endTextEditing();
    require(pixels(frozen) == translated,
            "later display changes cannot alter prepared save artifact");
    session->beginTextEditing();
    require(pixels(ScreenshotPinnedWindowTestAccess::fileSave(window)) ==
                config.imageSource.materializedImage,
            "text-only editor preserves ordinary image export");
    session->endTextEditing();
    const auto waitFile = [&](const QString& path) {
        QElapsedTimer timer;
        timer.start();
        while ((!QFileInfo::exists(path) || window.property("saveDialogOpen").toBool()) &&
               timer.elapsed() < 10000)
            waitForUi(10);
        require(QFileInfo::exists(path), "recognition image file produced");
        waitForUi(20);
        return QImage(path).convertToFormat(QImage::Format_ARGB32_Premultiplied);
    };
    ScreenshotPinnedWindowTestAccess::quickSave(window);
    require(waitFile(directory.filePath(QStringLiteral("Recognized.png"))) == recognized,
            "Quick Save exports recognition pixels");
    require(output.setSaveAsFileDialog(QStringLiteral("snow_shot")), "select custom save dialog");
    ScreenshotPinnedWindowTestAccess::saveAsFile(window);
    auto* modal =
        window.findChild<adqt::widgets::AdModal*>(QStringLiteral("screenshotSaveAsFileModal"));
    require(modal, "recognition custom save dialog opens");
    require(session->activateCachedTextTranslation(), "restore translation during save dialog");
    QElapsedTimer ready;
    ready.start();
    while (!modal->acceptButton()->isEnabled() && ready.elapsed() < 10000)
        waitForUi(5);
    require(modal->acceptButton()->isEnabled(), "recognition preview becomes ready");
    modal->contentWidget()
        ->findChild<adqt::widgets::AdLineEdit*>(QStringLiteral("saveFilenameInput"))
        ->setText(QStringLiteral("frozen.png"));
    modal->acceptButton()->click();
    require(waitFile(directory.filePath(QStringLiteral("frozen.png"))) == recognized,
            "custom dialog saves activation snapshot despite later translation updates");
    require(output.setSaveAsFileDialog(QStringLiteral("system")), "select native dialog flow");
    const bool native = QApplication::testAttribute(Qt::AA_DontUseNativeDialogs);
    QApplication::setAttribute(Qt::AA_DontUseNativeDialogs, true);
    const auto restoreNative =
        qScopeGuard([native] { QApplication::setAttribute(Qt::AA_DontUseNativeDialogs, native); });
    QTimer accept;
    accept.setInterval(10);
    QObject::connect(&accept, &QTimer::timeout, &window, [&] {
        for (QWidget* widget : QApplication::topLevelWidgets()) {
            if (auto* dialog = qobject_cast<QFileDialog*>(widget)) {
                session->endTextEditing();
                dialog->selectFile(directory.filePath(QStringLiteral("native.png")));
                QMetaObject::invokeMethod(dialog, "accept", Qt::DirectConnection);
            }
        }
    });
    accept.start();
    ScreenshotPinnedWindowTestAccess::saveAsFile(window);
    accept.stop();
    require(waitFile(directory.filePath(QStringLiteral("native.png"))) == translated,
            "native dialog also freezes recognition before modal event processing");
    ScreenshotPinnedWindowTestAccess::leaveViewportAtPreviousDpi(window, 1.5);
    require(pixels(ScreenshotPinnedWindowTestAccess::fileSave(window)) == recognized,
            "window zoom and display DPI do not change exported recognition pixels");
    ScreenshotPinnedWindowTestAccess::rotateRecognitionOffscreen(window);
    const auto& displayed = ScreenshotPinnedWindowTestAccess::displayedRecognition(window);
    require(displayed.lines[0].sourceLineQuads[0] == displayed.lines[0].quad &&
                pixels(ScreenshotPinnedWindowTestAccess::fileSave(window)).size() ==
                    QSize(180, 320),
            "rotation maps paragraph background regions together with text and native dimensions");
}

void pinnedQuickSaveKeepsWindowAndConfiguredOutput() {
    const snow_shot::storage::ScreenshotSettings settings;
    QTemporaryDir directory;
    const QString output = directory.filePath(QStringLiteral("new/nested"));
    require(directory.isValid() && settings.setImageSaveDirectory(output) &&
                settings.setImageFormat(QStringLiteral("png")) &&
                settings.setAutoSaveFilenameFormat(QStringLiteral("PinnedQuick")) &&
                settings.setLastManualSaveDirectory(directory.path()) &&
                settings.setLastManualSaveFormat(QStringLiteral("jpeg")),
            "pinned quick-save settings unavailable");
    QImage image(160, 100, QImage::Format_ARGB32_Premultiplied);
    image.fill(QColor(25, 120, 180));
    auto* window = new ScreenshotPinnedWindow;
    QPointer<ScreenshotPinnedWindow> guarded(window);
    ScreenshotPinnedWindow::Config config;
    config.screen = QGuiApplication::primaryScreen();
    config.nativeGeometry = physicalPinGeometry(*config.screen, QPoint(40, 40), image.size());
    config.canvasSourceRect = QRectF(QPointF(), QSizeF(image.size()));
    config.imageSource = ScreenshotImageSource::fromImage(image, config.canvasSourceRect);
    config.automaticTextRecognition = false;
    config.enableEditing = true;
    require(window->present(config), "pinned quick-save source unavailable");
    waitForUi(50);
    auto* edit = buttonNamed(*window, QStringLiteral("Enable drawing mode"));
    require(edit, "pinned drawing control unavailable");
    edit->click();
    waitForUi(50);
    auto* controller = window->findChild<ScreenshotPinnedEditController*>();
    auto* toolbar = controller && controller->toolbarWindow()
                        ? controller->toolbarWindow()->palette()
                        : nullptr;
    require(toolbar, "pinned quick-save toolbar unavailable");
    auto* canvas = window->findChild<SnowCanvasWidget*>();
    require(canvas && canvas->setCanvasTool(SnowCanvasTool::Shape),
            "pinned quick-save drawing fixture unavailable");
    SnowCanvasShapeStyle annotation;
    annotation.stroke = QColor(240, 20, 20);
    annotation.strokeWidth = 4.0;
    require(canvas->setCanvasShapeStylePatch(annotation,
                                             SnowCanvasShapeStylePropertyStrokeColor |
                                                 SnowCanvasShapeStylePropertyStrokeWidth,
                                             SnowCanvasShapeKind::Rectangle),
            "quick-save annotation style unavailable");
    const auto pointer = [canvas](QEvent::Type type, QPointF position, Qt::MouseButton button,
                                  Qt::MouseButtons buttons) {
        QMouseEvent event(type, position, canvas->mapToGlobal(position.toPoint()), button, buttons,
                          Qt::NoModifier);
        QCoreApplication::sendEvent(canvas, &event);
    };
    pointer(QEvent::MouseButtonPress, QPointF(20, 20), Qt::LeftButton, Qt::LeftButton);
    pointer(QEvent::MouseMove, QPointF(100, 70), Qt::NoButton, Qt::LeftButton);
    pointer(QEvent::MouseButtonRelease, QPointF(100, 70), Qt::LeftButton, Qt::NoButton);
    require(canvas->canvasHistoryState().canUndo, "quick-save annotation was not committed");

    auto* quick =
        toolbar->findChild<adqt::widgets::AdButton*>(QStringLiteral("screenshotQuickSaveButton"));
    require(quick, "pinned quick-save source button unavailable");
    QApplication::clipboard()->setText(QStringLiteral("Keep clipboard"));
    const auto waitForFile = [&](const QString& path) {
        QElapsedTimer timer;
        timer.start();
        while (!QFileInfo::exists(path) && timer.elapsed() < 10000)
            waitForUi(10);
        require(QFileInfo::exists(path), "pinned quick-save did not produce its file");
        waitForUi(30);
    };
    for (const QString& dialog : {QStringLiteral("system"), QStringLiteral("snow_shot")}) {
        require(settings.setSaveAsFileDialog(dialog), "pinned dialog setting unavailable");
        const QString name = dialog == QStringLiteral("system")
                                 ? QStringLiteral("PinnedQuick.png")
                                 : QStringLiteral("PinnedQuick_1.png");
        quick->click();
        quick->click();
        waitForFile(QDir(output).filePath(name));
        const QImage saved(QDir(output).filePath(name));
        bool hasAnnotation = false;
        for (int y = 0; y < saved.height(); ++y) {
            for (int x = 0; x < saved.width(); ++x) {
                const QColor pixel = saved.pixelColor(x, y);
                hasAnnotation =
                    hasAnnotation || (pixel.red() > 180 && pixel.green() < 80 && pixel.blue() < 80);
            }
        }
        require(hasAnnotation, "pinned Quick save must include the current annotations");

        require(window->isVisible() && controller->editMode() &&
                    !window->property("saveDialogOpen").toBool() &&
                    !window->findChild<adqt::widgets::AdModal*>() &&
                    QImage(QDir(output).filePath(name)).size() == image.size(),
                "quick-save must bypass both dialogs and retain the edited pin");
    }
    require(
        QDir(output).entryList(QDir::Files).size() == 2 &&
            QApplication::clipboard()->text() == QStringLiteral("Keep clipboard") &&
            settings.lastManualSaveDirectory() == directory.path() &&
            settings.lastManualSaveFormat() == QStringLiteral("jpeg"),
        "duplicate quick-save requests must coalesce without changing clipboard/manual settings");
    require(settings.setImageSaveDirectory(QString()), "empty directory setup failed");
    quick->click();
    waitForUi(100);
    require(window->isVisible() && QDir(output).entryList(QDir::Files).size() == 2,
            "a quick-save failure must retain the pin and produce no fallback file");
    require(settings.setImageSaveDirectory(output), "retry directory setup failed");
    quick->click();
    waitForFile(QDir(output).filePath(QStringLiteral("PinnedQuick_2.png")));
    quick->click();
    window->close();
    require(processUntilDeleted(guarded, 2000),
            "closing a pin must cancel pending quick-save safely");
}

void pinnedSaveDialogRoutingAndCancellation() {
    using adqt::widgets::AdLineEdit;
    using adqt::widgets::AdModal;
    const snow_shot::storage::ScreenshotSettings settings;
    QTemporaryDir directory;
    require(directory.isValid() && settings.setImageSaveDirectory(directory.path()),
            "pinned save test directory unavailable");
    QImage image(160, 100, QImage::Format_ARGB32_Premultiplied);
    image.fill(QColor(25, 120, 180));
    auto* window = new ScreenshotPinnedWindow;
    QPointer<ScreenshotPinnedWindow> guarded(window);
    ScreenshotPinnedWindow::Config config;
    config.screen = QGuiApplication::primaryScreen();
    config.nativeGeometry = physicalPinGeometry(*config.screen, QPoint(40, 40), image.size());
    config.canvasSourceRect = QRectF(QPointF(), QSizeF(image.size()));
    config.imageSource = ScreenshotImageSource::fromImage(image, config.canvasSourceRect);
    config.automaticTextRecognition = false;
    require(window->present(config), "pinned save source could not be presented");
    waitForUi(50);
    auto* action =
        pinnedMenuActionNamed(*window, QStringLiteral("screenshotPinnedSaveAsFileAction"));
    require(action, "pinned Save as file action missing");

    require(settings.setSaveAsFileDialog(QStringLiteral("system")), "system routing setup failed");
    const bool previousNativeSetting = QApplication::testAttribute(Qt::AA_DontUseNativeDialogs);
    QApplication::setAttribute(Qt::AA_DontUseNativeDialogs, true);
    const auto restoreNativeSetting = qScopeGuard([previousNativeSetting] {
        QApplication::setAttribute(Qt::AA_DontUseNativeDialogs, previousNativeSetting);
    });
    require(settings.setLastManualSaveFormat(QStringLiteral("jpeg")),
            "remembered native format setup failed");
    bool systemDialogSeen = false;
    bool rememberedFormatSeen = false;
    QTimer dismiss;
    dismiss.setInterval(10);
    QObject::connect(&dismiss, &QTimer::timeout, window, [&] {
        for (auto* widget : QApplication::topLevelWidgets()) {
            if (auto* dialog = qobject_cast<QFileDialog*>(widget)) {
                systemDialogSeen = true;
                rememberedFormatSeen =
                    dialog->selectedNameFilter().contains(QStringLiteral("*.jpg")) &&
                    dialog->selectedFiles().value(0).endsWith(QStringLiteral(".jpg"));
                for (const QString& filter : dialog->nameFilters()) {
                    if (filter.contains(QStringLiteral("*.png")))
                        dialog->selectNameFilter(filter);
                }
                rememberedFormatSeen =
                    rememberedFormatSeen &&
                    dialog->selectedNameFilter().contains(QStringLiteral("*.png"));
                dialog->reject();
            }
        }
    });
    dismiss.start();
    action->trigger();
    dismiss.stop();
    require(systemDialogSeen && rememberedFormatSeen &&
                settings.lastManualSaveFormat() == QStringLiteral("jpeg") &&
                !window->findChild<AdModal*>(QStringLiteral("screenshotSaveAsFileModal")),
            "System must retain the QFileDialog route");
    const QString nativeDirectory = directory.filePath(QStringLiteral("native"));
    require(QDir().mkpath(nativeDirectory), "native save directory setup failed");
    QTimer accept;
    accept.setInterval(10);
    QObject::connect(&accept, &QTimer::timeout, window, [&] {
        for (auto* widget : QApplication::topLevelWidgets()) {
            if (auto* dialog = qobject_cast<QFileDialog*>(widget)) {
                // A recognized suffix takes precedence over the remembered JPEG filter.
                dialog->selectFile(QDir(nativeDirectory).filePath(QStringLiteral("native.bmp")));
                QMetaObject::invokeMethod(dialog, "accept", Qt::DirectConnection);
            }
        }
    });
    accept.start();
    action->trigger();
    accept.stop();
    require(settings.lastManualSaveFormat() == QStringLiteral("bmp"),
            "native save must remember the accepted filename's effective format");
    QElapsedTimer nativeSaved;
    nativeSaved.start();
    while (settings.lastManualSaveDirectory() != nativeDirectory && nativeSaved.elapsed() < 10000)
        waitForUi(10);
    require(settings.lastManualSaveDirectory() == nativeDirectory &&
                QFileInfo::exists(QDir(nativeDirectory).filePath(QStringLiteral("native.bmp"))),
            "native save must finish writing the selected format");
    require(settings.setLastManualSaveDirectory(directory.path()),
            "restore custom save directory failed");
    require(settings.setLastManualSaveFormat(QStringLiteral("png")) &&
                settings.setSaveAsFileDialog(QStringLiteral("snow_shot")),
            "Snow Shot routing setup failed");
    auto* editButton = buttonNamed(*window, QStringLiteral("Enable drawing mode"));
    require(editButton, "pinned save test drawing button missing");
    editButton->click();
    waitForUi(30);
    auto* editController = window->findChild<ScreenshotPinnedEditController*>();
    auto* toolbar = editController ? editController->toolbarWindow() : nullptr;
    require(toolbar && toolbar->isVisible(), "pinned save test toolbar must be visible");
    action->trigger();
    auto* modal = window->findChild<AdModal*>(QStringLiteral("screenshotSaveAsFileModal"));
    require(modal && modal->mode() == AdModal::Mode::Window,
            "pinned save must open Snow Shot dialog");
    require(toolbar->isVisible() &&
                modal->contentWidget()->window()->windowHandle()->transientParent() ==
                    toolbar->windowHandle(),
            "pinned save must stay above its visible editing toolbar");
    modal->rejectButton()->click();
    waitForUi(30);
    QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
    require(guarded && guarded->isVisible() && !guarded->property("saveDialogOpen").toBool() &&
                QDir(directory.path()).entryList(QDir::Files).isEmpty(),
            "cancel must keep pinned window editable and save no file");
    action->trigger();
    modal = window->findChild<AdModal*>(QStringLiteral("screenshotSaveAsFileModal"));
    require(modal, "pinned save must reopen after cancel");
    QElapsedTimer ready;
    ready.start();
    while (!modal->acceptButton()->isEnabled() && ready.elapsed() < 10000)
        waitForUi(10);
    if (!modal->acceptButton()->isEnabled()) {
        auto* error = modal->contentWidget()->findChild<QLabel*>(QStringLiteral("saveErrorLabel"));
        if (error)
            std::cerr << "pinned dialog: " << error->text().toStdString() << '\n';
    }
    require(modal->acceptButton()->isEnabled(), "pinned export source failed to load");
    modal->contentWidget()
        ->findChild<AdLineEdit*>(QStringLiteral("saveFilenameInput"))
        ->setText(QStringLiteral("pin.png"));
    modal->acceptButton()->click();
    const QString path = QDir(directory.path()).filePath(QStringLiteral("pin.png"));
    ready.restart();
    while ((!QFileInfo::exists(path) || window->property("saveDialogOpen").toBool()) &&
           ready.elapsed() < 10000)
        waitForUi(10);
    require(QFileInfo::exists(path) && window->isVisible() &&
                settings.lastManualSaveDirectory() == directory.path(),
            "successful pinned save must preserve window and remember destination");
    window->close();
    require(processUntilDeleted(guarded, 2000), "pinned save fixture did not close");
}
#ifdef Q_OS_WIN
void pinnedCloseReleaseNative() {
    close_release_native_test::Receiver receiver;
    const snow_shot::storage::PinToScreenSettings settings;
    require(settings.setMiddleMouseButtonAction(QStringLiteral("close")),
            "configure native middle close");
    require(settings.setDoubleClickAction(QStringLiteral("close")),
            "configure native double close");
    for (const auto button : {Qt::NoButton, Qt::MiddleButton, Qt::LeftButton}) {
        QImage image(500, 350, QImage::Format_ARGB32_Premultiplied);
        image.fill(Qt::white);
        auto* window = new ScreenshotPinnedWindow;
        QPointer<ScreenshotPinnedWindow> guarded(window);
        ScreenshotPinnedWindow::Config config;
        config.nativeGeometry = QRect(150, 150, 500, 350);
        config.canvasSourceRect = QRectF(image.rect());
        config.imageSource = ScreenshotImageSource::fromImage(image, config.canvasSourceRect);
        config.screen = QGuiApplication::primaryScreen();
        config.automaticTextRecognition = false;
        require(window->present(config), "present native close test pin");
        waitForUi(100);
        receiver.verify(*window, button, button == Qt::LeftButton, button != Qt::NoButton);
        require(processUntilDeleted(guarded, 2000), "native release must retire the last pin");
    }
}
#endif

} // namespace

int main(int argc, char* argv[]) {
    PinnedWindowTestApplication app(argc, argv);
#ifdef Q_OS_WIN
    if (close_release_native_test::receiverRequested()) {
        return close_release_native_test::runReceiver();
    }
#endif

    QApplication::setQuitOnLastWindowClosed(false);

    try {
        // Keep the whole binary hermetic from the first product call:
        // without this, lazily initialized storage lands in the developer's
        // real AppData (see IsolatedPinnedStorage).
        IsolatedPinnedStorage processStorage;
#ifdef Q_OS_WIN
        if (app.arguments().contains(QStringLiteral("--close-release-native"))) {
            pinnedCloseReleaseNative();
            return 0;
        }
#endif
        SnowCanvasRuntime sourceRuntime;
        require(sourceRuntime.isValid(), "source runtime creation failed");
        if (app.arguments().contains(QStringLiteral("--scale-readout-only"))) {
            pinnedReadoutOffscreen();
            return 0;
        }
        if (app.arguments().contains(QStringLiteral("--passive-geometry-only"))) {
            pinnedGeometryQueriesDoNotCreateNativeWindows();
            return 0;
        }
        if (app.arguments().contains(QStringLiteral("--escape-activation-only"))) {
            pinnedEscapeBurst(true);
            return 0;
        }
        if (app.arguments().contains(QStringLiteral("--escape-burst-only"))) {
            pinnedEscapeBurst();
            return 0;
        }
        if (app.arguments().contains(QStringLiteral("--tool-rotation-only"))) {
            pinnedDrawingToolbarMatchesCaptureInteractions(sourceRuntime, true);
            return 0;
        }
        if (app.arguments().contains(QStringLiteral("--middle-click-only"))) {
            pinnedMiddleClickActions();
            if (QGuiApplication::platformName() != QStringLiteral("offscreen")) {
                pinnedOcrDoubleClickUsesDragRegion(true);
            }
            return 0;
        }
        if (app.arguments().contains(QStringLiteral("--double-click-only"))) {
            if (QGuiApplication::platformName() == QStringLiteral("offscreen")) {
                pinnedOffscreenDoubleClickActions();
            } else {
                enlargedPinnedThumbnailRemainsVisible();
                pinnedThumbnailTracksCurrentMousePosition();
                pinnedDoubleClickActions();
                pinnedOcrDoubleClickUsesDragRegion();
            }
            return 0;
        }
        if (app.arguments().contains(QStringLiteral("--translation-only"))) {
            runPinnedOriginalImageTranslationTests();
            return 0;
        }
        if (app.arguments().contains(QStringLiteral("--image-conversion-only"))) {
            pinnedImageConversionsSurviveRestartWithoutProvider();
            return 0;
        }
        if (app.arguments().contains(QStringLiteral("--recognition-save-only"))) {
            pinnedRecognitionSaveSnapshotsAndRoutesOffscreen();
            return 0;
        }
        if (app.arguments().contains(QStringLiteral("--quick-save-only"))) {
            pinnedQuickSaveKeepsWindowAndConfiguredOutput();
            return 0;
        }
        if (app.arguments().contains(QStringLiteral("--save-dialog-only"))) {
            pinnedSaveDialogRoutingAndCancellation();
            return 0;
        }
        if (app.arguments().contains(QStringLiteral("--shortcut-display-only"))) {
            pinnedShortcutDisplayUsesSettingsFormat();
            return 0;
        }
        if (app.arguments().contains(QStringLiteral("--pinned-shortcut-only"))) {
            pinnedConfiguredShortcutUpdatesImmediately(sourceRuntime);
            return 0;
        }
        if (app.arguments().contains(QStringLiteral("--movement-shortcut-only"))) {
            pinnedMovementShortcutsMoveIdleWindow();
            return 0;
        }
        if (app.arguments().contains(QStringLiteral("--drawing-shortcut-toggle-only"))) {
            pinnedDrawingShortcutsToggleActiveTool();
            return 0;
        }
        if (app.arguments().contains(QStringLiteral("--toolbar-lifecycle-only"))) {
            pinnedEditToolbarControlsCanvasHistory(sourceRuntime);
            return 0;
        }
        if (app.arguments().contains(QStringLiteral("--large-edit-only"))) {
            pinnedLargeImageRemainsOpenWhenEnteringDrawingMode(sourceRuntime);
            return 0;
        }
        if (app.arguments().contains(QStringLiteral("--async-presentation-only"))) {
            pinnedAsyncPresentationDefersContent(sourceRuntime);
            return 0;
        }
        if (app.arguments().contains(QStringLiteral("--pooling-only"))) {
            pinnedWindowPoolReusesAndReplenishesPreparedShell();
            return 0;
        }
        if (app.arguments().contains(QStringLiteral("--qr-copy-only"))) {
            pinnedQrResultCopiesWithKeyboardShortcut();
            return 0;
        }
        if (app.arguments().contains(QStringLiteral("--cached-ocr-only"))) {
            pinnedSelectionRendersCachedOcrInCanvasCoordinates();
            return 0;
        }
        if (app.arguments().contains(QStringLiteral("--initial-ocr-only"))) {
            pinnedSelectionRendersCachedOcrInCanvasCoordinates(false, false, true);
            return 0;
        }
        if (app.arguments().contains(QStringLiteral("--restored-ocr-only"))) {
            restoredPinnedSelectionRendersCachedOcrAfterStorageRestart();
            return 0;
        }
        if (app.arguments().contains(QStringLiteral("--early-ocr-snapshot-only"))) {
            pinnedSnapshotRetainsRecognitionBeforeDeferredSetup();
            return 0;
        }
        if (app.arguments().contains(QStringLiteral("--invalid-ocr-restore-only"))) {
            restoredInvalidOcrDoesNotSuppressRecognition();
            return 0;
        }
        if (app.arguments().contains(QStringLiteral("--recognition-shortcut-only"))) {
            pinnedEditingRecognitionShortcutsUsePaletteCommands();
            pinnedRecognitionShortcutTogglesResults();
            return 0;
        }
        if (app.arguments().contains(QStringLiteral("--cached-ocr-provider-only"))) {
            cachedPinnedOcrAvailableWithoutRecognitionProvider();
            return 0;
        }
        if (app.arguments().contains(QStringLiteral("--transformed-ocr-only"))) {
            pinnedTransformResetPersistsWithoutResize();
            transformedPinnedOcrTracksCanvasViewport();
            return 0;
        }
        if (app.arguments().contains(QStringLiteral("--lazy-recognition-only"))) {
            pinnedRecognitionAvailableThroughLazyProvider();
            return 0;
        }
        if (app.arguments().contains(QStringLiteral("--group-menu-only"))) {
            groupMenuActionsExposeIconsAndCleanupState();
            return 0;
        }
        if (app.arguments().contains(QStringLiteral("--deferred-presentation-only"))) {
            pinnedAsyncPresentationDefersContent(sourceRuntime);
            return 0;
        }
        if (app.arguments().contains(QStringLiteral("--deferred-group-switch-only"))) {
            pinnedDeferredPresentationSurvivesGroupSwitch(sourceRuntime);
            return 0;
        }
        if (app.arguments().contains(QStringLiteral("--deferred-user-close-only"))) {
            deferredPinUserCloseCancelsLateMaterialization();
            return 0;
        }
        if (app.arguments().contains(QStringLiteral("--restore-wiring-only"))) {
            restoredPinnedWindowIgnoresMonitorDpiChange(sourceRuntime);
            restoredThumbnailScaleMenuStaysConsistentThroughExit(sourceRuntime);
            restoredFractionalScaleCopiesTheDisplayedViewport(sourceRuntime);
            restoredPinnedWindowKeepsExactWheelLevelAtSameDpi(sourceRuntime);
            return 0;
        }
        for (const QString& scenario : {QStringLiteral("appearance"), QStringLiteral("dpi"),
                                        QStringLiteral("snapshot"), QStringLiteral("copy")}) {
            if (app.arguments().contains(QStringLiteral("--thumbnail-%1-only").arg(scenario))) {
                restoredThumbnailStateOffscreen(scenario);
                return 0;
            }
        }
        if (app.arguments().contains(QStringLiteral("--thumbnail-recreation-only"))) {
            thumbnailAnimationSurvivesRecreation(true);
            thumbnailAnimationSurvivesRecreation(false);
            return 0;
        }
        if (app.arguments().contains(QStringLiteral("--thumbnail-reentry-only"))) {
            thumbnailReentryPreservesExpandedGeometry();
            thumbnailReentryPreservesExpandedGeometry(true);
            return 0;
        }
        if (app.arguments().contains(QStringLiteral("--tray-pin-runtime-only"))) {
            pinnedControlsMatchReferenceStyle(sourceRuntime);
            return 0;
        }
#if defined(Q_OS_WIN) || defined(_WIN32)
        if (app.arguments().contains(QStringLiteral("--system-move-loop-only"))) {
            pinnedSystemMoveLoopAcceptsMovementShortcuts();
            return 0;
        }
        if (app.arguments().contains(QStringLiteral("--native-drag-shortcut-only"))) {
            pinnedNativeDragAcceptsCursorMovementShortcuts(sourceRuntime);
            return 0;
        }
        if (app.arguments().contains(QStringLiteral("--native-drag-dpi-only"))) {
            pinnedNativeDragCrossingDpiBoundaryPreservesDestination(sourceRuntime);
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
        restoredPinnedWindowIgnoresMonitorDpiChange(sourceRuntime);
        restoredThumbnailScaleMenuStaysConsistentThroughExit(sourceRuntime);
        restoredFractionalScaleCopiesTheDisplayedViewport(sourceRuntime);
        restoredPinnedWindowKeepsExactWheelLevelAtSameDpi(sourceRuntime);
        pinnedCopyIncludesSourceCanvasDrawing();
        pinnedQrResultCopiesWithKeyboardShortcut();
        pinnedSelectionRendersCachedOcrInCanvasCoordinates();
        restoredPinnedSelectionRendersCachedOcrAfterStorageRestart();
        pinnedSnapshotRetainsRecognitionBeforeDeferredSetup();
        restoredInvalidOcrDoesNotSuppressRecognition();
        pinnedRecognitionShortcutTogglesResults();
        pinnedEditingRecognitionShortcutsUsePaletteCommands();
        cachedPinnedOcrAvailableWithoutRecognitionProvider();
        transformedPinnedOcrTracksCanvasViewport();
        pinnedTransformResetPersistsWithoutResize();
        groupedPinnedWindowSignalConnectionsDoNotAssert();
        groupMenuActionsExposeIconsAndCleanupState();
        pinnedRecognitionAvailableThroughLazyProvider();
        pinnedDeferredPresentationSurvivesGroupSwitch(sourceRuntime);
        deferredPinUserCloseCancelsLateMaterialization();
        pinnedAsyncPresentationDefersContent(sourceRuntime);
        pinnedControlsMatchReferenceStyle(sourceRuntime);
        pinnedThumbnailUsesOpaqueThemeBackground(sourceRuntime);
        pinnedControlsHideBelowMinimumNativeSize(sourceRuntime);
        pinnedLargeImageRemainsOpenWhenEnteringDrawingMode(sourceRuntime);
        pinnedDrawingShortcutsToggleActiveTool();
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
