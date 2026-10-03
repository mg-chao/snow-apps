#include "snow_draw_engine_qt/snow_canvas_image.h"
#include "snow_shot/presentation/screenshotoriginalimagepreviewwindow.h"

#include "snow_shot/presentation/screenshotimagerendering.h"
#include "widgets/dpi_stable_window_controller.h"

#ifdef Q_OS_MACOS
#include "../pinned/pinnedwindowplatform.h"
#include "snow_shot/platform/screenshotnative.h"
#endif

#include <QApplication>
#include <QEvent>
#include <QHideEvent>
#include <QPaintEvent>
#include <QPainter>
#include <QResizeEvent>
#include <QScreen>
#include <QScopedValueRollback>
#include <QScopeGuard>
#include <QTimer>
#include <QWindow>

#include <algorithm>
#include <cmath>
#include <limits>
#include <utility>

#if defined(Q_OS_WIN) || defined(_WIN32)
#include <QtGui/qscreen_platform.h>
#include <qt_windows.h>
#endif

namespace {
constexpr int kPreviewGap = 4;

bool finiteRect(const QRectF& rect) {
    return rect.isValid() && !rect.isEmpty() && std::isfinite(rect.x()) &&
           std::isfinite(rect.y()) && std::isfinite(rect.width()) && std::isfinite(rect.height()) &&
           std::isfinite(rect.x() + rect.width()) && std::isfinite(rect.y() + rect.height());
}

struct PlacementContext final {
    QScreen* screen = nullptr;
    QRect workArea;
};

qreal distanceToRect(const QPointF& point, const QRect& rect) {
    const qreal dx =
        point.x() - std::clamp(point.x(), qreal(rect.left()), qreal(rect.x()) + rect.width());
    const qreal dy =
        point.y() - std::clamp(point.y(), qreal(rect.top()), qreal(rect.y()) + rect.height());
    return dx * dx + dy * dy;
}

PlacementContext placementContext(const QRect& result) {
#if defined(Q_OS_WIN) || defined(_WIN32)
    if (ScreenshotOriginalImagePreviewWindow::usesPhysicalGeometry()) {
        const RECT rectangle{result.x(), result.y(), result.x() + result.width(),
                             result.y() + result.height()};
        const HMONITOR monitor = MonitorFromRect(&rectangle, MONITOR_DEFAULTTONEAREST);
        MONITORINFO info{};
        info.cbSize = sizeof(info);
        if (!monitor || GetMonitorInfoW(monitor, &info) == FALSE)
            return {};
        QScreen* selected = nullptr;
        for (QScreen* screen : QGuiApplication::screens()) {
            const auto* native = screen->nativeInterface<QNativeInterface::QWindowsScreen>();
            if (native && native->handle() == monitor) {
                selected = screen;
                break;
            }
        }
        return {selected,
                QRect(info.rcWork.left, info.rcWork.top, info.rcWork.right - info.rcWork.left,
                      info.rcWork.bottom - info.rcWork.top)};
    }
#endif
    QScreen* selected = nullptr;
    qint64 largestArea = -1;
    qreal shortestDistance = std::numeric_limits<qreal>::max();
    const QPointF center = QRectF(result).center();
    for (QScreen* screen : QGuiApplication::screens()) {
        const QRect intersection = result.intersected(screen->geometry());
        const qint64 area = qint64(intersection.width()) * intersection.height();
        const qreal distance = distanceToRect(center, screen->geometry());
        if (area > largestArea || (area == largestArea && distance < shortestDistance)) {
            selected = screen;
            largestArea = area;
            shortestDistance = distance;
        }
    }
    if (!selected)
        return {};
#ifdef Q_OS_MACOS
    return {selected,
            snow_shot::presentation::pinnedDisplayGeometry(*selected).usableBounds.toRect()};
#else
    return {selected, selected->availableGeometry()};
#endif
}

#if defined(Q_OS_WIN) || defined(_WIN32)
int coveringLogicalExtent(int pixels, qreal dpr) {
    int extent = std::max(1, qRound(pixels / dpr));
    while (qRound(extent * dpr) < pixels)
        ++extent;
    return extent;
}

HWND nativeWindow(WId id) {
    return reinterpret_cast<HWND>(id); // NOLINT(performance-no-int-to-ptr)
}

bool applyNativeGeometry(QWidget* window, const QRect& target) {
    const HWND hwnd = nativeWindow(window->internalWinId());
    if (!hwnd)
        return false;
    const UINT flags = SWP_NOZORDER | SWP_NOACTIVATE;
    if (SetWindowPos(hwnd, nullptr, target.x(), target.y(), target.width(), target.height(),
                     flags) == FALSE)
        return false;
    if (ScreenshotOriginalImagePreviewWindow::nativeClientRect(window) == target)
        return true;
    // QWidget can round the extent onto its logical grid in WM_WINDOWPOSCHANGING. The
    // recognition viewport owns this physical extent, including odd fractional-DPI sizes.
    return SetWindowPos(hwnd, nullptr, target.x(), target.y(), target.width(), target.height(),
                        flags | SWP_NOSENDCHANGING) != FALSE &&
           ScreenshotOriginalImagePreviewWindow::nativeClientRect(window) == target;
}
#endif
} // namespace

