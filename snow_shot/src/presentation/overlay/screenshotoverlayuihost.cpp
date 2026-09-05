#include "snow_shot/presentation/screenshotoverlayuihost.h"

#include "../capture/screenshotcaptureperfinstrumentation.h"
#include "snow_draw_engine_qt/snow_canvas_widget.h"
#include "snow_shot/presentation/components/icons/iconrenderutils.h"
#include "snow_shot/presentation/components/icons/snowshoticons.h"
#include "snow_shot/presentation/screenshotcolorpickerwidget.h"
#include "snow_shot/presentation/screenshotselectiontoolbarwidget.h"
#include "snow_shot/presentation/screenshotoverlaywindow.h"
#include "snow_shot/presentation/screenshottoolbarcommands.h"
#include "snow_shot/presentation/screenshottoolbarwindow.h"

#include <algorithm>
#include <optional>
#include <utility>

#include <QCoreApplication>
#include <QCursor>
#include <QEvent>
#include <QFontMetrics>
#include <QGuiApplication>
#include <QMouseEvent>
#include <QPainter>
#include <QPaintEvent>
#include <QPen>
#include <QPixmap>
#include <QVector>
#include <QWidget>
#include <QtMath>

#if defined(Q_OS_WIN) || defined(_WIN32)
#include <qt_windows.h>
#endif

namespace {
constexpr int kShortcutHintsMargin = 16;
constexpr int kShortcutHintsPadding = 16;
constexpr int kShortcutHintsRadius = 6;
constexpr int kShortcutHintsFontSize = 14;
constexpr int kShortcutHintsLineHeight = 22;
constexpr int kShortcutHintsRowSpacing = 16;
constexpr int kShortcutHintsColonMarginLeft = 2;
constexpr int kShortcutHintsColonMarginRight = 8;
constexpr int kShortcutHintsChipHorizontalPadding = 16;
constexpr int kShortcutHintsChipVerticalPadding = 2;
constexpr int kShortcutHintsChipRadius = 6;
constexpr int kShortcutHintsChipSpacing = 8;
constexpr int kShortcutHintsIconSize = 14;
constexpr int kShortcutHintsIconTextGap = 8;
constexpr int kShortcutHintsChipTrailingSpace = 8;

namespace custom_outlined_icons = snow_shot::presentation::icons::custom::outlined;

class ScreenshotShortcutHintsWidget final : public QWidget {
  public:
    explicit ScreenshotShortcutHintsWidget(QWidget* parent = nullptr) : QWidget(parent) {
        setObjectName(QStringLiteral("screenshotShortcutHints"));
        setAttribute(Qt::WA_TranslucentBackground, true);
        setAttribute(Qt::WA_NoSystemBackground, true);
        setAttribute(Qt::WA_TransparentForMouseEvents, true);
        setFocusPolicy(Qt::NoFocus);
        QFont hintFont = font();
        hintFont.setPixelSize(kShortcutHintsFontSize);
        hintFont.setWeight(QFont::Normal);
        setFont(hintFont);
        // The hint is click-through, so observe application mouse moves to hide it on hover.
        if (QCoreApplication::instance() != nullptr) {
            QCoreApplication::instance()->installEventFilter(this);
        }
        hide();
    }

    void setPresentation(ScreenshotShortcutHintMode mode, qreal opacity) {
        m_mode = mode;
        m_context.reset();
        m_opacity = std::clamp<qreal>(opacity, 0.0, 1.0);
        updateTranslatedLines();
        if (!hasVisiblePresentation()) {
            hide();
        }
    }

    void setPresentation(const ScreenshotShortcutHintContext& context, qreal opacity) {
        m_context = context;
        m_mode = screenshotShortcutHintModeForContext(context);
        m_opacity = std::clamp<qreal>(opacity, 0.0, 1.0);
        updateTranslatedLines();
        if (!hasVisiblePresentation()) {
            hide();
        }
    }

    [[nodiscard]] bool hasVisiblePresentation() const {
        return !m_rows.isEmpty() && m_opacity > 0.0;
    }

    void setObscuringSelection(const QRectF& selectionGlobal) {
        m_selectionGlobal = selectionGlobal;
    }

