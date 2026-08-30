#include "snow_shot/presentation/screenshotoverlaycanvaspresenter.h"

#include "snow_draw_engine_qt/snow_canvas_widget.h"
#include "snow_shot/presentation/screenshotdisplaysession.h"
#include "snow_shot/presentation/screenshotgeometry.h"
#include "snow_shot/presentation/screenshotoverlaywindow.h"

#include <QCursor>
#include <QScreen>
#include <QTimer>
#include <QVector>

#include <cstdint>
#include <utility>

namespace {
void updateOverlayCursorsForDisplaySession(const ScreenshotDisplaySession& displaySession,
                                           bool selecting, bool dragging);

} // namespace

ScreenshotOverlayCanvasPresenter::ScreenshotOverlayCanvasPresenter(OverlayFactory ensureOverlay)
    : m_ensureOverlay(std::move(ensureOverlay)) {}

void ScreenshotOverlayCanvasPresenter::clearOverlayCanvas(ScreenshotOverlayWindow* overlay) const {
    if (overlay == nullptr || overlay->canvas() == nullptr) {
        return;
    }

    overlay->resetScreenshotRendering();
    overlay->canvas()->cancelActiveTextEditing();
    overlay->canvas()->clearCursorForLayer(SnowCanvasCursorLayer::Host);
}

namespace {
enum class DisplayModelApplyMode : std::uint8_t {
    PrepareOnly,
    ApplyCapturedImage,
};

void configureCanvasBackground(ScreenshotOverlayWindow& overlay, bool clearCanvasBackground) {
    overlay.setCanvasClearBackgroundEnabled(clearCanvasBackground);
}

void applyDisplayModelsToDisplaySession(
    const ScreenshotOverlayCanvasPresenter::OverlayFactory& ensureOverlay,
    ScreenshotDisplaySession& displaySession, DisplayModelApplyMode mode) {
    const bool applyCapturedImage = mode == DisplayModelApplyMode::ApplyCapturedImage;
    const bool clearCanvasBackground = applyCapturedImage;

    if (!ensureOverlay) {
        return;
    }

    displaySession.forEachMutableActiveDisplay([&](qsizetype index, CapturedDisplayModel& display) {
        ScreenshotOverlayWindow* overlay = displaySession.overlayAt(index);
        if (overlay == nullptr) {
            overlay = displaySession.ensureOverlayAt(index, ensureOverlay);
        }
        if (overlay == nullptr) {
            return;
        }

        SnowCanvasWidget* canvas = overlay->canvas();
        if (canvas == nullptr) {
            return;
        }
        configureCanvasBackground(*overlay, clearCanvasBackground);

        const ScreenshotDisplayViewportGeometry viewport =
            ScreenshotGeometryMapper::displayViewportGeometry(display);
        if (!viewport.valid) {
            return;
        }

        if (applyCapturedImage) {
            overlay->setScreenshotImage(
                display.image, ScreenshotGeometryMapper::displayImageSourceCanvasRect(display));
        }
        canvas->setViewportCamera(viewport.canvasCenter.x(), viewport.canvasCenter.y(),
                                  viewport.canvasToLogicalScale);
        if (display.screen != nullptr && overlay->screen() != display.screen) {
            overlay->setScreen(display.screen);
        }
        if (overlay->geometry() != viewport.logicalRect) {
            overlay->setGeometry(viewport.logicalRect);
        }
        if (applyCapturedImage) {
            overlay->update();
        }
    });
}
} // namespace

void ScreenshotOverlayCanvasPresenter::prepareDisplayModels(
    ScreenshotDisplaySession& displaySession) const {
    applyDisplayModelsToDisplaySession(m_ensureOverlay, displaySession,
                                       DisplayModelApplyMode::PrepareOnly);
}

void ScreenshotOverlayCanvasPresenter::applyDisplayModels(
    ScreenshotDisplaySession& displaySession) const {
    applyDisplayModelsToDisplaySession(m_ensureOverlay, displaySession,
                                       DisplayModelApplyMode::ApplyCapturedImage);
}