struct ScreenshotOriginalImagePreviewWindow::NativeSurface final {
#if defined(Q_OS_WIN) || defined(_WIN32)
    explicit NativeSurface(const QSize& extent) : size(extent) {
        dc = CreateCompatibleDC(nullptr);
        if (!dc)
            return;
        BITMAPINFO info{};
        info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
        info.bmiHeader.biWidth = size.width();
        info.bmiHeader.biHeight = -size.height();
        info.bmiHeader.biPlanes = 1;
        info.bmiHeader.biBitCount = 32;
        info.bmiHeader.biCompression = BI_RGB;
        bitmap = CreateDIBSection(dc, &info, DIB_RGB_COLORS, &pixels, nullptr, 0);
        if (bitmap)
            previousBitmap = SelectObject(dc, bitmap);
    }
    ~NativeSurface() {
        if (dc && previousBitmap && previousBitmap != HGDI_ERROR)
            SelectObject(dc, previousBitmap);
        if (bitmap)
            DeleteObject(bitmap);
        if (dc)
            DeleteDC(dc);
    }
    [[nodiscard]] bool isValid() const {
        return dc && bitmap && pixels && previousBitmap && previousBitmap != HGDI_ERROR;
    }
    [[nodiscard]] static QImage imageFor(const std::shared_ptr<NativeSurface>& surface,
                                         qreal dpr = 1) {
        if (!surface || !surface->isValid())
            return {};
        QImage image(
            static_cast<uchar*>(surface->pixels), surface->size.width(), surface->size.height(),
            qsizetype(surface->size.width()) * 4, QImage::Format_ARGB32_Premultiplied,
            [](void* holder) { delete static_cast<std::shared_ptr<NativeSurface>*>(holder); },
            new std::shared_ptr<NativeSurface>(surface));
        image.setDevicePixelRatio(dpr);
        return image;
    }
    [[nodiscard]] bool publish(WId windowId, const QRect& target) const {
        if (!isValid() || target.size() != size)
            return false;
        const HWND hwnd = nativeWindow(windowId);
        if (!hwnd)
            return false;
        const LONG_PTR style = GetWindowLongPtrW(hwnd, GWL_EXSTYLE);
        if (!(style & WS_EX_LAYERED))
            SetWindowLongPtrW(hwnd, GWL_EXSTYLE, style | WS_EX_LAYERED);
        const POINT destination{target.x(), target.y()};
        const POINT source{};
        const SIZE extent{size.width(), size.height()};
        const BLENDFUNCTION blend{AC_SRC_OVER, 0, 255, AC_SRC_ALPHA};
        const UPDATELAYEREDWINDOWINFO update{sizeof(UPDATELAYEREDWINDOWINFO),
                                             nullptr,
                                             &destination,
                                             &extent,
                                             dc,
                                             &source,
                                             0,
                                             &blend,
                                             ULW_ALPHA,
                                             nullptr};
        return UpdateLayeredWindowIndirect(hwnd, &update) != FALSE;
    }

    HDC dc = nullptr;
    HBITMAP bitmap = nullptr;
    HGDIOBJ previousBitmap = nullptr;
    void* pixels = nullptr;
    QSize size;
#endif
};