    void refreshVisibility(const QPointF& cursorGlobal) {
        m_cursorGlobal = cursorGlobal;
        const QRectF hintAreaGlobal(mapToGlobal(QPoint(0, 0)), size());
        const bool visible = parentWidget() != nullptr && hasVisiblePresentation() &&
                             !screenshotShortcutHintAreaIsObscured(
                                 hintAreaGlobal, m_selectionGlobal, m_cursorGlobal);
        setVisible(visible);
        if (visible) {
            raise();
        }
    }

  protected:
    bool eventFilter(QObject* watched, QEvent* event) override {
        if (event != nullptr && event->type() == QEvent::MouseMove) {
            const auto* mouseEvent = static_cast<QMouseEvent*>(event);
            refreshVisibility(mouseEvent->globalPosition());
        }
        return QWidget::eventFilter(watched, event);
    }

    void changeEvent(QEvent* event) override {
        QWidget::changeEvent(event);
        if (event != nullptr && event->type() == QEvent::LanguageChange) {
            updateTranslatedLines();
        }
    }

    void paintEvent(QPaintEvent* event) override {
        Q_UNUSED(event);
        QPainter painter(this);
        painter.setRenderHint(QPainter::Antialiasing, true);
        painter.setOpacity(m_opacity);
        painter.setPen(Qt::NoPen);
        painter.setBrush(QColor(0, 0, 0, 163));
        painter.drawRoundedRect(QRectF(rect()).adjusted(0.5, 0.5, -0.5, -0.5),
                                kShortcutHintsRadius, kShortcutHintsRadius);

        const QFontMetrics metrics(font());
        const QColor contentColor(Qt::white);
        int rowTop = kShortcutHintsPadding;
        for (const ScreenshotShortcutHintRow& row : m_rows) {
            const int labelWidth = metrics.horizontalAdvance(row.label);
            painter.setPen(QColor(255, 255, 255, 184));
            painter.drawText(QRect(kShortcutHintsPadding, rowTop, labelWidth,
                                   kShortcutHintsLineHeight),
                             Qt::AlignLeft | Qt::AlignVCenter, row.label);

            const int colonLeft = kShortcutHintsPadding + labelWidth +
                                  kShortcutHintsColonMarginLeft;
            const int colonWidth = metrics.horizontalAdvance(QLatin1Char(':'));
            painter.drawText(QRect(colonLeft, rowTop, colonWidth,
                                   kShortcutHintsLineHeight),
                             Qt::AlignLeft | Qt::AlignVCenter, QStringLiteral(":"));
            int chipLeft = colonLeft + colonWidth + kShortcutHintsColonMarginRight;
            const auto icon = row.input == ScreenshotShortcutHintInput::Mouse
                                  ? custom_outlined_icons::WheelMouse()
                                  : custom_outlined_icons::Keyboard();
            const QPixmap iconPixmap =
                snow_shot::presentation::icons::renderTintedIconPixmap(
                    icon, QSize(kShortcutHintsIconSize, kShortcutHintsIconSize),
                    devicePixelRatioF(), contentColor);
            const QStringList chipLabels =
                row.shortcutChips.isEmpty() ? QStringList{row.shortcut} : row.shortcutChips;
            for (const QString& chipLabel : chipLabels) {
                const int chipWidth = shortcutChipWidth(chipLabel, metrics);
                const QRectF chipRect(chipLeft, rowTop - kShortcutHintsChipVerticalPadding,
                                      chipWidth, shortcutChipHeight());
                painter.setRenderHint(QPainter::Antialiasing, true);
                painter.setPen(QPen(contentColor, 1.0));
                painter.setBrush(Qt::NoBrush);
                painter.drawRoundedRect(chipRect.adjusted(0.5, 0.5, -0.5, -0.5),
                                        kShortcutHintsChipRadius, kShortcutHintsChipRadius);

                const int iconLeft = chipLeft + kShortcutHintsChipHorizontalPadding;
                const int iconTop =
                    rowTop + (kShortcutHintsLineHeight - kShortcutHintsIconSize) / 2;
                if (!iconPixmap.isNull()) {
                    painter.drawPixmap(iconLeft, iconTop, iconPixmap);
                }

                const int textLeft = iconLeft + kShortcutHintsIconSize +
                                     kShortcutHintsIconTextGap;
                const int textWidth = metrics.horizontalAdvance(chipLabel);
                painter.setRenderHint(QPainter::Antialiasing, false);
                painter.setPen(contentColor);
                painter.drawText(QRect(textLeft, rowTop, textWidth, kShortcutHintsLineHeight),
                                 Qt::AlignLeft | Qt::AlignVCenter, chipLabel);
                chipLeft += chipWidth + kShortcutHintsChipSpacing;
            }
            rowTop += kShortcutHintsLineHeight + kShortcutHintsRowSpacing;
        }
    }

