// SPDX-License-Identifier: GPL-3.0-or-later
#include "livewindowpresentation.h"
#include "livewindowinputrouting.h"
#include "../../presentation/pinned/livewindownative_p.h"

#include <QScopedValueRollback>
#include <QWidget>
#include <commctrl.h>
#include <dwmapi.h>
#include <uxtheme.h>
#include <windowsx.h>
#include <mutex>
#include <optional>

namespace snow_shot::presentation {
namespace {
QRect clientGeometry(HWND window) {
    RECT rect{};
    POINT origin{};
    if (!GetClientRect(window, &rect) || !ClientToScreen(window, &origin))
        return {};
    return {origin.x, origin.y, rect.right - rect.left, rect.bottom - rect.top};
}

bool sameProperties(const DWM_THUMBNAIL_PROPERTIES& left, const DWM_THUMBNAIL_PROPERTIES& right) {
    return left.dwFlags == right.dwFlags && left.opacity == right.opacity &&
           left.fVisible == right.fVisible &&
           left.fSourceClientAreaOnly == right.fSourceClientAreaOnly &&
           EqualRect(&left.rcSource, &right.rcSource) &&
           EqualRect(&left.rcDestination, &right.rcDestination);
}
} // namespace

struct LiveWindowPresentationSurface::Impl {
    explicit Impl(HWND sourceWindow) : source(sourceWindow) {}
    ~Impl() {
        stop();
    }

    bool attach(HWND window) {
        if (shell == window && host && thumbnail)
            return true;
        stop();
        static std::once_flag once;
        static ATOM windowClass = 0;
        std::call_once(once, [] {
            WNDCLASSEXW wc{};
            wc.cbSize = sizeof(wc);
            wc.style = CS_DBLCLKS;
            wc.lpfnWndProc = windowProc;
            wc.hInstance = GetModuleHandleW(nullptr);
            wc.lpszClassName = L"SnowShotLiveWindowHost";
            wc.hCursor = LoadCursorW(nullptr, MAKEINTRESOURCEW(32512));
            windowClass = RegisterClassExW(&wc);
            BufferedPaintInit();
        });
        if (!windowClass)
            return false;
        shell = window;
        originalOwner = GetWindow(shell, GW_OWNER);
        host = CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE | WS_EX_LAYERED,
                               MAKEINTRESOURCEW(windowClass), L"", WS_POPUP, 0, 0, 1, 1,
                               originalOwner, nullptr, GetModuleHandleW(nullptr), this);
        if (!host)
            return false;
        const MARGINS glass{-1, -1, -1, -1};
        if (FAILED(DwmExtendFrameIntoClientArea(host, &glass)) ||
            FAILED(DwmRegisterThumbnail(host, source, &thumbnail)))
            return false;
        SetLastError(ERROR_SUCCESS);
        if (!SetWindowLongPtrW(shell, GWLP_HWNDPARENT, reinterpret_cast<LONG_PTR>(host)) &&
            GetLastError() != ERROR_SUCCESS)
            return false;
        return SetWindowSubclass(shell, shellProc, subclassId(),
                                 reinterpret_cast<DWORD_PTR>(this)) != FALSE;
    }

    UINT_PTR subclassId() const {
        return reinterpret_cast<UINT_PTR>(this);
    }

    void stop() {
        // Detach before destroying the owner HWND: Windows destroys owned windows with it.
        if (shell) {
            RemoveWindowSubclass(shell, shellProc, subclassId());
            if (IsWindow(shell) && GetWindow(shell, GW_OWNER) == host)
                SetWindowLongPtrW(
                    shell, GWLP_HWNDPARENT,
                    reinterpret_cast<LONG_PTR>(IsWindow(originalOwner) ? originalOwner : nullptr));
        }
        shell = nullptr;
        originalOwner = nullptr;
        if (thumbnail) {
            DwmUnregisterThumbnail(thumbnail);
            thumbnail = nullptr;
        }
        if (host) {
            DestroyWindow(host);
            host = nullptr;
        }
        properties.reset();
        alpha.reset();
        healthy = true;
    }

    bool present(const LiveWindowPresentation& presentation, const QRectF& nextCrop,
                 const QRect& nextSourceBounds) {
        if (!presentation.owner || !healthy)
            return false;
        if (!attach(reinterpret_cast<HWND>(presentation.owner->winId())))
            return false;
        margins = presentation.nativeContentMargins;
        crop = nextCrop;
        sourceBounds = nextSourceBounds;
        if (!GetWindowRect(source, &sourceFrame))
            return false;
        return applyAppearance(presentation) && synchronize();
    }