namespace {
void raiseOverlayWindow(ScreenshotOverlayWindow& overlay) {
    overlay.raise();
}

void activateAndFocusOverlayWindow(ScreenshotOverlayWindow& overlay) {
    overlay.activateWindow();

    if (overlay.canvas() != nullptr) {
        overlay.canvas()->setFocus(Qt::OtherFocusReason);
    }
    overlay.commitInitialSelectionCursor();
}

void activateOverlayWindow(ScreenshotOverlayWindow& overlay) {
    raiseOverlayWindow(overlay);
    activateAndFocusOverlayWindow(overlay);
}

void scheduleOverlayActivation(ScreenshotOverlayWindow* overlay) {
    if (overlay == nullptr) {
        return;
    }

    QTimer::singleShot(0, overlay, [overlay]() {
        if (overlay == nullptr || !overlay->isVisible()) {
            return;
        }

        activateAndFocusOverlayWindow(*overlay);
        QTimer::singleShot(0, overlay, [overlay]() {
            if (overlay != nullptr && overlay->isVisible()) {
                overlay->commitInitialSelectionCursor();
            }
        });
    });
}

void showOverlayWindow(ScreenshotOverlayWindow& overlay) {
    overlay.showPreparedFrame();
}

struct ActiveOverlayEntry {
    const CapturedDisplayModel* display = nullptr;
    ScreenshotOverlayWindow* overlay = nullptr;
};

QVector<ActiveOverlayEntry> activeOverlayEntries(const ScreenshotDisplaySession& displaySession) {
    QVector<ActiveOverlayEntry> entries;
    entries.reserve(static_cast<int>(displaySession.size()));
    displaySession.forEachActiveOverlay(
        [&](qsizetype, const CapturedDisplayModel& display, ScreenshotOverlayWindow* overlay) {
            entries.push_back(ActiveOverlayEntry{&display, overlay});
        });
    return entries;
}

qsizetype overlayEntryIndexAtPosition(const QVector<ActiveOverlayEntry>& entries,
                                      const QPoint& cursorPosition) {
    for (qsizetype index = 0; index < entries.size(); ++index) {
        const CapturedDisplayModel* display = entries.at(index).display;
        if (display != nullptr && display->logicalRect.contains(cursorPosition, false)) {
            return index;
        }
    }

    return -1;
}

qsizetype preferredOverlayEntryIndex(const QVector<ActiveOverlayEntry>& entries,
                                     const QPoint& cursorPosition) {
    const qsizetype cursorIndex = overlayEntryIndexAtPosition(entries, cursorPosition);
    return cursorIndex >= 0 || entries.isEmpty() ? cursorIndex : 0;
}

void showCapturedImageOverlayNow(ScreenshotOverlayWindow& overlay) {
    if (!overlay.isVisible()) {
        showOverlayWindow(overlay);
    }
    raiseOverlayWindow(overlay);
    scheduleOverlayActivation(&overlay);
}

void showCapturedImageOverlayDeferred(ScreenshotOverlayWindow* overlay) {
    if (overlay == nullptr) {
        return;
    }

    QTimer::singleShot(0, overlay, [overlay]() {
        if (overlay == nullptr) {
            return;
        }

        if (!overlay->isVisible()) {
            showOverlayWindow(*overlay);
        }
        raiseOverlayWindow(*overlay);
        scheduleOverlayActivation(overlay);
    });
}

void showCapturedImageOverlaysForDisplaySession(const ScreenshotDisplaySession& displaySession) {
    const QVector<ActiveOverlayEntry> entries = activeOverlayEntries(displaySession);
    const qsizetype preferredIndex = preferredOverlayEntryIndex(entries, QCursor::pos());

    if (preferredIndex >= 0 && preferredIndex < entries.size()) {
        showCapturedImageOverlayNow(*entries.at(preferredIndex).overlay);
    }

    for (qsizetype index = 0; index < entries.size(); ++index) {
        if (index == preferredIndex) {
            continue;
        }
        showCapturedImageOverlayDeferred(entries.at(index).overlay);
    }
}

void showOverlayWindowsForDisplaySession(const ScreenshotDisplaySession& displaySession,
                                         ScreenshotOverlayShowMode mode) {
    if (mode == ScreenshotOverlayShowMode::CapturedImage) {
        showCapturedImageOverlaysForDisplaySession(displaySession);
        return;
    }

    displaySession.forEachActiveOverlay(
        [mode](qsizetype, const CapturedDisplayModel&, ScreenshotOverlayWindow* overlay) {
            const bool wasVisible = overlay->isVisible();

            if (!wasVisible) {
                showOverlayWindow(*overlay);
            }

            if (mode == ScreenshotOverlayShowMode::PreparedPreview) {
                activateOverlayWindow(*overlay);
            }
        });
}
} // namespace