  private:
    [[nodiscard]] static int shortcutChipHeight() {
        return kShortcutHintsLineHeight + kShortcutHintsChipVerticalPadding * 2;
    }

    [[nodiscard]] static int shortcutChipWidth(const QString& shortcut,
                                               const QFontMetrics& metrics) {
        return kShortcutHintsChipHorizontalPadding * 2 + kShortcutHintsIconSize +
               kShortcutHintsIconTextGap + metrics.horizontalAdvance(shortcut);
    }

    [[nodiscard]] static int shortcutChipsWidth(const ScreenshotShortcutHintRow& row,
                                                const QFontMetrics& metrics) {
        if (row.shortcutChips.isEmpty()) {
            return shortcutChipWidth(row.shortcut, metrics);
        }
        int width = kShortcutHintsChipSpacing * (static_cast<int>(row.shortcutChips.size()) - 1);
        for (const QString& chipLabel : row.shortcutChips) {
            width += shortcutChipWidth(chipLabel, metrics);
        }
        return width;
    }

    void updateTranslatedLines() {
        const QVector<ScreenshotShortcutHintRow> nextRows =
            m_context.has_value() ? screenshotShortcutHintRows(*m_context)
                                  : screenshotShortcutHintRows(m_mode);
        if (m_rows != nextRows) {
            m_rows = nextRows;
            setAccessibleName(screenshotShortcutHintLines(m_rows).join(QLatin1Char('\n')));
            updateSize();
            update();
        }
    }

    void updateSize() {
        const QFontMetrics metrics(font());
        int width = 0;
        for (const ScreenshotShortcutHintRow& row : m_rows) {
            int rowWidth = metrics.horizontalAdvance(row.label);
            rowWidth += kShortcutHintsColonMarginLeft +
                        metrics.horizontalAdvance(QLatin1Char(':')) +
                        kShortcutHintsColonMarginRight;
            rowWidth += shortcutChipsWidth(row, metrics) + kShortcutHintsChipTrailingSpace;
            width = std::max(width, rowWidth);
        }
        const int rowCount = static_cast<int>(m_rows.size());
        const int contentHeight =
            rowCount > 0 ? kShortcutHintsLineHeight * rowCount +
                               kShortcutHintsRowSpacing * (rowCount - 1)
                         : 0;
        setFixedSize(width + kShortcutHintsPadding * 2,
                     contentHeight + kShortcutHintsPadding * 2);
    }

