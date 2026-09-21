#include "../../presentation/pinned/pinnedwindowplatform.h"
#include "snow_shot/presentation/screenshotpinnedwindow.h"
#include "pinnedwindownative.h"
#include "../../presentation/pinned/screenshotpinnednativegeometrycontroller.h"
#include "../../presentation/pinned/screenshotpinnedgeometrymapping.h"
#include "../../presentation/pinned/screenshotpinnedhidetotopcontroller.h"
#include "snow_shot/presentation/screenshotpinnededitcontroller.h"
#include <QWindow>
#include <QCursor>
#include <qt_windows.h>
#include <windowsx.h>
#include <algorithm>

namespace native = screenshot_pinned_window_native;
namespace resize_geometry = screenshot_pinned_resize_geometry;
namespace {
constexpr int kResizeHitWidth = 6;
constexpr int kMinimumScalePercent = 10;
constexpr int kMaximumScalePercent = 500;
template <typename T> T* pointerFromLParam(LPARAM value) {
    return reinterpret_cast<T*>(value);
}
QSize physicalSizeAtScale(const QSize& size, int percent) {
    return resize_geometry::scaledSize(size, percent / 100.);
}
#if defined(Q_OS_WIN) || defined(_WIN32)
Qt::Edges resizeEdgesForNativeHitTest(LRESULT hitTest) {
    switch (hitTest) {
    case HTLEFT:
        return Qt::LeftEdge;
    case HTRIGHT:
        return Qt::RightEdge;
    case HTTOP:
        return Qt::TopEdge;
    case HTBOTTOM:
        return Qt::BottomEdge;
    case HTTOPLEFT:
        return Qt::TopEdge | Qt::LeftEdge;
    case HTTOPRIGHT:
        return Qt::TopEdge | Qt::RightEdge;
    case HTBOTTOMLEFT:
        return Qt::BottomEdge | Qt::LeftEdge;
    case HTBOTTOMRIGHT:
        return Qt::BottomEdge | Qt::RightEdge;
    default:
        return {};
    }
}

bool dragHandleForSizingEdge(WPARAM sizingEdge, resize_geometry::DragHandle* handle) {
    if (handle == nullptr) {
        return false;
    }
    switch (sizingEdge) {
    case WMSZ_TOPLEFT:
        *handle = resize_geometry::DragHandle::TopLeft;
        return true;
    case WMSZ_TOPRIGHT:
        *handle = resize_geometry::DragHandle::TopRight;
        return true;
    case WMSZ_BOTTOMRIGHT:
        *handle = resize_geometry::DragHandle::BottomRight;
        return true;
    case WMSZ_BOTTOMLEFT:
        *handle = resize_geometry::DragHandle::BottomLeft;
        return true;
    case WMSZ_TOP:
        *handle = resize_geometry::DragHandle::Top;
        return true;
    case WMSZ_RIGHT:
        *handle = resize_geometry::DragHandle::Right;
        return true;
    case WMSZ_BOTTOM:
        *handle = resize_geometry::DragHandle::Bottom;
        return true;
    case WMSZ_LEFT:
        *handle = resize_geometry::DragHandle::Left;
        return true;
    default:
        return false;
    }
}

bool dragHandleForHitTest(LRESULT hitTest, resize_geometry::DragHandle* handle) {
    if (handle == nullptr) {
        return false;
    }
    switch (hitTest) {
    case HTTOPLEFT:
        *handle = resize_geometry::DragHandle::TopLeft;
        return true;
    case HTTOP:
        *handle = resize_geometry::DragHandle::Top;
        return true;
    case HTTOPRIGHT:
        *handle = resize_geometry::DragHandle::TopRight;
        return true;
    case HTRIGHT:
        *handle = resize_geometry::DragHandle::Right;
        return true;
    case HTBOTTOMRIGHT:
        *handle = resize_geometry::DragHandle::BottomRight;
        return true;
    case HTBOTTOM:
        *handle = resize_geometry::DragHandle::Bottom;
        return true;
    case HTBOTTOMLEFT:
        *handle = resize_geometry::DragHandle::BottomLeft;
        return true;
    case HTLEFT:
        *handle = resize_geometry::DragHandle::Left;
        return true;
    default:
        return false;
    }
}

QRect qRectFromNativeRect(const RECT& rect) {
    return QRect(rect.left, rect.top, std::max(1, static_cast<int>(rect.right - rect.left)),
                 std::max(1, static_cast<int>(rect.bottom - rect.top)));
}

void writeNativeRect(const QRect& source, RECT* target) {
    if (target == nullptr) {
        return;
    }
    target->left = source.left();
    target->top = source.top();
    target->right = source.left() + source.width();
    target->bottom = source.top() + source.height();
}
#endif

} // namespace
bool PinnedWindowWindowsEvents::handle(ScreenshotPinnedWindow& window, const QByteArray& eventType,
                                       void* message, qintptr* result) {

    const bool isWindowsMessage = eventType == QByteArrayLiteral("windows_generic_MSG") ||
                                  eventType == QByteArrayLiteral("windows_dispatcher_MSG");
    if (isWindowsMessage && message != nullptr) {
        auto* nativeMessage = static_cast<MSG*>(message);
        const HWND pinnedHwnd = nativeMessage->hwnd;
        if (pinnedHwnd == nullptr) {
            return false;
        }

        if (window.m_clickThroughActive) {
            if (nativeMessage->message == WM_NCHITTEST) {
                if (result != nullptr) {
                    *result = HTTRANSPARENT;
                }
                return true;
            }
            if (nativeMessage->message == WM_MOUSEACTIVATE) {
                if (result != nullptr) {
                    *result = MA_NOACTIVATE;
                }
                return true;
            }
        }

        if (window.m_hideToTop != nullptr &&
            window.m_hideToTop->state() == ScreenshotPinnedHideToTopController::State::Hidden) {
            if (nativeMessage->message == WM_NCHITTEST) {
                if (result != nullptr) {
                    *result = HTTRANSPARENT;
                }
                return true;
            }
            if (nativeMessage->message == WM_MOUSEACTIVATE) {
                if (result != nullptr) {
                    *result = MA_NOACTIVATE;
                }
                return true;
            }
        }

        // The pinned image surface is exposed as HTCAPTION so a press can
        // start physical window capture. That changes the normal client hover
        // path into non-client mouse messages, and USER32's leave tracking,
        // like Qt's synthesized Enter/Leave, follows the client area. Mouse
        // messages only arm leave tracking; presence always re-resolves from
        // the live cursor through window.applyNativePointerPresence().
        const UINT pointerMessage = nativeMessage->message;
        const bool pointerMove = pointerMessage == WM_MOUSEMOVE || pointerMessage == WM_NCMOUSEMOVE;
        const bool pointerLeave =
            pointerMessage == WM_MOUSELEAVE || pointerMessage == WM_NCMOUSELEAVE;
        if (pointerMove || pointerLeave) {
            static_cast<void>(window.applyNativePointerPresence());
            window.m_hideToTop->refreshPointer();
            if (pointerMove) {
                TRACKMOUSEEVENT tracking{};
                tracking.cbSize = sizeof(tracking);
                tracking.dwFlags = TME_LEAVE;
                if (pointerMessage == WM_NCMOUSEMOVE) {
                    tracking.dwFlags |= TME_NONCLIENT;
                }
                tracking.hwndTrack = pinnedHwnd;
                TrackMouseEvent(&tracking);
            }
        }

        // Qt and USER32 release capture while handing off a pending drag.
        // WM_EXITSIZEMOVE and mouse release still finish the transaction.
        const bool pendingSystemMoveHandoff =
            nativeMessage->message == WM_CAPTURECHANGED && nativeMessage->lParam == 0 &&
            window.m_nativeGeometryController != nullptr &&
            window.m_nativeGeometryController->phase() ==
                ScreenshotPinnedNativeGeometryController::Phase::MovePending;
        const bool moveCancelled = window.m_windowDragActive && !pendingSystemMoveHandoff &&
                                   (nativeMessage->message == WM_CANCELMODE ||
                                    (nativeMessage->message == WM_CAPTURECHANGED &&
                                     reinterpret_cast<HWND>(nativeMessage->lParam) != pinnedHwnd));
        const bool resizeCancelled =
            window.m_systemSizingActive && nativeMessage->message == WM_CANCELMODE;
        if (moveCancelled || resizeCancelled) {
            static_cast<void>(window.finishNativeGeometryInteraction());
            window.m_systemSizingActive = false;
            if (window.m_windowDragActive) {
                window.finishWindowMove();
            }
            if (window.m_editController != nullptr) {
                window.m_editController->endTemporaryResizeWindowTool();
                window.m_editController->endNativeWindowInteraction();
            }
        }
        if (nativeMessage->message == WM_WINDOWPOSCHANGING &&
            window.m_nativeGeometryController != nullptr) {
            auto* position = pointerFromLParam<WINDOWPOS>(nativeMessage->lParam);
            if (position != nullptr) {
                const bool moveRequested = (position->flags & SWP_NOMOVE) == 0;
                const bool sizeRequested = (position->flags & SWP_NOSIZE) == 0;
                QRect proposal = window.m_nativeGeometryController->targetGeometry();
                if (!proposal.isValid() || proposal.isEmpty()) {
                    proposal = native::currentClientGeometry(reinterpret_cast<WId>(pinnedHwnd));
                }
                if (proposal.isValid() && !proposal.isEmpty()) {
                    if (moveRequested) {
                        proposal.moveTopLeft(QPoint(position->x, position->y));
                    }
                    if (sizeRequested) {
                        proposal.setSize(
                            QSize(std::max(1, position->cx), std::max(1, position->cy)));
                    }
                    const QRect constrained = window.m_nativeGeometryController->constrainWindowPos(
                        proposal, moveRequested, sizeRequested);
                    if (moveRequested) {
                        position->x = constrained.x();
                        position->y = constrained.y();
                    }
                    if (sizeRequested) {
                        position->cx = constrained.width();
                        position->cy = constrained.height();
                    }
                }
                // A translucent QWidget is published with UpdateLayeredWindow.
                // Letting USER preserve/copy old client bits while Qt replaces
                // that alpha surface makes live shrinking alternate between
                // the stale surface and a cleared backing-store frame.
                if (!window.m_presented || (sizeRequested && window.m_systemSizingActive)) {
                    position->flags |= SWP_NOCOPYBITS;
                }
            }
        }

        if (nativeMessage->message == WM_DPICHANGED &&
            window.m_nativeGeometryController != nullptr) {
            auto* suggestedRect = pointerFromLParam<RECT>(nativeMessage->lParam);
            if (suggestedRect != nullptr && !window.m_presented) {
                writeNativeRect(window.m_nativeGeometryController->targetGeometry(), suggestedRect);
            } else if (suggestedRect != nullptr &&
                       window.m_nativeGeometryController->adoptDpiTarget(
                           qRectFromNativeRect(*suggestedRect), window.physicalCursorPosition())) {
                // The adopted target equals the system suggestion verbatim;
                // writing it back is how the proposed geometry gets applied.
                writeNativeRect(window.m_nativeGeometryController->targetGeometry(), suggestedRect);
            }
        }

        if (nativeMessage->message == WM_GETMINMAXINFO &&
            window.nativeTrackSizeConstraintsEnabled()) {
            const QSize baseline = window.orientedInitialPhysicalSize();
            auto* limits = pointerFromLParam<MINMAXINFO>(nativeMessage->lParam);
            if (limits != nullptr && baseline.isValid() && !baseline.isEmpty()) {
                const QSize minimumSize = physicalSizeAtScale(baseline, kMinimumScalePercent);
                const QSize maximumSize =
                    physicalSizeAtScale(baseline, kMaximumScalePercent).expandedTo(minimumSize);
                const auto trackLimits = resize_geometry::trackSizeLimits(
                    minimumSize, maximumSize,
                    window.m_nativeGeometryController->committedGeometry().size(),
                    window.m_nativeGeometryController->targetGeometry().size());
                limits->ptMinTrackSize.x = trackLimits.minimum.width();
                limits->ptMinTrackSize.y = trackLimits.minimum.height();
                limits->ptMaxTrackSize.x = trackLimits.maximum.width();
                limits->ptMaxTrackSize.y = trackLimits.maximum.height();
                if (result != nullptr) {
                    *result = 0;
                }
                return true;
            }
        }

        if (nativeMessage->message == WM_SETCURSOR && window.m_windowDragCursorSet &&
            LOWORD(nativeMessage->lParam) == HTCAPTION) {
            const Qt::CursorShape dragCursorShape =
                window.m_windowDragActive ? Qt::ClosedHandCursor : Qt::OpenHandCursor;
            // HTCAPTION bypasses Qt's client cursor path. Reapply the native
            // handle because Windows may still be holding a resize cursor.
            if (!native::applyCursor(dragCursorShape)) {
                if (QWindow* handle = window.windowHandle()) {
                    handle->unsetCursor();
                    handle->setCursor(QCursor(dragCursorShape));
                }
            }
            if (result != nullptr) {
                *result = TRUE;
            }
            return true;
        }

        if (nativeMessage->message == WM_NCHITTEST) {
            int hitTest = HTCLIENT;
            const QRect nativeGeometry =
                native::currentClientGeometry(reinterpret_cast<WId>(pinnedHwnd));
            const QPoint screenPosition(GET_X_LPARAM(nativeMessage->lParam),
                                        GET_Y_LPARAM(nativeMessage->lParam));
            const ScreenshotPinnedGeometryMapping mapping(nativeGeometry, window.size(),
                                                          window.devicePixelRatioF());
            if (window.interactiveResizingEnabled() && nativeGeometry.isValid() &&
                !nativeGeometry.isEmpty()) {
                const QSize nativeHit = mapping.nativeHitSize(kResizeHitWidth);
                const int nativeHitWidth = nativeHit.width();
                const int nativeHitHeight = nativeHit.height();
                const bool inside = mapping.containsNativePosition(screenPosition);
                if (inside) {
                    const bool left = screenPosition.x() < nativeGeometry.left() + nativeHitWidth;
                    const bool right = screenPosition.x() >= nativeGeometry.left() +
                                                                 nativeGeometry.width() -
                                                                 nativeHitWidth;
                    const bool top = screenPosition.y() < nativeGeometry.top() + nativeHitHeight;
                    const bool bottom = screenPosition.y() >= nativeGeometry.top() +
                                                                  nativeGeometry.height() -
                                                                  nativeHitHeight;

                    if (left && top) {
                        hitTest = HTTOPLEFT;
                    } else if (right && top) {
                        hitTest = HTTOPRIGHT;
                    } else if (right && bottom) {
                        hitTest = HTBOTTOMRIGHT;
                    } else if (left && bottom) {
                        hitTest = HTBOTTOMLEFT;
                    } else if (top) {
                        hitTest = HTTOP;
                    } else if (right) {
                        hitTest = HTRIGHT;
                    } else if (bottom) {
                        hitTest = HTBOTTOM;
                    } else if (left) {
                        hitTest = HTLEFT;
                    }
                }

                if (hitTest == HTCLIENT) {
                    const QPoint clientPosition = mapping.localPosition(screenPosition).toPoint();
                    if (window.windowDragEnabledAt(clientPosition) && !window.m_geometryAnimating) {
                        hitTest = HTCAPTION;
                    }
                }
            } else if (window.windowDragEnabled()) {
                if (nativeGeometry.isValid() && !nativeGeometry.isEmpty()) {
                    const QPoint clientPosition = mapping.localPosition(screenPosition).toPoint();
                    if (window.windowDragEnabledAt(clientPosition)) {
                        hitTest = HTCAPTION;
                    }
                }
            }
            if (hitTest == HTCAPTION) {
                window.setWindowDragCursor(window.m_windowDragActive ? Qt::ClosedHandCursor
                                                                     : Qt::OpenHandCursor);
            } else if (!window.m_windowDragActive) {
                window.clearWindowDragCursor();
            }
            if (result != nullptr) {
                *result = hitTest;
            }
            return true;
        }

        if (nativeMessage->message == WM_NCLBUTTONDBLCLK && nativeMessage->wParam == HTCAPTION) {
            const QPoint nativePosition(GET_X_LPARAM(nativeMessage->lParam),
                                        GET_Y_LPARAM(nativeMessage->lParam));
            const QRect nativeGeometry = window.currentNativeGeometry();
            if (nativeGeometry.isValid() && !nativeGeometry.isEmpty()) {
                const ScreenshotPinnedGeometryMapping mapping(nativeGeometry, window.size(),
                                                              window.devicePixelRatioF());
                const QPoint position = mapping.localPosition(nativePosition).toPoint();
                static_cast<void>(window.handleDoubleClick(position));
            }
            // The image is a synthetic caption. Never let USER32 maximize it,
            // including when the configured action is None or dragging is disabled.
            if (result != nullptr) {
                *result = 0;
            }
            return true;
        }

        if ((nativeMessage->message == WM_NCMBUTTONDOWN ||
             nativeMessage->message == WM_NCMBUTTONDBLCLK) &&
            nativeMessage->wParam == HTCAPTION) {
            const QPoint nativePosition(GET_X_LPARAM(nativeMessage->lParam),
                                        GET_Y_LPARAM(nativeMessage->lParam));
            const QRect nativeGeometry = window.currentNativeGeometry();
            if (nativeGeometry.isValid() && !nativeGeometry.isEmpty()) {
                const ScreenshotPinnedGeometryMapping mapping(nativeGeometry, window.size(),
                                                              window.devicePixelRatioF());
                const QPoint position = mapping.localPosition(nativePosition).toPoint();
                static_cast<void>(window.handleMiddleClick(position));
            }
            // Consume the synthetic caption press so Qt cannot dispatch it again.
            if (result != nullptr) {
                *result = 0;
            }
            return true;
        }

        if (nativeMessage->message == WM_NCRBUTTONDOWN && nativeMessage->wParam == HTCAPTION) {
            // The image surface is a synthetic native caption. Suppress the
            // default half of the non-client context interaction.
            if (result != nullptr) {
                *result = 0;
            }
            return true;
        }

        if (nativeMessage->message == WM_NCRBUTTONUP) {
            // Ordinary pinned content is exposed as HTCAPTION so Windows can
            // provide native dragging. That turns right-clicks into
            // non-client messages, bypassing Qt's QContextMenuEvent path. The
            // message point is in native pixels, while QMenu expects Qt global
            // coordinates.
            const QPoint nativePosition(GET_X_LPARAM(nativeMessage->lParam),
                                        GET_Y_LPARAM(nativeMessage->lParam));
            window.showContextMenu(window.globalPositionForNativePosition(nativePosition));
            if (result != nullptr) {
                *result = 0;
            }
            return true;
        }

        if (nativeMessage->message == WM_NCLBUTTONUP || nativeMessage->message == WM_LBUTTONUP) {
            static_cast<void>(window.finishNativeGeometryInteraction());
            if (nativeMessage->wParam == HTCAPTION || window.m_windowDragActive) {
                window.finishWindowMove();
            }
            if (window.m_editController != nullptr) {
                window.m_editController->endTemporaryResizeWindowTool();
                window.m_editController->endNativeWindowInteraction();
            }
        }

        if (nativeMessage->message == WM_NCLBUTTONDOWN) {
            QWindow* handle = window.windowHandle();
            const Qt::Edges edges =
                resizeEdgesForNativeHitTest(static_cast<LRESULT>(nativeMessage->wParam));
            bool started = false;
            bool temporarilySelectedResizeWindow = false;
            if (handle != nullptr && edges != Qt::Edges() && window.interactiveResizingEnabled()) {
                if (window.m_editController != nullptr && window.m_editController->editMode() &&
                    !window.m_editController->resizeWindowToolActive()) {
                    temporarilySelectedResizeWindow =
                        window.m_editController->beginTemporaryResizeWindowTool();
                }
                resize_geometry::DragHandle dragHandle = resize_geometry::DragHandle::BottomRight;
                if (dragHandleForHitTest(static_cast<LRESULT>(nativeMessage->wParam),
                                         &dragHandle) &&
                    window.m_nativeGeometryController != nullptr &&
                    window.m_nativeGeometryController->beginResize(dragHandle)) {
                    window.exitHideToTop();
                    started = handle->startSystemResize(edges);
                }
            } else if (handle != nullptr && nativeMessage->wParam == HTCAPTION &&
                       window.windowDragEnabled()) {
                started = window.startWindowMove();
            }
            if (!started) {
                if (window.m_nativeGeometryController != nullptr) {
                    window.m_nativeGeometryController->cancelPendingInteraction();
                }
                if (temporarilySelectedResizeWindow && window.m_editController != nullptr) {
                    window.m_editController->endTemporaryResizeWindowTool();
                }
            }
            if (started) {
                if (result != nullptr) {
                    *result = 0;
                }
                return true;
            }
        }

        if (nativeMessage->message == WM_ENTERSIZEMOVE) {
            if (window.m_editController != nullptr && window.m_editController->editMode()) {
                window.m_editController->beginNativeWindowInteraction();
            }
            if (window.m_nativeGeometryController != nullptr) {
                const auto phase = window.m_nativeGeometryController->phase();
                if (phase == ScreenshotPinnedNativeGeometryController::Phase::ResizePending ||
                    phase == ScreenshotPinnedNativeGeometryController::Phase::Resizing) {
                    window.m_preserveScaleForSettledGeometry = false;
                    window.m_systemSizingActive = true;
                }
            }
        }

        if (nativeMessage->message == WM_MOVING && window.m_nativeGeometryController != nullptr) {
            auto* proposedNativeRect = pointerFromLParam<RECT>(nativeMessage->lParam);
            POINT cursor{};
            if (proposedNativeRect != nullptr && GetCursorPos(&cursor) != FALSE) {
                const QRect target = window.m_nativeGeometryController->updateMove(
                    qRectFromNativeRect(*proposedNativeRect), QPoint(cursor.x, cursor.y));
                if (target.isValid() && !target.isEmpty()) {
                    if (target != window.m_nativeGeometryController->committedGeometry()) {
                        window.exitHideToTop();
                    }
                    writeNativeRect(target, proposedNativeRect);
                    if (result != nullptr) {
                        *result = TRUE;
                    }
                    return true;
                }
            }
        }

        if (nativeMessage->message == WM_SIZING && window.interactiveResizingEnabled()) {
            window.m_preserveScaleForSettledGeometry = false;
            resize_geometry::DragHandle handle = resize_geometry::DragHandle::BottomRight;
            auto* proposedNativeRect = pointerFromLParam<RECT>(nativeMessage->lParam);
            const QSize baseline = window.orientedInitialPhysicalSize();
            if (proposedNativeRect != nullptr &&
                dragHandleForSizingEdge(nativeMessage->wParam, &handle) &&
                window.m_nativeGeometryController != nullptr) {
                const std::optional<QRect> modified =
                    window.m_nativeGeometryController->updateResize(
                        qRectFromNativeRect(*proposedNativeRect), handle, baseline,
                        kMinimumScalePercent / 100.0, kMaximumScalePercent / 100.0);
                if (!modified.has_value()) {
                    return false;
                }
                window.exitHideToTop();
                writeNativeRect(*modified, proposedNativeRect);
                window.m_systemSizingActive = true;
                window.setEffectiveScale(100.0 * modified->width() / std::max(1, baseline.width()),
                                         true);
                if (result != nullptr) {
                    *result = TRUE;
                }
                return true;
            }
        }

        if (nativeMessage->message == WM_EXITSIZEMOVE) {
            static_cast<void>(window.finishNativeGeometryInteraction());
            window.m_systemSizingActive = false;
            if (window.m_windowDragActive) {
                window.finishWindowMove();
            }
            if (window.m_editController != nullptr) {
                window.m_editController->endTemporaryResizeWindowTool();
                window.m_editController->endNativeWindowInteraction();
            }
        }

        if (nativeMessage->message == WM_WINDOWPOSCHANGED &&
            window.m_nativeGeometryController != nullptr && window.m_presented) {
            window.handleNativeGeometryObservation();
        }
    }

    return false;
}