void ScreenshotOverlayCanvasPresenter::showOverlayWindows(
    const ScreenshotDisplaySession& displaySession, ScreenshotOverlayShowMode mode) const {
    showOverlayWindowsForDisplaySession(displaySession, mode);
}

namespace {
void updateOverlayStateForDisplaySession(const ScreenshotDisplaySession& displaySession,
                                         const QRectF& selection, int cornerRadius, int shadowWidth,
                                         const QColor& shadowColor, bool selectionToolbarHovered,
                                         bool selectionHandlesVisible, bool intelligentSelecting,
                                         bool manualSelecting, bool dragging) {
    const bool hasSelection = selection.isValid() && !selection.isEmpty();
    const ScreenshotHalfOpenRect selectionRect =
        hasSelection ? ScreenshotHalfOpenRect::fromRectF(selection) : ScreenshotHalfOpenRect();
    displaySession.forEachActiveOverlay([&](qsizetype, const CapturedDisplayModel& display,
                                            ScreenshotOverlayWindow* overlay) {
        SnowCanvasWidget* canvas = overlay->canvas();
        if (canvas == nullptr) {
            return;
        }
        const ScreenshotHalfOpenRect displayRect =
            ScreenshotHalfOpenRect::fromRectF(ScreenshotGeometryMapper::displayCanvasRect(display));
        const bool selectionIntersectsDisplay =
            hasSelection && selectionRect.intersects(displayRect);
        overlay->setScreenshotMaskVisible(true);
        if (selectionIntersectsDisplay) {
            overlay->setScreenshotSelection(selection, selectionHandlesVisible, cornerRadius,
                                            shadowWidth, shadowColor, selectionToolbarHovered);
        } else {
            overlay->clearScreenshotSelection();
        }
    });
    updateOverlayCursorsForDisplaySession(displaySession, intelligentSelecting || manualSelecting,
                                          dragging);
}
} // namespace

void ScreenshotOverlayCanvasPresenter::updateOverlayState(
    const ScreenshotDisplaySession& displaySession, const QRectF& selection, int cornerRadius,
    int shadowWidth, const QColor& shadowColor, bool selectionToolbarHovered,
    bool selectionHandlesVisible, bool intelligentSelecting, bool manualSelecting,
    bool dragging) const {
    updateOverlayStateForDisplaySession(displaySession, selection, cornerRadius, shadowWidth,
                                        shadowColor, selectionToolbarHovered,
                                        selectionHandlesVisible, intelligentSelecting,
                                        manualSelecting, dragging);
}

namespace {
void updateOverlayCursorsForDisplaySession(const ScreenshotDisplaySession& displaySession,
                                           bool selecting, bool dragging) {
    if (!dragging) {
        displaySession.forEachOverlay(
            [](qsizetype, ScreenshotOverlayWindow* overlay) {
                if (overlay != nullptr && overlay->canvas() != nullptr) {
                    overlay->canvas()->clearCursorForLayer(SnowCanvasCursorLayer::Host);
                }
            });
    }

    if (!selecting) {
        return;
    }

    displaySession.forEachActiveOverlay(
        [](qsizetype, const CapturedDisplayModel&, ScreenshotOverlayWindow* overlay) {
            SnowCanvasWidget* canvas = overlay->canvas();
            if (canvas != nullptr) {
                canvas->setCursorForLayer(SnowCanvasCursorLayer::Host,
                                          QCursor(Qt::CrossCursor));
            }
        });
}
} // namespace