    QVector<ScreenshotShortcutHintRow> m_rows;
    std::optional<ScreenshotShortcutHintContext> m_context;
    ScreenshotShortcutHintMode m_mode = ScreenshotShortcutHintMode::Hidden;
    qreal m_opacity = 0.0;
    QRectF m_selectionGlobal;
    QPointF m_cursorGlobal;
};

template <typename Widget> Widget* trackedWidget(QPointer<Widget>& pointer) {
    return pointer.data();
}

void showPreparedWidget(QWidget* widget) {
    if (widget == nullptr) {
        return;
    }

    const bool wasVisible = widget->isVisible();
    const bool concealFirstPaint = !wasVisible && widget->isWindow() &&
                                   QGuiApplication::platformName() == QStringLiteral("windows");
    const qreal previousOpacity = widget->windowOpacity();
    if (concealFirstPaint) {
        widget->setWindowOpacity(0.0);
    }

    {
        SNOW_SHOT_CAPTURE_PERF_SCOPE("show_prepared.show");
        widget->show();
    }
    {
        SNOW_SHOT_CAPTURE_PERF_SCOPE("show_prepared.repaint");
        widget->repaint();
    }
    {
        SNOW_SHOT_CAPTURE_PERF_SCOPE("show_prepared.posted_events");
        QCoreApplication::sendPostedEvents(widget->window(), QEvent::UpdateRequest);
    }

#if defined(Q_OS_WIN) || defined(_WIN32)
    if (QGuiApplication::platformName() == QStringLiteral("windows")) {
        SNOW_SHOT_CAPTURE_PERF_SCOPE("show_prepared.redraw_window");
        const HWND hwnd = reinterpret_cast<HWND>(widget->winId());
        if (hwnd != nullptr) {
            static_cast<void>(RedrawWindow(hwnd, nullptr, nullptr,
                                           RDW_INVALIDATE | RDW_UPDATENOW | RDW_ALLCHILDREN));
        }
    }
#endif

    if (concealFirstPaint) {
        widget->setWindowOpacity(previousOpacity);
    }
}

void showPreparedChildWidget(QWidget* widget) {
    if (widget == nullptr) {
        return;
    }

    {
        SNOW_SHOT_CAPTURE_PERF_SCOPE("show_prepared_child.show");
        widget->show();
    }
    {
        SNOW_SHOT_CAPTURE_PERF_SCOPE("show_prepared_child.repaint");
        widget->repaint();
    }
    {
        SNOW_SHOT_CAPTURE_PERF_SCOPE("show_prepared_child.posted_events");
        QCoreApplication::sendPostedEvents(widget, QEvent::UpdateRequest);
    }
}
} // namespace

ScreenshotOverlayUiHost::ScreenshotOverlayUiHost() = default;

ScreenshotOverlayUiHost::~ScreenshotOverlayUiHost() {
    destroyUiResources();
}

void ScreenshotOverlayUiHost::setToolbarCommandSinks(
    ScreenshotToolbarCommandSink& toolbarCommands,
    ScreenshotSelectionToolbarCommandSink& selectionToolbarCommands) {
    m_toolbarCommands = &toolbarCommands;
    m_selectionToolbarCommands = &selectionToolbarCommands;

    if (m_selectionToolbar == nullptr) {
        auto* selectionToolbar = new ScreenshotSelectionToolbarWidget(selectionToolbarCommands);
        m_ownedWidgets.add(selectionToolbar);
        m_selectionToolbar = selectionToolbar;
        selectionToolbar->hide();
        selectionToolbar->prewarm();
    }

    if (m_shortcutHints == nullptr) {
        auto* hints = new ScreenshotShortcutHintsWidget();
        m_ownedWidgets.add(hints);
        m_shortcutHints = hints;
    }
}

ScreenshotToolbarWindow* ScreenshotOverlayUiHost::ensureToolbar() {
    if (m_toolbar == nullptr) {
        if (m_toolbarCommands == nullptr) {
            return nullptr;
        }
        auto* toolbar = new ScreenshotToolbarWindow(*m_toolbarCommands);
        m_ownedWidgets.add(toolbar);
        m_toolbar = toolbar;
    }
    m_toolbar->restoreNativeSurface();
    return trackedWidget(m_toolbar);
}

ScreenshotToolbarWindow* ScreenshotOverlayUiHost::toolbar() const {
    return m_toolbar.data();
}