    bool applyAppearance(const LiveWindowPresentation& presentation) {
        // Promoting an owner also sends placement messages to its owned shell. Do not
        // reposition the owner recursively while Windows is committing that Z-order change.
        const QScopedValueRollback<bool> guard(synchronizing, true);
        const LONG_PTR oldStyle = GetWindowLongPtrW(host, GWL_EXSTYLE);
        const LONG_PTR newStyle = presentation.clickThrough ? oldStyle | WS_EX_TRANSPARENT
                                                            : oldStyle & ~WS_EX_TRANSPARENT;
        if (oldStyle != newStyle) {
            SetLastError(ERROR_SUCCESS);
            if (!SetWindowLongPtrW(host, GWL_EXSTYLE, newStyle) && GetLastError() != ERROR_SUCCESS)
                return false;
        }
        const auto nextAlpha = static_cast<BYTE>(
            qRound(qBound(qreal(0), presentation.effectiveOpacity, qreal(1)) * 255));
        if (alpha != nextAlpha) {
            if (!SetLayeredWindowAttributes(host, 0, nextAlpha, LWA_ALPHA))
                return false;
            alpha = nextAlpha;
        }
        // Changing bands is a policy transition. A refresh or move must preserve Z order,
        // including unowned Qt popup windows above the shell.
        // NOOWNERZORDER is also required here: promoting an owner of an already-topmost
        // shell can otherwise succeed without promoting the owner, causing endless retries.
        if (((oldStyle & WS_EX_TOPMOST) != 0) != presentation.alwaysOnTop &&
            !SetWindowPos(host, presentation.alwaysOnTop ? HWND_TOPMOST : HWND_NOTOPMOST, 0, 0, 0,
                          0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_NOOWNERZORDER))
            return false;
        if (background != presentation.backgroundColor) {
            background = presentation.backgroundColor;
            InvalidateRect(host, nullptr, FALSE);
        }
        return true;
    }

    bool updateThumbnail(const QSize& size, bool show) {
        const auto mapping = live_window_detail::cropMapping(
            crop, QRectF(QPointF(), sourceBounds.size()), QRectF(QPointF(), size));
        DWM_THUMBNAIL_PROPERTIES next{};
        next.dwFlags = DWM_TNP_OPACITY | DWM_TNP_VISIBLE | DWM_TNP_SOURCECLIENTAREAONLY;
        next.opacity = 255;
        next.fVisible = show && !mapping.source.isEmpty();
        next.fSourceClientAreaOnly = FALSE;
        if (!mapping.source.isEmpty()) {
            next.dwFlags |= DWM_TNP_RECTDESTINATION | DWM_TNP_RECTSOURCE;
            // rcSource is relative to GetWindowRect, whereas WGC crops are relative to
            // the visible frame. DwmQueryThumbnailSourceSize can exclude resize margins;
            // its dimensions do not change rcSource's origin or physical pixel scale.
            const QRectF pixels = live_window_detail::windowRelativeCrop(
                mapping.source, sourceBounds.topLeft(), QPoint(sourceFrame.left, sourceFrame.top));
            next.rcSource = {qRound(pixels.left()), qRound(pixels.top()), qRound(pixels.right()),
                             qRound(pixels.bottom())};
            next.rcDestination = {
                qRound(mapping.destination.left()), qRound(mapping.destination.top()),
                qRound(mapping.destination.right()), qRound(mapping.destination.bottom())};
        }
        if (properties && sameProperties(*properties, next))
            return true;
        if (FAILED(DwmUpdateThumbnailProperties(thumbnail, &next)))
            return false;
        properties = next;
        return true;
    }

    bool synchronize() {
        if (synchronizing || !host || !shell)
            return healthy;
        const QScopedValueRollback<bool> guard(synchronizing, true);
        const QRect geometry = clientGeometry(shell).marginsRemoved(margins);
        const bool show = IsWindowVisible(shell) && !IsIconic(shell) && !geometry.isEmpty();
        if (!updateThumbnail(geometry.size(), show))
            return healthy = false;
        // Promoting the host to the topmost band can put it above its already-topmost
        // owned shell. Ownership alone does not restore the decoration/input layer.
        // Insert only the host below the shell, preserving popups above the pair.
        UINT flags = SWP_NOACTIVATE | SWP_NOOWNERZORDER;
        if (GetWindow(shell, GW_HWNDNEXT) == host)
            flags |= SWP_NOZORDER;
        const QRect previous = clientGeometry(host);
        if (previous.topLeft() == geometry.topLeft() || !show)
            flags |= SWP_NOMOVE;
        if (previous.size() == geometry.size() || !show)
            flags |= SWP_NOSIZE;
        if ((IsWindowVisible(host) != FALSE) != show)
            flags |= show ? SWP_SHOWWINDOW : SWP_HIDEWINDOW;
        if ((flags & (SWP_NOMOVE | SWP_NOSIZE)) != (SWP_NOMOVE | SWP_NOSIZE) ||
            (flags & (SWP_SHOWWINDOW | SWP_HIDEWINDOW)) || !(flags & SWP_NOZORDER)) {
            if (!SetWindowPos(host, shell, geometry.x(), geometry.y(), geometry.width(),
                              geometry.height(), flags))
                return healthy = false;
        }
        return true;
    }