ScreenshotOriginalImagePreviewWindow::ScreenshotOriginalImagePreviewWindow(QWidget* parent)
    : QWidget(parent, Qt::Tool | Qt::FramelessWindowHint | Qt::WindowStaysOnTopHint |
                          Qt::WindowDoesNotAcceptFocus | Qt::WindowTransparentForInput) {
    setObjectName(QStringLiteral("screenshotOriginalImagePreviewWindow"));
    setWindowFlag(Qt::NoDropShadowWindowHint);
    setAttribute(Qt::WA_ShowWithoutActivating);
    setAttribute(Qt::WA_TransparentForMouseEvents);
    setAttribute(Qt::WA_TranslucentBackground);
    setAutoFillBackground(false);
    setFocusPolicy(Qt::NoFocus);
    const auto watch = [this](QScreen* screen) {
        connect(screen, &QScreen::geometryChanged, this, [this] { scheduleRefresh(); });
        connect(screen, &QScreen::availableGeometryChanged, this, [this] { scheduleRefresh(); });
        connect(screen, &QScreen::logicalDotsPerInchChanged, this, [this] { scheduleRefresh(); });
    };
    for (QScreen* screen : QGuiApplication::screens())
        watch(screen);
    connect(qGuiApp, &QGuiApplication::screenAdded, this, [this, watch](QScreen* screen) {
        watch(screen);
        scheduleRefresh();
    });
    connect(qGuiApp, &QGuiApplication::screenRemoved, this, [this] { scheduleRefresh(); });
}

ScreenshotOriginalImagePreviewWindow::~ScreenshotOriginalImagePreviewWindow() = default;

bool ScreenshotOriginalImagePreviewWindow::usesPhysicalGeometry() {
#if defined(Q_OS_WIN) || defined(_WIN32)
    return QGuiApplication::platformName() == QStringLiteral("windows");
#else
    return false;
#endif
}

QRect ScreenshotOriginalImagePreviewWindow::nativeClientRect(QWidget* widget) {
    if (!widget)
        return {};
#if defined(Q_OS_WIN) || defined(_WIN32)
    if (usesPhysicalGeometry()) {
        QWidget* topLevel = widget->window();
        const HWND hwnd = nativeWindow(topLevel->internalWinId());
        RECT client{};
        POINT origin{};
        if (!hwnd || GetClientRect(hwnd, &client) == FALSE ||
            ClientToScreen(hwnd, &origin) == FALSE)
            return {};
        const QRect native(origin.x, origin.y, client.right - client.left,
                           client.bottom - client.top);
        if (widget == topLevel)
            return native;
        if (topLevel->width() <= 0 || topLevel->height() <= 0)
            return {};
        const QPoint logicalOrigin = widget->mapTo(topLevel, QPoint());
        const auto x = [&](int value) {
            return qRound(qreal(value) * native.width() / topLevel->width());
        };
        const auto y = [&](int value) {
            return qRound(qreal(value) * native.height() / topLevel->height());
        };
        return QRect(native.x() + x(logicalOrigin.x()), native.y() + y(logicalOrigin.y()),
                     x(logicalOrigin.x() + widget->width()) - x(logicalOrigin.x()),
                     y(logicalOrigin.y() + widget->height()) - y(logicalOrigin.y()))
            .intersected(native);
    }
#endif
    return QRect(widget->mapToGlobal(QPoint()), widget->size());
}

QRect ScreenshotOriginalImagePreviewWindow::placement(const QRect& result, const QRect& workArea,
                                                      int gap) {
    if (!result.isValid() || result.isEmpty())
        return {};
    gap = std::max(0, gap);
    const QRect left(result.x() - result.width() - gap, result.y(), result.width(),
                     result.height());
    const QRect above(result.x(), result.y() - result.height() - gap, result.width(),
                      result.height());
    const QRect right(result.x() + result.width() + gap, result.y(), result.width(),
                      result.height());
    for (const QRect& candidate : {left, above, right}) {
        if (workArea.contains(candidate))
            return candidate;
    }
    return left;
}