void ScreenshotOverlayUiHost::attachToolbarToOverlay(ScreenshotOverlayWindow* overlay) {
    ScreenshotToolbarWindow* toolbarWindow = ensureToolbar();
    if (toolbarWindow == nullptr) {
        return;
    }

    if (m_toolbarStyleConnection) {
        if (m_toolbarStyleCanvas != nullptr) {
            m_toolbarStyleCanvas->endTextStylePopupInteraction(toolbarWindow);
        }
        QObject::disconnect(m_toolbarStyleConnection);
        m_toolbarStyleConnection = {};
    }
    if (m_toolbarHistoryConnection) {
        QObject::disconnect(m_toolbarHistoryConnection);
        m_toolbarHistoryConnection = {};
    }
    if (m_toolbarStylePopupBeginConnection) {
        QObject::disconnect(m_toolbarStylePopupBeginConnection);
        m_toolbarStylePopupBeginConnection = {};
    }
    if (m_toolbarStylePopupEndConnection) {
        QObject::disconnect(m_toolbarStylePopupEndConnection);
        m_toolbarStylePopupEndConnection = {};
    }
    m_toolbarStyleCanvas = nullptr;
    toolbarWindow->setOwnerWindow(overlay);

    if (overlay == nullptr || overlay->canvas() == nullptr) {
        return;
    }

    SnowCanvasWidget* canvas = overlay->canvas();
    m_toolbarStyleCanvas = canvas;
    toolbarWindow->setStyleToolbarState(canvas->canvasStyleToolbarState());
    toolbarWindow->setHistoryState(canvas->canvasHistoryState());
    toolbarWindow->setWatermarkConfig(canvas->canvasWatermarkConfig());
    toolbarWindow->setSpotlightConfig(canvas->canvasSpotlightConfig());
    m_toolbarStyleConnection =
        QObject::connect(canvas, &SnowCanvasWidget::styleToolbarStateChanged, toolbarWindow,
                         [toolbarWindow, canvas]() {
                             toolbarWindow->setStyleToolbarState(canvas->canvasStyleToolbarState());
                             toolbarWindow->setWatermarkConfig(canvas->canvasWatermarkConfig());
                             toolbarWindow->setSpotlightConfig(canvas->canvasSpotlightConfig());
                         });
    m_toolbarHistoryConnection = QObject::connect(
        canvas, &SnowCanvasWidget::historyStateChanged, toolbarWindow, [toolbarWindow, canvas]() {
            toolbarWindow->setHistoryState(canvas->canvasHistoryState());
        });
    if (ScreenshotToolPalette* palette = toolbarWindow->palette()) {
        m_toolbarStylePopupBeginConnection = QObject::connect(
            palette, &ScreenshotToolPalette::textStylePopupInteractionBegan, toolbarWindow,
            [canvas]() { canvas->beginTextStylePopupInteraction(); });
        m_toolbarStylePopupEndConnection = QObject::connect(
            palette, &ScreenshotToolPalette::textStylePopupInteractionEnded, toolbarWindow,
            [canvas, toolbarWindow]() { canvas->endTextStylePopupInteraction(toolbarWindow); });
    }
}

void ScreenshotOverlayUiHost::undoCanvasEdit() {
    if (m_toolbarStyleCanvas != nullptr) {
        static_cast<void>(m_toolbarStyleCanvas->undo());
    }
}

void ScreenshotOverlayUiHost::redoCanvasEdit() {
    if (m_toolbarStyleCanvas != nullptr) {
        static_cast<void>(m_toolbarStyleCanvas->redo());
    }
}

ScreenshotSelectionToolbarWidget* ScreenshotOverlayUiHost::selectionToolbar() const {
    return m_selectionToolbar.data();
}

void ScreenshotOverlayUiHost::attachSelectionToolbarToOverlay(ScreenshotOverlayWindow* overlay) {
    ScreenshotSelectionToolbarWidget* toolbarWidget = trackedWidget(m_selectionToolbar);
    if (toolbarWidget == nullptr) {
        return;
    }
    if (toolbarWidget->parentWidget() == overlay) {
        return;
    }

    const bool wasVisible = toolbarWidget->isVisible();
    toolbarWidget->hide();
    toolbarWidget->setParent(overlay, Qt::Widget);
    toolbarWidget->setAttribute(Qt::WA_TranslucentBackground, true);
    toolbarWidget->setAttribute(Qt::WA_NoSystemBackground, true);
    toolbarWidget->setFocusPolicy(Qt::NoFocus);
    if (wasVisible && overlay != nullptr && overlay->isVisible()) {
        showPreparedChildWidget(toolbarWidget);
        toolbarWidget->raise();
    }
}