void ScreenshotOverlayCanvasPresenter::updateOverlayCursors(
    const ScreenshotDisplaySession& displaySession, bool selecting, bool dragging) const {
    updateOverlayCursorsForDisplaySession(displaySession, selecting, dragging);
}

void ScreenshotOverlayCanvasPresenter::updateGuideLines(
    const ScreenshotDisplaySession& displaySession, ScreenshotOverlayWindow* owner,
    const QPointF& localPosition, bool selecting, const QColor& cursorColor,
    const QColor& monitorCenterColor) const {
    const bool guideLinesEnabled = (cursorColor.isValid() && cursorColor.alpha() > 0) ||
                                   (monitorCenterColor.isValid() && monitorCenterColor.alpha() > 0);
    ScreenshotOverlayWindow* nextOwner = selecting && guideLinesEnabled ? owner : nullptr;
    if (nextOwner != nullptr && m_guideLineOwner != nextOwner) {
        bool activeOwner = false;
        displaySession.forEachActiveOverlay(
            [&](qsizetype, const CapturedDisplayModel&, ScreenshotOverlayWindow* overlay) {
                activeOwner = activeOwner || overlay == nextOwner;
            });
        if (!activeOwner) {
            nextOwner = nullptr;
        }
    }

    if (m_guideLineOwner != nextOwner && m_guideLineOwner != nullptr) {
        m_guideLineOwner->clearScreenshotGuideLines();
    }
    m_guideLineOwner = nextOwner;
    if (nextOwner != nullptr) {
        nextOwner->setScreenshotGuideLines(localPosition, cursorColor, monitorCenterColor);
    }
}

void ScreenshotOverlayCanvasPresenter::updateGuideLinesAtGlobalPosition(
    const ScreenshotDisplaySession& displaySession, const QPoint& globalPosition, bool selecting,
    const QColor& cursorColor, const QColor& monitorCenterColor) const {
    if (!selecting) {
        clearGuideLines(displaySession);
        return;
    }

    const QVector<ActiveOverlayEntry> entries = activeOverlayEntries(displaySession);
    const qsizetype ownerIndex = overlayEntryIndexAtPosition(entries, globalPosition);
    if (ownerIndex < 0 || ownerIndex >= entries.size()) {
        clearGuideLines(displaySession);
        return;
    }

    ScreenshotOverlayWindow* owner = entries.at(ownerIndex).overlay;
    const QPointF localPosition = QPointF(globalPosition - owner->geometry().topLeft());
    updateGuideLines(displaySession, owner, localPosition, true, cursorColor, monitorCenterColor);
}

void ScreenshotOverlayCanvasPresenter::clearGuideLines(
    const ScreenshotDisplaySession& displaySession) const {
    const QPointer<ScreenshotOverlayWindow> previousOwner = m_guideLineOwner;
    m_guideLineOwner = nullptr;
    if (previousOwner != nullptr) {
        previousOwner->clearScreenshotGuideLines();
    }
    displaySession.forEachOverlay([previousOwner](qsizetype, ScreenshotOverlayWindow* overlay) {
        if (overlay != previousOwner) {
            overlay->clearScreenshotGuideLines();
        }
    });
}

void ScreenshotOverlayCanvasPresenter::setOverlayCursor(
    ScreenshotOverlayWindow* overlay, ScreenshotSelectionDragMode dragMode) const {
    if (overlay == nullptr) {
        return;
    }

    SnowCanvasWidget* canvas = overlay->canvas();
    if (canvas == nullptr) {
        return;
    }

    switch (dragMode) {
    case ScreenshotSelectionDragMode::Marquee:
        canvas->setCursorForLayer(SnowCanvasCursorLayer::Host, QCursor(Qt::CrossCursor));
        break;
    case ScreenshotSelectionDragMode::All:
        canvas->setCursorForLayer(SnowCanvasCursorLayer::Host, QCursor(Qt::SizeAllCursor));
        break;
    case ScreenshotSelectionDragMode::TopLeft:
    case ScreenshotSelectionDragMode::BottomRight:
        canvas->setCursorForLayer(SnowCanvasCursorLayer::Host, QCursor(Qt::SizeFDiagCursor));
        break;
    case ScreenshotSelectionDragMode::TopRight:
    case ScreenshotSelectionDragMode::BottomLeft:
        canvas->setCursorForLayer(SnowCanvasCursorLayer::Host, QCursor(Qt::SizeBDiagCursor));
        break;
    case ScreenshotSelectionDragMode::Top:
    case ScreenshotSelectionDragMode::Bottom:
        canvas->setCursorForLayer(SnowCanvasCursorLayer::Host, QCursor(Qt::SizeVerCursor));
        break;
    case ScreenshotSelectionDragMode::Right:
    case ScreenshotSelectionDragMode::Left:
        canvas->setCursorForLayer(SnowCanvasCursorLayer::Host, QCursor(Qt::SizeHorCursor));
        break;
    case ScreenshotSelectionDragMode::None:
    default:
        canvas->clearCursorForLayer(SnowCanvasCursorLayer::Host);
        break;
    }
}