    bool suppressPointerLeave(UINT message) const {
        POINT point{};
        if (!GetCursorPos(&point))
            return false;
        const HWND hit = WindowFromPoint(point);
        return (hit == shell || hit == host) && live_window_detail::suppressInternalPointerLeave(
                                                    shell, message, MAKELPARAM(point.x, point.y));
    }

    static LRESULT CALLBACK shellProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam,
                                      UINT_PTR, DWORD_PTR data) {
        auto* self = reinterpret_cast<Impl*>(data);
        // Qt tracks the shell after forwarded content input. USER32 then reports a
        // shell leave because the transparent pixel belongs to the host. Preserve
        // client tracking inside the pair, but let Qt clear child hover when the
        // pointer enters non-client content. The shell tracks that presence itself.
        if ((message == WM_MOUSELEAVE || message == WM_NCMOUSELEAVE) &&
            self->suppressPointerLeave(message))
            return 0;
        if (message == WM_NCDESTROY) {
            self->stop();
        } else if (message == WM_WINDOWPOSCHANGED) {
            // Read committed native client pixels before Qt delivers move/resize events.
            // This also runs inside the Windows modal move loop, without a Qt timer turn.
            self->synchronize();
        }
        return DefSubclassProc(window, message, wParam, lParam);
    }

    static LRESULT CALLBACK windowProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam) {
        auto* self = reinterpret_cast<Impl*>(GetWindowLongPtrW(window, GWLP_USERDATA));
        if (message == WM_NCCREATE) {
            self = static_cast<Impl*>(reinterpret_cast<CREATESTRUCTW*>(lParam)->lpCreateParams);
            SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
        }
        if (self && message == WM_PAINT) {
            PAINTSTRUCT paint{};
            const HDC dc = BeginPaint(window, &paint);
            RECT bounds{};
            GetClientRect(window, &bounds);
            HDC buffered = nullptr;
            BP_PAINTPARAMS parameters{};
            parameters.cbSize = sizeof(parameters);
            const HPAINTBUFFER buffer =
                BeginBufferedPaint(dc, &bounds, BPBF_TOPDOWNDIB, &parameters, &buffered);
            const HBRUSH brush = CreateSolidBrush(
                RGB(self->background.red(), self->background.green(), self->background.blue()));
            FillRect(buffered ? buffered : dc, &bounds, brush);
            DeleteObject(brush);
            if (buffer) {
                BufferedPaintSetAlpha(buffer, &bounds, 255);
                EndBufferedPaint(buffer, TRUE);
            }
            EndPaint(window, &paint);
            return 0;
        }
        if (self && self->shell && (message == WM_MOUSELEAVE || message == WM_NCMOUSELEAVE)) {
            // Apply the same client/non-client transition policy to both HWNDs.
            SendMessageW(self->shell, message, 0, 0);
            return 0;
        }
        if (self && (message == WM_MOUSEMOVE || message == WM_NCMOUSEMOVE)) {
            // The content HWND owns leave tracking while the pointer is over the
            // shell's transparent interior, including when the pointer stops moving.
            const DWORD flags = TME_LEAVE | (message == WM_NCMOUSEMOVE ? TME_NONCLIENT : 0);
            TRACKMOUSEEVENT tracking{sizeof(tracking), flags, window, 0};
            TrackMouseEvent(&tracking);
        }
        if (self) {
            if (const auto result = live_window_detail::forwardPointerMessage(
                    window, self->shell, message, wParam, lParam))
                return *result;
        }
        if (message == WM_ERASEBKGND)
            return 1;
        return DefWindowProcW(window, message, wParam, lParam);
    }

    HWND source = nullptr;
    HWND shell = nullptr;
    HWND host = nullptr;
    HWND originalOwner = nullptr;
    HTHUMBNAIL thumbnail = nullptr;
    QMargins margins;
    QRectF crop;
    QRect sourceBounds;
    RECT sourceFrame{};
    QColor background = Qt::white;
    std::optional<DWM_THUMBNAIL_PROPERTIES> properties;
    std::optional<BYTE> alpha;
    bool synchronizing = false;
    bool healthy = true;
};

LiveWindowPresentationSurface::LiveWindowPresentationSurface(HWND source)
    : m_impl(std::make_unique<Impl>(source)) {}
LiveWindowPresentationSurface::~LiveWindowPresentationSurface() = default;
bool LiveWindowPresentationSurface::present(const LiveWindowPresentation& presentation,
                                            const QRectF& crop, const QRect& sourceBounds) {
    if (m_impl->present(presentation, crop, sourceBounds))
        return true;
    stop();
    return false;
}
bool LiveWindowPresentationSurface::presenting() const {
    // Visibility is native window state, not a switch back to CPU frame presentation.
    return m_impl->thumbnail && m_impl->shell && m_impl->healthy;
}
void LiveWindowPresentationSurface::stop() {
    m_impl->stop();
}
} // namespace snow_shot::presentation