ScreenshotColorPickerWidget* ScreenshotOverlayUiHost::ensureColorPicker() {
    if (m_colorPicker == nullptr) {
        auto* colorPicker = new ScreenshotColorPickerWidget();
        m_ownedWidgets.add(colorPicker);
        m_colorPicker = colorPicker;
        colorPicker->setCenterGuideLineColor(m_colorPickerCenterGuideLineColor);
        colorPicker->hide();
    }
    return trackedWidget(m_colorPicker);
}

ScreenshotColorPickerWidget* ScreenshotOverlayUiHost::colorPicker() const {
    return m_colorPicker.data();
}

void ScreenshotOverlayUiHost::updateColorPicker(ScreenshotOverlayWindow* overlay,
                                                const QImage& image, const QRect& physicalRect,
                                                const QPoint& physicalPoint,
                                                const QPointF& localPosition, qreal opacity) {
    if (overlay == nullptr) {
        hideColorPicker();
        return;
    }

    ScreenshotColorPickerWidget* picker = ensureColorPicker();
    SnowCanvasWidget* canvas = overlay->canvas();
    QWidget* pickerParent =
        canvas != nullptr ? static_cast<QWidget*>(canvas) : static_cast<QWidget*>(overlay);
    if (picker->parentWidget() != pickerParent) {
        picker->hidePicker();
        picker->setParent(pickerParent);
        picker->setAttribute(Qt::WA_TranslucentBackground, true);
        picker->setAttribute(Qt::WA_NoSystemBackground, true);
        picker->setAttribute(Qt::WA_TransparentForMouseEvents, true);
        picker->setFocusPolicy(Qt::NoFocus);
    }

    picker->setCaptureImage(image, physicalRect);
    picker->updatePicker(physicalPoint, localPosition, opacity);
}

void ScreenshotOverlayUiHost::hideColorPicker() {
    if (m_colorPicker != nullptr) {
        m_colorPicker->hidePicker();
    }
}

void ScreenshotOverlayUiHost::setColorPickerCenterGuideLineColor(const QColor& color) {
    m_colorPickerCenterGuideLineColor = color.isValid() ? color : QColor(0, 0, 0, 0);
    if (m_colorPicker != nullptr) {
        m_colorPicker->setCenterGuideLineColor(m_colorPickerCenterGuideLineColor);
    }
}

void ScreenshotOverlayUiHost::resetColorPickerForNewCapture() {
    if (m_colorPicker != nullptr) {
        m_colorPicker->resetForNewCapture();
    }
}

void ScreenshotOverlayUiHost::hideColorPickerForOverlay(ScreenshotOverlayWindow* overlay) const {
    if (colorPickerBelongsToOverlay(overlay)) {
        m_colorPicker->hidePicker();
    }
}

bool ScreenshotOverlayUiHost::colorPickerBelongsToOverlay(
    const ScreenshotOverlayWindow* overlay) const {
    if (m_colorPicker == nullptr || overlay == nullptr) {
        return false;
    }

    const QWidget* parent = m_colorPicker->parentWidget();
    return parent == overlay || parent == overlay->canvas();
}

bool ScreenshotOverlayUiHost::screenshotUiContainsGlobalCursor() const {
    const QPoint globalPosition = QCursor::pos();
    if (m_toolbar != nullptr && m_toolbar->isVisible() &&
        m_toolbar->containsInteractiveGlobalPoint(globalPosition)) {
        return true;
    }

    if (m_selectionToolbar != nullptr && m_selectionToolbar->isVisible() &&
        !m_selectionToolbar->testAttribute(Qt::WA_TransparentForMouseEvents) &&
        m_selectionToolbar->containsInteractiveGlobalPoint(globalPosition)) {
        return true;
    }

    return false;
}