bool ScreenshotOriginalImagePreviewWindow::present(
    const ScreenshotOriginalImagePreviewState& state) {
    const auto notifyHidden = qScopeGuard([this] {
        if (!isVisible())
            emit hidden();
    });
    const QScopedValueRollback<bool> presenting(m_presenting, true);
    if (state.image.isNull() || !finiteRect(state.imageRectInViewport) ||
        !state.resultRect.isValid() || state.resultRect.isEmpty()) {
        clear();
        return false;
    }
    const PlacementContext context = placementContext(state.resultRect);
    if (!context.screen || !context.workArea.isValid()) {
        clear();
        return false;
    }
    m_sourceImage = state.image;
    m_imageRectInViewport = state.imageRectInViewport;
    m_resultRect = state.resultRect;
    if (m_transientOwner != state.transientOwner || m_aboveSibling != state.aboveSibling ||
        m_pinned != state.pinned || m_staysOnTop != state.staysOnTop)
        m_siblingStackedAbove = false;
    m_transientOwner = state.transientOwner;
    m_aboveSibling = state.aboveSibling;
    m_pinned = state.pinned;
    m_staysOnTop = state.staysOnTop;
    const int gap = usesPhysicalGeometry()
                        ? qRound(kPreviewGap * context.screen->devicePixelRatio())
                        : kPreviewGap;
    if (!applyGeometry(placement(m_resultRect, context.workArea, gap), context.screen)) {
        hide();
        return false;
    }
    refreshStacking();
    if (!refreshRaster()) {
        hide();
        return false;
    }
    if (!isVisible()) {
        show();
        // Qt can restore the flag-derived native level while mapping a hidden tool window.
        // Reconcile the pin's current stacking policy after that first mapping too.
        refreshStacking();
    }
    reconcileNativeGeometry();
    return isVisible();
}

void ScreenshotOriginalImagePreviewWindow::clear() {
    hide();
    m_sourceImage = {};
    m_viewportImage = {};
    m_nativeSurface.reset();
    m_imageRectInViewport = {};
    m_resultRect = {};
    m_targetGeometry = {};
    m_transientOwner.clear();
    m_aboveSibling.clear();
    m_siblingStackedAbove = false;
    m_cachedSourceKey = 0;
    m_cachedImageRect = {};
    m_cachedPixelSize = {};
    m_cachedOutputDpr = 0;
    if (windowHandle())
        windowHandle()->setTransientParent(nullptr);
    if (m_dpiController)
        m_dpiController->resetBaseline();
}

void ScreenshotOriginalImagePreviewWindow::refreshStacking() {
    if (!windowHandle())
        return;
    QWindow* owner = m_transientOwner ? m_transientOwner->window()->windowHandle() : nullptr;
    if (windowHandle()->transientParent() != owner)
        windowHandle()->setTransientParent(owner);
#ifdef Q_OS_MACOS
    if (QGuiApplication::platformName() == QStringLiteral("cocoa")) {
        if (m_pinned) {
            if (auto* platform = snow_shot::presentation::configurePinnedAuxiliary(this)) {
                static_cast<void>(platform->setInputTransparent(true));
                static_cast<void>(platform->setStaysOnTop(m_staysOnTop));
            }
            if (isVisible() && m_aboveSibling && m_aboveSibling != this &&
                m_aboveSibling->isWindow() && m_aboveSibling->isVisible() &&
                nativeClientRect(m_aboveSibling).intersects(m_targetGeometry)) {
                if (!m_siblingStackedAbove)
                    m_siblingStackedAbove =
                        snow_shot::platform::stackScreenshotWindowBelow(this, m_aboveSibling);
            } else {
                m_siblingStackedAbove = false;
            }
        } else {
            snow_shot::platform::configureScreenshotRecognitionWindow(this);
        }
        return;
    }
#endif
#if defined(Q_OS_WIN) || defined(_WIN32)
    if (usesPhysicalGeometry()) {
        const HWND hwnd = nativeWindow(internalWinId());
        const bool currentlyOnTop = (GetWindowLongPtrW(hwnd, GWL_EXSTYLE) & WS_EX_TOPMOST) != 0;
        if (currentlyOnTop != m_staysOnTop)
            static_cast<void>(SetWindowPos(hwnd, m_staysOnTop ? HWND_TOPMOST : HWND_NOTOPMOST, 0, 0,
                                           0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE));
        if (isVisible() && m_aboveSibling && m_aboveSibling != this && m_aboveSibling->isWindow() &&
            m_aboveSibling->isVisible() && m_aboveSibling->internalWinId() &&
            nativeClientRect(m_aboveSibling).intersects(m_targetGeometry)) {
            const HWND sibling = nativeWindow(m_aboveSibling->internalWinId());
            const bool siblingOnTop =
                (GetWindowLongPtrW(sibling, GWL_EXSTYLE) & WS_EX_TOPMOST) != 0;
            bool alreadyAbove = false;
            for (HWND before = GetWindow(hwnd, GW_HWNDPREV); before;
                 before = GetWindow(before, GW_HWNDPREV)) {
                if (before == sibling) {
                    alreadyAbove = true;
                    break;
                }
            }
            if (siblingOnTop == m_staysOnTop && !alreadyAbove)
                static_cast<void>(
                    SetWindowPos(hwnd, sibling, 0, 0, 0, 0,
                                 SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_NOOWNERZORDER));
        }
        return;
    }
#endif
    if (windowFlags().testFlag(Qt::WindowStaysOnTopHint) != m_staysOnTop)
        setWindowFlag(Qt::WindowStaysOnTopHint, m_staysOnTop);
}