namespace {
void setCanvasInteractionEnabledForDisplaySession(const ScreenshotDisplaySession& displaySession,
                                                  bool enabled) {
    displaySession.forEachOverlay([enabled](qsizetype, ScreenshotOverlayWindow* overlay) {
        if (overlay->canvas() != nullptr) {
            overlay->canvas()->setInteractionEnabled(enabled);
        }
    });
}

void setCanvasToolForDisplaySession(const ScreenshotDisplaySession& displaySession,
                                    SnowCanvasTool tool) {
    displaySession.forEachOverlay([tool](qsizetype, ScreenshotOverlayWindow* overlay) {
        if (overlay->canvas() != nullptr) {
            overlay->canvas()->setCanvasTool(tool);
        }
    });
}
} // namespace

void ScreenshotOverlayCanvasPresenter::setCanvasInteractionEnabled(
    const ScreenshotDisplaySession& displaySession, bool enabled) const {
    setCanvasInteractionEnabledForDisplaySession(displaySession, enabled);
}

void ScreenshotOverlayCanvasPresenter::setCanvasTool(const ScreenshotDisplaySession& displaySession,
                                                     SnowCanvasTool tool) const {
    setCanvasToolForDisplaySession(displaySession, tool);
}

bool ScreenshotOverlayCanvasPresenter::resetEditingState(
    const ScreenshotDisplaySession& displaySession) const {
    bool reset = true;
    displaySession.forEachOverlay([&reset](qsizetype, ScreenshotOverlayWindow* overlay) {
        SnowCanvasWidget* canvas = overlay->canvas();
        if (canvas != nullptr && !canvas->resetEditingState()) {
            reset = false;
        }
    });
    return reset;
}

namespace {
bool tryCurrentRectangleStyleForDisplaySession(const ScreenshotDisplaySession& displaySession,
                                               SnowCanvasShapeStyle* outStyle) {
    if (outStyle == nullptr) {
        return false;
    }

    bool found = false;
    displaySession.forEachActiveOverlay(
        [&](qsizetype, const CapturedDisplayModel&, ScreenshotOverlayWindow* overlay) {
            if (found || overlay->canvas() == nullptr) {
                return;
            }
            *outStyle = overlay->canvas()->canvasStyleToolbarState().shapeStyle;
            found = true;
        });
    if (found) {
        return true;
    }

    displaySession.forEachOverlay([&](qsizetype, ScreenshotOverlayWindow* overlay) {
        if (found || overlay->canvas() == nullptr) {
            return;
        }
        *outStyle = overlay->canvas()->canvasStyleToolbarState().shapeStyle;
        found = true;
    });
    return found;
}
} // namespace

bool ScreenshotOverlayCanvasPresenter::tryCurrentRectangleStyle(
    const ScreenshotDisplaySession& displaySession, SnowCanvasShapeStyle* outStyle) const {
    return tryCurrentRectangleStyleForDisplaySession(displaySession, outStyle);
}