void ScreenshotOverlayUiHost::updateShortcutHints(
    ScreenshotOverlayWindow* overlay, const ScreenshotShortcutHintContext& context, qreal opacity,
    const QRectF& selectionGlobal) {
    const ScreenshotShortcutHintMode mode = screenshotShortcutHintModeForContext(context);
    auto* hints = static_cast<ScreenshotShortcutHintsWidget*>(m_shortcutHints.data());
    if (overlay == nullptr || hints == nullptr || mode == ScreenshotShortcutHintMode::Hidden ||
        opacity <= 0.0) {
        hideShortcutHints();
        return;
    }

    if (hints->parentWidget() != overlay) {
        hints->hide();
        hints->setParent(overlay, Qt::Widget);
        hints->setAttribute(Qt::WA_TranslucentBackground, true);
        hints->setAttribute(Qt::WA_NoSystemBackground, true);
        hints->setAttribute(Qt::WA_TransparentForMouseEvents, true);
        hints->setFocusPolicy(Qt::NoFocus);
        hints->setWindowFlags(Qt::Widget);
    }
    hints->setPresentation(context, opacity);
    if (!hints->hasVisiblePresentation()) {
        hints->hide();
        return;
    }

    const int y = std::max(kShortcutHintsMargin,
                           overlay->height() - hints->height() - kShortcutHintsMargin);
    hints->move(kShortcutHintsMargin, y);
    hints->setObscuringSelection(selectionGlobal);
    hints->refreshVisibility(QCursor::pos());
}

void ScreenshotOverlayUiHost::hideShortcutHints() {
    auto* hints = static_cast<ScreenshotShortcutHintsWidget*>(m_shortcutHints.data());
    if (hints != nullptr) {
        hints->setPresentation(ScreenshotShortcutHintMode::Hidden, 0.0);
    }
}

bool ScreenshotOverlayUiHost::stepToolbarStrokeWidth(int direction) {
    return m_toolbar != nullptr && m_toolbar->stepStrokeWidth(direction);
}

bool ScreenshotOverlayUiHost::stepToolbarSelectionOpacity(int direction) {
    return m_toolbar != nullptr && m_toolbar->stepSelectionOpacity(direction);
}

bool ScreenshotOverlayUiHost::stepToolbarSpotlightOpacity(int direction) {
    return m_toolbar != nullptr && m_toolbar->stepSpotlightOpacity(direction);
}

bool ScreenshotOverlayUiHost::stepToolbarFilterIntensity(int direction) {
    return m_toolbar != nullptr && m_toolbar->stepFilterIntensity(direction);
}

bool ScreenshotOverlayUiHost::stepToolbarPenFilterStrokeWidth(int direction) {
    return m_toolbar != nullptr && m_toolbar->stepPenFilterStrokeWidth(direction);
}

bool ScreenshotOverlayUiHost::stepToolbarWatermarkFontSize(int direction) {
    return m_toolbar != nullptr && m_toolbar->stepWatermarkFontSize(direction);
}

void ScreenshotOverlayUiHost::resetToolbarForNewCapture() {
    if (m_toolbar != nullptr) {
        const bool wasVisible = m_toolbar->isVisible();
        m_toolbar->resetForNewCapture();
        if (wasVisible) {
            // Refresh the translucent native surface before it is hidden. Otherwise
            // Windows can briefly reuse the previous frame when the toolbar is shown
            // for the next capture.
            m_toolbar->repaint();
        }
    }
    if (m_selectionToolbar != nullptr) {
        const bool wasVisible = m_selectionToolbar->isVisible();
        m_selectionToolbar->resetForNewCapture();
        if (wasVisible) {
            // The selection toolbar is normally a child of a layered overlay.
            // Paint its reset state while that surface is still composited so
            // a later show cannot reuse the previous selection frame.
            m_selectionToolbar->repaint();
        }
    }
}

void ScreenshotOverlayUiHost::hideToolbar() {
    if (m_toolbar != nullptr) {
        m_toolbar->hide();
    }
    hideSelectionToolbar();
}

void ScreenshotOverlayUiHost::releaseToolbarNativeSurface() {
    if (m_toolbar != nullptr) {
        m_toolbar->releaseNativeSurface();
    }
}

void ScreenshotOverlayUiHost::showToolbar() {
    ScreenshotToolbarWindow* toolbarWindow = ensureToolbar();
    if (toolbarWindow == nullptr) {
        return;
    }
    toolbarWindow->prepareForDisplay();
    showPreparedWidget(toolbarWindow);
    toolbarWindow->raise();
}