bool ScreenshotOriginalImagePreviewWindow::applyGeometry(const QRect& target, QScreen* screen) {
    const QScopedValueRollback<bool> applying(m_applyingGeometry, true);
    m_targetGeometry = target;
    if (internalWinId() == 0) {
        setScreen(screen);
        // winId() marks this tool WA_NativeWindow and can promote its owner's alien
        // recognition/canvas siblings. Create only the preview's own native surface.
        create();
    }
    if (!usesPhysicalGeometry()) {
        if (windowHandle()->screen() != screen)
            windowHandle()->setScreen(screen);
        if (geometry() != target)
            setGeometry(target);
        return true;
    }
#if defined(Q_OS_WIN) || defined(_WIN32)
    if (!m_dpiController) {
        m_dpiController = new adqt::widgets::AdDpiStableWindowController(this, this);
        m_dpiController->resetBaseline();
        connect(m_dpiController, &adqt::widgets::AdDpiStableWindowController::scaleCommitReady,
                this, [this] {
                    if (isVisible() && !m_sourceImage.isNull() && m_targetGeometry.isValid()) {
                        static_cast<void>(applyGeometry(m_targetGeometry, this->screen()));
                        static_cast<void>(refreshRaster());
                    }
                });
        connect(m_dpiController, &adqt::widgets::AdDpiStableWindowController::scaleCommitCompleted,
                this, [this] { scheduleRefresh(); });
    }
    windowHandle()->installEventFilter(this);
    const qreal dpr = windowHandle()->devicePixelRatio();
    const QSize logicalSize(coveringLogicalExtent(target.width(), dpr),
                            coveringLogicalExtent(target.height(), dpr));
    if (size() != logicalSize)
        resize(logicalSize);
    if (m_dpiController->stablePhysicalClientSize() != target.size()) {
        m_dpiController->resetBaseline();
        if (!m_dpiController->captureBaseline(dpr, target.size()))
            return false;
    }
    return nativeClientRect(this) == target || applyNativeGeometry(this, target);
#else
    return false;
#endif
}