bool ScreenshotOverlayCanvasPresenter::tryCurrentStyleToolbarState(
    const ScreenshotDisplaySession& displaySession, SnowCanvasStyleToolbarState* outState) const {
    if (outState == nullptr) {
        return false;
    }

    bool found = false;
    displaySession.forEachActiveOverlay([outState, &found](qsizetype, const CapturedDisplayModel&,
                                                           ScreenshotOverlayWindow* overlay) {
        if (found || overlay == nullptr || overlay->canvas() == nullptr) {
            return;
        }
        *outState = overlay->canvas()->canvasStyleToolbarState();
        found = true;
    });
    if (found) {
        return true;
    }

    displaySession.forEachOverlay([outState, &found](qsizetype, ScreenshotOverlayWindow* overlay) {
        if (found || overlay == nullptr || overlay->canvas() == nullptr) {
            return;
        }
        *outState = overlay->canvas()->canvasStyleToolbarState();
        found = true;
    });
    return found;
}

namespace {
SnowCanvasShapeStyle
currentRectangleStyleForDisplaySession(const ScreenshotDisplaySession& displaySession) {
    SnowCanvasShapeStyle style{};
    if (tryCurrentRectangleStyleForDisplaySession(displaySession, &style)) {
        return style;
    }
    return {};
}
} // namespace

SnowCanvasShapeStyle ScreenshotOverlayCanvasPresenter::currentRectangleStyle(
    const ScreenshotDisplaySession& displaySession) const {
    return currentRectangleStyleForDisplaySession(displaySession);
}

void ScreenshotOverlayCanvasPresenter::setShapeStylePatch(
    const ScreenshotDisplaySession& displaySession, const SnowCanvasShapeStyle& style,
    quint32 properties, SnowCanvasShapeKind kind) const {
    displaySession.forEachOverlay(
        [&style, properties, kind](qsizetype, ScreenshotOverlayWindow* overlay) {
            if (overlay != nullptr && overlay->canvas() != nullptr) {
                overlay->canvas()->setCanvasShapeStylePatch(style, properties, kind);
            }
        });
}

void ScreenshotOverlayCanvasPresenter::setFilterStyle(
    const ScreenshotDisplaySession& displaySession, const SnowCanvasFilterStyle& style,
    quint32 properties) const {
    displaySession.forEachOverlay(
        [&style, properties](qsizetype, ScreenshotOverlayWindow* overlay) {
            if (overlay != nullptr && overlay->canvas() != nullptr) {
                overlay->canvas()->setCanvasFilterStyle(style, properties);
            }
        });
}

void ScreenshotOverlayCanvasPresenter::setWatermarkConfig(
    const ScreenshotDisplaySession& displaySession, const SnowCanvasWatermarkConfig& config) const {
    displaySession.forEachOverlay([&config](qsizetype, ScreenshotOverlayWindow* overlay) {
        if (overlay != nullptr && overlay->canvas() != nullptr) {
            overlay->canvas()->setCanvasWatermarkConfig(config);
        }
    });
}

void ScreenshotOverlayCanvasPresenter::previewWatermarkConfig(
    const ScreenshotDisplaySession& displaySession, const SnowCanvasWatermarkConfig& config) const {
    displaySession.forEachOverlay([&config](qsizetype, ScreenshotOverlayWindow* overlay) {
        if (overlay != nullptr && overlay->canvas() != nullptr) {
            overlay->canvas()->previewCanvasWatermarkConfig(config);
        }
    });
}

void ScreenshotOverlayCanvasPresenter::setSpotlightConfig(
    ScreenshotDisplaySession& displaySession, const SnowCanvasSpotlightConfig& config) const {
    displaySession.forEachOverlay([&config](qsizetype, ScreenshotOverlayWindow* overlay) {
        if (overlay != nullptr && overlay->canvas() != nullptr) {
            overlay->canvas()->setCanvasSpotlightConfig(config);
        }
    });
}

void ScreenshotOverlayCanvasPresenter::previewSpotlightConfig(
    ScreenshotDisplaySession& displaySession, const SnowCanvasSpotlightConfig& config) const {
    displaySession.forEachOverlay([&config](qsizetype, ScreenshotOverlayWindow* overlay) {
        if (overlay != nullptr && overlay->canvas() != nullptr) {
            overlay->canvas()->previewCanvasSpotlightConfig(config);
        }
    });
}