void ScreenshotOverlayUiHost::hideSelectionToolbar() {
    if (m_selectionToolbar != nullptr) {
        m_selectionToolbar->hide();
        // Keep the pooled widget in its canonical idle state so every subsequent
        // attach/show cycle starts from the known Full display mode and input region.
        m_selectionToolbar->resetForNewCapture();
    }
}

void ScreenshotOverlayUiHost::showSelectionToolbar() {
    ScreenshotSelectionToolbarWidget* toolbarWidget = trackedWidget(m_selectionToolbar);
    if (toolbarWidget == nullptr || toolbarWidget->parentWidget() == nullptr) {
        return;
    }
    toolbarWidget->prepareForDisplay();
    showPreparedChildWidget(toolbarWidget);
    toolbarWidget->raise();
}

void ScreenshotOverlayUiHost::raiseSelectionToolbar() {
    if (m_selectionToolbar != nullptr) {
        m_selectionToolbar->raise();
    }
}

void ScreenshotOverlayUiHost::detachOverlayTransientUi(ScreenshotOverlayWindow* overlay) {
    if (overlay == nullptr) {
        return;
    }

    if (m_selectionToolbar != nullptr && m_selectionToolbar->parentWidget() == overlay) {
        m_selectionToolbar->hide();
        m_selectionToolbar->setParent(nullptr);
    }

    if (m_toolbarStyleCanvas == overlay->canvas() && m_toolbarStyleConnection) {
        m_toolbarStyleCanvas->endTextStylePopupInteraction(m_toolbar.data());
        QObject::disconnect(m_toolbarStyleConnection);
        m_toolbarStyleConnection = {};
        if (m_toolbarHistoryConnection) {
            QObject::disconnect(m_toolbarHistoryConnection);
            m_toolbarHistoryConnection = {};
        }
        if (m_toolbarStylePopupBeginConnection) {
            QObject::disconnect(m_toolbarStylePopupBeginConnection);
            m_toolbarStylePopupBeginConnection = {};
        }
        if (m_toolbarStylePopupEndConnection) {
            QObject::disconnect(m_toolbarStylePopupEndConnection);
            m_toolbarStylePopupEndConnection = {};
        }
        m_toolbarStyleCanvas = nullptr;
    }
    if (m_toolbar != nullptr && m_toolbar->parentWidget() == overlay) {
        m_toolbar->hide();
        m_toolbar->setOwnerWindow(nullptr);
    }
    if (colorPickerBelongsToOverlay(overlay)) {
        m_colorPicker->hidePicker();
        m_colorPicker->setParent(nullptr);
    }
    if (m_shortcutHints != nullptr && m_shortcutHints->parentWidget() == overlay) {
        hideShortcutHints();
        m_shortcutHints->setParent(nullptr);
    }
}

void ScreenshotOverlayUiHost::destroyUiResources() {
    if (m_toolbarStyleConnection) {
        if (m_toolbarStyleCanvas != nullptr) {
            m_toolbarStyleCanvas->endTextStylePopupInteraction(m_toolbar.data());
        }
        QObject::disconnect(m_toolbarStyleConnection);
        m_toolbarStyleConnection = {};
    }
    if (m_toolbarHistoryConnection) {
        QObject::disconnect(m_toolbarHistoryConnection);
        m_toolbarHistoryConnection = {};
    }
    if (m_toolbarStylePopupBeginConnection) {
        QObject::disconnect(m_toolbarStylePopupBeginConnection);
        m_toolbarStylePopupBeginConnection = {};
    }
    if (m_toolbarStylePopupEndConnection) {
        QObject::disconnect(m_toolbarStylePopupEndConnection);
        m_toolbarStylePopupEndConnection = {};
    }
    m_toolbarStyleCanvas = nullptr;

    if (m_toolbar != nullptr) {
        m_toolbar->hide();
    }

    if (m_selectionToolbar != nullptr) {
        m_selectionToolbar->hide();
    }

    if (m_colorPicker != nullptr) {
        m_colorPicker->hidePicker();
    }
    if (m_shortcutHints != nullptr) {
        m_shortcutHints->hide();
    }

    m_ownedWidgets.clear();
    m_toolbar = nullptr;
    m_selectionToolbar = nullptr;
    m_colorPicker = nullptr;
    m_shortcutHints = nullptr;
}