bool ScreenshotOriginalImagePreviewWindow::refreshRaster() {
    if (m_sourceImage.isNull() || !m_resultRect.isValid())
        return false;
    const qreal outputDpr = devicePixelRatioF();
    if (!std::isfinite(outputDpr) || outputDpr <= 0)
        return false;
    const qreal geometryScale = usesPhysicalGeometry() ? 1.0 : outputDpr;
    const qreal pixelWidth = m_resultRect.width() * geometryScale;
    const qreal pixelHeight = m_resultRect.height() * geometryScale;
    if (!std::isfinite(pixelWidth) || !std::isfinite(pixelHeight) ||
        pixelWidth > std::numeric_limits<int>::max() ||
        pixelHeight > std::numeric_limits<int>::max())
        return false;
    const QSize pixelSize(std::max(1, qRound(pixelWidth)), std::max(1, qRound(pixelHeight)));
    const QRectF pixelImageRect(m_imageRectInViewport.x() * geometryScale,
                                m_imageRectInViewport.y() * geometryScale,
                                m_imageRectInViewport.width() * geometryScale,
                                m_imageRectInViewport.height() * geometryScale);
    if (!finiteRect(pixelImageRect))
        return false;
    if (m_cachedSourceKey == m_sourceImage.cacheKey() && m_cachedImageRect == pixelImageRect &&
        m_cachedPixelSize == pixelSize) {
        if (!qFuzzyCompare(m_cachedOutputDpr, outputDpr)) {
            // Moving a Windows preview to another DPI grid only changes its paint metadata;
            // its authoritative physical viewport and image mapping retain the same pixels.
#if defined(Q_OS_WIN) || defined(_WIN32)
            if (m_nativeSurface)
                m_viewportImage = NativeSurface::imageFor(m_nativeSurface, outputDpr);
            else
#endif
                m_viewportImage.setDevicePixelRatio(outputDpr);
            m_cachedOutputDpr = outputDpr;
            update();
        }
        return true;
    }
    QImage viewport;
#if defined(Q_OS_WIN) || defined(_WIN32)
    std::shared_ptr<NativeSurface> nativeSurface;
    if (usesPhysicalGeometry()) {
        nativeSurface = std::make_shared<NativeSurface>(pixelSize);
        if (!nativeSurface->isValid())
            return false;
        // The cached QImage paints directly into the DIB used for native publication. Its
        // cleanup holder also keeps snapshots alive after this window replaces or clears it.
        viewport = NativeSurface::imageFor(nativeSurface);
    } else
#endif
        viewport = snowCanvasAllocateImage(pixelSize, QImage::Format_ARGB32_Premultiplied);
    if (viewport.isNull())
        return false;
    viewport.fill(Qt::transparent);
    QPainter painter(&viewport);
    painter.setRenderHint(QPainter::SmoothPixmapTransform,
                          pixelImageRect.size() != QSizeF(m_sourceImage.size()));
    paintExposedScreenshotImage(painter, pixelImageRect, m_sourceImage,
                                QRectF(QPointF(), QSizeF(m_sourceImage.size())),
                                QRegion(viewport.rect()));
    painter.end();
    viewport.setDevicePixelRatio(outputDpr);
    m_viewportImage = std::move(viewport);
#if defined(Q_OS_WIN) || defined(_WIN32)
    m_nativeSurface = std::move(nativeSurface);
#endif
    m_cachedSourceKey = m_sourceImage.cacheKey();
    m_cachedImageRect = pixelImageRect;
    m_cachedPixelSize = pixelSize;
    m_cachedOutputDpr = outputDpr;
    ++m_rasterGeneration;
    update();
    return true;
}

void ScreenshotOriginalImagePreviewWindow::hideEvent(QHideEvent* event) {
    QWidget::hideEvent(event);
    if (!m_presenting)
        emit hidden();
}

void ScreenshotOriginalImagePreviewWindow::paintEvent(QPaintEvent* event) {
    Q_UNUSED(event);
    QPainter painter(this);
    painter.setCompositionMode(QPainter::CompositionMode_Source);
    painter.fillRect(rect(), Qt::transparent);
    if (!m_viewportImage.isNull())
        painter.drawImage(QPointF(), m_viewportImage);
}

void ScreenshotOriginalImagePreviewWindow::resizeEvent(QResizeEvent* event) {
    QWidget::resizeEvent(event);
#if defined(Q_OS_WIN) || defined(_WIN32)
    if (usesPhysicalGeometry() && m_targetGeometry.isValid() && !m_sourceImage.isNull() &&
        !m_adjustingPaintSurface) {
        // QWidgetWindow rounds native extents onto its logical grid before forwarding this
        // event. Keep the paint surface large enough for every physical row and column; the
        // native window itself remains pinned to the exact recognition viewport rectangle.
        const qreal dpr = devicePixelRatioF();
        const QSize coveringSize(coveringLogicalExtent(m_targetGeometry.width(), dpr),
                                 coveringLogicalExtent(m_targetGeometry.height(), dpr));
        if (size() != coveringSize) {
            const QScopedValueRollback<bool> adjusting(m_adjustingPaintSurface, true);
            resize(coveringSize);
        }
    }
#endif
}