void ScreenshotOverlayCanvasPresenter::setTextStyle(const ScreenshotDisplaySession& displaySession,
                                                    const SnowCanvasTextStyle& style) const {
    displaySession.forEachOverlay([&style](qsizetype, ScreenshotOverlayWindow* overlay) {
        if (overlay != nullptr && overlay->canvas() != nullptr) {
            static_cast<void>(overlay->canvas()->setCanvasTextStyle(style));
        }
    });
}

void ScreenshotOverlayCanvasPresenter::setSerialNumberStyle(
    const ScreenshotDisplaySession& displaySession,
    const SnowCanvasSerialNumberStyle& style) const {
    displaySession.forEachOverlay([&style](qsizetype, ScreenshotOverlayWindow* overlay) {
        if (overlay != nullptr && overlay->canvas() != nullptr) {
            static_cast<void>(overlay->canvas()->setCanvasSerialNumberStyle(style));
        }
    });
}

namespace {
void adjustSelectedSerialNumbersForDisplaySession(const ScreenshotDisplaySession& displaySession,
                                                  qint64 delta) {
    displaySession.forEachOverlay([delta](qsizetype, ScreenshotOverlayWindow* overlay) {
        if (overlay->canvas() != nullptr) {
            overlay->canvas()->adjustSelectedSerialNumbers(delta);
        }
    });
}
} // namespace

void ScreenshotOverlayCanvasPresenter::adjustSelectedSerialNumbers(
    const ScreenshotDisplaySession& displaySession, qint64 delta) const {
    adjustSelectedSerialNumbersForDisplaySession(displaySession, delta);
}

namespace {
void createTextForSelectedSerialNumberForDisplaySession(
    const ScreenshotDisplaySession& displaySession) {
    displaySession.forEachOverlay([](qsizetype, ScreenshotOverlayWindow* overlay) {
        if (overlay->canvas() != nullptr) {
            overlay->canvas()->createSerialNumberText();
        }
    });
}
} // namespace

void ScreenshotOverlayCanvasPresenter::createTextForSelectedSerialNumber(
    const ScreenshotDisplaySession& displaySession) const {
    createTextForSelectedSerialNumberForDisplaySession(displaySession);
}

void ScreenshotOverlayCanvasPresenter::reorderSelectedElements(
    const ScreenshotDisplaySession& displaySession, SnowCanvasSelectionOrder order) const {
    bool handled = false;
    displaySession.forEachOverlay([order, &handled](qsizetype, ScreenshotOverlayWindow* overlay) {
        if (!handled && overlay != nullptr && overlay->canvas() != nullptr) {
            static_cast<void>(overlay->canvas()->reorderSelected(order));
            handled = true;
        }
    });
}

void ScreenshotOverlayCanvasPresenter::setSelectedElementsOpacity(
    const ScreenshotDisplaySession& displaySession, qreal opacity) const {
    bool handled = false;
    displaySession.forEachOverlay([opacity, &handled](qsizetype, ScreenshotOverlayWindow* overlay) {
        if (!handled && overlay != nullptr && overlay->canvas() != nullptr) {
            static_cast<void>(overlay->canvas()->setSelectedOpacity(opacity));
            handled = true;
        }
    });
}

void ScreenshotOverlayCanvasPresenter::duplicateSelectedElements(
    const ScreenshotDisplaySession& displaySession) const {
    bool handled = false;
    displaySession.forEachOverlay([&handled](qsizetype, ScreenshotOverlayWindow* overlay) {
        if (!handled && overlay != nullptr && overlay->canvas() != nullptr) {
            static_cast<void>(overlay->canvas()->duplicateSelected());
            handled = true;
        }
    });
}

void ScreenshotOverlayCanvasPresenter::deleteSelectedElements(
    const ScreenshotDisplaySession& displaySession) const {
    bool handled = false;
    displaySession.forEachOverlay([&handled](qsizetype, ScreenshotOverlayWindow* overlay) {
        if (!handled && overlay != nullptr && overlay->canvas() != nullptr) {
            static_cast<void>(overlay->canvas()->deleteSelected());
            handled = true;
        }
    });
}