bool ScreenshotOriginalImagePreviewWindow::event(QEvent* event) {
    const bool handled = QWidget::event(event);
    if (event && event->type() == QEvent::UpdateRequest)
        reconcileNativeGeometry();
    if (event && (event->type() == QEvent::Hide || event->type() == QEvent::WinIdChange))
        m_siblingStackedAbove = false;
    if (event &&
        (event->type() == QEvent::DevicePixelRatioChange ||
         event->type() == QEvent::ScreenChangeInternal || event->type() == QEvent::WinIdChange) &&
        !m_applyingGeometry)
        scheduleRefresh();
    return handled;
}

bool ScreenshotOriginalImagePreviewWindow::eventFilter(QObject* watched, QEvent* event) {
    if (usesPhysicalGeometry() && watched == windowHandle() && event &&
        (event->type() == QEvent::Expose || event->type() == QEvent::Resize) &&
        !m_forwardingWindowEvent) {
        // QWidgetWindow's expose and resize handlers can flush the backing store directly,
        // bypassing the QWidget update request. Complete that dispatch once, then restore the
        // native rectangle before the event returns, just as for a regular repaint below.
        const QPointer<ScreenshotOriginalImagePreviewWindow> alive(this);
        m_forwardingWindowEvent = true;
        QCoreApplication::sendEvent(watched, event);
        if (alive) {
            m_forwardingWindowEvent = false;
            reconcileNativeGeometry();
        }
        return true;
    }
    return QWidget::eventFilter(watched, event);
}

void ScreenshotOriginalImagePreviewWindow::reconcileNativeGeometry() {
#if defined(Q_OS_WIN) || defined(_WIN32)
    if (usesPhysicalGeometry() && isVisible() && !m_sourceImage.isNull() &&
        m_targetGeometry.isValid() && !m_applyingGeometry && m_nativeSurface &&
        nativeClientRect(this) != m_targetGeometry) {
        // Qt publishes translucent frames using an integer logical frame rectangle, which can
        // clip a final physical row or column at fractional DPI. Publish the single cached DIB
        // with the authoritative physical extent instead of growing a cropped layered surface.
        const QScopedValueRollback<bool> applying(m_applyingGeometry, true);
        if (!m_nativeSurface->publish(internalWinId(), m_targetGeometry))
            hide();
    }
#endif
}

void ScreenshotOriginalImagePreviewWindow::scheduleRefresh() {
    if (m_refreshPending || !isVisible() || m_sourceImage.isNull())
        return;
    m_refreshPending = true;
    QTimer::singleShot(0, this, [this] {
        m_refreshPending = false;
        // A drag or a parent hide may suppress the preview before this queued update runs.
        if (!isVisible() || m_sourceImage.isNull())
            return;
        static_cast<void>(
            present({m_sourceImage, m_imageRectInViewport, m_resultRect, m_transientOwner.data(),
                     m_pinned, m_staysOnTop, m_aboveSibling.data()}));
    });
}

bool ScreenshotOriginalImagePreviewWindow::nativeEvent(const QByteArray& eventType, void* message,
                                                       qintptr* result) {
#if defined(Q_OS_WIN) || defined(_WIN32)
    if (usesPhysicalGeometry() && message && m_targetGeometry.isValid() &&
        (eventType == QByteArrayLiteral("windows_generic_MSG") ||
         eventType == QByteArrayLiteral("windows_dispatcher_MSG"))) {
        const auto* native = static_cast<MSG*>(message);
        if (native->hwnd == nativeWindow(internalWinId())) {
            if (adqt::widgets::AdDpiStableWindowController::enforceStablePhysicalSizeForMessage(
                    message, internalWinId(), m_targetGeometry.size(), true, result))
                return true;
            if (native->message == WM_WINDOWPOSCHANGING && native->lParam) {
                auto* position = reinterpret_cast<WINDOWPOS*>(native->lParam);
                if (!(position->flags & SWP_NOMOVE)) {
                    position->x = m_targetGeometry.x();
                    position->y = m_targetGeometry.y();
                }
            }
            if (native->message == WM_WINDOWPOSCHANGED) {
                // Keep the passive preview fixed while native ownership or DPI updates settle.
                reconcileNativeGeometry();
            }
        }
    }
#endif
    return QWidget::nativeEvent(eventType, message, result);
}
