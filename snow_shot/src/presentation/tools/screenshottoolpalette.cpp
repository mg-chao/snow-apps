#include "snow_shot/presentation/screenshottoolpalette.h"

#include "screenshottoolbarperfinstrumentation.h"

#include "snow_shot/presentation/screenshottoolbarmainpanel.h"
#include "screenshottoolpalettebuttons.h"
#include "screenshottoolpalettestylecontrols.h"
#include "snow_shot/presentation/components/icons/iconrenderutils.h"
#include "snow_shot/presentation/components/icons/snowshoticons.h"
#include "snow_shot/presentation/screenshottoolbarlayoutmodel.h"
#include "snow_shot/presentation/styles/themecolorscheme.h"
#include "snow_shot/presentation/styles/thememanager.h"
#include "snow_shot/storage/settingsadapters.h"

#include "antd_icons.h"
#include "widgets/button.h"
#include "widgets/control_scale.h"
#include "widgets/radio.h"
#include "widgets/radio_button_group.h"
#include "widgets/select.h"
#include "widgets/slider.h"
#include "widgets/popover.h"

#include <QColor>
#include <QEvent>
#include <QFrame>
#include <QGraphicsDropShadowEffect>
#include <QBoxLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QLayout>
#include <QPaintEvent>
#include <QPainter>
#include <QOperatingSystemVersion>
#include <QSizePolicy>
#include <QSignalBlocker>
#include <QSpacerItem>
#include <QStandardItem>
#include <QStandardItemModel>
#include <QSet>
#include <QStringList>
#include <QVBoxLayout>
#include <QWheelEvent>
#include <QtGlobal>

#include <algorithm>
#include <cmath>
#include <numeric>
#include <utility>

namespace {
[[maybe_unused]] constexpr const char* kScreenshotToolPaletteTranslations[] = {
    QT_TRANSLATE_NOOP("ScreenshotToolPalette", "Pen filter"),
    QT_TRANSLATE_NOOP("ScreenshotToolPalette", "Rectangle filter"),
    QT_TRANSLATE_NOOP("ScreenshotToolPalette", "Filter type"),
    QT_TRANSLATE_NOOP("ScreenshotToolPalette", "Mosaic"),
    QT_TRANSLATE_NOOP("ScreenshotToolPalette", "Gaussian blur"),
    QT_TRANSLATE_NOOP("ScreenshotToolPalette", "Grayscale"),
    QT_TRANSLATE_NOOP("ScreenshotToolPalette", "Inversion"),
    QT_TRANSLATE_NOOP("ScreenshotToolPalette", "Filter intensity"),
    QT_TRANSLATE_NOOP("ScreenshotToolPalette", "Adjust filter intensity"),
    QT_TRANSLATE_NOOP("ScreenshotToolPalette", "Opacity"),
    QT_TRANSLATE_NOOP("ScreenshotToolPalette", "Adjust opacity"),
    QT_TRANSLATE_NOOP("ScreenshotToolPalette", "Rectangle highlight"),
    QT_TRANSLATE_NOOP("ScreenshotToolPalette", "Vertical scrolling"),
    QT_TRANSLATE_NOOP("ScreenshotToolPalette", "Horizontal scrolling"),
};

namespace outlined_icons = adqt::icons::antd::outlined;
namespace custom_outlined_icons = snow_shot::presentation::icons::custom::outlined;
namespace toolbar_layout = snow_shot::presentation::toolbar_layout;

constexpr int TOOLBAR_ITEM_SPACING = 8;
[[maybe_unused]] constexpr const char* TOOLTIP_TRANSLATIONS[] = {
    QT_TRANSLATE_NOOP("ScreenshotToolPalette", "Edit selection"),
    QT_TRANSLATE_NOOP("ScreenshotToolPalette", "Select elements"),
    QT_TRANSLATE_NOOP("ScreenshotToolPalette", "Shape"),
    QT_TRANSLATE_NOOP("ScreenshotToolPalette", "Arrow"),
    QT_TRANSLATE_NOOP("ScreenshotToolPalette", "Line"),
    QT_TRANSLATE_NOOP("ScreenshotToolPalette", "Pen"),
    QT_TRANSLATE_NOOP("ScreenshotToolPalette", "Highlight"),
    QT_TRANSLATE_NOOP("ScreenshotToolPalette", "Pen highlight"),
    QT_TRANSLATE_NOOP("ScreenshotToolPalette", "Spotlight"),
    QT_TRANSLATE_NOOP("ScreenshotToolPalette", "Text"),
    QT_TRANSLATE_NOOP("ScreenshotToolPalette", "Serial number"),
    QT_TRANSLATE_NOOP("ScreenshotToolPalette", "Filter"),
    QT_TRANSLATE_NOOP("ScreenshotToolPalette", "Eraser"),
    QT_TRANSLATE_NOOP("ScreenshotToolPalette", "Watermark"),
    QT_TRANSLATE_NOOP("ScreenshotToolPalette", "Undo"),
    QT_TRANSLATE_NOOP("ScreenshotToolPalette", "Redo"),
    QT_TRANSLATE_NOOP("ScreenshotToolPalette", "Record screen"),
    QT_TRANSLATE_NOOP("ScreenshotToolPalette", "Pin to screen"),
    QT_TRANSLATE_NOOP("ScreenshotToolPalette", "Text recognition"),
    QT_TRANSLATE_NOOP("ScreenshotToolPalette", "Table recognition"),
    QT_TRANSLATE_NOOP("ScreenshotToolPalette", "Barcode recognition"),
    QT_TRANSLATE_NOOP("ScreenshotToolPalette", "Edit"),
    QT_TRANSLATE_NOOP("ScreenshotToolPalette", "Text translation"),
    QT_TRANSLATE_NOOP("ScreenshotToolPalette", "Translation settings"),
    QT_TRANSLATE_NOOP("ScreenshotToolPalette", "Merge cells"),
    QT_TRANSLATE_NOOP("ScreenshotToolPalette", "Split cells"),
    QT_TRANSLATE_NOOP("ScreenshotToolPalette", "Reset"),
    QT_TRANSLATE_NOOP("ScreenshotToolPalette", "Formatting"),
    QT_TRANSLATE_NOOP("ScreenshotToolPalette", "Keep line breaks"),
    QT_TRANSLATE_NOOP("ScreenshotToolPalette", "Remove line breaks"),
    QT_TRANSLATE_NOOP("ScreenshotToolPalette", "Punctuation"),
    QT_TRANSLATE_NOOP("ScreenshotToolPalette", "Half-width"),
    QT_TRANSLATE_NOOP("ScreenshotToolPalette", "Full-width"),
    QT_TRANSLATE_NOOP("ScreenshotToolPalette", "Scrolling screenshot"),
    QT_TRANSLATE_NOOP("ScreenshotToolPalette", "Save as file"),
    QT_TRANSLATE_NOOP("ScreenshotToolPalette", "Cancel screenshot"),
    QT_TRANSLATE_NOOP("ScreenshotToolPalette", "Copy to clipboard"),
    QT_TRANSLATE_NOOP("ScreenshotToolPalette", "Confirm edit"),
    QT_TRANSLATE_NOOP("ScreenshotToolPalette", "Start recording"),
    QT_TRANSLATE_NOOP("ScreenshotToolPalette", "Stop recording"),
    QT_TRANSLATE_NOOP("ScreenshotToolPalette", "Pause recording"),
    QT_TRANSLATE_NOOP("ScreenshotToolPalette", "Resume recording"),
    QT_TRANSLATE_NOOP("ScreenshotToolPalette", "Record microphone"),
    QT_TRANSLATE_NOOP("ScreenshotToolPalette", "Record speakers"),
    QT_TRANSLATE_NOOP("ScreenshotToolPalette", "Open recording folder"),
    QT_TRANSLATE_NOOP("ScreenshotToolPalette", "Close recording"),
    QT_TRANSLATE_NOOP("ScreenshotToolPalette", "Copy animated image"),
    QT_TRANSLATE_NOOP("ScreenshotToolPalette", "Copy video"),
};
constexpr int TOOLBAR_SEPARATOR_HEIGHT = 16;
constexpr int TOOLBAR_SEPARATOR_WIDTH = 1;
constexpr int TOOLBAR_SEPARATOR_SIDE_SPACING = 12;
constexpr int RECORDING_DURATION_WIDTH = 54;
constexpr int RECORDING_DURATION_FONT_SIZE = 14;
constexpr int TOOLBAR_ROW_SPACING = 6;
constexpr int STYLE_BUTTON_SIZE = 28;
constexpr int STYLE_ICON_SIZE = 18;
constexpr int TOOLBAR_PANEL_HORIZONTAL_MARGIN = 12;
constexpr int TOOLBAR_PANEL_VERTICAL_MARGIN = 4;
constexpr int STYLE_PANEL_HORIZONTAL_MARGIN = 10;
constexpr int STYLE_PANEL_VERTICAL_MARGIN = 4;
constexpr int STYLE_ITEM_SPACING = 4;
constexpr int STYLE_GROUP_SPACING = 8;
constexpr int COMPACT_SLIDER_ICON_SIZE = 16;
constexpr int COMPACT_SLIDER_WIDTH = 96;
constexpr int TOOLBAR_PANEL_RADIUS = 8;
constexpr qreal TOOLBAR_SHADOW_BLUR_RADIUS = 18.0;
constexpr qreal TOOLBAR_SHADOW_OFFSET_X = 0.0;
constexpr qreal TOOLBAR_SHADOW_OFFSET_Y = 3.0;
constexpr QColor TOOLBAR_SHADOW_COLOR(0, 0, 0, 90);

QColor toolbarSurfaceColor(const snow_shot::presentation::styles::ThemeColorScheme& scheme) {
    return scheme.map.colorBgContainer.isValid() ? scheme.map.colorBgContainer : QColor(Qt::white);
}

QColor toolbarSeparatorColor(const snow_shot::presentation::styles::ThemeColorScheme& scheme) {
    return scheme.map.colorBorder.isValid() ? scheme.map.colorBorder : QColor(0xd9, 0xd9, 0xd9);
}

QString cssColor(const QColor& color) {
    if (color.alpha() == 255) {
        return color.name(QColor::HexRgb);
    }

    return QStringLiteral("rgba(%1, %2, %3, %4)")
        .arg(color.red())
        .arg(color.green())
        .arg(color.blue())
        .arg(color.alpha());
}

QString styleToolbarSeparatorStyleSheet() {
    return QStringLiteral("QFrame { background: %1; border: 0px; }")
        .arg(cssColor(
            toolbarSeparatorColor(snow_shot::presentation::styles::generateThemeColorScheme())));
}

void applyMainToolbarToolActiveStyle(adqt::widgets::AdButton* button, bool active) {
    if (button == nullptr) {
        return;
    }

    button->setButtonStyle(active ? adqt::widgets::AdButton::ButtonStyle::Solid
                                  : adqt::widgets::AdButton::ButtonStyle::Text);
    button->setAccentRole(active ? adqt::widgets::AdButton::AccentRole::Primary
                                 : adqt::widgets::AdButton::AccentRole::Neutral);
}

const QVector<QColor>& spotlightColorValues() {
    static const QVector<QColor> values{
        QColor(QStringLiteral("#f5222d")), QColor(QStringLiteral("#52c41a")),
        QColor(QStringLiteral("#1677ff")), QColor(QStringLiteral("#fadb14")),
        QColor(QStringLiteral("#000000")),
    };
    return values;
}

class ScreenshotToolbarPanel final : public QFrame {
  public:
    explicit ScreenshotToolbarPanel(QWidget* parent)
        : QFrame(parent), m_backgroundColor(toolbarSurfaceColor(
                              snow_shot::presentation::styles::generateThemeColorScheme())) {
        const auto& themeManager = snow_shot::presentation::styles::ThemeManager::instance();
        connect(&themeManager, &snow_shot::presentation::styles::ThemeManager::themeChanged, this,
                [this](const snow_shot::presentation::styles::ThemeColorScheme& scheme) {
                    const QColor background = toolbarSurfaceColor(scheme);
                    if (m_backgroundColor == background) {
                        return;
                    }
                    m_backgroundColor = background;
                    update();
                });
    }

    void setPanelRadius(qreal radius) {
        radius = qMax<qreal>(0.0, radius);
        if (qFuzzyCompare(m_radius + 1.0, radius + 1.0)) {
            return;
        }
        m_radius = radius;
        update();
    }

  protected:
    void paintEvent(QPaintEvent* event) override {
        Q_UNUSED(event);
        QPainter painter(this);
        painter.setRenderHint(QPainter::Antialiasing, true);
        painter.setPen(Qt::NoPen);
        painter.setBrush(m_backgroundColor);
        painter.drawRoundedRect(rect(), m_radius, m_radius);
    }

  private:
    qreal m_radius = TOOLBAR_PANEL_RADIUS;
    QColor m_backgroundColor;
};

adqt::icons::IconRef primaryIcon(const adqt::icons::IconRef& iconRef) {
    const auto scheme = snow_shot::presentation::styles::generateThemeColorScheme();
    return snow_shot::presentation::icons::withPrimaryColor(iconRef, scheme.map.colorPrimary);
}

QFrame* createPanel(QWidget* parent, const QString& objectName) {
    auto* panel = new ScreenshotToolbarPanel(parent);
    panel->setObjectName(objectName);
    panel->setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Fixed);

    auto* shadow = new QGraphicsDropShadowEffect(panel);
    shadow->setBlurRadius(TOOLBAR_SHADOW_BLUR_RADIUS);
    shadow->setOffset(TOOLBAR_SHADOW_OFFSET_X, TOOLBAR_SHADOW_OFFSET_Y);
    shadow->setColor(TOOLBAR_SHADOW_COLOR);
    panel->setGraphicsEffect(shadow);
    return panel;
}

int wheelVerticalDelta(const QWheelEvent* event) {
    if (event == nullptr) {
        return 0;
    }

    const QPoint pixelDelta = event->pixelDelta();
    if (!pixelDelta.isNull()) {
        return pixelDelta.y();
    }
    return event->angleDelta().y();
}

ScreenshotToolPaletteButtonMetrics styleButtonMetrics(qreal physicalScale) {
    return ScreenshotToolPaletteButtonMetrics{
        STYLE_BUTTON_SIZE,
        STYLE_ICON_SIZE,
        physicalScale,
    };
}

ScreenshotToolPaletteButtonMetrics actionButtonMetrics(qreal physicalScale) {
    return ScreenshotToolPaletteButtonMetrics{32, 24, physicalScale};
}

bool hasSelectedCanvasElements(const SnowCanvasStyleToolbarState& state) {
    return state.source == SnowCanvasStyleToolbarSource::SelectedRectangle ||
           state.source == SnowCanvasStyleToolbarSource::SelectedArrow ||
           state.source == SnowCanvasStyleToolbarSource::SelectedLine ||
           state.source == SnowCanvasStyleToolbarSource::SelectedFreeDraw ||
           state.source == SnowCanvasStyleToolbarSource::SelectedRectangleHighlight ||
           state.source == SnowCanvasStyleToolbarSource::SelectedPenHighlight ||
           state.source == SnowCanvasStyleToolbarSource::SelectedSpotlight ||
           state.source == SnowCanvasStyleToolbarSource::SelectedRectangleFilter ||
           state.source == SnowCanvasStyleToolbarSource::SelectedPenFilter ||
           state.source == SnowCanvasStyleToolbarSource::SelectedText ||
           state.source == SnowCanvasStyleToolbarSource::SelectedSerialNumber;
}

bool filterTypeSupportsIntensity(SnowCanvasFilterType type) {
    return type != SnowCanvasFilterType::Grayscale && type != SnowCanvasFilterType::Inversion;
}

bool toolUsesActionToolbar(ScreenshotToolPalette::Tool tool) {
    return tool == ScreenshotToolPalette::Tool::Select ||
           tool == ScreenshotToolPalette::Tool::Ocr ||
           tool == ScreenshotToolPalette::Tool::TextTranslation ||
           tool == ScreenshotToolPalette::Tool::Table ||
           tool == ScreenshotToolPalette::Tool::ScrollingScreenshot;
}

std::optional<ScreenshotToolPalette::ActionFamily> actionFamilyForTool(
    ScreenshotToolPalette::Tool tool) {
    switch (tool) {
    case ScreenshotToolPalette::Tool::Select:
        return ScreenshotToolPalette::ActionFamily::Selection;
    case ScreenshotToolPalette::Tool::Ocr:
    case ScreenshotToolPalette::Tool::TextTranslation:
        return ScreenshotToolPalette::ActionFamily::TextRecognition;
    case ScreenshotToolPalette::Tool::Table:
    case ScreenshotToolPalette::Tool::Qr:
        return ScreenshotToolPalette::ActionFamily::TableRecognition;
    case ScreenshotToolPalette::Tool::ScrollingScreenshot:
        return ScreenshotToolPalette::ActionFamily::ScrollingRecognition;
    default:
        return std::nullopt;
    }
}

bool toolUsesStyleToolbar(ScreenshotToolPalette::Tool tool) {
    switch (tool) {
    case ScreenshotToolPalette::Tool::Shape:
    case ScreenshotToolPalette::Tool::Arrow:
    case ScreenshotToolPalette::Tool::Line:
    case ScreenshotToolPalette::Tool::FreeDraw:
    case ScreenshotToolPalette::Tool::RectangleHighlight:
    case ScreenshotToolPalette::Tool::PenHighlight:
    case ScreenshotToolPalette::Tool::Spotlight:
    case ScreenshotToolPalette::Tool::RectangleFilter:
    case ScreenshotToolPalette::Tool::PenFilter:
    case ScreenshotToolPalette::Tool::Watermark:
    case ScreenshotToolPalette::Tool::Text:
    case ScreenshotToolPalette::Tool::SerialNumber:
        return true;
    case ScreenshotToolPalette::Tool::Move:
    case ScreenshotToolPalette::Tool::Select:
    case ScreenshotToolPalette::Tool::Eraser:
    case ScreenshotToolPalette::Tool::Ocr:
    case ScreenshotToolPalette::Tool::TextTranslation:
    case ScreenshotToolPalette::Tool::Table:
    case ScreenshotToolPalette::Tool::Qr:
    case ScreenshotToolPalette::Tool::ScrollingScreenshot:
        return false;
    }
    return false;
}

std::optional<ScreenshotToolPalette::Tool> styleFamilyForTool(
    ScreenshotToolPalette::Tool tool) {
    if (!toolUsesStyleToolbar(tool)) {
        return std::nullopt;
    }
    if (tool == ScreenshotToolPalette::Tool::Shape ||
        tool == ScreenshotToolPalette::Tool::Line ||
        tool == ScreenshotToolPalette::Tool::FreeDraw) {
        return ScreenshotToolPalette::Tool::Shape;
    }
    if (tool == ScreenshotToolPalette::Tool::RectangleHighlight ||
        tool == ScreenshotToolPalette::Tool::PenHighlight) {
        return ScreenshotToolPalette::Tool::RectangleHighlight;
    }
    if (tool == ScreenshotToolPalette::Tool::RectangleFilter ||
        tool == ScreenshotToolPalette::Tool::PenFilter) {
        return ScreenshotToolPalette::Tool::RectangleFilter;
    }
    return tool;
}

namespace toolbar_settings = snow_shot::storage;

ScreenshotToolPalette::Tool tableQrToolFromSetting(const QString& value) {
    return value == QStringLiteral("qr") ? ScreenshotToolPalette::Tool::Qr
                                         : ScreenshotToolPalette::Tool::Table;
}

QString tableQrToolSetting(ScreenshotToolPalette::Tool tool) {
    return tool == ScreenshotToolPalette::Tool::Qr ? QStringLiteral("qr") : QStringLiteral("table");
}

ScreenshotToolPalette::Tool drawingToolFromItem(toolbar_layout::Item item) {
    switch (item) {
    case toolbar_layout::Item::Shape:
        return ScreenshotToolPalette::Tool::Shape;
    case toolbar_layout::Item::Arrow:
        return ScreenshotToolPalette::Tool::Arrow;
    case toolbar_layout::Item::Line:
        return ScreenshotToolPalette::Tool::Line;
    case toolbar_layout::Item::FreeDraw:
        return ScreenshotToolPalette::Tool::FreeDraw;
    case toolbar_layout::Item::Highlighter:
        return ScreenshotToolPalette::Tool::PenHighlight;
    case toolbar_layout::Item::Spotlight:
        return ScreenshotToolPalette::Tool::Spotlight;
    case toolbar_layout::Item::Text:
        return ScreenshotToolPalette::Tool::Text;
    case toolbar_layout::Item::SerialNumber:
        return ScreenshotToolPalette::Tool::SerialNumber;
    case toolbar_layout::Item::Filter:
        return ScreenshotToolPalette::Tool::PenFilter;
    case toolbar_layout::Item::Eraser:
        return ScreenshotToolPalette::Tool::Eraser;
    case toolbar_layout::Item::Watermark:
        return ScreenshotToolPalette::Tool::Watermark;
    }
    return ScreenshotToolPalette::Tool::Shape;
}

ScreenshotToolPalette::Tool toolbarFacingDrawingTool(ScreenshotToolPalette::Tool tool) {
    return tool == ScreenshotToolPalette::Tool::RectangleHighlight
               ? ScreenshotToolPalette::Tool::PenHighlight
               : tool;
}

QString drawingToolItemId(ScreenshotToolPalette::Tool tool) {
    switch (toolbarFacingDrawingTool(tool)) {
    case ScreenshotToolPalette::Tool::Shape:
        return QStringLiteral("shape");
    case ScreenshotToolPalette::Tool::Arrow:
        return QStringLiteral("arrow");
    case ScreenshotToolPalette::Tool::Line:
        return QStringLiteral("line");
    case ScreenshotToolPalette::Tool::FreeDraw:
        return QStringLiteral("free-draw");
    case ScreenshotToolPalette::Tool::PenHighlight:
        return QStringLiteral("highlighter");
    case ScreenshotToolPalette::Tool::Spotlight:
        return QStringLiteral("spotlight");
    case ScreenshotToolPalette::Tool::Text:
        return QStringLiteral("text");
    case ScreenshotToolPalette::Tool::SerialNumber:
        return QStringLiteral("serial-number");
    case ScreenshotToolPalette::Tool::RectangleFilter:
    case ScreenshotToolPalette::Tool::PenFilter:
        return QStringLiteral("filter");
    case ScreenshotToolPalette::Tool::Eraser:
        return QStringLiteral("eraser");
    case ScreenshotToolPalette::Tool::Watermark:
        return QStringLiteral("watermark");
    default:
        return {};
    }
}

QString drawingShortcutToolIdForItemId(const QString& itemId) {
    if (itemId == QStringLiteral("select")) {
        return QStringLiteral("select");
    }
    if (itemId == QStringLiteral("shape")) {
        return QStringLiteral("shape");
    }
    if (itemId == QStringLiteral("arrow")) {
        return QStringLiteral("arrow");
    }
    if (itemId == QStringLiteral("free-draw")) {
        return QStringLiteral("brush");
    }
    if (itemId == QStringLiteral("highlighter")) {
        return QStringLiteral("highlight");
    }
    if (itemId == QStringLiteral("text")) {
        return QStringLiteral("text");
    }
    if (itemId == QStringLiteral("serial-number")) {
        return QStringLiteral("serial_number");
    }
    if (itemId == QStringLiteral("filter")) {
        return QStringLiteral("filter");
    }
    if (itemId == QStringLiteral("eraser")) {
        return QStringLiteral("eraser");
    }
    if (itemId == QStringLiteral("watermark")) {
        return QStringLiteral("watermark");
    }
    return {};
}

QString drawingShortcutToolIdForTooltipSource(const QString& source) {
    if (source == QStringLiteral("Select elements")) {
        return QStringLiteral("select");
    }
    if (source == QStringLiteral("Shape")) {
        return QStringLiteral("shape");
    }
    if (source == QStringLiteral("Arrow")) {
        return QStringLiteral("arrow");
    }
    if (source == QStringLiteral("Pen")) {
        return QStringLiteral("brush");
    }
    if (source == QStringLiteral("Highlight") ||
        source == QStringLiteral("Pen highlight")) {
        return QStringLiteral("highlight");
    }
    if (source == QStringLiteral("Text")) {
        return QStringLiteral("text");
    }
    if (source == QStringLiteral("Serial number")) {
        return QStringLiteral("serial_number");
    }
    if (source == QStringLiteral("Filter")) {
        return QStringLiteral("filter");
    }
    if (source == QStringLiteral("Eraser")) {
        return QStringLiteral("eraser");
    }
    if (source == QStringLiteral("Watermark")) {
        return QStringLiteral("watermark");
    }
    return {};
}

QString formatDrawingShortcutForTooltip(QString shortcut) {
    shortcut = shortcut.trimmed();
    shortcut.replace(QStringLiteral("Period"), QStringLiteral("."));
    shortcut.replace(QStringLiteral("Comma"), QStringLiteral(","));
    if (QOperatingSystemVersion::currentType() == QOperatingSystemVersion::MacOS) {
        shortcut.replace(QStringLiteral("Meta"), QStringLiteral("Command"));
        shortcut.replace(QStringLiteral("Alt"), QStringLiteral("Option"));
        shortcut.replace(QStringLiteral("Ctrl"), QStringLiteral("Control"));
    } else {
        shortcut.replace(QStringLiteral("Meta"), QStringLiteral("Win"));
        shortcut.replace(QStringLiteral("Super"), QStringLiteral("Win"));
    }
    return shortcut;
}

void applyScreenshotShortcutTooltip(QWidget* widget, const QString& source,
                                    const QString& actionId) {
    if (widget == nullptr || source.isEmpty() || actionId.isEmpty()) {
        return;
    }

    widget->setProperty("snowShotScreenshotShortcutTooltipSource", source);
    widget->setProperty("snowShotScreenshotShortcutTooltipActionId", actionId);
    const QStringList shortcuts =
        snow_shot::storage::ScreenshotShortcutSettings().shortcuts(actionId);
    QStringList displayShortcuts;
    for (const QString& shortcut : shortcuts) {
        const QString displayShortcut = formatDrawingShortcutForTooltip(shortcut);
        if (!displayShortcut.isEmpty()) {
            displayShortcuts.push_back(displayShortcut);
        }
    }
    const QString title = ScreenshotToolPaletteTranslationText(source).translated();
    if (displayShortcuts.isEmpty()) {
        configureScreenshotToolPaletteTooltip(widget,
                                              ScreenshotToolPaletteTranslationText(source));
        return;
    }

    configureScreenshotToolPaletteTooltip(
        widget, ScreenshotToolPaletteTranslationText(QStringLiteral("%1 (%2)"))
                    .arg(title)
                    .arg(displayShortcuts.join(QStringLiteral(", "))));
    const QByteArray sourceUtf8 = source.toUtf8();
    setScreenshotToolPaletteAccessibleNameSource(widget, sourceUtf8.constData());
    widget->setAccessibleName(title);
}

void applyPinToScreenShortcutTooltip(QWidget* widget, const QString& source,
                                     const QString& actionId) {
    if (widget == nullptr || source.isEmpty() || actionId.isEmpty()) {
        return;
    }

    const QStringList shortcuts =
        snow_shot::storage::PinToScreenShortcutSettings().shortcuts(actionId);
    QStringList displayShortcuts;
    for (const QString& shortcut : shortcuts) {
        const QString displayShortcut = formatDrawingShortcutForTooltip(shortcut);
        if (!displayShortcut.isEmpty()) {
            displayShortcuts.push_back(displayShortcut);
        }
    }
    const QString title = ScreenshotToolPaletteTranslationText(source).translated();
    if (displayShortcuts.isEmpty()) {
        configureScreenshotToolPaletteTooltip(
            widget, ScreenshotToolPaletteTranslationText(source));
        return;
    }

    configureScreenshotToolPaletteTooltip(
        widget, ScreenshotToolPaletteTranslationText(QStringLiteral("%1 (%2)"))
                    .arg(title)
                    .arg(displayShortcuts.join(QStringLiteral(", "))));
    setScreenshotToolPaletteAccessibleNameSource(widget, source.toUtf8().constData());
    widget->setAccessibleName(title);
}

void applyDrawingShortcutTooltip(QWidget* widget, const QString& source,
                                 const QString& itemId = QString()) {
    if (widget == nullptr || source.isEmpty()) {
        return;
    }

    const QString toolId = itemId.isEmpty() ? drawingShortcutToolIdForTooltipSource(source)
                                            : drawingShortcutToolIdForItemId(itemId);
    if (toolId.isEmpty()) {
        return;
    }

    widget->setProperty("snowShotDrawingShortcutTooltipSource", source);
    const QStringList shortcuts = snow_shot::storage::DrawingShortcutSettings().shortcuts(toolId);
    QStringList displayShortcuts;
    for (const QString& shortcut : shortcuts) {
        const QString displayShortcut = formatDrawingShortcutForTooltip(shortcut);
        if (!displayShortcut.isEmpty()) {
            displayShortcuts.push_back(displayShortcut);
        }
    }
    if (displayShortcuts.isEmpty()) {
        return;
    }

    const QString title = ScreenshotToolPaletteTranslationText(source).translated();
    const QString tooltip = QStringLiteral("%1 (%2)")
                                .arg(title, displayShortcuts.join(QStringLiteral(", ")));
    widget->setToolTip(tooltip);
    widget->setAccessibleName(title);
}

std::optional<snow_shot::storage::ScreenshotToolbarLayout>
initialToolbarLayout(const ScreenshotToolPalette::Options& options) {
    if (options.toolbarLayout.has_value()) {
        return toolbar_layout::normalizedLayout(*options.toolbarLayout);
    }
    const bool hasDrawingTools =
        options.showShapeTool || options.showArrowTool || options.showLineTool ||
        options.showFreeDrawTool || options.showHighlightTool ||
        options.showRectangleHighlightTool || options.showPenHighlightTool ||
        options.showSpotlightTool || options.showTextTool || options.showSerialNumberTool ||
        options.showFilterTool || options.showEraserTool || options.showWatermarkTool;
    return hasDrawingTools ? std::optional(toolbar_layout::normalizedLayout(
                                 snow_shot::storage::ScreenshotToolbarLayout{}))
                           : std::nullopt;
}

} // namespace

ScreenshotToolPalette::ScreenshotToolPalette(const Options& options, QWidget* parent)
    : QWidget(parent), m_styleDefaults(options.styleDefaults), m_options(options),
      m_toolbarLayout(initialToolbarLayout(options)) {
    const toolbar_settings::ScreenshotToolbarSettings settings;
    m_tableQrEntryTool = tableQrToolFromSetting(settings.tableQrTool());

    setAttribute(Qt::WA_TranslucentBackground, true);
    setAttribute(Qt::WA_NoSystemBackground, true);
    setAutoFillBackground(false);
    setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Fixed);

    auto* layout = new QVBoxLayout(this);
    m_rootLayout = layout;
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);

    m_styleControls = std::make_unique<ScreenshotToolPaletteStyleControls>(
        ScreenshotToolPaletteStyleControlCallbacks{
            [this](const SnowCanvasShapeStyle& style, quint32 properties,
                   SnowCanvasShapeKind kind) { emit shapeStyleChanged(style, properties, kind); },
            [this](const SnowCanvasTextStyle& style) { emit textStyleChanged(style); },
            [this]() { emit textStylePopupInteractionBegan(); },
            [this]() { emit textStylePopupInteractionEnded(); },
            [this](const SnowCanvasSerialNumberStyle& style) {
                emit serialNumberStyleChanged(style);
            },
            [this]() { emit serialNumberDecrementRequested(); },
            [this]() { emit serialNumberIncrementRequested(); },
            [this]() { emit serialNumberCreateTextRequested(); },
            [this](const SnowCanvasWatermarkConfig& config) {
                emit watermarkConfigChanged(config);
            },
            [this](const SnowCanvasWatermarkConfig& config) {
                emit watermarkPreviewChanged(config);
            },
            [this]() {
                updateToolbarGeometry();
                emit visibleContentChanged();
            },
            [this](adqt::widgets::AdColorPicker* picker) {
                emit canvasColorSamplingRequested(picker);
            },
        },
        m_styleDefaults);

    createMainToolbar(options);
    if (options.enableStyleToolbar) {
        createSecondaryToolbarShell();
    }
    updateToolbarGeometry();
    resetStyleState();
    installWheelFilters(this);

    const auto& themeManager = snow_shot::presentation::styles::ThemeManager::instance();
    connect(&themeManager, &snow_shot::presentation::styles::ThemeManager::themeChanged, this,
            [this](const snow_shot::presentation::styles::ThemeColorScheme&) {
                const QString separatorStyle = styleToolbarSeparatorStyleSheet();
                for (QFrame* separator : std::as_const(m_styleSeparatorFrames)) {
                    if (separator != nullptr) {
                        separator->setStyleSheet(separatorStyle);
                    }
                }
                refreshThemeDependentIcons();
                refreshFilterEditorMetrics(m_filterEditor);
                refreshFilterEditorMetrics(m_penFilterEditor);
                ScreenshotToolPaletteSliderEditor spotlightEditor;
                spotlightEditor.icon = m_spotlightOpacityIcon;
                spotlightEditor.slider = m_spotlightOpacitySlider;
                spotlightEditor.iconRef = custom_outlined_icons::Opacity();
                spotlightEditor.baseIconSize = COMPACT_SLIDER_ICON_SIZE;
                spotlightEditor.baseSliderWidth = COMPACT_SLIDER_WIDTH;
                configureScreenshotToolPaletteSliderEditor(spotlightEditor,
                                                           styleButtonMetrics(m_physicalScale));
            });
}

ScreenshotToolPalette::~ScreenshotToolPalette() {
    m_styleControls->clearTextStylePopupInteractions();
}

void ScreenshotToolPalette::hideEvent(QHideEvent* event) {
    m_styleControls->clearTextStylePopupInteractions();
    QWidget::hideEvent(event);
}

QWidget* ScreenshotToolPalette::mainPanel() const {
    return m_mainPanel;
}

QWidget* ScreenshotToolPalette::actionPanel() const {
    return m_selectActionPanel;
}

QWidget* ScreenshotToolPalette::stylePanel() const {
    return m_rectangleStylePanel;
}

QWidget* ScreenshotToolPalette::dragHandle() const {
    return m_mainPanel != nullptr ? m_mainPanel->dragHandle() : nullptr;
}

QWidget* ScreenshotToolPalette::trailingDragHandle() const {
    return m_mainPanel != nullptr ? m_mainPanel->trailingDragHandle() : nullptr;
}

QSize ScreenshotToolPalette::contentSizeHint() const {
    ensureLayoutApplied();
    const QSize contentSize = m_layoutResult.contentSize;
    if (!contentSize.isEmpty()) {
        return contentSize;
    }

    return sizeHint();
}

QRect ScreenshotToolPalette::occupiedContentRect() const {
    ensureLayoutApplied();
    return m_layoutResult.occupiedContentRect;
}

QRect ScreenshotToolPalette::visualContentRect() const {
    ensureLayoutApplied();
    return m_layoutResult.occupiedContentRect;
}

QRect ScreenshotToolPalette::fullContentRect() const {
    ensureLayoutApplied();
    return m_layoutResult.fullContentRect;
}

QRect ScreenshotToolPalette::bottomPlacementContentRect() const {
    return placementSnapshot().bottom.occupiedContentRect;
}

QRect ScreenshotToolPalette::topPlacementContentRect() const {
    return placementSnapshot().top.occupiedContentRect;
}

QRect ScreenshotToolPalette::topRightMainToolbarContentRect() const {
    return placementSnapshot().top.mainToolbarContentRect;
}

QRect ScreenshotToolPalette::mainToolbarContentRect() const {
    ensureLayoutApplied();
    return m_layoutResult.mainToolbarContentRect;
}

ScreenshotToolbarPlacementSnapshot ScreenshotToolPalette::placementSnapshot() const {
    ensureLayoutApplied();
    return buildPlacementSnapshot();
}

void ScreenshotToolPalette::prepareForDisplay() {
    ensureLayoutApplied();
}

bool ScreenshotToolPalette::setShadowMargins(const QMargins& margins) {
    if (m_baseShadowMargins == margins) {
        return false;
    }

    m_baseShadowMargins = margins;
    m_shadowMargins = QMargins(scaledMetric(margins.left()), scaledMetric(margins.top()),
                               scaledMetric(margins.right()), scaledMetric(margins.bottom()));
    markLayoutDirty();
    ensureLayoutApplied();
    return true;
}

bool ScreenshotToolPalette::setPhysicalScale(qreal scale) {
    if (!std::isfinite(scale) || scale <= 0.0) {
        scale = 1.0;
    }
    scale = std::clamp<qreal>(scale, 0.25, 4.0);
    if (qFuzzyCompare(m_physicalScale + 1.0, scale + 1.0)) {
        return false;
    }

    m_physicalScale = scale;
    ++m_metricProfileRevision;
    applyScaledToolbarMetrics();
    ensureLayoutApplied();
    return true;
}

qreal ScreenshotToolPalette::physicalScale() const {
    return m_physicalScale;
}

void ScreenshotToolPalette::setToolbarLayout(
    const snow_shot::storage::ScreenshotToolbarLayout& layout) {
    const snow_shot::storage::ScreenshotToolbarLayout normalized =
        toolbar_layout::normalizedLayout(layout);
    if (m_toolbarLayout.has_value() && *m_toolbarLayout == normalized) {
        return;
    }
    m_toolbarLayout = normalized;
    applyMainToolbarLayout(true);
}

void ScreenshotToolPalette::resetStyleState() {
    m_styleControls->reset();
    refreshFilterEditorState(m_filterEditor, false);
    refreshFilterEditorState(m_penFilterEditor, true);
    setSpotlightConfig(m_styleDefaults.spotlight);
    m_selectionOpacityAvailable = false;
    updateSelectionActionAvailability(false);
}

void ScreenshotToolPalette::setCreationStyleDefaults(const SnowCanvasStyleDefaults& defaults) {
    m_styleControls->setCreationStyleDefaults(defaults);
    refreshFilterEditorState(m_filterEditor, false);
    refreshFilterEditorState(m_penFilterEditor, true);
}

SnowCanvasStyleDefaults ScreenshotToolPalette::creationStyleDefaults() const {
    return m_styleControls != nullptr ? m_styleControls->creationStyleDefaults()
                                      : snow_shot::presentation::screenshotCanvasStyleDefaults();
}

bool ScreenshotToolPalette::stepStrokeWidth(int direction) {
    return m_styleControls->stepStrokeWidth(direction);
}

bool ScreenshotToolPalette::stepSelectionOpacity(int direction) {
    if (direction == 0 || m_activeTool != Tool::Select || !m_hasSelectedElements ||
        m_selectionOpacitySlider == nullptr) {
        return false;
    }

    const int current = qRound(m_selectionOpacitySlider->value());
    const int next = std::clamp<int>(current + (direction > 0 ? 5 : -5),
                                     qRound(m_selectionOpacitySlider->minimum()),
                                     qRound(m_selectionOpacitySlider->maximum()));
    const bool mixed = m_selectionOpacitySlider->property("mixed").toBool();
    if (next == current && !mixed) {
        return false;
    }

    setSelectionOpacity(next / 100.0);
    emit selectionOpacityChanged(m_selectionOpacity);
    return true;
}

bool ScreenshotToolPalette::stepSpotlightOpacity(int direction) {
    if (direction == 0 || m_activeTool != Tool::Spotlight || m_spotlightOpacitySlider == nullptr ||
        !m_spotlightOpacitySlider->isEnabled()) {
        return false;
    }

    const int current = qRound(m_spotlightOpacitySlider->value());
    const int next =
        std::clamp(current + (direction > 0 ? 5 : -5), qRound(m_spotlightOpacitySlider->minimum()),
                   qRound(m_spotlightOpacitySlider->maximum()));
    if (next != current) {
        m_spotlightOpacitySlider->setValue(next);
    }
    return true;
}

bool ScreenshotToolPalette::stepFilterIntensity(int direction) {
    if (direction == 0 || m_activeTool != Tool::RectangleFilter ||
        m_filterEditor.intensitySlider == nullptr || !m_filterEditor.intensitySlider->isEnabled()) {
        return false;
    }

    const int current = qRound(m_styleControls->styleState().rectangleFilterStyle.strength * 100.0);
    const int next = qBound(0, current + (direction > 0 ? 1 : -1), 100);
    if (next != current) {
        m_styleControls->styleState().rectangleFilterStyle.strength = next / 100.0;
        m_styleControls->styleState().creationRectangleFilterStyle.strength = next / 100.0;
        {
            const QSignalBlocker blocker(m_filterEditor.intensitySlider);
            m_filterEditor.intensitySlider->setValue(next);
        }
        emit filterStyleChanged(m_styleControls->styleState().rectangleFilterStyle,
                                SnowCanvasFilterStylePropertyStrength);
    }
    return true;
}

bool ScreenshotToolPalette::stepPenFilterStrokeWidth(int direction) {
    if (direction == 0 || m_activeTool != Tool::PenFilter) {
        return false;
    }

    const double next = qBound(1.0,
                               m_styleControls->styleState().penFilterStyle.strokeWidth +
                                   (direction > 0 ? 1.0 : -1.0),
                               72.0);
    const bool wasMixed = (m_styleControls->styleState().filterStyleMixed &
                           SnowCanvasFilterStylePropertyStrokeWidth) != 0;
    if (next != m_styleControls->styleState().penFilterStyle.strokeWidth || wasMixed) {
        m_styleControls->styleState().penFilterStyle.strokeWidth = next;
        m_styleControls->styleState().creationPenFilterStyle.strokeWidth = next;
        m_styleControls->styleState().filterStyleMixed &= ~SnowCanvasFilterStylePropertyStrokeWidth;
        updatePenFilterStrokeWidthControls();
        emit filterStyleChanged(m_styleControls->styleState().penFilterStyle,
                                SnowCanvasFilterStylePropertyStrokeWidth);
    }
    return true;
}

bool ScreenshotToolPalette::stepWatermarkFontSize(int direction) {
    return direction != 0 && m_activeTool == Tool::Watermark && m_styleControls != nullptr &&
           m_styleControls->stepWatermarkFontSize(direction);
}

void ScreenshotToolPalette::setStyleToolbarAboveMain(bool above) {
    if (m_styleToolbarAboveMain == above) {
        return;
    }

    m_styleToolbarAboveMain = above;
    markLayoutDirty(true);
    updateToolbarGeometry();
    emit visibleContentChanged();
}

void ScreenshotToolPalette::setStyleToolbarVisible(bool visible) {
    if (m_rectangleStylePanel == nullptr) {
        return;
    }

    // Visibility belongs to the active tool. Callers may suppress an active
    // style row, but they cannot make a style row appear for Select or another
    // tool whose secondary row has a different purpose.
    if (setSecondaryToolbarVisibility(m_actionToolbarTargetVisible,
                                      visible && activeToolUsesStyleToolbar())) {
        updateToolbarGeometry();
        update();
        emit visibleContentChanged();
    }
}

bool ScreenshotToolPalette::styleToolbarVisible() const {
    return m_styleToolbarTargetVisible;
}

bool ScreenshotToolPalette::actionToolbarVisible() const {
    return m_actionToolbarTargetVisible;
}

bool ScreenshotToolPalette::setSecondaryToolbarVisibility(bool actionToolbarVisible,
                                                          bool styleToolbarVisible) {
    if (m_selectActionPanel == nullptr || m_rectangleStylePanel == nullptr) {
        return false;
    }

    const bool ocrVisible =
        m_activeTool == Tool::Ocr || m_activeTool == Tool::TextTranslation;
    const bool tableVisible = m_activeTool == Tool::Table;
    const bool qrVisible = m_activeTool == Tool::Qr;
    const bool scrollingVisible = m_activeTool == Tool::ScrollingScreenshot;
    const bool recognitionActionVisible =
        ocrVisible || tableVisible || qrVisible || scrollingVisible;
    const bool recognitionControlsMatch =
        (m_textEditButton == nullptr || m_textEditButton->isHidden() == !ocrVisible) &&
        (m_tableMergeButton == nullptr || m_tableMergeButton->isHidden() == !tableVisible) &&
        (m_scrollingRecognitionControls == nullptr ||
         m_scrollingRecognitionControls->isHidden() == !scrollingVisible);
    if (m_actionToolbarTargetVisible == actionToolbarVisible &&
        m_styleToolbarTargetVisible == styleToolbarVisible && recognitionControlsMatch) {
        return false;
    }

    m_actionToolbarTargetVisible = actionToolbarVisible;
    m_styleToolbarTargetVisible = styleToolbarVisible;
    for (QWidget* widget : std::as_const(m_selectionActionControls)) {
        if (widget != nullptr) {
            widget->setVisible(!recognitionActionVisible);
        }
    }
    if (m_textEditButton != nullptr) {
        m_textEditButton->setVisible(ocrVisible);
    }
    if (m_textTranslateButton != nullptr) {
        m_textTranslateButton->setVisible(ocrVisible);
    }
    if (m_textResetButton != nullptr) {
        m_textResetButton->setVisible(ocrVisible);
    }
    if (m_textSettingsButton != nullptr) {
        m_textSettingsButton->setVisible(ocrVisible);
    }
    if (m_textFormattingSelect != nullptr) {
        m_textFormattingSelect->setVisible(ocrVisible);
    }
    if (m_textPunctuationSelect != nullptr) {
        m_textPunctuationSelect->setVisible(ocrVisible);
    }
    if (m_tableMergeButton != nullptr) {
        m_tableMergeButton->setVisible(tableVisible);
    }
    if (m_tableSplitButton != nullptr) {
        m_tableSplitButton->setVisible(tableVisible);
    }
    if (m_tableResetButton != nullptr) {
        m_tableResetButton->setVisible(tableVisible);
    }
    if (m_scrollingRecognitionControls != nullptr) {
        m_scrollingRecognitionControls->setVisible(scrollingVisible);
    }
    // The OCR mode shares the selection action panel for compatibility with
    // the existing toolbar layout, but it must not retain selection-only
    // separators and spacers. Otherwise the OCR controls appear shifted right
    // with the old selection toolbar contents still occupying the row.
    const auto setSpacersVisible = [this](const QVector<QSpacerItem*>& spacers, bool visible) {
        for (QSpacerItem* spacer : spacers) {
            setStyleToolbarSpacingVisible(spacer, visible);
        }
    };
    setSpacersVisible(m_selectionActionSpacers, !recognitionActionVisible);
    setSpacersVisible(m_textActionSpacers, ocrVisible);
    setSpacersVisible(m_tableActionSpacers, tableVisible);
    for (QFrame* separator : std::as_const(m_styleSeparatorFrames)) {
        if (separator != nullptr && separator->parentWidget() == m_selectActionPanel) {
            separator->setVisible(!recognitionActionVisible);
        }
    }
    if (m_selectActionLayout != nullptr) {
        m_selectActionLayout->invalidate();
        m_selectActionPanel->updateGeometry();
        applyCumulativeStyleLayoutMetrics(m_selectActionPanel);
    }
    // Visibility changes which secondary row participates in the root layout.
    // Keep the layout dirty so its measured extent and row geometry are rebuilt
    // synchronously by the next geometry query.
    markLayoutDirty(false);
    return true;
}

void ScreenshotToolPalette::updateSelectionActionAvailability(bool hasSelection) {
    if (m_selectionActionAvailabilityInitialized && m_hasSelectedElements == hasSelection) {
        return;
    }
    m_selectionActionAvailabilityInitialized = true;
    m_hasSelectedElements = hasSelection;
    for (QWidget* control : std::as_const(m_selectionActionControls)) {
        if (control != nullptr) {
            control->setEnabled(hasSelection);
        }
    }
    if (m_selectionOpacitySlider != nullptr) {
        m_selectionOpacitySlider->setEnabled(m_selectionOpacityAvailable);
    }
    updateSelectionOpacityIcon();
}

void ScreenshotToolPalette::updateSelectionOpacityIcon() {
    if (m_selectionOpacityIcon == nullptr) {
        return;
    }

    const int controlSize = scaledMetric(32);
    const int iconSize = scaledMetric(STYLE_ICON_SIZE);
    const auto scheme = snow_shot::presentation::styles::generateThemeColorScheme();
    const QColor iconColor =
        m_selectionOpacitySlider != nullptr && !m_selectionOpacitySlider->isEnabled()
            ? scheme.map.colorTextQuaternary
            : scheme.map.colorText;
    m_selectionOpacityIcon->setFixedSize(controlSize, controlSize);
    m_selectionOpacityIcon->setPixmap(snow_shot::presentation::icons::renderTintedIconPixmap(
        custom_outlined_icons::Opacity(), QSize(iconSize, iconSize), devicePixelRatioF(),
        iconColor));
}

void ScreenshotToolPalette::updateFilterIntensityIcon(FilterEditor& editor) {
    if (editor.intensityIcon == nullptr) {
        return;
    }
    const auto scheme = snow_shot::presentation::styles::generateThemeColorScheme();
    const QColor iconColor =
        editor.intensitySlider != nullptr && !editor.intensitySlider->isEnabled()
            ? scheme.map.colorTextQuaternary
            : scheme.map.colorText;
    const int iconSize = scaledMetric(COMPACT_SLIDER_ICON_SIZE);
    editor.intensityIcon->setPixmap(snow_shot::presentation::icons::renderTintedIconPixmap(
        custom_outlined_icons::Blur(), QSize(iconSize, iconSize), devicePixelRatioF(), iconColor));
}

void ScreenshotToolPalette::refreshThemeDependentIcons() {
    updateSelectionOpacityIcon();
    updateFilterIntensityIcon(m_filterEditor);
    updateFilterIntensityIcon(m_penFilterEditor);
    m_styleControls->refreshThemeIcons(styleButtonMetrics(m_physicalScale));

    const auto scheme = snow_shot::presentation::styles::generateThemeColorScheme();
    if (m_copyButton != nullptr && !m_options.copyButtonWithNeutralIcon) {
        m_copyButton->setIconRef(snow_shot::presentation::icons::withPrimaryColor(
            outlined_icons::Copy(), scheme.map.colorPrimary));
    }
    if (m_confirmButton != nullptr) {
        m_confirmButton->setIconRef(snow_shot::presentation::icons::withPrimaryColor(
            outlined_icons::Check(), scheme.map.colorPrimary));
    }
    if (m_recordStartButton != nullptr) {
        m_recordStartButton->setIconRef(snow_shot::presentation::icons::withPrimaryColor(
            custom_outlined_icons::RecordingStart(), scheme.map.colorPrimary));
    }
    if (m_recordResumeButton != nullptr) {
        m_recordResumeButton->setIconRef(snow_shot::presentation::icons::withPrimaryColor(
            custom_outlined_icons::RecordingResume(), scheme.map.colorPrimary));
    }

    updateRecordingControls();
    updateRecordingControlMetrics();
}

void ScreenshotToolPalette::updatePenFilterStrokeWidthControls() {
    const bool mixed = (m_styleControls->styleState().filterStyleMixed &
                        SnowCanvasFilterStylePropertyStrokeWidth) != 0;
    m_styleControls->updatePenFilterStrokeWidthControls(
        m_styleControls->styleState().penFilterStyle.strokeWidth, mixed);
}

void ScreenshotToolPalette::setActiveTool(Tool tool) {
    SNOW_SHOT_TOOLBAR_PERF_SCOPE("palette.set_active_tool");
    if (m_releasingSecondaryResources) {
        return;
    }
    const bool activeToolNoop = m_activeTool.has_value() && *m_activeTool == tool;
    bool secondaryContentsEvicted = false;
    if (!activeToolNoop) {
        const std::optional<ActionFamily> previousActionFamily =
            m_activeTool.has_value() ? actionFamilyForTool(*m_activeTool) : std::nullopt;
        const std::optional<ActionFamily> nextActionFamily = actionFamilyForTool(tool);
        const std::optional<Tool> previousStyleFamily =
            m_activeTool.has_value() ? styleFamilyForTool(*m_activeTool) : std::nullopt;
        const std::optional<Tool> nextStyleFamily = styleFamilyForTool(tool);
        const bool sameActionFamily = previousActionFamily.has_value() &&
                                      nextActionFamily.has_value() &&
                                      previousActionFamily == nextActionFamily &&
                                      !previousStyleFamily.has_value() &&
                                      !nextStyleFamily.has_value();
        const bool sameStyleFamily = previousStyleFamily.has_value() &&
                                     nextStyleFamily.has_value() &&
                                     previousStyleFamily == nextStyleFamily &&
                                     !previousActionFamily.has_value() &&
                                     !nextActionFamily.has_value();
        const bool targetActionFamilyReady =
            nextActionFamily.has_value() &&
            m_actionFamilyStates.value(static_cast<int>(*nextActionFamily),
                                       MaterializationState::Uninitialized) ==
                MaterializationState::Ready;
        const bool targetStyleFamilyReady =
            nextStyleFamily.has_value() &&
            m_styleFamilyStates.value(static_cast<int>(*nextStyleFamily),
                                      MaterializationState::Uninitialized) ==
                MaterializationState::Ready;
        const bool preserveExplicitlyMaterializedInitialTarget =
            !m_activeTool.has_value() && (targetActionFamilyReady || targetStyleFamilyReady);
        if (!sameActionFamily && !sameStyleFamily &&
            !preserveExplicitlyMaterializedInitialTarget) {
            secondaryContentsEvicted = evictSecondaryToolbarContents();
        }
    }
    if (const std::optional<ActionFamily> family = actionFamilyForTool(tool);
        family.has_value() && toolUsesActionToolbar(tool)) {
        static_cast<void>(ensureActionFamily(*family));
    }
    if (toolUsesStyleToolbar(tool)) {
        static_cast<void>(ensureStyleFamily(tool));
    }
    selectDynamicEntryTool(tool);
    synchronizeFilterModeGroups(tool);
    if (activeToolNoop) {
        SNOW_SHOT_TOOLBAR_PERF_COUNTER("palette.active_tool_noop");
        const bool styleControlsChanged = setStyleControlsActive(tool);
        const bool visibilityChanged = applyActiveToolSecondaryToolbarVisibility();
        if (styleControlsChanged || visibilityChanged) {
            updateToolbarGeometry();
            update();
            emit visibleContentChanged();
        }
        return;
    }

    adqt::widgets::AdButton* activeButton = nullptr;
    switch (tool) {
    case Tool::Move:
        activeButton = m_moveButton;
        break;
    case Tool::Select:
        activeButton = m_selectButton;
        break;
    case Tool::Shape:
        activeButton = drawingToolEntryButton(tool);
        break;
    case Tool::Arrow:
        activeButton = drawingToolEntryButton(tool);
        break;
    case Tool::Line:
        activeButton = drawingToolEntryButton(tool);
        break;
    case Tool::FreeDraw:
        activeButton = drawingToolEntryButton(tool);
        break;
    case Tool::RectangleHighlight:
        activeButton = drawingToolEntryButton(tool);
        break;
    case Tool::PenHighlight:
        activeButton = drawingToolEntryButton(tool);
        break;
    case Tool::Spotlight:
        activeButton = drawingToolEntryButton(tool);
        break;
    case Tool::Eraser:
        activeButton = drawingToolEntryButton(tool);
        break;
    case Tool::RectangleFilter:
    case Tool::PenFilter:
        activeButton = drawingToolEntryButton(tool);
        break;
    case Tool::Watermark:
        activeButton = drawingToolEntryButton(tool);
        break;
    case Tool::Text:
        activeButton = drawingToolEntryButton(tool);
        break;
    case Tool::SerialNumber:
        activeButton = drawingToolEntryButton(tool);
        break;
    case Tool::Ocr:
        activeButton = m_ocrButton;
        break;
    case Tool::TextTranslation:
        activeButton = m_textTranslationButton;
        break;
    case Tool::Table:
        activeButton = m_tableButton;
        break;
    case Tool::Qr:
        activeButton = m_tableButton;
        break;
    case Tool::ScrollingScreenshot:
        activeButton = m_scrollingScreenshotButton;
        break;
    default:
        clearActiveTool();
        return;
    }

    m_activeTool = tool;
    setActiveToolButton(activeButton);
    updateTextRecognitionBusy();
    updateHistoryActionAvailability();
    const bool styleControlsChanged = setStyleControlsActive(tool);
    const bool visibilityChanged = applyActiveToolSecondaryToolbarVisibility();
    if (visibilityChanged || secondaryContentsEvicted) {
        updateToolbarGeometry();
        update();
        emit visibleContentChanged();
    } else if (styleControlsChanged && activeToolUsesStyleToolbar()) {
        updateStyleToolbarGeometryOnly();
        update();
        emit visibleContentChanged();
    }
}

void ScreenshotToolPalette::activateTableQrTool(Tool tool, bool toggleVisibleButton) {
    if (tool == Tool::Table && m_tableEnabled) {
        activateToolFromToolbar(tool, toggleVisibleButton);
    } else if (tool == Tool::Qr && m_qrEnabled) {
        activateToolFromToolbar(tool, toggleVisibleButton);
    }
}

void ScreenshotToolPalette::setTableQrEntryTool(Tool tool) {
    if (tool != Tool::Table && tool != Tool::Qr) {
        return;
    }
    if (m_tableQrEntryTool != tool) {
        static_cast<void>(
            toolbar_settings::ScreenshotToolbarSettings().setTableQrTool(tableQrToolSetting(tool)));
    }
    m_tableQrEntryTool = tool;
    refreshTableQrTrigger();
}

void ScreenshotToolPalette::refreshTableQrTrigger() {
    if (m_tableButton == nullptr) {
        return;
    }
    const bool table = m_tableQrEntryTool == Tool::Table;
    configureScreenshotToolPaletteTooltip(m_tableButton,
                                          table ? "Table recognition" : "Barcode recognition");
    applyScreenshotShortcutTooltip(
        m_tableButton,
        table ? QStringLiteral("Table recognition") : QStringLiteral("Barcode recognition"),
        table ? QStringLiteral("table_recognition") : QStringLiteral("qr_code_recognition"));
    setScreenshotToolPaletteToolButtonIcon(m_tableButton,
                                           table ? custom_outlined_icons::TableRecognition()
                                                 : custom_outlined_icons::ScanQrcode());
    updateTableQrEnabled();
}

void ScreenshotToolPalette::selectDynamicEntryTool(Tool tool) {
    if (!drawingToolItemId(tool).isEmpty()) {
        selectDrawingToolGroupEntry(tool);
    } else if (tool == Tool::Table || tool == Tool::Qr) {
        setTableQrEntryTool(tool);
    }
}

void ScreenshotToolPalette::updateTableQrBusy() {
    if (m_tableOptionButton != nullptr) {
        m_tableOptionButton->setBusy(m_tableBusy);
    }
    if (m_qrButton != nullptr) {
        m_qrButton->setBusy(m_qrBusy);
    }
    if (m_tableButton != nullptr) {
        const bool currentBusy = m_tableQrEntryTool == Tool::Qr ? m_qrBusy : m_tableBusy;
        m_tableButton->setBusy(m_tableQrPopover != nullptr ? m_tableBusy || m_qrBusy : currentBusy);
    }
}

void ScreenshotToolPalette::updateTableQrEnabled() {
    if (m_tableOptionButton != nullptr) {
        m_tableOptionButton->setEnabled(m_tableEnabled);
    }
    if (m_qrButton != nullptr) {
        m_qrButton->setEnabled(m_qrEnabled);
    }
    if (m_tableButton != nullptr) {
        const bool currentEnabled = m_tableQrEntryTool == Tool::Qr ? m_qrEnabled : m_tableEnabled;
        m_tableButton->setEnabled(m_tableQrPopover != nullptr ? m_tableEnabled || m_qrEnabled
                                                              : currentEnabled);
    }
}

void ScreenshotToolPalette::setSelectionOpacity(qreal opacity, bool mixed) {
    opacity = std::clamp<qreal>(opacity, 0.0, 1.0);
    if (m_selectionOpacityInitialized && snowCanvasExactDoubleEqual(m_selectionOpacity, opacity) &&
        m_selectionOpacityMixed == mixed) {
        return;
    }
    m_selectionOpacityInitialized = true;
    m_selectionOpacity = opacity;
    m_selectionOpacityMixed = mixed;
    if (m_selectionOpacitySlider != nullptr) {
        const QSignalBlocker blocker(m_selectionOpacitySlider);
        m_selectionOpacitySlider->setValue(qRound(m_selectionOpacity * 100.0));
        m_selectionOpacitySlider->setProperty("mixed", mixed);
        m_selectionOpacitySlider->setAccessibleDescription(
            mixed ? tr("Mixed") : QStringLiteral("%1%").arg(m_selectionOpacitySlider->value()));
    }
}

void ScreenshotToolPalette::clearActiveTool() {
    const bool secondaryContentsEvicted = evictSecondaryToolbarContents();
    if (!m_activeTool.has_value() && m_activeToolButton == nullptr &&
        !secondaryContentsEvicted) {
        return;
    }
    m_activeTool.reset();
    setActiveToolButton(nullptr);
    updateHistoryActionAvailability();
    m_activeStyleTool.reset();
    if (secondaryContentsEvicted || applyActiveToolSecondaryToolbarVisibility()) {
        updateToolbarGeometry();
        update();
        emit visibleContentChanged();
    }
}

void ScreenshotToolPalette::setScrollingScreenshotMode(bool enabled) {
    if (m_scrollingScreenshotMode == enabled) {
        return;
    }
    m_scrollingScreenshotMode = enabled;
    if (enabled) {
        setScrollingRecognitionMode(ScreenshotScrollingRecognitionMode::Vertical);
    }
    if (enabled) {
        setStyleToolbarVisible(false);
        setActiveTool(Tool::ScrollingScreenshot);
    } else if (m_activeTool == Tool::ScrollingScreenshot) {
        setActiveTool(Tool::Move);
    } else {
        setStyleToolbarVisible(activeToolUsesStyleToolbar());
    }
}

void ScreenshotToolPalette::setRecordingState(RecordingState state) {
    if (m_recordingState == state) {
        return;
    }
    m_recordingState = state;
    updateRecordingControls();
}

void ScreenshotToolPalette::setRecordingDuration(qint64 durationMilliseconds) {
    m_recordingDurationMilliseconds = qMax<qint64>(0, durationMilliseconds);
    if (m_recordDurationLabel == nullptr) {
        return;
    }
    const qint64 totalSeconds = m_recordingDurationMilliseconds / 1000;
    const qint64 hours = totalSeconds / 3600;
    const qint64 minutes = (totalSeconds / 60) % 60;
    const qint64 seconds = totalSeconds % 60;
    const QString timestamp = QStringLiteral("%1:%2:%3")
                                  .arg(hours, 2, 10, QLatin1Char('0'))
                                  .arg(minutes, 2, 10, QLatin1Char('0'))
                                  .arg(seconds, 2, 10, QLatin1Char('0'));
    if (m_recordDurationLabel->text() != timestamp) {
        m_recordDurationLabel->setText(timestamp);
    }
}

void ScreenshotToolPalette::setRecordingMicrophoneEnabled(bool enabled) {
    if (m_recordingMicrophoneEnabled == enabled) {
        return;
    }
    m_recordingMicrophoneEnabled = enabled;
    updateRecordingControls();
}

void ScreenshotToolPalette::setRecordingSystemAudioEnabled(bool enabled) {
    if (m_recordingSystemAudioEnabled == enabled) {
        return;
    }
    m_recordingSystemAudioEnabled = enabled;
    updateRecordingControls();
}

void ScreenshotToolPalette::setRecordingBusy(bool busy) {
    if (m_recordingBusy == busy) {
        return;
    }
    m_recordingBusy = busy;
    updateRecordingControls();
}

void ScreenshotToolPalette::setOcrBusy(bool busy) {
    m_ocrBusy = busy;
    updateTextRecognitionBusy();
}

void ScreenshotToolPalette::setOcrEnabled(bool enabled) {
    if (m_ocrEnabled == enabled) {
        return;
    }
    m_ocrEnabled = enabled;
    if (m_ocrButton != nullptr) {
        m_ocrButton->setEnabled(enabled);
    }
    if (m_textTranslationButton != nullptr) {
        m_textTranslationButton->setEnabled(enabled);
    }
}

void ScreenshotToolPalette::setScrollingRecognitionMode(ScreenshotScrollingRecognitionMode mode) {
    if (m_scrollingRecognitionMode == mode) {
        updateScrollingRecognitionButtons();
        return;
    }
    m_scrollingRecognitionMode = mode;
    updateScrollingRecognitionButtons();
    emit scrollingRecognitionModeChanged(mode);
}

ScreenshotScrollingRecognitionMode ScreenshotToolPalette::scrollingRecognitionMode() const {
    return m_scrollingRecognitionMode;
}

void ScreenshotToolPalette::updateScrollingRecognitionButtons() {
    const auto updateButton = [this](adqt::widgets::AdButton* button,
                                     ScreenshotScrollingRecognitionMode mode) {
        if (button == nullptr) {
            return;
        }
        applyMainToolbarToolActiveStyle(button, m_scrollingRecognitionMode == mode);
    };
    updateButton(m_scrollingVerticalButton, ScreenshotScrollingRecognitionMode::Vertical);
    updateButton(m_scrollingHorizontalButton, ScreenshotScrollingRecognitionMode::Horizontal);
}

void ScreenshotToolPalette::setTableBusy(bool busy) {
    m_tableBusy = busy;
    updateTableQrBusy();
}

void ScreenshotToolPalette::setTableEnabled(bool enabled) {
    if (m_tableEnabled == enabled) {
        return;
    }
    m_tableEnabled = enabled;
    updateTableQrEnabled();
}

void ScreenshotToolPalette::setQrBusy(bool busy) {
    m_qrBusy = busy;
    updateTableQrBusy();
}

void ScreenshotToolPalette::setQrEnabled(bool enabled) {
    if (m_qrEnabled == enabled) {
        return;
    }
    m_qrEnabled = enabled;
    updateTableQrEnabled();
}

void ScreenshotToolPalette::setTableEditingState(bool available, bool canUndo, bool canRedo,
                                                  bool canMerge, bool canSplit, bool canReset) {
    m_tableEditingAvailable = available;
    m_tableCanUndo = canUndo;
    m_tableCanRedo = canRedo;
    m_tableCanMerge = canMerge;
    m_tableCanSplit = canSplit;
    m_tableCanReset = canReset;
    if (m_tableMergeButton != nullptr) {
        m_tableMergeButton->setEnabled(available && canMerge);
    }
    if (m_tableSplitButton != nullptr) {
        m_tableSplitButton->setEnabled(available && canSplit);
    }
    if (m_tableResetButton != nullptr) {
        m_tableResetButton->setEnabled(available && canReset);
    }
    updateHistoryActionAvailability();
}

void ScreenshotToolPalette::setTextEditingState(bool available, bool editing, bool canUndo,
                                                bool canRedo) {
    m_textEditing = editing;
    m_textEditingAvailable = available && (editing || m_textTranslating);
    m_textCanUndo = canUndo;
    m_textCanRedo = canRedo;
    if (m_textEditButton != nullptr) {
        m_textEditButton->setEnabled(available);
        applyMainToolbarToolActiveStyle(m_textEditButton, editing);
    }
    if (m_textTranslateButton != nullptr) {
        m_textTranslateButton->setEnabled(available && !m_textTranslationStreaming);
    }
    if (m_textFormattingSelect != nullptr) {
        m_textFormattingSelect->setEnabled(available && !m_textTranslating);
    }
    if (m_textPunctuationSelect != nullptr) {
        m_textPunctuationSelect->setEnabled(available && !m_textTranslating);
    }
    if (m_textResetButton != nullptr) {
        m_textResetButton->setEnabled(available &&
                                      (editing || (m_textTranslating && m_textCanReset)) &&
                                      !m_textTranslationStreaming);
    }
    updateHistoryActionAvailability();
}

void ScreenshotToolPalette::setTextTranslationState(bool available, bool translating,
                                                    bool streaming, bool canUndo,
                                                    bool canRedo, bool canReset) {
    m_textTranslating = translating;
    m_textTranslationStreaming = streaming;
    m_textEditingAvailable = available && (translating || m_textEditing);
    if (translating) {
        m_textCanUndo = canUndo;
        m_textCanRedo = canRedo;
    }
    m_textCanReset = canReset;
    if (m_textTranslateButton != nullptr) {
        m_textTranslateButton->setEnabled(available);
        applyMainToolbarToolActiveStyle(m_textTranslateButton, translating);
    }
    if (m_textEditButton != nullptr) {
        applyMainToolbarToolActiveStyle(m_textEditButton, available && !translating && m_textEditing);
    }
    if (m_textFormattingSelect != nullptr) {
        m_textFormattingSelect->setEnabled(available && !streaming);
    }
    if (m_textPunctuationSelect != nullptr) {
        m_textPunctuationSelect->setEnabled(available && !streaming);
    }
    if (m_textResetButton != nullptr) {
        m_textResetButton->setEnabled(
            available && !streaming &&
            (translating ? canReset : m_textEditing));
    }
    if (m_textSettingsButton != nullptr) {
        m_textSettingsButton->setEnabled(available);
    }
    if (translating && m_textTranslationButton != nullptr && m_activeTool == Tool::Ocr) {
        setActiveTool(Tool::TextTranslation);
    } else {
        updateTextRecognitionBusy();
    }
    updateHistoryActionAvailability();
}

void ScreenshotToolPalette::setTextTransformSelections(const QString& formatting,
                                                         const QString& punctuation) {
    m_textFormattingSelection = formatting;
    m_textPunctuationSelection = punctuation;
    if (m_textFormattingSelect != nullptr) {
        const QSignalBlocker blocker(m_textFormattingSelect);
        m_textFormattingSelect->setCurrentValue(formatting.isEmpty() ? QVariant{}
                                                                     : QVariant{formatting});
    }
    if (m_textPunctuationSelect != nullptr) {
        const QSignalBlocker blocker(m_textPunctuationSelect);
        m_textPunctuationSelect->setCurrentValue(punctuation.isEmpty() ? QVariant{}
                                                                       : QVariant{punctuation});
    }
}

bool ScreenshotToolPalette::scrollingScreenshotMode() const {
    return m_scrollingScreenshotMode;
}

SnowCanvasShapeStyle ScreenshotToolPalette::rectangleStyle() const {
    return m_styleControls->rectangleStyle();
}

void ScreenshotToolPalette::setRectangleStyle(const SnowCanvasShapeStyle& style) {
    m_styleControls->setRectangleStyle(style);
}

void ScreenshotToolPalette::setStyleToolbarState(const SnowCanvasStyleToolbarState& state) {
    SNOW_SHOT_TOOLBAR_PERF_SCOPE("palette.set_style_toolbar_state");
    m_styleControls->setStyleToolbarState(state);
    const bool hasSelectedElements = hasSelectedCanvasElements(state);
    m_selectionOpacityAvailable =
        hasSelectedElements && state.source != SnowCanvasStyleToolbarSource::SelectedSpotlight;
    updateSelectionActionAvailability(hasSelectedElements);
    // Canvas style state and palette tool state are delivered independently.
    // Style state synchronizes values, but only a style-capable active tool may
    // choose an editor. The active tool alone owns secondary-row visibility.
    const bool styleToolbarActive = activeToolUsesStyleToolbar();
    if (state.source == SnowCanvasStyleToolbarSource::Watermark) {
        if (!styleToolbarActive) {
            return;
        }
        static_cast<void>(ensureStyleFamily(Tool::Watermark));
        if (setStyleControlsActive(Tool::Watermark)) {
            updateToolbarGeometry();
            update();
            emit visibleContentChanged();
        }
        return;
    }
    if (state.source == SnowCanvasStyleToolbarSource::Eraser) {
        return;
    }
    if (state.source == SnowCanvasStyleToolbarSource::DefaultRectangleFilter ||
        state.source == SnowCanvasStyleToolbarSource::SelectedRectangleFilter ||
        state.source == SnowCanvasStyleToolbarSource::DefaultPenFilter ||
        state.source == SnowCanvasStyleToolbarSource::SelectedPenFilter) {
        const bool penFilterSource =
            state.source == SnowCanvasStyleToolbarSource::DefaultPenFilter ||
            state.source == SnowCanvasStyleToolbarSource::SelectedPenFilter;
        const Tool filterTool = penFilterSource ? Tool::PenFilter : Tool::RectangleFilter;
        if (styleToolbarActive) {
            static_cast<void>(ensureStyleFamily(filterTool));
        }
        SnowCanvasFilterStyle& currentStyle =
            penFilterSource ? m_styleControls->styleState().penFilterStyle
                            : m_styleControls->styleState().rectangleFilterStyle;
        adqt::widgets::AdSelect* typeSelect =
            penFilterSource ? m_penFilterEditor.typeSelect : m_filterEditor.typeSelect;
        adqt::widgets::AdSlider* intensitySlider =
            penFilterSource ? m_penFilterEditor.intensitySlider : m_filterEditor.intensitySlider;
        const bool sourceChanged = m_styleControls->styleState().filterStyleSource != state.source;
        const quint32 mixedChanged =
            m_styleControls->styleState().filterStyleMixed ^ state.filterStyleMixed;
        const bool typeChanged = sourceChanged || currentStyle.type != state.filterStyle.type ||
                                 (mixedChanged & SnowCanvasFilterStylePropertyType) != 0;
        const bool strengthChanged =
            sourceChanged ||
            !snowCanvasExactDoubleEqual(currentStyle.strength, state.filterStyle.strength) ||
            (mixedChanged & SnowCanvasFilterStylePropertyStrength) != 0;
        const bool opacityChanged =
            sourceChanged ||
            !snowCanvasExactDoubleEqual(currentStyle.opacity, state.filterStyle.opacity) ||
            (mixedChanged & SnowCanvasFilterStylePropertyOpacity) != 0;
        const bool strokeWidthChanged =
            sourceChanged ||
            !snowCanvasExactDoubleEqual(currentStyle.strokeWidth, state.filterStyle.strokeWidth) ||
            (mixedChanged & SnowCanvasFilterStylePropertyStrokeWidth) != 0;
        const bool mixedType = (state.filterStyleMixed & SnowCanvasFilterStylePropertyType) != 0;
        if (intensitySlider != nullptr) {
            intensitySlider->setEnabled(mixedType ||
                                        filterTypeSupportsIntensity(state.filterStyle.type));
        }
        FilterEditor& editor = penFilterSource ? m_penFilterEditor : m_filterEditor;
        updateFilterIntensityIcon(editor);
        if (!typeChanged && !strengthChanged && !opacityChanged && !strokeWidthChanged) {
#if defined(SNOW_SHOT_TEST_HOOKS)
            ++m_styleStateNoopCount;
#endif
            SNOW_SHOT_TOOLBAR_PERF_COUNTER("style.state_noop");
            return;
        }
        m_styleControls->styleState().filterStyleSource = state.source;
        currentStyle = state.filterStyle;
        if (state.source == SnowCanvasStyleToolbarSource::DefaultPenFilter) {
            m_styleControls->styleState().creationPenFilterStyle = state.filterStyle;
        } else if (state.source == SnowCanvasStyleToolbarSource::DefaultRectangleFilter) {
            m_styleControls->styleState().creationRectangleFilterStyle = state.filterStyle;
        }
        m_styleControls->styleState().filterStyleMixed = state.filterStyleMixed;
        if ((state.source == SnowCanvasStyleToolbarSource::SelectedRectangleFilter ||
             state.source == SnowCanvasStyleToolbarSource::SelectedPenFilter) &&
            m_activeTool == Tool::Select && opacityChanged) {
            setSelectionOpacity(state.filterStyle.opacity,
                                (state.filterStyleMixed & SnowCanvasFilterStylePropertyOpacity) !=
                                    0);
        }
        if (typeChanged && typeSelect != nullptr) {
#if defined(SNOW_SHOT_TEST_HOOKS)
            ++m_propertyGroupRefreshCount;
#endif
            SNOW_SHOT_TOOLBAR_PERF_COUNTER("style.filter.type_refresh");
            const QSignalBlocker blocker(typeSelect);
            if (mixedType) {
                typeSelect->setCurrentIndex(-1);
            } else {
                typeSelect->setCurrentData(static_cast<int>(state.filterStyle.type),
                                           adqt::widgets::AdSelect::DefaultValueRole);
            }
        }
        if (strengthChanged && intensitySlider != nullptr) {
#if defined(SNOW_SHOT_TEST_HOOKS)
            ++m_propertyGroupRefreshCount;
#endif
            SNOW_SHOT_TOOLBAR_PERF_COUNTER("style.filter.strength_refresh");
            const QSignalBlocker blocker(intensitySlider);
            intensitySlider->setValue(qRound(state.filterStyle.strength * 100.0));
            intensitySlider->setAccessibleDescription(
                QStringLiteral("%1%").arg(intensitySlider->value()));
            intensitySlider->setProperty(
                "mixed", (state.filterStyleMixed & SnowCanvasFilterStylePropertyStrength) != 0);
        }
        if (opacityChanged) {
#if defined(SNOW_SHOT_TEST_HOOKS)
            ++m_propertyGroupRefreshCount;
#endif
            SNOW_SHOT_TOOLBAR_PERF_COUNTER("style.filter.opacity_refresh");
        }
        if (penFilterSource && strokeWidthChanged) {
            updatePenFilterStrokeWidthControls();
        }
        synchronizeFilterModeGroups(filterTool);
        if (!styleToolbarActive) {
            return;
        }
        if (setStyleControlsActive(filterTool)) {
            updateToolbarGeometry();
            update();
            emit visibleContentChanged();
        }
        return;
    }
    Tool activeStyleTool = Tool::Shape;
    bool styleToolbarChanged = true;
    if (m_activeTool == Tool::Select) {
        qreal opacity = 1.0;
        bool mixed = false;
        if (state.source == SnowCanvasStyleToolbarSource::SelectedText) {
            opacity = state.textStyle.opacity;
            mixed = (state.textStyleMixed & SnowCanvasTextStyleMixedOpacity) != 0;
        } else if (state.source == SnowCanvasStyleToolbarSource::SelectedSerialNumber) {
            opacity = state.serialNumberStyle.opacity;
            mixed = (state.serialNumberStyleMixed & SnowCanvasSerialNumberStyleMixedOpacity) != 0;
        } else if (state.source == SnowCanvasStyleToolbarSource::SelectedRectangle ||
                   state.source == SnowCanvasStyleToolbarSource::SelectedArrow ||
                   state.source == SnowCanvasStyleToolbarSource::SelectedLine ||
                   state.source == SnowCanvasStyleToolbarSource::SelectedFreeDraw ||
                   state.source == SnowCanvasStyleToolbarSource::SelectedRectangleHighlight ||
                   state.source == SnowCanvasStyleToolbarSource::SelectedPenHighlight) {
            opacity = state.shapeStyle.opacity;
            mixed = (state.shapeStyleMixed & SnowCanvasShapeStyleMixedOpacity) != 0;
        }
        setSelectionOpacity(opacity, mixed);
        activeStyleTool = Tool::Shape;
    } else if (state.source == SnowCanvasStyleToolbarSource::DefaultArrow ||
               state.source == SnowCanvasStyleToolbarSource::SelectedArrow) {
        activeStyleTool = Tool::Arrow;
    } else if (state.source == SnowCanvasStyleToolbarSource::DefaultLine ||
               state.source == SnowCanvasStyleToolbarSource::SelectedLine) {
        activeStyleTool = Tool::Line;
    } else if (state.source == SnowCanvasStyleToolbarSource::DefaultFreeDraw ||
               state.source == SnowCanvasStyleToolbarSource::SelectedFreeDraw) {
        activeStyleTool = Tool::FreeDraw;
    } else if (state.source == SnowCanvasStyleToolbarSource::DefaultRectangleHighlight ||
               state.source == SnowCanvasStyleToolbarSource::SelectedRectangleHighlight) {
        activeStyleTool = Tool::RectangleHighlight;
    } else if (state.source == SnowCanvasStyleToolbarSource::DefaultPenHighlight ||
               state.source == SnowCanvasStyleToolbarSource::SelectedPenHighlight) {
        activeStyleTool = Tool::PenHighlight;
    } else if (state.source == SnowCanvasStyleToolbarSource::DefaultSpotlight ||
               state.source == SnowCanvasStyleToolbarSource::SelectedSpotlight) {
        activeStyleTool = Tool::Spotlight;
    } else if (state.source == SnowCanvasStyleToolbarSource::DefaultText ||
               state.source == SnowCanvasStyleToolbarSource::SelectedText) {
        activeStyleTool = Tool::Text;
    } else if (state.source == SnowCanvasStyleToolbarSource::DefaultSerialNumber ||
               state.source == SnowCanvasStyleToolbarSource::SelectedSerialNumber) {
        activeStyleTool = Tool::SerialNumber;
    } else if (state.source == SnowCanvasStyleToolbarSource::DefaultRectangle ||
               state.source == SnowCanvasStyleToolbarSource::SelectedRectangle) {
        activeStyleTool = Tool::Shape;
    } else {
        styleToolbarChanged = false;
    }

    if (styleToolbarChanged && styleToolbarActive) {
        static_cast<void>(ensureStyleFamily(activeStyleTool));
        if (setStyleControlsActive(activeStyleTool)) {
            updateToolbarGeometry();
            update();
            emit visibleContentChanged();
        }
    }
}

void ScreenshotToolPalette::setWatermarkConfig(const SnowCanvasWatermarkConfig& config) {
    m_styleControls->setWatermarkConfig(config);
}

void ScreenshotToolPalette::setSpotlightConfig(const SnowCanvasSpotlightConfig& config) {
    m_styleControls->styleState().spotlightConfig = config;
    m_styleControls->updateSpotlightColorControls(config.color);
    if (m_spotlightOpacitySlider != nullptr) {
        const QSignalBlocker blocker(m_spotlightOpacitySlider);
        m_spotlightOpacitySlider->setValue(qRound(std::clamp(config.opacity, 0.0, 1.0) * 100.0));
        m_spotlightOpacitySlider->setAccessibleDescription(
            QStringLiteral("%1%").arg(qRound(m_spotlightOpacitySlider->value())));
    }
}

void ScreenshotToolPalette::updateToolbarGeometry() {
    markLayoutDirty();
    ensureLayoutApplied();
}

void ScreenshotToolPalette::updateStyleToolbarGeometryOnly() {
    SNOW_SHOT_TOOLBAR_PERF_SCOPE("palette.style_panel_geometry");
    if (m_rectangleStylePanel == nullptr) {
        return;
    }

    m_rectangleStylePanel->ensurePolished();
    const QSize styleSize = styleToolbarSizeHint();
    if (m_rectangleStylePanel->size() != styleSize ||
        m_rectangleStylePanel->minimumSize() != styleSize ||
        m_rectangleStylePanel->maximumSize() != styleSize) {
        m_rectangleStylePanel->setFixedSize(styleSize);
    }
    // The root layout owns both row geometry and the palette extent.  Re-run
    // that single path after an active editor changes size so no stale row
    // position can leak into placement snapshots.
    updateToolbarGeometry();
}

void ScreenshotToolPalette::markLayoutDirty(bool rowOrderChanged) {
    m_layoutDirty = true;
    m_rowOrderDirty = m_rowOrderDirty || rowOrderChanged;
}

void ScreenshotToolPalette::ensureLayoutApplied() const {
    SNOW_SHOT_TOOLBAR_PERF_SCOPE("palette.ensure_layout_applied");
    if (!m_layoutDirty) {
        return;
    }

    auto* self = const_cast<ScreenshotToolPalette*>(this);
    self->m_layoutDirty = false;
    if (self->m_rootLayout == nullptr || self->m_mainPanel == nullptr) {
        return;
    }

    self->commitLayout();
    self->m_layoutResult.paletteSize = self->size();
    self->m_layoutResult.contentSize = self->contentSizeForVisibleRows();
    self->m_layoutResult.contentOffset = self->contentOffset();
    self->m_layoutResult.fullContentRect =
        QRect(QPoint(0, 0), self->fullContentSize());
    self->m_layoutResult.mainToolbarContentRect = self->panelContentRect(self->m_mainPanel);
    QRect cachedOccupied = self->m_layoutResult.mainToolbarContentRect;
    if (self->m_actionToolbarTargetVisible) {
        cachedOccupied = cachedOccupied.united(self->panelContentRect(self->m_selectActionPanel));
    }
    if (self->m_styleToolbarTargetVisible) {
        cachedOccupied = cachedOccupied.united(self->panelContentRect(self->m_rectangleStylePanel));
    }
    self->m_layoutResult.occupiedContentRect = cachedOccupied;
    SNOW_SHOT_TOOLBAR_PERF_COUNTER("layout.commit");
#if defined(SNOW_SHOT_TEST_HOOKS)
    ++self->m_layoutCommitCount;
#endif
}

void ScreenshotToolPalette::commitLayout() {
    SNOW_SHOT_TOOLBAR_PERF_SCOPE("palette.layout_commit");
    if (m_rootLayout == nullptr || m_mainPanel == nullptr) {
        return;
    }

    // Size hints are consumed synchronously by the placement code.  Activate
    // every row layout before reading them; otherwise a visibility/material-
    // ization change can leave the first committed frame with the previous
    // (usually much smaller) nested-layout extent.
    const auto activateLayout = [](QWidget* widget) {
        if (widget == nullptr) {
            return;
        }
        widget->ensurePolished();
        if (QLayout* layout = widget->layout()) {
            layout->activate();
        }
    };

    activateLayout(m_mainPanel);
    const QSize mainSize = m_mainPanel->sizeHint();
    if (m_mainPanel->size() != mainSize || m_mainPanel->minimumSize() != mainSize ||
        m_mainPanel->maximumSize() != mainSize) {
        m_mainPanel->setFixedSize(mainSize);
    }
    if (m_selectActionPanel != nullptr) {
        activateLayout(m_selectActionPanel);
        const QSize actionSize = m_selectActionPanel->sizeHint();
        if (m_selectActionPanel->size() != actionSize ||
            m_selectActionPanel->minimumSize() != actionSize ||
            m_selectActionPanel->maximumSize() != actionSize) {
            m_selectActionPanel->setFixedSize(actionSize);
        }
    }
    if (m_rectangleStylePanel != nullptr) {
        activateLayout(m_rectangleStylePanel);
        const QSize styleSize = styleToolbarSizeHint();
        if (m_rectangleStylePanel->size() != styleSize ||
            m_rectangleStylePanel->minimumSize() != styleSize ||
            m_rectangleStylePanel->maximumSize() != styleSize) {
            m_rectangleStylePanel->setFixedSize(styleSize);
        }
    }
    const QSize contentSize = contentSizeForVisibleRows();
    const QSize paletteSize = contentSize + QSize(m_shadowMargins.left() + m_shadowMargins.right(),
                                                  m_shadowMargins.top() + m_shadowMargins.bottom());
    if (m_rootLayout->contentsMargins() != m_shadowMargins) {
        m_rootLayout->setContentsMargins(m_shadowMargins.left(), m_shadowMargins.top(),
                                         m_shadowMargins.right(), m_shadowMargins.bottom());
    }
    const int rowSpacing = scaledMetric(TOOLBAR_ROW_SPACING);
    if (m_rootLayout->spacing() != rowSpacing) {
        m_rootLayout->setSpacing(rowSpacing);
    }
    if (size() != paletteSize || minimumSize() != paletteSize || maximumSize() != paletteSize) {
        setFixedSize(paletteSize);
    }
    updateToolbarRowGeometry(m_styleToolbarTargetVisible);
    // Always activate after visibility/order changes.  This is the synchronous
    // boundary used by placement callers and prevents first-show geometry from
    // observing a pre-layout child size.
    if (layout() != nullptr) {
        layout()->activate();
    }
    // A row can be resized by the palette's fixed-size commit after its own
    // metrics were applied. Reapply the child layout geometry at this
    // synchronous boundary so callers never observe the previous row extent.
    const auto activateRowLayout = [](QWidget* row) {
        if (row == nullptr || row->layout() == nullptr) {
            return;
        }
        row->layout()->setGeometry(row->contentsRect());
        row->layout()->activate();
    };
    activateRowLayout(m_mainPanel);
    activateRowLayout(m_selectActionPanel);
    activateRowLayout(m_rectangleStylePanel);
}

QSize ScreenshotToolPalette::styleToolbarSizeHint() {
    if (m_rectangleStylePanel == nullptr || m_rectangleStyleLayout == nullptr) {
        return {};
    }

    QSize controlsSize;
    if (m_activeStyleControlsWidget != nullptr) {
        if (m_activeStyleControlsWidget->layout() != nullptr) {
            m_activeStyleControlsWidget->layout()->activate();
            controlsSize = m_activeStyleControlsWidget->layout()->sizeHint();
        } else {
            controlsSize = m_activeStyleControlsWidget->sizeHint();
        }
    }

    const QMargins margins = m_rectangleStyleLayout->contentsMargins();
    const QSize intrinsicSize =
        controlsSize + QSize(margins.left() + margins.right(), margins.top() + margins.bottom());
    return intrinsicSize;
}

QSize ScreenshotToolPalette::maximumSecondaryToolbarSizeHint() const {
    if (m_rectangleStylePanel == nullptr || m_rectangleStyleLayout == nullptr) {
        return {};
    }

    QSize controlsSize;
    const QWidget* controlGroups[] = {
        m_rectangleStyleControlsWidget,    m_arrowStyleControlsWidget,
        m_highlightStyleControlsWidget,    m_penHighlightStyleControlsWidget,
        m_spotlightStyleControlsWidget,    m_textStyleControlsWidget,
        m_serialNumberStyleControlsWidget, m_watermarkStyleControlsWidget,
    };
    for (const QWidget* group : controlGroups) {
        if (group == nullptr) {
            continue;
        }

        if (group->layout() != nullptr) {
            group->layout()->activate();
        }
        controlsSize = controlsSize.expandedTo(group->sizeHint());
    }

    const QMargins margins = m_rectangleStyleLayout->contentsMargins();
    return controlsSize + QSize(margins.left() + margins.right(), margins.top() + margins.bottom());
}

int ScreenshotToolPalette::scaledMetric(int value) const {
    if (value <= 0) {
        return 0;
    }
    return qMax(1, qRound(static_cast<qreal>(value) * m_physicalScale));
}

qreal ScreenshotToolPalette::scaledMetric(qreal value) const {
    return value * m_physicalScale;
}

QMargins ScreenshotToolPalette::scaledMargins(int left, int top, int right, int bottom) const {
    return QMargins(scaledMetric(left), scaledMetric(top), scaledMetric(right),
                    scaledMetric(bottom));
}

QMargins ScreenshotToolPalette::scaledPanelMargins(int horizontalMargin, int verticalMargin,
                                                   int baseContentHeight) const {
    const int horizontal = scaledMetric(horizontalMargin);
    const int contentHeight = scaledMetric(baseContentHeight);
    const int totalHeight = scaledMetric(baseContentHeight + verticalMargin * 2);
    const int top = std::min(scaledMetric(verticalMargin),
                             std::max(0, totalHeight - contentHeight));
    const int bottom = std::max(0, totalHeight - contentHeight - top);
    return QMargins(horizontal, top, horizontal, bottom);
}

void ScreenshotToolPalette::addMainToolbarSpacing(int baseSpacing) {
    if (m_mainPanel != nullptr) {
        m_mainPanel->addSpacing(baseSpacing);
    }
}

void ScreenshotToolPalette::addMainToolbarSeparator() {
    if (m_mainPanel != nullptr) {
        m_mainPanel->addSeparator();
    }
}

QSpacerItem* ScreenshotToolPalette::addStyleToolbarSpacing(QBoxLayout* layout, int baseSpacing) {
    if (layout == nullptr) {
        return nullptr;
    }

    auto* spacer =
        new QSpacerItem(scaledMetric(baseSpacing), 0, QSizePolicy::Fixed, QSizePolicy::Minimum);
    layout->addSpacerItem(spacer);
    m_styleSpacingItems.push_back(SpacingItem{spacer, layout->parentWidget(), baseSpacing});
    return spacer;
}

QSpacerItem* ScreenshotToolPalette::insertStyleToolbarSpacing(QBoxLayout* layout, int index,
                                                              int baseSpacing) {
    if (layout == nullptr) {
        return nullptr;
    }

    auto* spacer =
        new QSpacerItem(scaledMetric(baseSpacing), 0, QSizePolicy::Fixed, QSizePolicy::Minimum);
    layout->insertSpacerItem(index, spacer);
    m_styleSpacingItems.push_back(SpacingItem{spacer, layout->parentWidget(), baseSpacing});
    return spacer;
}

void ScreenshotToolPalette::setStyleToolbarSpacingVisible(QSpacerItem* spacer, bool visible) {
    if (spacer == nullptr) {
        return;
    }

    for (SpacingItem& item : m_styleSpacingItems) {
        if (item.item != spacer) {
            continue;
        }

        item.visible = visible;
        spacer->changeSize(visible ? scaledMetric(item.baseSpacing) : 0, 0, QSizePolicy::Fixed,
                           QSizePolicy::Minimum);
        if (m_rectangleStyleControlsWidget != nullptr) {
            if (QLayout* layout = m_rectangleStyleControlsWidget->layout()) {
                layout->invalidate();
            }
            m_rectangleStyleControlsWidget->updateGeometry();
        }
        return;
    }
}

QFrame* ScreenshotToolPalette::createStyleToolbarSeparator(QWidget* parent) {
    auto* separator = new QFrame(parent);
    separator->setFrameShape(QFrame::NoFrame);
    separator->setFixedSize(scaledMetric(TOOLBAR_SEPARATOR_WIDTH),
                            scaledMetric(TOOLBAR_SEPARATOR_HEIGHT));
    separator->setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Fixed);
    separator->setStyleSheet(styleToolbarSeparatorStyleSheet());
    m_styleSeparatorFrames.push_back(separator);
    return separator;
}

void ScreenshotToolPalette::updatePanelMetrics(QFrame* panel) {
    if (panel == nullptr) {
        return;
    }

    updatePanelStyle(panel);
    if (auto* shadow = qobject_cast<QGraphicsDropShadowEffect*>(panel->graphicsEffect())) {
        shadow->setBlurRadius(scaledMetric(TOOLBAR_SHADOW_BLUR_RADIUS));
        shadow->setOffset(scaledMetric(TOOLBAR_SHADOW_OFFSET_X),
                          scaledMetric(TOOLBAR_SHADOW_OFFSET_Y));
    }
}

void ScreenshotToolPalette::updatePanelStyle(QFrame* panel) {
    auto* toolbarPanel = dynamic_cast<ScreenshotToolbarPanel*>(panel);
    if (toolbarPanel == nullptr) {
        return;
    }
    toolbarPanel->setPanelRadius(scaledMetric(TOOLBAR_PANEL_RADIUS));
}

void ScreenshotToolPalette::applyScaledToolbarMetrics() {
    m_shadowMargins = scaledMargins(m_baseShadowMargins.left(), m_baseShadowMargins.top(),
                                    m_baseShadowMargins.right(), m_baseShadowMargins.bottom());

    if (m_mainPanel != nullptr) {
        m_mainPanel->setPhysicalScale(m_physicalScale);
    }
    for (const DrawingToolGroup& group : std::as_const(m_drawingToolGroups)) {
        if (group.ownsTrigger) {
            configureScreenshotToolPaletteBaseButton(group.trigger, nullptr,
                                                     actionButtonMetrics(m_physicalScale));
        }
        configureScreenshotToolPaletteOptionPopoverEditor(
            group.popover, group.optionButtons, TOOLBAR_ITEM_SPACING, actionButtonMetrics(1.0));
    }
    const ScreenshotToolPaletteButtonMetrics popupMetrics = actionButtonMetrics(1.0);
    configureScreenshotToolPaletteOptionPopoverEditor(m_tableQrPopover, m_tableQrOptionButtons,
                                                      TOOLBAR_ITEM_SPACING, popupMetrics);

    applyStyleMetricsForScope(m_activeStyleControlsWidget);

    if (m_selectActionPanel != nullptr) {
        const auto metrics = actionButtonMetrics(m_physicalScale);
        for (adqt::widgets::AdButton* button :
             m_selectActionPanel->findChildren<adqt::widgets::AdButton*>()) {
            if (button != nullptr) {
                const QByteArray tooltip = button->toolTip().toUtf8();
                configureScreenshotToolPaletteStyleButton(button, tooltip.constData(), metrics);
            }
        }
        for (adqt::widgets::AdButton* button :
             {m_scrollingVerticalButton, m_scrollingHorizontalButton}) {
            configureScreenshotToolPaletteStyleButton(button, nullptr, metrics);
        }
        if (m_scrollingRecognitionControls != nullptr &&
            m_scrollingRecognitionControls->layout() != nullptr) {
            m_scrollingRecognitionControls->layout()->setSpacing(scaledMetric(STYLE_ITEM_SPACING));
        }
        ScreenshotToolPaletteSliderEditor opacityEditor;
        opacityEditor.icon = m_selectionOpacityIcon;
        opacityEditor.slider = m_selectionOpacitySlider;
        opacityEditor.iconRef = custom_outlined_icons::Opacity();
        opacityEditor.baseIconSize = STYLE_ICON_SIZE;
        opacityEditor.baseSliderWidth = COMPACT_SLIDER_WIDTH;
        configureScreenshotToolPaletteSliderEditor(opacityEditor, metrics);
        updateSelectionOpacityIcon();
    }

    if (m_rectangleStyleLayout != nullptr) {
        m_rectangleStyleLayout->setContentsMargins(scaledPanelMargins(
            STYLE_PANEL_HORIZONTAL_MARGIN, STYLE_PANEL_VERTICAL_MARGIN, STYLE_BUTTON_SIZE));
        m_rectangleStyleLayout->setSpacing(0);
        m_rectangleStyleLayout->invalidate();
    }
    if (m_selectActionLayout != nullptr) {
        m_selectActionLayout->setContentsMargins(scaledPanelMargins(
            TOOLBAR_PANEL_HORIZONTAL_MARGIN, TOOLBAR_PANEL_VERTICAL_MARGIN, 32));
        m_selectActionLayout->setSpacing(0);
        m_selectActionLayout->invalidate();
    }

    for (QFrame* separator : std::as_const(m_styleSeparatorFrames)) {
        if (separator != nullptr && m_selectActionPanel != nullptr &&
            (separator->parentWidget() == m_selectActionPanel ||
             m_selectActionPanel->isAncestorOf(separator))) {
            separator->setFixedSize(scaledMetric(TOOLBAR_SEPARATOR_WIDTH),
                                    scaledMetric(TOOLBAR_SEPARATOR_HEIGHT));
        }
    }
    for (const SpacingItem& item : std::as_const(m_styleSpacingItems)) {
        if (item.item != nullptr && item.owner != nullptr && m_selectActionPanel != nullptr &&
            (item.owner == m_selectActionPanel || m_selectActionPanel->isAncestorOf(item.owner))) {
            item.item->changeSize(item.visible ? scaledMetric(item.baseSpacing) : 0, 0,
                                  QSizePolicy::Fixed, QSizePolicy::Minimum);
        }
    }
    applyCumulativeStyleLayoutMetrics(m_selectActionPanel);

    for (QFrame* panel : m_panelFrames) {
        updatePanelMetrics(panel);
    }

    updateRecordingControlMetrics();
    updateToolbarGeometry();
}

void ScreenshotToolPalette::applyStyleMetricsForScope(QWidget* scope) {
    if (scope == nullptr || m_styleMetricRevisions.value(scope) == m_metricProfileRevision) {
        return;
    }

    ScreenshotToolPaletteButtonMetrics metrics = styleButtonMetrics(m_physicalScale);
    metrics.scope = scope;
    if (m_styleControls != nullptr) {
        m_styleControls->refreshToolbarMetrics(metrics);
    }
    for (adqt::widgets::AdRadioButtonGroup* group : m_highlightModeGroups) {
        configureScreenshotToolPaletteStyleRadioButtonGroup(group, metrics);
    }
    for (adqt::widgets::AdRadioButtonGroup* group : m_filterModeGroups) {
        configureScreenshotToolPaletteStyleRadioButtonGroup(group, metrics);
    }

    if (scope == m_filterStyleControlsWidget) {
        refreshFilterEditorMetrics(m_filterEditor);
    }
    if (scope == m_penFilterStyleControlsWidget) {
        refreshFilterEditorMetrics(m_penFilterEditor);
    }
    if (scope == m_spotlightStyleControlsWidget) {
        ScreenshotToolPaletteSliderEditor spotlightEditor;
        spotlightEditor.icon = m_spotlightOpacityIcon;
        spotlightEditor.slider = m_spotlightOpacitySlider;
        spotlightEditor.iconRef = custom_outlined_icons::Opacity();
        spotlightEditor.baseIconSize = COMPACT_SLIDER_ICON_SIZE;
        spotlightEditor.baseSliderWidth = COMPACT_SLIDER_WIDTH;
        configureScreenshotToolPaletteSliderEditor(spotlightEditor, metrics);
    }

    for (QFrame* separator : std::as_const(m_styleSeparatorFrames)) {
        if (separator != nullptr && scope->isAncestorOf(separator)) {
            separator->setFixedSize(scaledMetric(TOOLBAR_SEPARATOR_WIDTH),
                                    scaledMetric(TOOLBAR_SEPARATOR_HEIGHT));
        }
    }
    for (const SpacingItem& item : std::as_const(m_styleSpacingItems)) {
        if (item.item != nullptr && item.owner != nullptr &&
            (item.owner == scope || scope->isAncestorOf(item.owner))) {
            item.item->changeSize(item.visible ? scaledMetric(item.baseSpacing) : 0, 0,
                                  QSizePolicy::Fixed, QSizePolicy::Minimum);
        }
    }
    if (QBoxLayout* layout = qobject_cast<QBoxLayout*>(scope->layout())) {
        layout->invalidate();
    }
    applyCumulativeStyleLayoutMetrics(scope);
    m_styleMetricRevisions.insert(scope, m_metricProfileRevision);
}

void ScreenshotToolPalette::initializeStyleLayoutProfiles() {
    for (QBoxLayout* layout : std::as_const(m_styleControlLayouts)) {
        if (layout == nullptr) {
            continue;
        }

        auto existingProfile = std::find_if(
            m_styleLayoutProfiles.begin(), m_styleLayoutProfiles.end(),
            [layout](const StyleLayoutProfile& profile) { return profile.layout == layout; });
        const qsizetype managedItemCount =
            existingProfile == m_styleLayoutProfiles.end()
                ? 0
                : existingProfile->segments.size() + existingProfile->automaticGaps.size();
        if (existingProfile != m_styleLayoutProfiles.end() &&
            static_cast<qsizetype>(layout->count()) == managedItemCount) {
            continue;
        }

        layout->activate();
        const QMargins margins = layout->contentsMargins();
        const qreal referenceScale = std::max<qreal>(0.01, m_physicalScale);
        const int originalReferenceWidth = qMax(
            0, qRound(static_cast<qreal>(
                          layout->sizeHint().width() - margins.left() - margins.right()) /
                      referenceScale));
        const int originalSpacing =
            existingProfile != m_styleLayoutProfiles.end()
                ? existingProfile->referenceAutomaticSpacing
                : qMax(0, qRound(static_cast<qreal>(layout->spacing()) / referenceScale));
        const QVector<StyleLayoutSegment> previousSegments =
            existingProfile != m_styleLayoutProfiles.end()
                ? existingProfile->segments
                : QVector<StyleLayoutSegment>{};
        const QVector<QSpacerItem*> previousAutomaticGaps =
            existingProfile != m_styleLayoutProfiles.end()
                ? existingProfile->automaticGaps
                : QVector<QSpacerItem*>{};

        QVector<QLayoutItem*> items;
        while (layout->count() > 0) {
            if (QLayoutItem* item = layout->takeAt(0)) {
                if (previousAutomaticGaps.contains(item->spacerItem())) {
                    delete item;
                    continue;
                }
                items.append(item);
            }
        }
        if (existingProfile != m_styleLayoutProfiles.end()) {
            m_styleLayoutProfiles.erase(existingProfile);
        }

        StyleLayoutProfile profile;
        profile.layout = layout;
        profile.owner = layout->parentWidget();
        profile.referenceAutomaticSpacing = originalSpacing;
        profile.segments.reserve(items.size());
        profile.automaticGaps.reserve(std::max<qsizetype>(0, items.size() - 1));
        layout->setSpacing(0);
        for (qsizetype index = 0; index < items.size(); ++index) {
            QLayoutItem* item = items.at(index);
            StyleLayoutSegment segment;
            segment.widget = item->widget();
            segment.spacer = item->spacerItem();

            const auto previousSegment = std::find_if(
                previousSegments.cbegin(), previousSegments.cend(),
                [&segment](const StyleLayoutSegment& candidate) {
                    return (segment.widget != nullptr && candidate.widget == segment.widget) ||
                           (segment.spacer != nullptr && candidate.spacer == segment.spacer);
                });
            if (previousSegment != previousSegments.cend()) {
                segment.referenceWidth = previousSegment->referenceWidth;
            } else if (segment.spacer != nullptr) {
                const auto spacing = std::find_if(
                    m_styleSpacingItems.cbegin(), m_styleSpacingItems.cend(),
                    [&segment](const SpacingItem& candidate) {
                        return candidate.item == segment.spacer;
                    });
                segment.referenceWidth =
                    spacing != m_styleSpacingItems.cend()
                        ? spacing->baseSpacing
                        : qMax(0, qRound(static_cast<qreal>(item->sizeHint().width()) /
                                         referenceScale));
            } else if (m_styleSeparatorFrames.contains(
                           qobject_cast<QFrame*>(segment.widget))) {
                segment.referenceWidth = TOOLBAR_SEPARATOR_WIDTH;
            } else {
                segment.referenceWidth = qMax(
                    0, qRound(static_cast<qreal>(item->sizeHint().width()) / referenceScale));
            }
            if (segment.referenceWidth == 0 && segment.widget != nullptr) {
                const QSize widgetHint = segment.widget->sizeHint()
                                             .expandedTo(segment.widget->minimumSizeHint())
                                             .boundedTo(segment.widget->maximumSize())
                                             .expandedTo(segment.widget->minimumSize());
                segment.referenceWidth = qMax(
                    0, qRound(static_cast<qreal>(widgetHint.width()) / referenceScale));
            }
            profile.segments.append(segment);
            layout->addItem(item);

            if (profile.referenceAutomaticSpacing > 0 && index + 1 < items.size()) {
                auto* gap = new QSpacerItem(STYLE_ITEM_SPACING, 0, QSizePolicy::Fixed,
                                            QSizePolicy::Minimum);
                layout->addSpacerItem(gap);
                profile.automaticGaps.append(gap);
            }
        }
        int representedReferenceWidth = 0;
        bool hasActiveWidget = false;
        for (const StyleLayoutSegment& segment : std::as_const(profile.segments)) {
            bool active = segment.widget == nullptr || !segment.widget->isHidden();
            if (segment.spacer != nullptr) {
                const auto spacing = std::find_if(
                    m_styleSpacingItems.cbegin(), m_styleSpacingItems.cend(),
                    [&segment](const SpacingItem& candidate) {
                        return candidate.item == segment.spacer;
                    });
                active = spacing == m_styleSpacingItems.cend() || spacing->visible;
            }
            if (!active || segment.referenceWidth <= 0) {
                continue;
            }
            if (segment.widget != nullptr) {
                if (hasActiveWidget) {
                    representedReferenceWidth += profile.referenceAutomaticSpacing;
                }
                hasActiveWidget = true;
            }
            representedReferenceWidth += segment.referenceWidth;
        }
        profile.referenceWidthAdjustment = originalReferenceWidth - representedReferenceWidth;
        m_styleLayoutProfiles.append(std::move(profile));
    }
}
void ScreenshotToolPalette::applyCumulativeStyleLayoutMetrics(QWidget* scope) {
    if (scope == nullptr) {
        return;
    }

    for (StyleLayoutProfile& profile : m_styleLayoutProfiles) {
        if (profile.layout == nullptr || profile.owner != scope) {
            continue;
        }

        QVector<bool> activeSegments;
        activeSegments.reserve(profile.segments.size());
        for (const StyleLayoutSegment& segment : std::as_const(profile.segments)) {
            bool active = segment.widget == nullptr || !segment.widget->isHidden();
            if (segment.spacer != nullptr) {
                const auto spacing = std::find_if(
                    m_styleSpacingItems.cbegin(), m_styleSpacingItems.cend(),
                    [&segment](const SpacingItem& candidate) {
                        return candidate.item == segment.spacer;
                    });
                active = spacing == m_styleSpacingItems.cend() || spacing->visible;
            }
            activeSegments.append(active && segment.referenceWidth > 0);
        }

        QVector<int> referenceWidths;
        referenceWidths.reserve(profile.segments.size() + profile.automaticGaps.size());
        bool hasActiveWidget = false;
        for (qsizetype index = 0; index < profile.segments.size(); ++index) {
            referenceWidths.append(activeSegments.at(index)
                                       ? profile.segments.at(index).referenceWidth
                                       : 0);
            hasActiveWidget =
                hasActiveWidget ||
                (activeSegments.at(index) && profile.segments.at(index).widget != nullptr);
            if (index < profile.automaticGaps.size()) {
                const StyleLayoutSegment& next = profile.segments.at(index + 1);
                const bool nextIsActiveWidget =
                    activeSegments.at(index + 1) && next.widget != nullptr;
                referenceWidths.append(hasActiveWidget && nextIsActiveWidget
                                           ? profile.referenceAutomaticSpacing
                                           : 0);
            }
        }

        const int activeReferenceWidth =
            std::accumulate(referenceWidths.cbegin(), referenceWidths.cend(), 0) +
            profile.referenceWidthAdjustment;
        const int targetWidth =
            qMax(0, qRound(std::max(0, activeReferenceWidth) * m_physicalScale));
        const QVector<int> edges = adqt::widgets::scaleCumulativeWidths(
            referenceWidths, m_physicalScale, targetWidth);
        qsizetype edgeIndex = 0;
        for (qsizetype index = 0; index < profile.segments.size(); ++index) {
            const int activeWidth = edges.at(edgeIndex + 1) - edges.at(edgeIndex);
            StyleLayoutSegment& segment = profile.segments[index];
            if (segment.widget != nullptr) {
                const int standaloneWidth =
                    qMax(1, qRound(segment.referenceWidth * m_physicalScale));
                const bool isSeparator =
                    m_styleSeparatorFrames.contains(qobject_cast<QFrame*>(segment.widget));
                segment.widget->setFixedWidth(
                    activeSegments.at(index) ? (isSeparator ? qMax(1, activeWidth) : activeWidth)
                                             : standaloneWidth);
            } else if (segment.spacer != nullptr) {
                segment.spacer->changeSize(activeWidth, 0, QSizePolicy::Fixed,
                                           QSizePolicy::Minimum);
            }
            ++edgeIndex;

            if (index < profile.automaticGaps.size()) {
                const int gapWidth = edges.at(edgeIndex + 1) - edges.at(edgeIndex);
                profile.automaticGaps.at(index)->changeSize(
                    gapWidth, 0, QSizePolicy::Fixed, QSizePolicy::Minimum);
                ++edgeIndex;
            }
        }
        profile.layout->invalidate();
        return;
    }
}

void ScreenshotToolPalette::installWheelFilters(QObject* receiver, QWidget* scope) {
    auto* widget = qobject_cast<QWidget*>(receiver);
    if (widget == nullptr) {
        return;
    }

    const auto installRecursive = [&](auto&& self, QWidget* current) -> void {
        if (current == nullptr) {
            return;
        }

        if (current != widget) {
            current->installEventFilter(receiver);
        }

        const QList<QWidget*> childWidgets =
            current->findChildren<QWidget*>(QString(), Qt::FindDirectChildrenOnly);
        for (QWidget* child : childWidgets) {
            self(self, child);
        }
    };

    installRecursive(installRecursive, scope != nullptr ? scope : this);
}

bool ScreenshotToolPalette::handleToolbarWheel(QWheelEvent* event) {
    const int deltaY = wheelVerticalDelta(event);
    if (deltaY == 0 || (!m_styleToolbarTargetVisible && !m_actionToolbarTargetVisible)) {
        return false;
    }

    const int direction = deltaY > 0 ? 1 : -1;
    if (m_activeTool == Tool::Select) {
        if (!stepSelectionOpacity(direction)) {
            return false;
        }
        event->accept();
        return true;
    }
    if (m_activeTool == Tool::Move) {
        return false;
    }
    if (m_activeTool == Tool::Spotlight) {
        if (m_spotlightOpacitySlider == nullptr || !m_spotlightOpacitySlider->isEnabled()) {
            return false;
        }
        const QRect sliderRect(m_spotlightOpacitySlider->mapToGlobal(QPoint(0, 0)),
                               m_spotlightOpacitySlider->size());
        if (!sliderRect.contains(event->globalPosition().toPoint())) {
            return false;
        }
        static_cast<void>(stepSpotlightOpacity(direction));
        event->accept();
        return true;
    }
    if (m_activeTool == Tool::RectangleFilter) {
        if (!stepFilterIntensity(direction)) {
            return false;
        }
        event->accept();
        return true;
    }
    if (m_activeTool == Tool::PenFilter) {
        static_cast<void>(stepPenFilterStrokeWidth(direction));
        event->accept();
        return true;
    }
    if (m_watermarkStyleControlsWidget != nullptr && m_watermarkStyleControlsWidget->isVisible()) {
        if (m_styleControls->handleWatermarkWheel(event->globalPosition().toPoint(), direction)) {
            event->accept();
            return true;
        }
        return false;
    }
    if (m_textStyleControlsWidget != nullptr && m_textStyleControlsWidget->isVisible()) {
        if (m_styleControls->handleTextCornerRadiusWheel(event->globalPosition().toPoint(),
                                                         direction) ||
            m_styleControls->handleTextStrokeWidthWheel(event->globalPosition().toPoint(),
                                                        direction) ||
            m_styleControls->stepTextFontSize(direction)) {
            event->accept();
            return true;
        }
        return false;
    }
    if (m_serialNumberStyleControlsWidget != nullptr &&
        m_serialNumberStyleControlsWidget->isVisible()) {
        if (m_styleControls->handleSerialNumberWheel(event->globalPosition().toPoint(),
                                                     direction)) {
            event->accept();
            return true;
        }
        return false;
    }
    if (m_styleControls->handleCornerRadiusWheel(event->globalPosition().toPoint(), direction)) {
        event->accept();
        return true;
    }

    if (!stepStrokeWidth(direction)) {
        return false;
    }

    event->accept();
    return true;
}

bool ScreenshotToolPalette::eventFilter(QObject* watched, QEvent* event) {
    if (event != nullptr &&
        (event->type() == QEvent::Enter || event->type() == QEvent::HoverEnter)) {
        auto* trigger = qobject_cast<adqt::widgets::AdButton*>(watched);
        if (trigger == m_tableButton && m_tableQrPopover != nullptr &&
            m_tableQrPopover->contentWidget() == nullptr) {
            ensureTableQrPopover();
        } else if (trigger != nullptr) {
            ensureDrawingToolGroupPopover(trigger);
        }
    }
    if (event != nullptr && event->type() == QEvent::Wheel &&
        handleToolbarWheel(static_cast<QWheelEvent*>(event))) {
        return true;
    }
    return QWidget::eventFilter(watched, event);
}

void ScreenshotToolPalette::changeEvent(QEvent* event) {
    if (event != nullptr && event->type() == QEvent::LanguageChange) {
        retranslateUi();
    }
    QWidget::changeEvent(event);
}

void ScreenshotToolPalette::retranslateUi() {
    retranslateScreenshotToolPalette(this);
    if (m_mainPanel != nullptr) {
        retranslateScreenshotToolPalette(m_mainPanel);
        for (adqt::widgets::AdButton* button :
             m_mainPanel->findChildren<adqt::widgets::AdButton*>()) {
            if (button == nullptr) {
                continue;
            }
            const QString source =
                button->property("snowShotDrawingShortcutTooltipSource").toString();
            if (!source.isEmpty()) {
                applyDrawingShortcutTooltip(
                    button, source,
                    button->property("screenshotToolbarItemId").toString());
            }
            const QString screenshotSource =
                button->property("snowShotScreenshotShortcutTooltipSource").toString();
            const QString screenshotActionId =
                button->property("snowShotScreenshotShortcutTooltipActionId").toString();
            if (!screenshotSource.isEmpty() && !screenshotActionId.isEmpty()) {
                applyScreenshotShortcutTooltip(button, screenshotSource, screenshotActionId);
            }
        }
    }
    for (int groupIndex = 0; groupIndex < m_drawingToolGroups.size(); ++groupIndex) {
        refreshDrawingToolGroup(groupIndex);
    }
    if (m_tableButton != nullptr) {
        refreshTableQrTrigger();
    }
    if (m_selectionOpacitySlider != nullptr) {
        m_selectionOpacitySlider->setAccessibleDescription(
            m_selectionOpacityMixed
                ? tr("Mixed")
                : QStringLiteral("%1%").arg(qRound(m_selectionOpacity * 100.0)));
    }
    if (m_recordDurationLabel != nullptr) {
        m_recordDurationLabel->setAccessibleName(tr("Recording duration"));
    }
    refreshConfirmShortcutHint();
}

void ScreenshotToolPalette::refreshConfirmShortcutHint() {
    if (!m_options.showDrawingModeShortcutOnConfirm || m_confirmButton == nullptr) {
        return;
    }
    applyPinToScreenShortcutTooltip(m_confirmButton, QStringLiteral("Confirm edit"),
                                    QStringLiteral("drawing_mode"));
}

void ScreenshotToolPalette::paintEvent(QPaintEvent* event) {
    Q_UNUSED(event);

    QPainter painter(this);
    painter.setCompositionMode(QPainter::CompositionMode_Source);
    painter.fillRect(rect(), Qt::transparent);
}

void ScreenshotToolPalette::wheelEvent(QWheelEvent* event) {
    if (handleToolbarWheel(event)) {
        return;
    }
    QWidget::wheelEvent(event);
}

adqt::widgets::AdButton* ScreenshotToolPalette::addToolButton(const char* tooltip,
                                                              const adqt::icons::IconRef& iconRef) {
    if (m_mainPanel == nullptr) {
        return nullptr;
    }

    auto* button = m_mainPanel->createToolButton(tooltip, iconRef);
    applyDrawingShortcutTooltip(button, QString::fromUtf8(tooltip != nullptr ? tooltip : ""));
    return button;
}

adqt::widgets::AdButton* ScreenshotToolPalette::addActionButton(const char* tooltip,
                                                                const adqt::icons::IconRef& iconRef,
                                                                bool danger, bool primary) {
    return m_mainPanel != nullptr
               ? m_mainPanel->createActionButton(tooltip, iconRef, danger, primary)
               : nullptr;
}

void ScreenshotToolPalette::createMainToolbar(const Options& options) {
    ScreenshotToolbarMainPanel::Options panelOptions;
    panelOptions.showDragHandle = options.showDragHandle;
    m_mainPanel = new ScreenshotToolbarMainPanel(panelOptions, this);
    m_mainPanel->setPhysicalScale(m_physicalScale);
    QBoxLayout* panelLayout = m_mainPanel->contentLayout();

    const bool hasEditingTools = addMainToolButtons(options, panelLayout);
    const bool hasSecondaryTools =
        options.showScreenRecordButton || options.showOcrTool || options.showTextTranslationTool ||
        options.showTableTool ||
        options.showQrTool || options.showScrollingScreenshotTool ||
        (options.showSaveButton && !options.saveButtonWithResultActions) ||
        (!options.showRecordingControls && (options.actions & PinAction) != 0);
    if (hasEditingTools && hasSecondaryTools) {
        addMainToolbarSeparator();
    }
    addMainSecondaryButtons(options, panelLayout);
    if (options.showRecordingControls) {
        addRecordingControls(panelLayout);
    } else {
        const bool hasResultActions =
            (options.actions & (CancelAction | CopyAction)) != 0 ||
            (options.showSaveButton && options.saveButtonWithResultActions) ||
            ((options.actions & ConfirmAction) != 0 &&
             (options.separatorBeforeConfirm || (options.actions & PinAction) != 0));
        if ((hasEditingTools || hasSecondaryTools) && hasResultActions) {
            addMainToolbarSeparator();
        }
        addMainActionButtons(options, panelLayout);
    }

    if (options.showTrailingDragHandle) {
        m_mainPanel->addTrailingDragHandle();
    }

    if (m_rootLayout != nullptr) {
        m_rootLayout->addWidget(m_mainPanel, 0, Qt::AlignRight);
    }
    if (m_toolbarLayout.has_value()) {
        applyMainToolbarLayout(false);
    }
}

adqt::widgets::AdButton* ScreenshotToolPalette::drawingToolButton(const QString& itemId) const {
    const toolbar_layout::Descriptor* descriptor = toolbar_layout::descriptor(itemId);
    if (descriptor == nullptr) {
        return nullptr;
    }
    switch (descriptor->item) {
    case toolbar_layout::Item::Shape:
        return m_shapeButton;
    case toolbar_layout::Item::Arrow:
        return m_arrowButton;
    case toolbar_layout::Item::Line:
        return m_lineButton;
    case toolbar_layout::Item::FreeDraw:
        return m_freeDrawButton;
    case toolbar_layout::Item::Highlighter:
        return m_highlighterButton;
    case toolbar_layout::Item::Spotlight:
        return m_spotlightButton;
    case toolbar_layout::Item::Text:
        return m_textButton;
    case toolbar_layout::Item::SerialNumber:
        return m_serialNumberButton;
    case toolbar_layout::Item::Filter:
        return m_filterButton;
    case toolbar_layout::Item::Eraser:
        return m_eraserButton;
    case toolbar_layout::Item::Watermark:
        return m_watermarkButton;
    }
    return nullptr;
}

adqt::widgets::AdButton* ScreenshotToolPalette::drawingToolEntryButton(Tool tool) const {
    const QString itemId = drawingToolItemId(tool);
    for (const DrawingToolGroup& group : m_drawingToolGroups) {
        if (group.itemIds.contains(itemId)) {
            return group.trigger;
        }
    }
    return drawingToolButton(itemId);
}

void ScreenshotToolPalette::clearDrawingToolGroups() {
    for (const DrawingToolGroup& group : std::as_const(m_drawingToolGroups)) {
        if (group.ownsTrigger) {
            if (m_activeToolButton == group.trigger) {
                m_activeToolButton = nullptr;
            }
            delete group.trigger;
        }
    }
    m_drawingToolGroups.clear();
}

void ScreenshotToolPalette::activateDrawingTool(Tool tool) {
    setActiveTool(tool);
    switch (tool) {
    case Tool::Move:
        emit moveRequested();
        break;
    case Tool::Select:
        emit selectRequested();
        break;
    case Tool::Shape:
        emit shapeRequested();
        break;
    case Tool::Arrow:
        emit arrowRequested();
        break;
    case Tool::Line:
        emit lineRequested();
        break;
    case Tool::FreeDraw:
        emit freeDrawRequested();
        break;
    case Tool::RectangleHighlight:
        emit highlightRequested();
        break;
    case Tool::PenHighlight:
        emit penHighlightRequested();
        break;
    case Tool::Spotlight:
        emit spotlightRequested();
        break;
    case Tool::Eraser:
        emit eraserRequested();
        break;
    case Tool::RectangleFilter:
        emit rectangleFilterRequested();
        break;
    case Tool::PenFilter:
        emit penFilterRequested();
        break;
    case Tool::Watermark:
        emit watermarkRequested();
        break;
    case Tool::Text:
        emit textRequested();
        break;
    case Tool::SerialNumber:
        emit serialNumberRequested();
        break;
    case Tool::Ocr:
        emit ocrRequested();
        break;
    case Tool::TextTranslation:
        emit textTranslationRequested();
        break;
    case Tool::Table:
        emit tableRequested();
        break;
    case Tool::Qr:
        emit qrRequested();
        break;
    case Tool::ScrollingScreenshot:
        emit scrollingScreenshotRequested();
        break;
    default:
        break;
    }
}

void ScreenshotToolPalette::activateToolFromToolbar(Tool tool, bool toggleVisibleButton) {
    // Toolbar clicks are toggle-like: clicking the active drawing/action tool
    // returns to selection mode. Programmatic setActiveTool() calls remain
    // explicit so state synchronization does not unexpectedly toggle.
    adqt::widgets::AdButton* requestedButton = nullptr;
    switch (tool) {
    case Tool::Move:
        requestedButton = m_moveButton;
        break;
    case Tool::Select:
        requestedButton = m_selectButton;
        break;
    case Tool::Ocr:
        requestedButton = m_ocrButton;
        break;
    case Tool::TextTranslation:
        requestedButton = m_textTranslationButton;
        break;
    case Tool::Table:
    case Tool::Qr:
        requestedButton = m_tableButton;
        break;
    case Tool::ScrollingScreenshot:
        requestedButton = m_scrollingScreenshotButton;
        break;
    default:
        requestedButton = drawingToolEntryButton(tool);
        break;
    }
    const bool alreadyActive = !toggleVisibleButton
                                   ? m_activeTool.has_value() && *m_activeTool == tool
                               : requestedButton != nullptr
                                   ? m_activeToolButton == requestedButton
                                   : m_activeTool.has_value() && *m_activeTool == tool;
    const Tool requestedTool = alreadyActive && tool != Tool::Select ? Tool::Select : tool;
    activateDrawingTool(requestedTool);
}

void ScreenshotToolPalette::selectDrawingToolGroupEntry(Tool tool) {
    const Tool entryTool = toolbarFacingDrawingTool(tool);
    const QString itemId = drawingToolItemId(entryTool);
    for (int groupIndex = 0; groupIndex < m_drawingToolGroups.size(); ++groupIndex) {
        DrawingToolGroup& group = m_drawingToolGroups[groupIndex];
        if (!group.itemIds.contains(itemId) || group.entryTool == entryTool) {
            continue;
        }
        group.entryTool = entryTool;
        refreshDrawingToolGroup(groupIndex);
        return;
    }
}

void ScreenshotToolPalette::refreshDrawingToolGroup(int groupIndex) {
    if (groupIndex < 0 || groupIndex >= m_drawingToolGroups.size()) {
        return;
    }
    DrawingToolGroup& group = m_drawingToolGroups[groupIndex];
    const QString itemId = drawingToolItemId(group.entryTool);
    const toolbar_layout::Descriptor* descriptor = toolbar_layout::descriptor(itemId);
    if (group.trigger == nullptr || descriptor == nullptr) {
        return;
    }
    configureScreenshotToolPaletteTooltip(group.trigger, descriptor->label);
    applyDrawingShortcutTooltip(group.trigger, QString::fromUtf8(descriptor->label), itemId);
    setScreenshotToolPaletteToolButtonIcon(group.trigger, toolbar_layout::icon(descriptor->icon));
    group.trigger->setProperty("screenshotToolbarItemId", itemId);
    group.trigger->setProperty("screenshotToolbarPositionItems", group.itemIds);
    for (adqt::widgets::AdButton* optionButton : group.optionButtons) {
        if (optionButton == nullptr) {
            continue;
        }
        applyDrawingShortcutTooltip(
            optionButton,
            optionButton->property("snowShotDrawingShortcutTooltipSource").toString(),
            optionButton->property("screenshotToolbarItemId").toString());
    }
    const int activeValue = m_activeTool.has_value()
                                ? static_cast<int>(toolbarFacingDrawingTool(*m_activeTool))
                                : -1;
    updateScreenshotToolPaletteOptionPopoverEditor(group.optionButtons, group.optionValues,
                                                   activeValue);
}

void ScreenshotToolPalette::ensureDrawingToolGroupPopover(adqt::widgets::AdButton* trigger) {
    for (DrawingToolGroup& group : m_drawingToolGroups) {
        if (group.trigger != trigger || group.popover == nullptr ||
            group.popover->contentWidget() != nullptr || group.popoverConstructing) {
            continue;
        }
        group.popoverConstructing = true;
        ScreenshotToolPaletteOptionPopoverEditorConfig config;
        const QSet<QString> items(group.itemIds.cbegin(), group.itemIds.cend());
        if (items == QSet<QString>{QStringLiteral("arrow"), QStringLiteral("line")}) {
            config.contentObjectName = QStringLiteral("screenshotArrowLinePopoverContent");
        } else if (items ==
                   QSet<QString>{QStringLiteral("highlighter"), QStringLiteral("spotlight")}) {
            config.contentObjectName = QStringLiteral("screenshotHighlightPopoverContent");
        } else {
            config.contentObjectName = QStringLiteral("screenshotDrawingToolGroupPopoverContent");
        }
        config.optionSpacing = TOOLBAR_ITEM_SPACING;
        for (const QString& itemId : std::as_const(group.popoverItemIds)) {
            const toolbar_layout::Descriptor* descriptor = toolbar_layout::descriptor(itemId);
            if (descriptor == nullptr) {
                continue;
            }
            config.options.push_back({
                static_cast<int>(drawingToolFromItem(descriptor->item)),
                QString::fromUtf8(descriptor->label), toolbar_layout::icon(descriptor->icon)});
        }
        const auto editor = materializeScreenshotToolPaletteOptionPopoverEditor(
            group.popover, this, config,
            [this](int value) { activateToolFromToolbar(static_cast<Tool>(value), false); },
            actionButtonMetrics(1.0));
        group.optionButtons = editor.buttons;
        group.optionValues = editor.values;
        for (int index = 0;
             index < group.optionButtons.size() && index < group.popoverItemIds.size(); ++index) {
            auto* button = group.optionButtons.at(index);
            const QString itemId = group.popoverItemIds.at(index);
            button->setObjectName(QStringLiteral("screenshotDrawingToolGroupOption-%1").arg(itemId));
            button->setProperty("screenshotToolbarItemId", itemId);
            const toolbar_layout::Descriptor* descriptor = toolbar_layout::descriptor(itemId);
            if (descriptor != nullptr) {
                applyDrawingShortcutTooltip(button, QString::fromUtf8(descriptor->label), itemId);
            }
        }
        group.popoverConstructing = false;
        refreshDrawingToolGroup(static_cast<int>(&group - m_drawingToolGroups.data()));
        if (group.popover->contentWidget() != nullptr) {
            emit materializedScope(group.popover->contentWidget());
        }
        SNOW_SHOT_TOOLBAR_PERF_COUNTER("hydrate.group_popover");
        return;
    }
}

void ScreenshotToolPalette::ensureTableQrPopover() {
    if (m_tableQrPopover == nullptr || m_tableQrPopover->contentWidget() != nullptr) {
        return;
    }
    ScreenshotToolPaletteOptionPopoverEditorConfig config;
    config.contentObjectName = QStringLiteral("screenshotTableQrPopoverContent");
    config.optionSpacing = TOOLBAR_ITEM_SPACING;
    config.options = {
        {static_cast<int>(Tool::Table), QStringLiteral("Table recognition"),
         custom_outlined_icons::TableRecognition()},
        {static_cast<int>(Tool::Qr), QStringLiteral("Barcode recognition"),
         custom_outlined_icons::ScanQrcode()}};
    const auto editor = materializeScreenshotToolPaletteOptionPopoverEditor(
        m_tableQrPopover, this, config,
        [this](int value) { activateTableQrTool(static_cast<Tool>(value), false); },
        actionButtonMetrics(1.0));
    m_tableQrOptionButtons = editor.buttons;
    m_tableQrOptionValues = editor.values;
    m_tableOptionButton = m_tableQrOptionButtons.value(0);
    m_qrButton = m_tableQrOptionButtons.value(1);
    if (m_tableOptionButton != nullptr) {
        m_tableOptionButton->setObjectName(QStringLiteral("screenshotTableRecognitionOptionButton"));
        m_tableOptionButton->setBusyIndicatorPresentation(
            adqt::widgets::AdButton::BusyIndicatorPresentation::IsolatedSurface);
        applyScreenshotShortcutTooltip(m_tableOptionButton, QStringLiteral("Table recognition"),
                                       QStringLiteral("table_recognition"));
    }
    if (m_qrButton != nullptr) {
        m_qrButton->setObjectName(QStringLiteral("screenshotQrRecognitionOptionButton"));
        m_qrButton->setBusyIndicatorPresentation(
            adqt::widgets::AdButton::BusyIndicatorPresentation::IsolatedSurface);
        applyScreenshotShortcutTooltip(m_qrButton, QStringLiteral("Barcode recognition"),
                                       QStringLiteral("qr_code_recognition"));
    }
    updateTableQrBusy();
    updateTableQrEnabled();
    setActiveToolButton(m_activeToolButton);
    emit materializedScope(m_tableQrPopover->contentWidget());
    SNOW_SHOT_TOOLBAR_PERF_COUNTER("hydrate.table_qr_popover");
}

void ScreenshotToolPalette::applyMainToolbarLayout(bool notify) {
    if (m_mainPanel == nullptr || !m_toolbarLayout.has_value()) {
        return;
    }

    const snow_shot::storage::ScreenshotToolbarLayout normalized =
        toolbar_layout::normalizedLayout(*m_toolbarLayout);
    m_toolbarLayout = normalized;
    m_mainPanel->resetContentLayout();
    clearDrawingToolGroups();
    QBoxLayout* layout = m_mainPanel->contentLayout();
    if (layout == nullptr) {
        return;
    }

    bool hasContent = false;
    bool separated = false;
    const auto addFixedWidget = [this, layout, &hasContent, &separated](QWidget* widget) {
        if (widget == nullptr) {
            return;
        }
        if (hasContent && !separated) {
            addMainToolbarSpacing(TOOLBAR_ITEM_SPACING);
        }
        widget->show();
        layout->addWidget(widget, 0, Qt::AlignBottom);
        hasContent = true;
        separated = false;
    };
    const auto addSeparator = [this, &hasContent, &separated]() {
        if (hasContent && !separated) {
            addMainToolbarSeparator();
            separated = true;
        }
    };

    addFixedWidget(m_moveButton);
    addFixedWidget(m_selectButton);

    bool hasDrawingPositions = false;
    for (const QStringList& position : normalized.positions) {
        QStringList availableItemIds;
        QVector<Tool> tools;
        for (const QString& itemId : position) {
            const toolbar_layout::Descriptor* descriptor = toolbar_layout::descriptor(itemId);
            if (descriptor == nullptr || drawingToolButton(itemId) == nullptr) {
                continue;
            }
            availableItemIds.push_back(itemId);
            tools.push_back(drawingToolFromItem(descriptor->item));
        }
        if (tools.isEmpty()) {
            continue;
        }
        if (!hasDrawingPositions &&
            (m_options.separatorAfterSelect || m_options.separatorBeforeShape) && hasContent) {
            addSeparator();
        }
        if (hasContent && !separated) {
            addMainToolbarSpacing(TOOLBAR_ITEM_SPACING);
        }
        DrawingToolGroup group;
        group.itemIds = availableItemIds;
        group.tools = tools;
        group.entryTool = tools.constLast();
        if (tools.size() == 1) {
            group.trigger = drawingToolButton(availableItemIds.constFirst());
        } else {
            const toolbar_layout::Descriptor* entryDescriptor =
                toolbar_layout::descriptor(availableItemIds.constLast());
            if (entryDescriptor == nullptr) {
                continue;
            }
            group.trigger = createScreenshotToolPaletteToolButton(
                m_mainPanel, entryDescriptor->label, toolbar_layout::icon(entryDescriptor->icon),
                actionButtonMetrics(m_physicalScale));
            group.ownsTrigger = true;
            const QSet<QString> groupItems(availableItemIds.cbegin(), availableItemIds.cend());
            const QSet<QString> arrowLineItems{QStringLiteral("arrow"), QStringLiteral("line")};
            const QSet<QString> highlightItems{QStringLiteral("highlighter"),
                                               QStringLiteral("spotlight")};
            const bool arrowLineGroup = groupItems == arrowLineItems;
            const bool highlightGroup = groupItems == highlightItems;
            group.trigger->setObjectName(
                arrowLineGroup   ? QStringLiteral("screenshotArrowLineButton")
                : highlightGroup ? QStringLiteral("screenshotHighlightButton")
                                 : QStringLiteral("screenshotDrawingToolGroupButton%1")
                                       .arg(m_drawingToolGroups.size()));

            group.popover = createScreenshotToolPaletteOptionPopoverShell(group.trigger);
            // AdPopover intentionally suppresses open requests while it has no content. Observe
            // the trigger before the popover's hover delay elapses so the first real hover can
            // materialize the options and continue through the normal opening path.
            group.trigger->installEventFilter(this);
            for (int optionIndex = availableItemIds.size() - 1; optionIndex >= 0; --optionIndex) {
                const toolbar_layout::Descriptor* descriptor =
                    toolbar_layout::descriptor(availableItemIds.at(optionIndex));
                if (descriptor == nullptr) {
                    continue;
                }
                group.popoverItemIds.push_back(availableItemIds.at(optionIndex));
            }
            connect(group.popover, &adqt::widgets::AdPopover::visibilityRequested, this,
                    [this, trigger = group.trigger](bool visible) {
                        if (visible) {
                            ensureDrawingToolGroupPopover(trigger);
                        }
                    });
            connect(group.trigger, &adqt::widgets::AdButton::pressed, this,
                    [this, trigger = group.trigger]() {
                        for (const DrawingToolGroup& candidate :
                             std::as_const(m_drawingToolGroups)) {
                            if (candidate.trigger == trigger) {
                                activateToolFromToolbar(candidate.entryTool);
                                return;
                            }
                        }
                    });
        }
        if (group.trigger == nullptr) {
            continue;
        }
        group.trigger->show();
        layout->addWidget(group.trigger);
        m_drawingToolGroups.push_back(group);
        refreshDrawingToolGroup(m_drawingToolGroups.size() - 1);
        hasContent = true;
        separated = false;
        hasDrawingPositions = true;
    }

    if (hasContent && (m_undoButton != nullptr || m_redoButton != nullptr)) {
        addSeparator();
    }
    addFixedWidget(m_undoButton);
    addFixedWidget(m_redoButton);

    QVector<QWidget*> secondaryTools{m_tableButton, m_screenRecordButton, m_pinButton, m_ocrButton,
                                     m_textTranslationButton, m_scrollingScreenshotButton,
                                     m_options.saveButtonWithResultActions ? nullptr : m_saveButton};
    secondaryTools.erase(std::remove(secondaryTools.begin(), secondaryTools.end(), nullptr),
                         secondaryTools.end());
    if (!secondaryTools.isEmpty() && hasContent) {
        addSeparator();
    }
    for (QWidget* widget : secondaryTools) {
        addFixedWidget(widget);
    }

    QVector<QWidget*> resultActions{
        m_cancelButton,
        m_options.saveButtonWithResultActions ? m_saveButton : nullptr,
        m_copyButton,
        m_confirmButton,
    };
    resultActions.erase(std::remove(resultActions.begin(), resultActions.end(), nullptr),
                        resultActions.end());
    if (!resultActions.isEmpty() && hasContent) {
        addSeparator();
    }
    for (QWidget* widget : resultActions) {
        if (widget == m_confirmButton && m_options.separatorBeforeConfirm && !separated) {
            addSeparator();
        }
        addFixedWidget(widget);
    }
    if (m_options.showTrailingDragHandle) {
        m_mainPanel->addTrailingDragHandle();
    }

    if (m_activeTool.has_value() && !drawingToolItemId(*m_activeTool).isEmpty()) {
        selectDrawingToolGroupEntry(*m_activeTool);
        setActiveToolButton(drawingToolEntryButton(*m_activeTool));
    }

    updateToolbarGeometry();
    if (notify) {
        emit visibleContentChanged();
    }
}

bool ScreenshotToolPalette::addMainToolButtons(const Options& options, QBoxLayout* layout) {
    if (layout == nullptr) {
        return false;
    }

    bool hasButton = false;
    bool separated = false;
    const auto addButton = [this, layout, &hasButton, &separated](adqt::widgets::AdButton* button) {
        if (button == nullptr) {
            return;
        }
        if (hasButton && !separated) {
            addMainToolbarSpacing(TOOLBAR_ITEM_SPACING);
        }
        layout->addWidget(button);
        hasButton = true;
        separated = false;
    };
    const auto addSeparator = [this, &hasButton, &separated]() {
        if (hasButton && !separated) {
            addMainToolbarSeparator();
            separated = true;
        }
    };

    if (options.showMoveTool) {
        m_moveButton = addToolButton("Edit selection", custom_outlined_icons::ToolMove());
        addButton(m_moveButton);
        connect(m_moveButton, &adqt::widgets::AdButton::clicked, this, [this]() {
            activateToolFromToolbar(Tool::Move);
        });
    }

    if (options.showSelectTool) {
        m_selectButton = addToolButton("Select elements", custom_outlined_icons::ToolSelect());
        addButton(m_selectButton);
        connect(m_selectButton, &adqt::widgets::AdButton::clicked, this, [this]() {
            activateToolFromToolbar(Tool::Select);
        });
    }

    if (options.showShapeTool && (options.separatorAfterSelect || options.separatorBeforeShape)) {
        addSeparator();
    }

    if (options.showShapeTool) {
        m_shapeButton = addToolButton("Shape", custom_outlined_icons::ToolRectangle());
        addButton(m_shapeButton);
        connect(m_shapeButton, &adqt::widgets::AdButton::clicked, this, [this]() {
            activateToolFromToolbar(Tool::Shape);
        });
    }

    if (options.showArrowTool) {
        m_arrowButton = addToolButton("Arrow", custom_outlined_icons::ToolArrow());
        m_arrowButton->setObjectName(QStringLiteral("screenshotArrowButton"));
        addButton(m_arrowButton);
        connect(m_arrowButton, &adqt::widgets::AdButton::clicked, this, [this]() {
            activateToolFromToolbar(Tool::Arrow);
        });
    }
    if (options.showLineTool) {
        m_lineButton = addToolButton("Line", custom_outlined_icons::ToolLine());
        m_lineButton->setObjectName(QStringLiteral("screenshotLineButton"));
        addButton(m_lineButton);
        connect(m_lineButton, &adqt::widgets::AdButton::clicked, this, [this]() {
            activateToolFromToolbar(Tool::Line);
        });
    }

    if (options.showFreeDrawTool) {
        m_freeDrawButton = addToolButton("Pen", custom_outlined_icons::ToolFreeDraw());
        addButton(m_freeDrawButton);
        connect(m_freeDrawButton, &adqt::widgets::AdButton::clicked, this, [this]() {
            activateToolFromToolbar(Tool::FreeDraw);
        });
    }

    if (options.showHighlightTool || options.showRectangleHighlightTool ||
        options.showPenHighlightTool) {
        m_highlighterButton =
            addToolButton("Highlight", custom_outlined_icons::ToolHighlight());
        m_highlighterButton->setObjectName(QStringLiteral("screenshotHighlighterButton"));
        addButton(m_highlighterButton);
        connect(m_highlighterButton, &adqt::widgets::AdButton::clicked, this, [this]() {
            activateToolFromToolbar(Tool::PenHighlight);
        });
    }
    if (options.showSpotlightTool) {
        m_spotlightButton = addToolButton("Spotlight", custom_outlined_icons::ToolSpotlight());
        m_spotlightButton->setObjectName(QStringLiteral("screenshotSpotlightButton"));
        addButton(m_spotlightButton);
        connect(m_spotlightButton, &adqt::widgets::AdButton::clicked, this, [this]() {
            activateToolFromToolbar(Tool::Spotlight);
        });
    }

    if (options.showTextTool) {
        m_textButton = addToolButton("Text", custom_outlined_icons::ToolText());
        addButton(m_textButton);
        connect(m_textButton, &adqt::widgets::AdButton::clicked, this, [this]() {
            activateToolFromToolbar(Tool::Text);
        });
    }

    if (options.showSerialNumberTool) {
        m_serialNumberButton =
            addToolButton("Serial number", custom_outlined_icons::ToolSerialNumber());
        addButton(m_serialNumberButton);
        connect(m_serialNumberButton, &adqt::widgets::AdButton::clicked, this, [this]() {
            activateToolFromToolbar(Tool::SerialNumber);
        });
    }

    if (options.showFilterTool) {
        m_filterButton = addToolButton("Filter", custom_outlined_icons::ToolFilter());
        addButton(m_filterButton);
        connect(m_filterButton, &adqt::widgets::AdButton::clicked, this, [this]() {
            activateToolFromToolbar(Tool::PenFilter);
        });
    }

    if (options.showEraserTool) {
        m_eraserButton = addToolButton("Eraser", custom_outlined_icons::ToolEraser());
        addButton(m_eraserButton);
        connect(m_eraserButton, &adqt::widgets::AdButton::clicked, this, [this]() {
            activateToolFromToolbar(Tool::Eraser);
        });
    }

    if (options.showWatermarkTool) {
        m_watermarkButton = addToolButton("Watermark", custom_outlined_icons::ToolWatermark());
        m_watermarkButton->setObjectName(QStringLiteral("screenshotWatermarkButton"));
        addButton(m_watermarkButton);
        connect(m_watermarkButton, &adqt::widgets::AdButton::clicked, this, [this]() {
            activateToolFromToolbar(Tool::Watermark);
        });
    }

    if (options.showHistoryActions) {
        addSeparator();
        static_cast<void>(addMainHistoryButtons(options, layout));
    }

    return hasButton;
}

void ScreenshotToolPalette::setHistoryState(const SnowCanvasHistoryState& state) {
    m_canvasHistoryState = state;
    updateHistoryActionAvailability();
}

void ScreenshotToolPalette::updateHistoryActionAvailability() {
    const bool tableActive = m_activeTool == Tool::Table;
    const bool textActive =
        m_activeTool == Tool::Ocr || m_activeTool == Tool::TextTranslation;
    const bool qrActive = m_activeTool == Tool::Qr;
    if (m_undoButton != nullptr) {
        m_undoButton->setEnabled(qrActive      ? false
                                 : tableActive ? m_tableEditingAvailable && m_tableCanUndo
                                 : textActive  ? m_textEditingAvailable && m_textCanUndo
                                               : m_canvasHistoryState.canUndo);
    }
    if (m_redoButton != nullptr) {
        m_redoButton->setEnabled(qrActive      ? false
                                 : tableActive ? m_tableEditingAvailable && m_tableCanRedo
                                 : textActive  ? m_textEditingAvailable && m_textCanRedo
                                               : m_canvasHistoryState.canRedo);
    }
}

void ScreenshotToolPalette::updateTextRecognitionBusy() {
    const bool translationActive = m_activeTool == Tool::TextTranslation;
    if (m_ocrButton != nullptr) {
        m_ocrButton->setBusy(m_ocrBusy && !translationActive);
    }
    if (m_textTranslationButton != nullptr) {
        m_textTranslationButton->setBusy(
            translationActive && (m_ocrBusy || m_textTranslationStreaming));
    }
}

bool ScreenshotToolPalette::addMainHistoryButtons(const Options& options, QBoxLayout* layout) {
    if (!options.showHistoryActions || layout == nullptr) {
        return false;
    }

    m_undoButton = addActionButton("Undo", outlined_icons::Undo());
    m_redoButton = addActionButton("Redo", outlined_icons::Redo());
    applyScreenshotShortcutTooltip(m_undoButton, QStringLiteral("Undo"),
                                   QStringLiteral("undo"));
    applyScreenshotShortcutTooltip(m_redoButton, QStringLiteral("Redo"),
                                   QStringLiteral("redo"));
    m_undoButton->setObjectName(QStringLiteral("screenshotUndoButton"));
    m_redoButton->setObjectName(QStringLiteral("screenshotRedoButton"));
    m_undoButton->setEnabled(false);
    m_redoButton->setEnabled(false);
    layout->addWidget(m_undoButton);
    addMainToolbarSpacing(TOOLBAR_ITEM_SPACING);
    layout->addWidget(m_redoButton);
    connect(m_undoButton, &adqt::widgets::AdButton::clicked, this,
            &ScreenshotToolPalette::undoRequested);
    connect(m_redoButton, &adqt::widgets::AdButton::clicked, this,
            &ScreenshotToolPalette::redoRequested);
    return true;
}

bool ScreenshotToolPalette::addMainSecondaryButtons(const Options& options, QBoxLayout* layout) {
    if (layout == nullptr) {
        return false;
    }

    bool hasButton = false;
    const auto addButton = [this, layout, &hasButton](adqt::widgets::AdButton* button) {
        if (button == nullptr) {
            return;
        }
        if (hasButton) {
            addMainToolbarSpacing(TOOLBAR_ITEM_SPACING);
        }
        layout->addWidget(button);
        hasButton = true;
    };

    if (options.showTableTool && options.showQrTool) {
        m_tableButton =
            addToolButton("Table recognition", custom_outlined_icons::TableRecognition());
        applyScreenshotShortcutTooltip(m_tableButton, QStringLiteral("Table recognition"),
                                       QStringLiteral("table_recognition"));
        m_tableButton->setObjectName(QStringLiteral("screenshotTableQrButton"));
        m_tableButton->setBusyIndicatorPresentation(
            adqt::widgets::AdButton::BusyIndicatorPresentation::IsolatedSurface);
        addButton(m_tableButton);

        m_tableQrPopover = createScreenshotToolPaletteOptionPopoverShell(m_tableButton);
        m_tableButton->installEventFilter(this);
        connect(m_tableQrPopover, &adqt::widgets::AdPopover::visibilityRequested, this,
                [this](bool visible) {
                    if (visible) {
                        ensureTableQrPopover();
                    }
                });
        connect(m_tableButton, &adqt::widgets::AdButton::pressed, this,
                [this]() { activateTableQrTool(m_tableQrEntryTool); });
        refreshTableQrTrigger();
        updateTableQrBusy();
    } else if (options.showTableTool) {
        m_tableQrEntryTool = Tool::Table;
        m_tableButton =
            addToolButton("Table recognition", custom_outlined_icons::TableRecognition());
        applyScreenshotShortcutTooltip(m_tableButton, QStringLiteral("Table recognition"),
                                       QStringLiteral("table_recognition"));
        m_tableButton->setBusyIndicatorPresentation(
            adqt::widgets::AdButton::BusyIndicatorPresentation::IsolatedSurface);
        addButton(m_tableButton);
        connect(m_tableButton, &adqt::widgets::AdButton::clicked, this,
                [this]() { activateTableQrTool(Tool::Table); });
    } else if (options.showQrTool) {
        m_tableQrEntryTool = Tool::Qr;
        m_tableButton = addToolButton("Barcode recognition", custom_outlined_icons::ScanQrcode());
        applyScreenshotShortcutTooltip(m_tableButton, QStringLiteral("Barcode recognition"),
                                       QStringLiteral("qr_code_recognition"));
        m_tableButton->setBusyIndicatorPresentation(
            adqt::widgets::AdButton::BusyIndicatorPresentation::IsolatedSurface);
        addButton(m_tableButton);
        connect(m_tableButton, &adqt::widgets::AdButton::clicked, this,
                [this]() { activateTableQrTool(Tool::Qr); });
    }

    if (options.showScreenRecordButton) {
        m_screenRecordButton = addToolButton("Record screen", custom_outlined_icons::RecordScreen());
        applyScreenshotShortcutTooltip(m_screenRecordButton, QStringLiteral("Record screen"),
                                       QStringLiteral("video_recording"));
        addButton(m_screenRecordButton);
        connect(m_screenRecordButton, &adqt::widgets::AdButton::clicked, this,
                &ScreenshotToolPalette::screenRecordRequested);
    }

    if (!options.showRecordingControls && (options.actions & PinAction) != 0) {
        m_pinButton = addActionButton("Pin to screen", custom_outlined_icons::PinToScreen());
        applyScreenshotShortcutTooltip(m_pinButton, QStringLiteral("Pin to screen"),
                                       QStringLiteral("pin_to_screen"));
        m_pinButton->setObjectName(QStringLiteral("screenshotPinToScreenButton"));
        addButton(m_pinButton);
        connect(m_pinButton, &adqt::widgets::AdButton::clicked, this,
                &ScreenshotToolPalette::pinRequested);
    }

    if (options.showOcrTool) {
        m_ocrButton = addToolButton("Text recognition", custom_outlined_icons::ToolRecognizeText());
        applyScreenshotShortcutTooltip(m_ocrButton, QStringLiteral("Text recognition"),
                                       QStringLiteral("text_recognition"));
        m_ocrButton->setBusyIndicatorPresentation(
            adqt::widgets::AdButton::BusyIndicatorPresentation::IsolatedSurface);
        addButton(m_ocrButton);
        connect(m_ocrButton, &adqt::widgets::AdButton::clicked, this, [this]() {
            activateToolFromToolbar(Tool::Ocr);
        });
    }

    if (options.showTextTranslationTool) {
        m_textTranslationButton =
            addToolButton("Text translation", custom_outlined_icons::OcrTranslate());
        applyScreenshotShortcutTooltip(m_textTranslationButton, QStringLiteral("Text translation"),
                                       QStringLiteral("text_translation"));
        m_textTranslationButton->setObjectName(QStringLiteral("screenshotTextTranslationButton"));
        m_textTranslationButton->setBusyIndicatorPresentation(
            adqt::widgets::AdButton::BusyIndicatorPresentation::IsolatedSurface);
        addButton(m_textTranslationButton);
        connect(m_textTranslationButton, &adqt::widgets::AdButton::clicked, this, [this]() {
            activateToolFromToolbar(Tool::TextTranslation);
        });
    }

    if (options.showScrollingScreenshotTool) {
        m_scrollingScreenshotButton =
            addToolButton("Scrolling screenshot", custom_outlined_icons::ScrollingScreenshot());
        applyScreenshotShortcutTooltip(m_scrollingScreenshotButton,
                                       QStringLiteral("Scrolling screenshot"),
                                       QStringLiteral("scrolling_screenshot"));
        m_scrollingScreenshotButton->setObjectName(
            QStringLiteral("screenshotScrollingScreenshotButton"));
        addButton(m_scrollingScreenshotButton);
        connect(m_scrollingScreenshotButton, &adqt::widgets::AdButton::clicked, this, [this]() {
            activateToolFromToolbar(Tool::ScrollingScreenshot);
        });
    }

    if (options.showSaveButton && !options.saveButtonWithResultActions) {
        m_saveButton = addActionButton("Save as file", custom_outlined_icons::Save());
        applyScreenshotShortcutTooltip(m_saveButton, QStringLiteral("Save as file"),
                                       QStringLiteral("save_as_file"));
        m_saveButton->setObjectName(QStringLiteral("screenshotSaveAsFileButton"));
        addButton(m_saveButton);
        connect(m_saveButton, &adqt::widgets::AdButton::clicked, this,
                &ScreenshotToolPalette::saveRequested);
    }

    return hasButton;
}

ScreenshotToolPalette::Tool ScreenshotToolPalette::drawingShortcutEntryTool(
    const QString& itemId, Tool fallback) const {
    for (const DrawingToolGroup& group : m_drawingToolGroups) {
        if (group.itemIds.contains(itemId)) {
            return group.entryTool;
        }
    }
    return fallback;
}

bool ScreenshotToolPalette::activateDrawingShortcut(const QString& toolId) {
    Tool tool = Tool::Move;
    if (toolId == QStringLiteral("select")) {
        tool = Tool::Select;
    } else if (toolId == QStringLiteral("shape")) {
        tool = Tool::Shape;
    } else if (toolId == QStringLiteral("arrow")) {
        tool = Tool::Arrow;
    } else if (toolId == QStringLiteral("brush")) {
        tool = Tool::FreeDraw;
    } else if (toolId == QStringLiteral("highlight")) {
        tool = drawingShortcutEntryTool(QStringLiteral("highlighter"), Tool::PenHighlight);
    } else if (toolId == QStringLiteral("text")) {
        tool = Tool::Text;
    } else if (toolId == QStringLiteral("serial_number")) {
        tool = Tool::SerialNumber;
    } else if (toolId == QStringLiteral("filter")) {
        tool = drawingShortcutEntryTool(QStringLiteral("filter"), Tool::PenFilter);
    } else if (toolId == QStringLiteral("eraser")) {
        tool = Tool::Eraser;
    } else if (toolId == QStringLiteral("watermark")) {
        tool = Tool::Watermark;
    } else {
        return false;
    }
    activateDrawingTool(tool);
    return true;
}

void ScreenshotToolPalette::addMainActionButtons(const Options& options, QBoxLayout* layout) {
    if (layout == nullptr) {
        return;
    }

    bool hasButton = false;
    bool separated = false;
    const auto addButton = [this, layout, &hasButton, &separated](adqt::widgets::AdButton* button) {
        if (button == nullptr) {
            return;
        }
        if (hasButton && !separated) {
            addMainToolbarSpacing(TOOLBAR_ITEM_SPACING);
        }
        layout->addWidget(button);
        hasButton = true;
        separated = false;
    };

    if ((options.actions & CancelAction) != 0) {
        m_cancelButton = addActionButton("Cancel screenshot", outlined_icons::Close(), true);
        applyScreenshotShortcutTooltip(m_cancelButton, QStringLiteral("Cancel screenshot"),
                                       QStringLiteral("cancel_screenshot"));
        addButton(m_cancelButton);
        connect(m_cancelButton, &adqt::widgets::AdButton::clicked, this, [this]() {
            clearActiveTool();
            emit cancelRequested();
        });
    }

    if (options.showSaveButton && options.saveButtonWithResultActions) {
        m_saveButton = addActionButton("Save as file", custom_outlined_icons::Save());
        applyScreenshotShortcutTooltip(m_saveButton, QStringLiteral("Save as file"),
                                       QStringLiteral("save_as_file"));
        m_saveButton->setObjectName(QStringLiteral("screenshotSaveAsFileButton"));
        addButton(m_saveButton);
        connect(m_saveButton, &adqt::widgets::AdButton::clicked, this,
                &ScreenshotToolPalette::saveRequested);
    }

    if ((options.actions & CopyAction) != 0) {
        m_copyButton = addActionButton(
            "Copy to clipboard",
            options.copyButtonWithNeutralIcon ? outlined_icons::Copy()
                                               : primaryIcon(outlined_icons::Copy()));
        applyScreenshotShortcutTooltip(m_copyButton, QStringLiteral("Copy to clipboard"),
                                       QStringLiteral("copy_to_clipboard"));
        addButton(m_copyButton);
        connect(m_copyButton, &adqt::widgets::AdButton::clicked, this,
                &ScreenshotToolPalette::copyRequested);
    }

    if (options.separatorBeforeConfirm && (options.actions & ConfirmAction) != 0 && hasButton) {
        addMainToolbarSeparator();
        separated = true;
    }

    if ((options.actions & ConfirmAction) != 0) {
        m_confirmButton = addActionButton("Confirm edit", primaryIcon(outlined_icons::Check()));
        if (options.showDrawingModeShortcutOnConfirm) {
            applyPinToScreenShortcutTooltip(m_confirmButton, QStringLiteral("Confirm edit"),
                                            QStringLiteral("drawing_mode"));
        }
        addButton(m_confirmButton);
        connect(m_confirmButton, &adqt::widgets::AdButton::clicked, this,
                &ScreenshotToolPalette::confirmRequested);
    }
}

SnowCanvasFilterStyle& ScreenshotToolPalette::filterStyleForEditor(const FilterEditor& editor) {
    return editor.tool == Tool::PenFilter ? m_styleControls->styleState().penFilterStyle
                                          : m_styleControls->styleState().rectangleFilterStyle;
}

void ScreenshotToolPalette::synchronizeFilterModeGroups(Tool tool) {
    if (tool != Tool::RectangleFilter && tool != Tool::PenFilter) {
        return;
    }

    for (adqt::widgets::AdRadioButtonGroup* group : m_filterModeGroups) {
        if (group != nullptr) {
            group->setCheckedId(static_cast<int>(tool));
        }
    }
}

void ScreenshotToolPalette::refreshFilterEditorMetrics(FilterEditor& editor) {
    ScreenshotToolPaletteButtonMetrics metrics = styleButtonMetrics(m_physicalScale);
    metrics.scope = editor.controls;
    ScreenshotToolPaletteSelectEditor typeSelectEditor;
    typeSelectEditor.select = editor.typeSelect;
    configureScreenshotToolPaletteSelectEditor(typeSelectEditor, metrics);
    ScreenshotToolPaletteSliderEditor intensityEditor;
    intensityEditor.icon = editor.intensityIcon;
    intensityEditor.slider = editor.intensitySlider;
    intensityEditor.iconRef = custom_outlined_icons::Blur();
    intensityEditor.baseIconSize = COMPACT_SLIDER_ICON_SIZE;
    intensityEditor.baseSliderWidth = COMPACT_SLIDER_WIDTH;
    configureScreenshotToolPaletteSliderEditor(intensityEditor, metrics);
}

void ScreenshotToolPalette::refreshFilterEditorState(FilterEditor& editor, bool refreshWidth) {
    const SnowCanvasFilterStyle& style = filterStyleForEditor(editor);
    if (editor.intensitySlider != nullptr) {
        const bool mixedType = (m_styleControls->styleState().filterStyleMixed &
                                SnowCanvasFilterStylePropertyType) != 0;
        editor.intensitySlider->setEnabled(mixedType || filterTypeSupportsIntensity(style.type));
    }
    updateFilterIntensityIcon(editor);
    if (refreshWidth && editor.tool == Tool::PenFilter) {
        updatePenFilterStrokeWidthControls();
    }
}

QWidget* ScreenshotToolPalette::createStyleModeSelector(
    QWidget* parent, const QString& objectName, const QVector<StyleModeOption>& options,
    Tool initialTool, QVector<adqt::widgets::AdRadioButtonGroup*>& groups) {
    ScreenshotToolPaletteRadioEditorConfig config;
    config.objectName = objectName;
    config.initialId = static_cast<int>(initialTool);
    for (const StyleModeOption& option : options) {
        config.options.push_back({
            static_cast<int>(option.tool),
            option.tooltip,
            option.icon,
        });
    }
    const ScreenshotToolPaletteRadioEditor editor =
        createScreenshotToolPaletteRadioEditor(parent, config, styleButtonMetrics(m_physicalScale));
    connect(editor.group, &QButtonGroup::idClicked, this, [this](int id) {
        const Tool tool = static_cast<Tool>(id);
        setActiveTool(tool);
        switch (tool) {
        case Tool::RectangleHighlight:
            emit highlightRequested();
            break;
        case Tool::PenHighlight:
            emit penHighlightRequested();
            break;
        case Tool::RectangleFilter:
            emit rectangleFilterRequested();
            break;
        case Tool::PenFilter:
            emit penFilterRequested();
            break;
        default:
            break;
        }
    });
    groups.append(editor.group);
    return editor.container;
}

ScreenshotToolPalette::FilterEditor
ScreenshotToolPalette::createFilterEditor(const FilterEditorConfig& config) {
    FilterEditor editor;
    editor.tool = config.tool;
    editor.controls = new QWidget(m_rectangleStylePanel);
    editor.controls->setObjectName(config.controlsObjectName);

    auto* layout = new QHBoxLayout(editor.controls);
    m_styleControlLayouts.push_back(layout);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(scaledMetric(STYLE_ITEM_SPACING));

    layout->addWidget(createStyleModeSelector(
        editor.controls, QStringLiteral("screenshotFilterModeSelector"),
        {
            {QStringLiteral("Pen filter"), outlined_icons::Highlight(), Tool::PenFilter},
            {QStringLiteral("Rectangle filter"), custom_outlined_icons::ShapeRectangle(),
             Tool::RectangleFilter},
        },
        Tool::PenFilter, m_filterModeGroups));
    addStyleToolbarSpacing(layout, STYLE_GROUP_SPACING);

    ScreenshotToolPaletteSelectEditorConfig typeSelectConfig;
    typeSelectConfig.objectName = config.typeSelectObjectName;
    typeSelectConfig.accessibleName = QStringLiteral("Filter type");
    typeSelectConfig.tooltip = QStringLiteral("Filter type");
    typeSelectConfig.placeholder = QStringLiteral("Filter type");
    const ScreenshotToolPaletteSelectEditor typeSelectEditor =
        createScreenshotToolPaletteSelectEditor(editor.controls, typeSelectConfig,
                                                styleButtonMetrics(m_physicalScale));
    editor.typeSelect = typeSelectEditor.select;
    editor.typeSelect->setSortComparator(
        [](const adqt::widgets::AdSelect::Option& lhs, const adqt::widgets::AdSelect::Option& rhs) {
            return lhs.value.toInt() < rhs.value.toInt();
        });
    auto* typeModel = new QStandardItemModel(editor.typeSelect);
    const auto appendFilterType = [typeModel](const char* source, int value) {
        const ScreenshotToolPaletteTranslationText text(source);
        auto* item = new QStandardItem(text.translated());
        setScreenshotToolPaletteItemTranslationSource(item, text);
        item->setData(value, adqt::widgets::AdSelect::DefaultValueRole);
        typeModel->appendRow(item);
    };
    appendFilterType("Mosaic", 0);
    appendFilterType("Gaussian blur", 1);
    appendFilterType("Grayscale", 2);
    appendFilterType("Inversion", 3);
    editor.typeSelect->setModel(typeModel);
    layout->addWidget(editor.typeSelect);

    const auto addGroupSeparator = [this, layout, &editor]() {
        addStyleToolbarSpacing(layout, STYLE_GROUP_SPACING);
        layout->addWidget(createStyleToolbarSeparator(editor.controls));
        addStyleToolbarSpacing(layout, STYLE_GROUP_SPACING);
    };
    addGroupSeparator();

    if (config.includeStrokeWidth) {
        const auto setPenFilterWidth = [this](double width) {
            const double clamped = qBound(1.0, width, 72.0);
            const bool wasMixed = (m_styleControls->styleState().filterStyleMixed &
                                   SnowCanvasFilterStylePropertyStrokeWidth) != 0;
            if (qFuzzyCompare(m_styleControls->styleState().penFilterStyle.strokeWidth + 1.0,
                              clamped + 1.0) &&
                !wasMixed) {
                return;
            }
            m_styleControls->styleState().penFilterStyle.strokeWidth = clamped;
            m_styleControls->styleState().creationPenFilterStyle.strokeWidth = clamped;
            m_styleControls->styleState().filterStyleMixed &=
                ~SnowCanvasFilterStylePropertyStrokeWidth;
            updatePenFilterStrokeWidthControls();
            emit filterStyleChanged(m_styleControls->styleState().penFilterStyle,
                                    SnowCanvasFilterStylePropertyStrokeWidth);
        };
        m_styleControls->addPenFilterStrokeWidthControls(
            layout, editor.controls, this, filterStyleForEditor(editor).strokeWidth,
            [this, setPenFilterWidth]() {
                setPenFilterWidth(m_styleControls->styleState().penFilterStyle.strokeWidth + 1.0);
            },
            setPenFilterWidth, styleButtonMetrics(m_physicalScale));
        addGroupSeparator();
    }

    ScreenshotToolPaletteSliderEditorConfig intensityConfig;
    intensityConfig.iconObjectName = config.intensityIconObjectName;
    intensityConfig.sliderObjectName = config.intensitySliderObjectName;
    intensityConfig.accessibleName = QStringLiteral("Filter intensity");
    intensityConfig.sliderTooltip = QStringLiteral("Adjust filter intensity");
    intensityConfig.iconRef = custom_outlined_icons::Blur();
    intensityConfig.initialValue = 50;
    intensityConfig.baseIconSize = COMPACT_SLIDER_ICON_SIZE;
    intensityConfig.baseSliderWidth = COMPACT_SLIDER_WIDTH;
    const ScreenshotToolPaletteSliderEditor intensityEditor =
        createScreenshotToolPaletteSliderEditor(layout, editor.controls, intensityConfig,
                                                styleButtonMetrics(m_physicalScale));
    editor.intensityIcon = intensityEditor.icon;
    editor.intensitySlider = intensityEditor.slider;

    connect(editor.typeSelect, &adqt::widgets::AdSelect::currentValueChanged, this,
            [this, tool = config.tool](const QVariant& value) {
                if (!value.canConvert<int>()) {
                    return;
                }
                FilterEditor& target = tool == Tool::PenFilter ? m_penFilterEditor : m_filterEditor;
                SnowCanvasFilterStyle& style = filterStyleForEditor(target);
                style.type = static_cast<SnowCanvasFilterType>(value.toInt());
                if (tool == Tool::PenFilter) {
                    m_styleControls->styleState().creationPenFilterStyle.type = style.type;
                } else {
                    m_styleControls->styleState().creationRectangleFilterStyle.type = style.type;
                }
                if (target.intensitySlider != nullptr) {
                    target.intensitySlider->setEnabled(filterTypeSupportsIntensity(style.type));
                }
                updateFilterIntensityIcon(target);
                emit filterStyleChanged(style, SnowCanvasFilterStylePropertyType);
            });
    connect(editor.intensitySlider, &adqt::widgets::AdSlider::valueChanged, this,
            [this, tool = config.tool](double value) {
                FilterEditor& target = tool == Tool::PenFilter ? m_penFilterEditor : m_filterEditor;
                SnowCanvasFilterStyle& style = filterStyleForEditor(target);
                style.strength = qBound(0.0, value / 100.0, 1.0);
                if (tool == Tool::PenFilter) {
                    m_styleControls->styleState().creationPenFilterStyle.strength = style.strength;
                } else {
                    m_styleControls->styleState().creationRectangleFilterStyle.strength =
                        style.strength;
                }
                emit filterStyleChanged(style, SnowCanvasFilterStylePropertyStrength);
            });
    refreshFilterEditorMetrics(editor);
    return editor;
}

void ScreenshotToolPalette::createSecondaryToolbarShell() {
    if (m_selectActionPanel != nullptr) {
        return;
    }
    m_selectActionPanel = createPanel(this, QStringLiteral("screenshotSelectActionPanel"));
    if (auto* frame = qobject_cast<QFrame*>(m_selectActionPanel)) {
        m_panelFrames.push_back(frame);
        updatePanelMetrics(frame);
    }
    m_selectActionLayout = new QHBoxLayout(m_selectActionPanel);
    m_styleControlLayouts.push_back(m_selectActionLayout);
    m_selectActionLayout->setContentsMargins(scaledPanelMargins(
        TOOLBAR_PANEL_HORIZONTAL_MARGIN, TOOLBAR_PANEL_VERTICAL_MARGIN, 32));
    m_selectActionLayout->setSpacing(0);

    m_rectangleStylePanel = createPanel(this, QStringLiteral("screenshotRectangleStylePanel"));
    if (auto* frame = qobject_cast<QFrame*>(m_rectangleStylePanel)) {
        m_panelFrames.push_back(frame);
        updatePanelMetrics(frame);
    }
    m_rectangleStyleLayout = new QVBoxLayout(m_rectangleStylePanel);
    m_rectangleStyleLayout->setContentsMargins(scaledPanelMargins(
        STYLE_PANEL_HORIZONTAL_MARGIN, STYLE_PANEL_VERTICAL_MARGIN, STYLE_BUTTON_SIZE));
    m_rectangleStyleLayout->setSpacing(0);

    if (m_rootLayout != nullptr) {
        m_rootLayout->addWidget(m_selectActionPanel, 0, Qt::AlignRight);
        m_rootLayout->addWidget(m_rectangleStylePanel, 0, Qt::AlignRight);
    }
    m_selectActionPanel->hide();
    m_rectangleStylePanel->hide();
}

void ScreenshotToolPalette::clearSecondaryResourceBindings() {
    m_styleEditorBindings.clear();
    m_styleControlLayouts.clear();
    if (m_selectActionLayout != nullptr) {
        m_styleControlLayouts.push_back(m_selectActionLayout);
    }
    m_styleSeparatorFrames.clear();
    m_panelFrames.clear();
    if (auto* frame = qobject_cast<QFrame*>(m_selectActionPanel)) {
        m_panelFrames.push_back(frame);
    }
    if (auto* frame = qobject_cast<QFrame*>(m_rectangleStylePanel)) {
        m_panelFrames.push_back(frame);
    }
    m_styleSpacingItems.clear();
    m_styleLayoutProfiles.clear();
    m_styleMetricRevisions.clear();
    m_selectionActionControls.clear();
    m_selectionActionSpacers.clear();
    m_textActionSpacers.clear();
    m_tableActionSpacers.clear();
    m_highlightModeGroups.clear();
    m_filterModeGroups.clear();

    m_filterEditor = {};
    m_penFilterEditor = {};
    m_activeStyleControlsWidget = nullptr;
    m_activeStyleTool.reset();
    m_shapeStyleGroupSeparator = nullptr;
    m_shapeStyleGroupSeparatorLeadingSpacing = nullptr;
    m_shapeStyleGroupSeparatorTrailingSpacing = nullptr;

    m_selectionOpacityIcon = nullptr;
    m_selectionOpacitySlider = nullptr;
    m_textEditButton = nullptr;
    m_textTranslateButton = nullptr;
    m_textResetButton = nullptr;
    m_textSettingsButton = nullptr;
    m_tableMergeButton = nullptr;
    m_tableSplitButton = nullptr;
    m_tableResetButton = nullptr;
    m_textFormattingSelect = nullptr;
    m_textPunctuationSelect = nullptr;
    m_scrollingRecognitionControls = nullptr;
    m_scrollingVerticalButton = nullptr;
    m_scrollingHorizontalButton = nullptr;

    m_rectangleStyleControlsWidget = nullptr;
    m_arrowStyleControlsWidget = nullptr;
    m_highlightStyleControlsWidget = nullptr;
    m_penHighlightStyleControlsWidget = nullptr;
    m_spotlightStyleControlsWidget = nullptr;
    m_textStyleControlsWidget = nullptr;
    m_serialNumberStyleControlsWidget = nullptr;
    m_filterStyleControlsWidget = nullptr;
    m_penFilterStyleControlsWidget = nullptr;
    m_watermarkStyleControlsWidget = nullptr;
    m_spotlightOpacityIcon = nullptr;
    m_spotlightOpacitySlider = nullptr;

    m_actionFamilyStates.clear();
    m_styleFamilyStates.clear();
}

bool ScreenshotToolPalette::evictSecondaryToolbarContents() {
    if (m_releasingSecondaryResources ||
        (m_selectActionPanel == nullptr && m_rectangleStylePanel == nullptr)) {
        return false;
    }

    const bool hadContents =
        !m_actionFamilyStates.isEmpty() || !m_styleFamilyStates.isEmpty() ||
        (m_selectActionLayout != nullptr && m_selectActionLayout->count() > 0) ||
        (m_rectangleStyleLayout != nullptr && m_rectangleStyleLayout->count() > 0);
    if (!hadContents) {
        return false;
    }

    m_releasingSecondaryResources = true;
    m_styleControls->releaseControlBindings();
    m_actionToolbarTargetVisible = false;
    m_styleToolbarTargetVisible = false;

    // Publish null bindings before destroying the child widget subtrees.
    // Destruction can synchronously invoke focus, popup, or layout callbacks.
    clearSecondaryResourceBindings();

    const auto clearLayout = [](QBoxLayout* layout) {
        if (layout == nullptr) {
            return;
        }
        QVector<QWidget*> widgets;
        const auto takeLayoutItems = [&widgets](auto&& self, QLayout* currentLayout) -> void {
            while (QLayoutItem* item = currentLayout->takeAt(0)) {
                if (QWidget* widget = item->widget()) {
                    widgets.push_back(widget);
                    delete item;
                } else if (QLayout* childLayout = item->layout()) {
                    self(self, childLayout);
                    // QLayout is itself the QLayoutItem returned by takeAt().
                    delete childLayout;
                } else {
                    delete item;
                }
            }
        };
        takeLayoutItems(takeLayoutItems, layout);
        for (QWidget* widget : std::as_const(widgets)) {
            delete widget;
        }
        layout->invalidate();
    };
    clearLayout(m_selectActionLayout);
    clearLayout(m_rectangleStyleLayout);
    if (m_selectActionPanel != nullptr) {
        m_selectActionPanel->hide();
        m_selectActionPanel->updateGeometry();
    }
    if (m_rectangleStylePanel != nullptr) {
        m_rectangleStylePanel->hide();
        m_rectangleStylePanel->updateGeometry();
    }
    m_releasingSecondaryResources = false;
    markLayoutDirty(true);
    return true;
}

void ScreenshotToolPalette::registerStyleFamily(QWidget* controls,
                                                std::initializer_list<Tool> tools) {
    if (controls == nullptr || m_rectangleStyleLayout == nullptr) {
        return;
    }
    StyleEditorBinding binding;
    binding.controls = controls;
    for (Tool tool : tools) {
        binding.tools.push_back(tool);
        m_styleFamilyStates.insert(static_cast<int>(tool), MaterializationState::Ready);
    }
    m_styleEditorBindings.push_back(binding);
    m_rectangleStyleLayout->addWidget(controls);
    controls->hide();
    markLayoutDirty(false);
    initializeStyleLayoutProfiles();
    applyCumulativeStyleLayoutMetrics(controls);
    installWheelFilters(this, controls);
    emit materializedScope(controls);
}

bool ScreenshotToolPalette::ensureActionFamily(ActionFamily family) {
    if (!m_options.enableStyleToolbar || m_releasingSecondaryResources) {
        return false;
    }
    createSecondaryToolbarShell();
    const int key = static_cast<int>(family);
    const MaterializationState state =
        m_actionFamilyStates.value(key, MaterializationState::Uninitialized);
    if (state == MaterializationState::Ready) {
        return true;
    }
    if (state == MaterializationState::Constructing) {
        return false;
    }
    m_actionFamilyStates.insert(key, MaterializationState::Constructing);
    switch (family) {
    case ActionFamily::Selection:
        createSelectionActionFamily();
        break;
    case ActionFamily::TextRecognition:
        createTextRecognitionActionFamily();
        break;
    case ActionFamily::TableRecognition:
        createTableRecognitionActionFamily();
        break;
    case ActionFamily::ScrollingRecognition:
        createScrollingRecognitionActionFamily();
        break;
    }
    markLayoutDirty(false);
    m_actionFamilyStates.insert(key, MaterializationState::Ready);
    initializeStyleLayoutProfiles();
    installWheelFilters(this, m_selectActionPanel);
    emit materializedScope(m_selectActionPanel);
    SNOW_SHOT_TOOLBAR_PERF_COUNTER("hydrate.action_family");
    return true;
}

void ScreenshotToolPalette::createSelectionActionFamily() {
    if (m_selectActionLayout == nullptr || !m_selectionActionControls.isEmpty()) {
        return;
    }
    const auto addSelectButton =
        [this](const char* tooltip, const adqt::icons::IconRef& icon, auto signal) {
            auto* button = createScreenshotToolPaletteStyleActionButton(
                m_selectActionPanel, tooltip, icon, actionButtonMetrics(m_physicalScale));
            button->setEnabled(false);
            m_selectionActionControls.push_back(button);
            m_selectActionLayout->addWidget(button);
            connect(button, &adqt::widgets::AdButton::clicked, this, signal);
        };
    const auto addSpacing = [this](int spacing) {
        m_selectionActionSpacers.push_back(addStyleToolbarSpacing(m_selectActionLayout, spacing));
    };
    addSelectButton("Send to back", outlined_icons::VerticalAlignBottom(),
                    &ScreenshotToolPalette::sendSelectionToBackRequested);
    addSpacing(STYLE_ITEM_SPACING);
    addSelectButton("Send backward", outlined_icons::ArrowDown(),
                    &ScreenshotToolPalette::sendSelectionBackwardRequested);
    addSpacing(STYLE_ITEM_SPACING);
    addSelectButton("Bring forward", outlined_icons::ArrowUp(),
                    &ScreenshotToolPalette::bringSelectionForwardRequested);
    addSpacing(STYLE_ITEM_SPACING);
    addSelectButton("Bring to front", outlined_icons::VerticalAlignTop(),
                    &ScreenshotToolPalette::bringSelectionToFrontRequested);
    addSpacing(STYLE_GROUP_SPACING * 2);
    m_selectActionLayout->addWidget(createStyleToolbarSeparator(m_selectActionPanel));
    addSpacing(STYLE_GROUP_SPACING * 2);
    ScreenshotToolPaletteSliderEditorConfig opacityConfig;
    opacityConfig.iconObjectName = QStringLiteral("screenshotSelectionOpacityIcon");
    opacityConfig.sliderObjectName = QStringLiteral("screenshotSelectionOpacitySlider");
    opacityConfig.accessibleName = QStringLiteral("Opacity");
    opacityConfig.sliderTooltip = QStringLiteral("Adjust opacity");
    opacityConfig.iconRef = custom_outlined_icons::Opacity();
    opacityConfig.initialValue = qRound(m_selectionOpacity * 100.0);
    opacityConfig.baseIconSize = STYLE_ICON_SIZE;
    opacityConfig.baseSliderWidth = COMPACT_SLIDER_WIDTH;
    const auto editor = createScreenshotToolPaletteSliderEditor(
        m_selectActionLayout, m_selectActionPanel, opacityConfig,
        actionButtonMetrics(m_physicalScale));
    m_selectionOpacityIcon = editor.icon;
    m_selectionOpacitySlider = editor.slider;
    m_selectionActionControls.push_back(m_selectionOpacityIcon);
    m_selectionActionControls.push_back(m_selectionOpacitySlider);
    connect(m_selectionOpacitySlider, &adqt::widgets::AdSlider::valueChanged, this,
            [this](double value) {
                setSelectionOpacity(value / 100.0);
                if (!m_replayingMaterializedState) {
                    emit selectionOpacityChanged(m_selectionOpacity);
                }
            });
    addSpacing(STYLE_ITEM_SPACING);
    m_selectActionLayout->addWidget(createStyleToolbarSeparator(m_selectActionPanel));
    addSpacing(STYLE_GROUP_SPACING * 2);
    addSelectButton("Copy selected elements", custom_outlined_icons::Duplicate(),
                    &ScreenshotToolPalette::duplicateSelectionRequested);
    addSpacing(STYLE_ITEM_SPACING);
    addSelectButton("Delete selected elements", custom_outlined_icons::Trash(),
                    &ScreenshotToolPalette::deleteSelectionRequested);
    m_selectionActionAvailabilityInitialized = false;
    updateSelectionActionAvailability(m_hasSelectedElements);
    setSelectionOpacity(m_selectionOpacity, m_selectionOpacityMixed);
}

void ScreenshotToolPalette::createTextRecognitionActionFamily() {
    if (m_selectActionLayout == nullptr || m_textEditButton != nullptr) {
        return;
    }
    const auto addButton = [this](const char* tooltip, const adqt::icons::IconRef& icon,
                                  const QString& objectName) {
        auto* button = createScreenshotToolPaletteStyleActionButton(
            m_selectActionPanel, tooltip, icon, actionButtonMetrics(m_physicalScale));
        button->setObjectName(objectName);
        m_selectActionLayout->addWidget(button);
        return button;
    };
    m_textEditButton = addButton("Edit", outlined_icons::Edit(),
                                 QStringLiteral("screenshotOcrTextEditButton"));
    m_textActionSpacers.push_back(addStyleToolbarSpacing(m_selectActionLayout, STYLE_ITEM_SPACING));
    m_textTranslateButton = addButton("Text translation", custom_outlined_icons::OcrTranslate(),
                                      QStringLiteral("screenshotOcrTextTranslateButton"));
    m_textActionSpacers.push_back(addStyleToolbarSpacing(m_selectActionLayout, STYLE_ITEM_SPACING));
    m_textFormattingSelect = new adqt::widgets::AdSelect(m_selectActionPanel);
    m_textFormattingSelect->setObjectName(QStringLiteral("screenshotOcrTextFormattingSelect"));
    m_textFormattingSelect->setPlaceholder(tr("Formatting"));
    m_textFormattingSelect->setOptions(
        {{QStringLiteral("keep"), tr("Keep line breaks")},
         {QStringLiteral("remove"), tr("Remove line breaks")}});
    m_textFormattingSelect->setAllowClear(true);
    m_textFormattingSelect->setVariant(adqt::widgets::AdSelect::Variant::Borderless);
    m_textFormattingSelect->setPopupLayerMode(adqt::widgets::AdSelect::PopupLayerMode::QtTool);
    m_textFormattingSelect->setFixedWidth(132);
    m_selectActionLayout->addWidget(m_textFormattingSelect);
    m_textActionSpacers.push_back(addStyleToolbarSpacing(m_selectActionLayout, STYLE_ITEM_SPACING));
    m_textPunctuationSelect = new adqt::widgets::AdSelect(m_selectActionPanel);
    m_textPunctuationSelect->setObjectName(QStringLiteral("screenshotOcrTextPunctuationSelect"));
    m_textPunctuationSelect->setPlaceholder(tr("Punctuation"));
    m_textPunctuationSelect->setOptions(
        {{QStringLiteral("half"), tr("Half-width")},
         {QStringLiteral("full"), tr("Full-width")}});
    m_textPunctuationSelect->setAllowClear(true);
    m_textPunctuationSelect->setVariant(adqt::widgets::AdSelect::Variant::Borderless);
    m_textPunctuationSelect->setPopupLayerMode(adqt::widgets::AdSelect::PopupLayerMode::QtTool);
    m_textPunctuationSelect->setFixedWidth(132);
    m_selectActionLayout->addWidget(m_textPunctuationSelect);
    m_textActionSpacers.push_back(addStyleToolbarSpacing(m_selectActionLayout, STYLE_ITEM_SPACING));
    m_textResetButton = addButton("Reset", outlined_icons::Reload(),
                                  QStringLiteral("screenshotOcrTextResetButton"));
    m_textActionSpacers.push_back(addStyleToolbarSpacing(m_selectActionLayout, STYLE_ITEM_SPACING));
    m_textSettingsButton = addButton("Translation settings", outlined_icons::Setting(),
                                     QStringLiteral("screenshotOcrTextSettingsButton"));
    connect(m_textEditButton, &adqt::widgets::AdButton::clicked, this,
            &ScreenshotToolPalette::textEditRequested);
    connect(m_textTranslateButton, &adqt::widgets::AdButton::clicked, this,
            &ScreenshotToolPalette::textTranslateRequested);
    connect(m_textResetButton, &adqt::widgets::AdButton::clicked, this,
            &ScreenshotToolPalette::textResetRequested);
    connect(m_textSettingsButton, &adqt::widgets::AdButton::clicked, this,
            &ScreenshotToolPalette::textSettingsRequested);
    connect(m_textFormattingSelect, &adqt::widgets::AdSelect::currentValueChanged, this,
            [this](const QVariant& value) {
                if (!m_replayingMaterializedState) {
                    emit textFormattingRequested(value.toString());
                }
            });
    connect(m_textPunctuationSelect, &adqt::widgets::AdSelect::currentValueChanged, this,
            [this](const QVariant& value) {
                if (!m_replayingMaterializedState) {
                    emit textPunctuationRequested(value.toString());
                }
            });
    setTextEditingState(m_textEditingAvailable, m_textEditing, m_textCanUndo, m_textCanRedo);
    setTextTranslationState(m_textEditingAvailable, m_textTranslating,
                            m_textTranslationStreaming, m_textCanUndo, m_textCanRedo,
                            m_textCanReset);
    setTextTransformSelections(m_textFormattingSelection, m_textPunctuationSelection);
}

void ScreenshotToolPalette::createTableRecognitionActionFamily() {
    if (m_selectActionLayout == nullptr || m_tableMergeButton != nullptr) {
        return;
    }
    const auto add = [this](const char* text, const adqt::icons::IconRef& icon,
                            const QString& name) {
        auto* button = createScreenshotToolPaletteStyleActionButton(
            m_selectActionPanel, text, icon, actionButtonMetrics(m_physicalScale));
        button->setObjectName(name);
        m_selectActionLayout->addWidget(button);
        return button;
    };
    m_tableMergeButton = add("Merge cells", outlined_icons::MergeCells(),
                             QStringLiteral("screenshotTableMergeButton"));
    m_tableActionSpacers.push_back(addStyleToolbarSpacing(m_selectActionLayout, STYLE_ITEM_SPACING));
    m_tableSplitButton = add("Split cells", outlined_icons::SplitCells(),
                             QStringLiteral("screenshotTableSplitButton"));
    m_tableActionSpacers.push_back(addStyleToolbarSpacing(m_selectActionLayout, STYLE_ITEM_SPACING));
    m_tableResetButton = add("Reset", outlined_icons::Reload(),
                             QStringLiteral("screenshotTableResetButton"));
    connect(m_tableMergeButton, &adqt::widgets::AdButton::clicked, this,
            &ScreenshotToolPalette::tableMergeRequested);
    connect(m_tableSplitButton, &adqt::widgets::AdButton::clicked, this,
            &ScreenshotToolPalette::tableSplitRequested);
    connect(m_tableResetButton, &adqt::widgets::AdButton::clicked, this,
            &ScreenshotToolPalette::tableResetRequested);
    setTableEditingState(m_tableEditingAvailable, m_tableCanUndo, m_tableCanRedo,
                         m_tableCanMerge, m_tableCanSplit, m_tableCanReset);
}

void ScreenshotToolPalette::createScrollingRecognitionActionFamily() {
    if (m_selectActionLayout == nullptr || m_scrollingRecognitionControls != nullptr) {
        return;
    }
    m_scrollingRecognitionControls = new QWidget(m_selectActionPanel);
    m_scrollingRecognitionControls->setObjectName(
        QStringLiteral("screenshotScrollingRecognitionMode"));
    auto* layout = new QHBoxLayout(m_scrollingRecognitionControls);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(scaledMetric(STYLE_ITEM_SPACING));
    m_scrollingVerticalButton = createScreenshotToolPaletteStyleActionButton(
        m_scrollingRecognitionControls, "Vertical scrolling",
        custom_outlined_icons::ScrollingVertical(), actionButtonMetrics(m_physicalScale));
    m_scrollingHorizontalButton = createScreenshotToolPaletteStyleActionButton(
        m_scrollingRecognitionControls, "Horizontal scrolling",
        custom_outlined_icons::ScrollingHorizontal(), actionButtonMetrics(m_physicalScale));
    m_scrollingVerticalButton->setObjectName(QStringLiteral("screenshotScrollingVerticalButton"));
    m_scrollingHorizontalButton->setObjectName(
        QStringLiteral("screenshotScrollingHorizontalButton"));
    layout->addWidget(m_scrollingVerticalButton);
    layout->addWidget(m_scrollingHorizontalButton);
    m_selectActionLayout->addWidget(m_scrollingRecognitionControls);
    connect(m_scrollingVerticalButton, &adqt::widgets::AdButton::clicked, this,
            [this]() {
                setScrollingRecognitionMode(ScreenshotScrollingRecognitionMode::Vertical);
            });
    connect(m_scrollingHorizontalButton, &adqt::widgets::AdButton::clicked, this,
            [this]() {
                setScrollingRecognitionMode(ScreenshotScrollingRecognitionMode::Horizontal);
            });
    updateScrollingRecognitionButtons();
}

bool ScreenshotToolPalette::ensureStyleFamily(Tool tool) {
    if (!m_options.enableStyleToolbar || m_releasingSecondaryResources ||
        !toolUsesStyleToolbar(tool)) {
        return false;
    }
    createSecondaryToolbarShell();
    const int key = static_cast<int>(tool);
    const MaterializationState state =
        m_styleFamilyStates.value(key, MaterializationState::Uninitialized);
    if (state == MaterializationState::Ready) {
        return true;
    }
    if (state == MaterializationState::Constructing) {
        return false;
    }

    QVector<Tool> constructing{tool};
    if (tool == Tool::Shape || tool == Tool::Line || tool == Tool::FreeDraw) {
        constructing = {Tool::Shape, Tool::Line, Tool::FreeDraw};
    } else if (tool == Tool::RectangleHighlight || tool == Tool::PenHighlight) {
        constructing = {Tool::RectangleHighlight, Tool::PenHighlight};
    } else if (tool == Tool::RectangleFilter || tool == Tool::PenFilter) {
        constructing = {Tool::RectangleFilter, Tool::PenFilter};
    }
    for (Tool member : constructing) {
        m_styleFamilyStates.insert(static_cast<int>(member), MaterializationState::Constructing);
    }
    createStyleFamily(tool);
    const bool ready = styleControlsForTool(tool) != nullptr;
    for (Tool member : constructing) {
        m_styleFamilyStates.insert(static_cast<int>(member),
                                   ready ? MaterializationState::Ready
                                         : MaterializationState::Uninitialized);
    }
    if (!ready) {
        return false;
    }
    replayMaterializedState(tool);
    SNOW_SHOT_TOOLBAR_PERF_COUNTER("hydrate.style_family");
    return true;
}

void ScreenshotToolPalette::createStyleFamily(Tool tool) {
    if (m_rectangleStylePanel == nullptr || m_rectangleStyleLayout == nullptr) {
        return;
    }
    const auto createLayout = [this](QWidget* controls) {
        auto* layout = new QHBoxLayout(controls);
        m_styleControlLayouts.push_back(layout);
        layout->setContentsMargins(0, 0, 0, 0);
        layout->setSpacing(scaledMetric(STYLE_ITEM_SPACING));
        return layout;
    };
    const auto addSeparator = [this](QBoxLayout* layout, QWidget* parent) {
        addStyleToolbarSpacing(layout, STYLE_GROUP_SPACING);
        layout->addWidget(createStyleToolbarSeparator(parent));
        addStyleToolbarSpacing(layout, STYLE_GROUP_SPACING);
    };

    if ((tool == Tool::Shape || tool == Tool::Line || tool == Tool::FreeDraw) &&
        m_rectangleStyleControlsWidget == nullptr) {
        m_rectangleStyleControlsWidget = new QWidget(m_rectangleStylePanel);
        m_rectangleStyleControlsWidget->setObjectName(
            QStringLiteral("screenshotRectangleStyleControls"));
        auto* layout = createLayout(m_rectangleStyleControlsWidget);
        m_styleControls->addShapeControls(layout, m_rectangleStyleControlsWidget, this,
                                          styleButtonMetrics(m_physicalScale));
        m_shapeStyleGroupSeparatorLeadingSpacing =
            addStyleToolbarSpacing(layout, STYLE_GROUP_SPACING);
        m_shapeStyleGroupSeparator = createStyleToolbarSeparator(m_rectangleStyleControlsWidget);
        m_shapeStyleGroupSeparator->setObjectName(
            QStringLiteral("screenshotShapeStyleGroupSeparator"));
        layout->addWidget(m_shapeStyleGroupSeparator);
        m_shapeStyleGroupSeparatorTrailingSpacing =
            addStyleToolbarSpacing(layout, STYLE_GROUP_SPACING);
        m_styleControls->addStrokeColorControls(layout, m_rectangleStyleControlsWidget, this,
                                                styleButtonMetrics(m_physicalScale));
        addSeparator(layout, m_rectangleStyleControlsWidget);
        m_styleControls->addStrokeWidthControls(layout, m_rectangleStyleControlsWidget, this,
                                                styleButtonMetrics(m_physicalScale));
        addSeparator(layout, m_rectangleStyleControlsWidget);
        m_styleControls->addFillColorControls(layout, m_rectangleStyleControlsWidget, this,
                                              styleButtonMetrics(m_physicalScale));
        addStyleToolbarSpacing(layout, STYLE_ITEM_SPACING);
        m_styleControls->addCornerRadiusControl(layout, m_rectangleStyleControlsWidget,
                                                styleButtonMetrics(m_physicalScale));
        registerStyleFamily(m_rectangleStyleControlsWidget,
                            {Tool::Shape, Tool::Line, Tool::FreeDraw});
        return;
    }
    if (tool == Tool::Arrow && m_arrowStyleControlsWidget == nullptr) {
        m_arrowStyleControlsWidget = new QWidget(m_rectangleStylePanel);
        m_arrowStyleControlsWidget->setObjectName(QStringLiteral("screenshotArrowStyleControls"));
        auto* layout = createLayout(m_arrowStyleControlsWidget);
        m_styleControls->addArrowControls(
            layout, m_arrowStyleControlsWidget, this,
            [this, layout]() {
                addStyleToolbarSpacing(layout, STYLE_GROUP_SPACING);
                layout->addWidget(createStyleToolbarSeparator(m_arrowStyleControlsWidget));
                addStyleToolbarSpacing(layout, STYLE_GROUP_SPACING);
            },
            styleButtonMetrics(m_physicalScale));
        registerStyleFamily(m_arrowStyleControlsWidget, {Tool::Arrow});
        return;
    }
    if ((tool == Tool::RectangleHighlight || tool == Tool::PenHighlight) &&
        m_highlightStyleControlsWidget == nullptr) {
        const QVector<StyleModeOption> modes{
            {QStringLiteral("Pen highlight"), outlined_icons::Highlight(), Tool::PenHighlight},
            {QStringLiteral("Rectangle highlight"), custom_outlined_icons::ShapeRectangle(),
             Tool::RectangleHighlight}};
        m_highlightStyleControlsWidget = new QWidget(m_rectangleStylePanel);
        m_highlightStyleControlsWidget->setObjectName(
            QStringLiteral("screenshotHighlightStyleControls"));
        auto* rectangleLayout = createLayout(m_highlightStyleControlsWidget);
        m_styleControls->addHighlightControls(
            rectangleLayout, m_highlightStyleControlsWidget, this,
            [this, rectangleLayout]() {
                addStyleToolbarSpacing(rectangleLayout, STYLE_GROUP_SPACING);
                rectangleLayout->addWidget(
                    createStyleToolbarSeparator(m_highlightStyleControlsWidget));
                addStyleToolbarSpacing(rectangleLayout, STYLE_GROUP_SPACING);
            }, styleButtonMetrics(m_physicalScale));
        rectangleLayout->insertWidget(
            0, createStyleModeSelector(m_highlightStyleControlsWidget,
                                       QStringLiteral("screenshotHighlightModeSelector"), modes,
                                       Tool::PenHighlight, m_highlightModeGroups));
        insertStyleToolbarSpacing(rectangleLayout, 1, STYLE_GROUP_SPACING);
        registerStyleFamily(m_highlightStyleControlsWidget, {Tool::RectangleHighlight});

        m_penHighlightStyleControlsWidget = new QWidget(m_rectangleStylePanel);
        m_penHighlightStyleControlsWidget->setObjectName(
            QStringLiteral("screenshotPenHighlightStyleControls"));
        auto* penLayout = createLayout(m_penHighlightStyleControlsWidget);
        m_styleControls->addPenHighlightControls(
            penLayout, m_penHighlightStyleControlsWidget, this,
            [this, penLayout]() {
                addStyleToolbarSpacing(penLayout, STYLE_GROUP_SPACING);
                penLayout->addWidget(
                    createStyleToolbarSeparator(m_penHighlightStyleControlsWidget));
                addStyleToolbarSpacing(penLayout, STYLE_GROUP_SPACING);
            }, styleButtonMetrics(m_physicalScale));
        penLayout->insertWidget(
            0, createStyleModeSelector(m_penHighlightStyleControlsWidget,
                                       QStringLiteral("screenshotHighlightModeSelector"), modes,
                                       Tool::PenHighlight, m_highlightModeGroups));
        insertStyleToolbarSpacing(penLayout, 1, STYLE_GROUP_SPACING);
        registerStyleFamily(m_penHighlightStyleControlsWidget, {Tool::PenHighlight});
        return;
    }
    if (tool == Tool::Spotlight && m_spotlightStyleControlsWidget == nullptr) {
        m_spotlightStyleControlsWidget = new QWidget(m_rectangleStylePanel);
        m_spotlightStyleControlsWidget->setObjectName(
            QStringLiteral("screenshotSpotlightStyleControls"));
        auto* layout = createLayout(m_spotlightStyleControlsWidget);
        m_styleControls->addSpotlightColorControls(
            layout, m_spotlightStyleControlsWidget, this,
            m_styleControls->styleState().spotlightConfig.color, spotlightColorValues(),
            [this](const QColor& color) {
                m_styleControls->styleState().spotlightConfig.color = color;
                m_styleControls->updateSpotlightColorControls(color);
                if (!m_replayingMaterializedState) {
                    emit spotlightConfigChanged(m_styleControls->styleState().spotlightConfig);
                }
            },
            [this](const QColor& color) {
                m_styleControls->styleState().spotlightConfig.color = color;
                if (!m_replayingMaterializedState) {
                    emit spotlightPreviewChanged(m_styleControls->styleState().spotlightConfig);
                }
            }, styleButtonMetrics(m_physicalScale));
        addSeparator(layout, m_spotlightStyleControlsWidget);
        ScreenshotToolPaletteSliderEditorConfig config;
        config.iconObjectName = QStringLiteral("screenshotSpotlightOpacityIcon");
        config.sliderObjectName = QStringLiteral("screenshotSpotlightOpacitySlider");
        config.accessibleName = QStringLiteral("Opacity");
        config.sliderTooltip = QStringLiteral("Adjust opacity");
        config.iconRef = custom_outlined_icons::Opacity();
        config.initialValue = qRound(std::clamp(
            m_styleControls->styleState().spotlightConfig.opacity, 0.0, 1.0) * 100.0);
        config.baseIconSize = COMPACT_SLIDER_ICON_SIZE;
        config.baseSliderWidth = COMPACT_SLIDER_WIDTH;
        const auto editor = createScreenshotToolPaletteSliderEditor(
            layout, m_spotlightStyleControlsWidget, config, styleButtonMetrics(m_physicalScale));
        m_spotlightOpacityIcon = editor.icon;
        m_spotlightOpacitySlider = editor.slider;
        connect(m_spotlightOpacitySlider, &adqt::widgets::AdSlider::valueChanged, this,
                [this](double value) {
                    m_styleControls->styleState().spotlightConfig.opacity =
                        std::clamp(value / 100.0, 0.0, 1.0);
                    m_spotlightOpacitySlider->setAccessibleDescription(
                        QStringLiteral("%1%").arg(qRound(value)));
                    if (!m_replayingMaterializedState) {
                        emit spotlightConfigChanged(m_styleControls->styleState().spotlightConfig);
                    }
                });
        registerStyleFamily(m_spotlightStyleControlsWidget, {Tool::Spotlight});
        return;
    }
    if (tool == Tool::Text && m_textStyleControlsWidget == nullptr) {
        m_textStyleControlsWidget = new QWidget(m_rectangleStylePanel);
        m_textStyleControlsWidget->setObjectName(QStringLiteral("screenshotTextStyleControls"));
        auto* layout = createLayout(m_textStyleControlsWidget);
        m_styleControls->addTextControls(
            layout, m_textStyleControlsWidget, this,
            [this, layout]() {
                addStyleToolbarSpacing(layout, STYLE_GROUP_SPACING);
                layout->addWidget(createStyleToolbarSeparator(m_textStyleControlsWidget));
                addStyleToolbarSpacing(layout, STYLE_GROUP_SPACING);
            },
            styleButtonMetrics(m_physicalScale));
        registerStyleFamily(m_textStyleControlsWidget, {Tool::Text});
        return;
    }
    if (tool == Tool::SerialNumber && m_serialNumberStyleControlsWidget == nullptr) {
        m_serialNumberStyleControlsWidget = new QWidget(m_rectangleStylePanel);
        m_serialNumberStyleControlsWidget->setObjectName(
            QStringLiteral("screenshotSerialNumberStyleControls"));
        auto* layout = createLayout(m_serialNumberStyleControlsWidget);
        m_styleControls->addSerialNumberControls(
            layout, m_serialNumberStyleControlsWidget, this,
            [this, layout]() {
                addStyleToolbarSpacing(layout, STYLE_GROUP_SPACING);
                layout->addWidget(createStyleToolbarSeparator(m_serialNumberStyleControlsWidget));
                addStyleToolbarSpacing(layout, STYLE_GROUP_SPACING);
            },
            styleButtonMetrics(m_physicalScale));
        registerStyleFamily(m_serialNumberStyleControlsWidget, {Tool::SerialNumber});
        return;
    }
    if ((tool == Tool::RectangleFilter || tool == Tool::PenFilter) &&
        m_filterStyleControlsWidget == nullptr) {
        m_filterEditor = createFilterEditor({Tool::RectangleFilter,
            QStringLiteral("screenshotFilterStyleControls"),
            QStringLiteral("screenshotFilterTypeSelect"),
            QStringLiteral("screenshotFilterIntensityIcon"),
            QStringLiteral("screenshotFilterIntensitySlider"), false});
        m_filterStyleControlsWidget = m_filterEditor.controls;
        refreshFilterEditorState(m_filterEditor, false);
        registerStyleFamily(m_filterStyleControlsWidget, {Tool::RectangleFilter});
        m_penFilterEditor = createFilterEditor({Tool::PenFilter,
            QStringLiteral("screenshotPenFilterStyleControls"),
            QStringLiteral("screenshotPenFilterTypeSelect"),
            QStringLiteral("screenshotPenFilterIntensityIcon"),
            QStringLiteral("screenshotPenFilterIntensitySlider"), true});
        m_penFilterStyleControlsWidget = m_penFilterEditor.controls;
        refreshFilterEditorState(m_penFilterEditor, true);
        registerStyleFamily(m_penFilterStyleControlsWidget, {Tool::PenFilter});
        return;
    }
    if (tool == Tool::Watermark && m_watermarkStyleControlsWidget == nullptr) {
        m_watermarkStyleControlsWidget = new QWidget(m_rectangleStylePanel);
        m_watermarkStyleControlsWidget->setObjectName(
            QStringLiteral("screenshotWatermarkStyleControls"));
        auto* layout = createLayout(m_watermarkStyleControlsWidget);
        m_styleControls->addWatermarkControls(
            layout, m_watermarkStyleControlsWidget, this,
            [this, layout]() {
                addStyleToolbarSpacing(layout, STYLE_GROUP_SPACING);
                layout->addWidget(createStyleToolbarSeparator(m_watermarkStyleControlsWidget));
                addStyleToolbarSpacing(layout, STYLE_GROUP_SPACING);
            },
            styleButtonMetrics(m_physicalScale));
        registerStyleFamily(m_watermarkStyleControlsWidget, {Tool::Watermark});
    }
}

void ScreenshotToolPalette::replayMaterializedState(Tool tool) {
    m_replayingMaterializedState = true;
    m_styleControls->refreshThemeIcons(styleButtonMetrics(m_physicalScale));
    if (tool == Tool::Spotlight) {
        setSpotlightConfig(m_styleControls->styleState().spotlightConfig);
    }
    if (tool == Tool::Watermark) {
        m_styleControls->setWatermarkConfig(m_styleControls->styleState().m_watermarkConfig);
    }
    m_replayingMaterializedState = false;
}

void ScreenshotToolPalette::addRecordingControls(QBoxLayout* layout) {
    if (layout == nullptr) {
        return;
    }

    const auto addItemSpacing = [this]() { addMainToolbarSpacing(TOOLBAR_ITEM_SPACING); };

    m_recordStartButton =
        addActionButton("Start recording", primaryIcon(custom_outlined_icons::RecordingStart()));
    m_recordStopButton =
        addActionButton("Stop recording", custom_outlined_icons::RecordingStop(), true);
    m_recordPauseButton = addActionButton("Pause recording", outlined_icons::Pause());
    m_recordResumeButton =
        addActionButton("Resume recording", primaryIcon(custom_outlined_icons::RecordingResume()));
    layout->addWidget(m_recordStartButton);
    layout->addWidget(m_recordStopButton);
    addItemSpacing();
    layout->addWidget(m_recordPauseButton);
    layout->addWidget(m_recordResumeButton);

    addItemSpacing();
    m_recordDurationLabel = new QLabel(QStringLiteral("00:00:00"), m_mainPanel);
    m_recordDurationLabel->setObjectName(QStringLiteral("screenRecordingDuration"));
    m_recordDurationLabel->setAlignment(Qt::AlignCenter);
    m_recordDurationLabel->setAccessibleName(tr("Recording duration"));
    m_recordDurationLabel->setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Fixed);
    layout->addWidget(m_recordDurationLabel);
    addItemSpacing();

    m_recordMicrophoneButton =
        addActionButton("Record microphone", custom_outlined_icons::RecordingMicrophone());
    m_recordSystemAudioButton = addActionButton("Record speakers", outlined_icons::Sound());
    layout->addWidget(m_recordMicrophoneButton);
    addItemSpacing();
    layout->addWidget(m_recordSystemAudioButton);

    addMainToolbarSeparator();
    m_recordOpenFolderButton =
        addActionButton("Open recording folder", custom_outlined_icons::RecordingFolder());
    m_recordCloseButton = addActionButton("Close recording", outlined_icons::Close(), true);
    m_recordCopyAnimatedImageButton =
        addActionButton("Copy animated image", outlined_icons::Gif());
    m_recordCopyVideoButton = addActionButton("Copy video", outlined_icons::Copy());
    layout->addWidget(m_recordOpenFolderButton);
    addItemSpacing();
    layout->addWidget(m_recordCloseButton);
    addItemSpacing();
    layout->addWidget(m_recordCopyAnimatedImageButton);
    addItemSpacing();
    layout->addWidget(m_recordCopyVideoButton);

    connect(m_recordStartButton, &adqt::widgets::AdButton::clicked, this,
            &ScreenshotToolPalette::recordingStartRequested);
    connect(m_recordStopButton, &adqt::widgets::AdButton::clicked, this,
            &ScreenshotToolPalette::recordingStopRequested);
    connect(m_recordPauseButton, &adqt::widgets::AdButton::clicked, this,
            &ScreenshotToolPalette::recordingPauseRequested);
    connect(m_recordResumeButton, &adqt::widgets::AdButton::clicked, this,
            &ScreenshotToolPalette::recordingResumeRequested);
    connect(m_recordMicrophoneButton, &adqt::widgets::AdButton::clicked, this, [this]() {
        setRecordingMicrophoneEnabled(!m_recordingMicrophoneEnabled);
        emit recordingMicrophoneToggled(m_recordingMicrophoneEnabled);
    });
    connect(m_recordSystemAudioButton, &adqt::widgets::AdButton::clicked, this, [this]() {
        setRecordingSystemAudioEnabled(!m_recordingSystemAudioEnabled);
        emit recordingSystemAudioToggled(m_recordingSystemAudioEnabled);
    });
    connect(m_recordOpenFolderButton, &adqt::widgets::AdButton::clicked, this,
            &ScreenshotToolPalette::recordingOpenFolderRequested);
    connect(m_recordCloseButton, &adqt::widgets::AdButton::clicked, this,
            &ScreenshotToolPalette::recordingCloseRequested);
    connect(m_recordCopyAnimatedImageButton, &adqt::widgets::AdButton::clicked, this,
            &ScreenshotToolPalette::recordingCopyAnimatedImageRequested);
    connect(m_recordCopyVideoButton, &adqt::widgets::AdButton::clicked, this,
            &ScreenshotToolPalette::recordingCopyVideoRequested);

    updateRecordingControls();
    updateRecordingControlMetrics();
}

void ScreenshotToolPalette::updateToolbarRowGeometry(bool styleToolbarVisible) {
    SNOW_SHOT_TOOLBAR_PERF_SCOPE("palette.update_row_geometry");
    if (m_rootLayout == nullptr || m_mainPanel == nullptr) {
        return;
    }

    if (m_rowOrderDirty) {
        m_rootLayout->removeWidget(m_mainPanel);
        if (m_selectActionPanel != nullptr) {
            m_rootLayout->removeWidget(m_selectActionPanel);
        }
        if (m_rectangleStylePanel != nullptr) {
            m_rootLayout->removeWidget(m_rectangleStylePanel);
        }
        if (m_styleToolbarAboveMain) {
            if (m_selectActionPanel != nullptr) {
                m_rootLayout->addWidget(m_selectActionPanel, 0, Qt::AlignRight);
            }
            if (m_rectangleStylePanel != nullptr) {
                m_rootLayout->addWidget(m_rectangleStylePanel, 0, Qt::AlignRight);
            }
            m_rootLayout->addWidget(m_mainPanel, 0, Qt::AlignRight);
        } else {
            m_rootLayout->addWidget(m_mainPanel, 0, Qt::AlignRight);
            if (m_selectActionPanel != nullptr) {
                m_rootLayout->addWidget(m_selectActionPanel, 0, Qt::AlignRight);
            }
            if (m_rectangleStylePanel != nullptr) {
                m_rootLayout->addWidget(m_rectangleStylePanel, 0, Qt::AlignRight);
            }
        }
        m_rowOrderDirty = false;
        SNOW_SHOT_TOOLBAR_PERF_COUNTER("layout.row_reorder");
#if defined(SNOW_SHOT_TEST_HOOKS)
        ++m_rootRowReorderCount;
#endif
    }
    // `isVisible()` is false while the palette's parent is hidden, even when
    // the child has never been explicitly hidden. Use the child visibility
    // state so a new palette hides its secondary rows before first display.
    if (m_selectActionPanel != nullptr &&
        m_selectActionPanel->isHidden() == m_actionToolbarTargetVisible) {
        m_selectActionPanel->setVisible(m_actionToolbarTargetVisible);
    }
    if (m_rectangleStylePanel != nullptr &&
        m_rectangleStylePanel->isHidden() == styleToolbarVisible) {
        m_rectangleStylePanel->setVisible(styleToolbarVisible);
    }
}

void ScreenshotToolPalette::setActiveToolButton(adqt::widgets::AdButton* activeButton) {
    m_activeToolButton = activeButton;
    adqt::widgets::AdButton* buttons[] = {
        m_moveButton,
        m_selectButton,
        m_shapeButton,
        m_arrowButton,
        m_lineButton,
        m_freeDrawButton,
        m_highlighterButton,
        m_spotlightButton,
        m_eraserButton,
        m_filterButton,
        m_watermarkButton,
        m_textButton,
        m_serialNumberButton,
        m_ocrButton,
        m_textTranslationButton,
        m_tableButton,
        m_scrollingScreenshotButton,
    };

    for (adqt::widgets::AdButton* button : buttons) {
        if (button == nullptr) {
            continue;
        }

        applyMainToolbarToolActiveStyle(button, button == activeButton);
    }
    for (const DrawingToolGroup& group : std::as_const(m_drawingToolGroups)) {
        if (group.trigger != nullptr) {
            applyMainToolbarToolActiveStyle(group.trigger, group.trigger == activeButton);
        }
    }

    const int activeValue = m_activeTool.has_value()
                                ? static_cast<int>(toolbarFacingDrawingTool(*m_activeTool))
                                : -1;
    for (const DrawingToolGroup& group : std::as_const(m_drawingToolGroups)) {
        updateScreenshotToolPaletteOptionPopoverEditor(group.optionButtons, group.optionValues,
                                                       activeValue);
    }
    updateScreenshotToolPaletteOptionPopoverEditor(m_tableQrOptionButtons, m_tableQrOptionValues,
                                                   activeValue);
}

#if defined(SNOW_SHOT_TEST_HOOKS)
std::optional<ScreenshotToolPalette::Tool> ScreenshotToolPalette::activeToolForTests() const {
    return m_activeTool;
}

quint64 ScreenshotToolPalette::styleStateNoopCountForTests() const {
    return m_styleStateNoopCount +
           (m_styleControls != nullptr ? m_styleControls->styleStateNoopCount() : 0);
}

quint64 ScreenshotToolPalette::propertyGroupRefreshCountForTests() const {
    return m_propertyGroupRefreshCount +
           (m_styleControls != nullptr ? m_styleControls->propertyGroupRefreshCount() : 0);
}

quint64 ScreenshotToolPalette::layoutCommitCountForTests() const {
    return m_layoutCommitCount;
}

SnowCanvasStyleDefaults ScreenshotToolPalette::styleStateForTests() const {
    return m_styleControls->creationStyleDefaults();
}

ScreenshotToolPalette::MaterializationState
ScreenshotToolPalette::actionFamilyStateForTests(ActionFamily family) const {
    return m_actionFamilyStates.value(static_cast<int>(family),
                                      MaterializationState::Uninitialized);
}

ScreenshotToolPalette::MaterializationState
ScreenshotToolPalette::styleFamilyStateForTests(Tool tool) const {
    return m_styleFamilyStates.value(static_cast<int>(tool),
                                     MaterializationState::Uninitialized);
}
#endif

QWidget* ScreenshotToolPalette::styleControlsForTool(Tool tool) const {
    for (const StyleEditorBinding& binding : m_styleEditorBindings) {
        if (binding.tools.contains(tool)) {
            return binding.controls;
        }
    }
    return nullptr;
}

bool ScreenshotToolPalette::setStyleControlsActive(Tool tool) {
    synchronizeFilterModeGroups(tool);
    if (tool == Tool::RectangleHighlight || tool == Tool::PenHighlight) {
        for (adqt::widgets::AdRadioButtonGroup* group : m_highlightModeGroups) {
            if (group != nullptr) {
                group->setCheckedId(static_cast<int>(tool));
            }
        }
    }
    if (m_activeStyleTool.has_value() && *m_activeStyleTool == tool) {
        return false;
    }
    m_activeStyleTool = tool;

    m_activeStyleControlsWidget = styleControlsForTool(tool);
    if (m_styleControls != nullptr) {
        m_styleControls->setLineControlsActive(tool == Tool::Line);
        m_styleControls->setFreeDrawControlsActive(tool == Tool::FreeDraw);
        m_styleControls->setHighlightControlsActive(tool == Tool::RectangleHighlight);
        m_styleControls->setPenHighlightControlsActive(tool == Tool::PenHighlight);
        m_styleControls->setArrowControlsActive(tool == Tool::Arrow);
        m_styleControls->setTextControlsActive(tool == Tool::Text);
    }
    applyStyleMetricsForScope(m_activeStyleControlsWidget);
    if (m_rectangleStyleControlsWidget != nullptr) {
        m_rectangleStyleControlsWidget->setObjectName(
            tool == Tool::Line       ? QStringLiteral("screenshotLineStyleControls")
            : tool == Tool::FreeDraw ? QStringLiteral("screenshotFreeDrawStyleControls")
                                     : QStringLiteral("screenshotRectangleStyleControls"));
    }
    if (m_shapeStyleGroupSeparator != nullptr) {
        const bool shapeControlsVisible = tool != Tool::Line && tool != Tool::FreeDraw;
        m_shapeStyleGroupSeparator->setVisible(shapeControlsVisible);
        setStyleToolbarSpacingVisible(m_shapeStyleGroupSeparatorLeadingSpacing,
                                      shapeControlsVisible);
        setStyleToolbarSpacingVisible(m_shapeStyleGroupSeparatorTrailingSpacing,
                                      shapeControlsVisible);
    }
    for (const StyleEditorBinding& binding : std::as_const(m_styleEditorBindings)) {
        if (binding.controls != nullptr) {
            binding.controls->setVisible(binding.controls == m_activeStyleControlsWidget);
        }
    }
    if (m_rectangleStyleLayout != nullptr) {
        m_rectangleStyleLayout->invalidate();
    }
    applyCumulativeStyleLayoutMetrics(m_activeStyleControlsWidget);
    // The cumulative row sizing pass may round nested slider widths up or down
    // to fit the row. Reapply the compact editor metrics so filter and
    // spotlight sliders retain their explicit, shared width at every scale.
    if (m_activeStyleControlsWidget == m_filterStyleControlsWidget) {
        refreshFilterEditorMetrics(m_filterEditor);
    } else if (m_activeStyleControlsWidget == m_penFilterStyleControlsWidget) {
        refreshFilterEditorMetrics(m_penFilterEditor);
    } else if (m_activeStyleControlsWidget == m_spotlightStyleControlsWidget) {
        ScreenshotToolPaletteSliderEditor spotlightEditor;
        spotlightEditor.icon = m_spotlightOpacityIcon;
        spotlightEditor.slider = m_spotlightOpacitySlider;
        spotlightEditor.iconRef = custom_outlined_icons::Opacity();
        spotlightEditor.baseIconSize = COMPACT_SLIDER_ICON_SIZE;
        spotlightEditor.baseSliderWidth = COMPACT_SLIDER_WIDTH;
        configureScreenshotToolPaletteSliderEditor(spotlightEditor,
                                                   styleButtonMetrics(m_physicalScale));
    }
    return true;
}

bool ScreenshotToolPalette::applyActiveToolSecondaryToolbarVisibility() {
    if (!m_activeTool.has_value()) {
        return setSecondaryToolbarVisibility(false, false);
    }
    return setSecondaryToolbarVisibility(toolUsesActionToolbar(*m_activeTool),
                                         toolUsesStyleToolbar(*m_activeTool));
}

bool ScreenshotToolPalette::activeToolUsesStyleToolbar() const {
    return m_activeTool.has_value() && toolUsesStyleToolbar(*m_activeTool);
}

void ScreenshotToolPalette::updateRecordingControls() {
    const bool idle = m_recordingState == RecordingState::Idle;
    const bool recording = m_recordingState == RecordingState::Recording;
    const bool paused = m_recordingState == RecordingState::Paused;
    const bool active = recording || paused;
    const bool visibilityChanged =
        (m_recordStartButton != nullptr && m_recordStartButton->isVisible() != idle) ||
        (m_recordStopButton != nullptr && m_recordStopButton->isVisible() != active) ||
        (m_recordPauseButton != nullptr && m_recordPauseButton->isVisible() != !paused) ||
        (m_recordResumeButton != nullptr && m_recordResumeButton->isVisible() != paused);

    if (m_recordStartButton != nullptr) {
        m_recordStartButton->setVisible(idle);
        m_recordStartButton->setEnabled(idle && !m_recordingBusy);
    }
    if (m_recordStopButton != nullptr) {
        m_recordStopButton->setVisible(active);
        m_recordStopButton->setEnabled(active && !m_recordingBusy);
    }
    if (m_recordPauseButton != nullptr) {
        const bool pauseEnabled = recording && !m_recordingBusy;
        const auto scheme = snow_shot::presentation::styles::generateThemeColorScheme();

        // Mirror the original recording toolbar: pause is a text action whose
        // glyph becomes warning-yellow only while a recording is in progress.
        // With no explicit tint in every other state, the disabled palette
        // supplies the normal gray visual.
        m_recordPauseButton->setButtonStyle(adqt::widgets::AdButton::ButtonStyle::Text);
        m_recordPauseButton->setAccentRole(adqt::widgets::AdButton::AccentRole::Neutral);
        m_recordPauseButton->setIconRef(pauseEnabled
                                            ? snow_shot::presentation::icons::withPrimaryColor(
                                                  outlined_icons::Pause(), scheme.map.colorWarning)
                                            : outlined_icons::Pause());
        m_recordPauseButton->setVisible(!paused);
        m_recordPauseButton->setEnabled(pauseEnabled);
    }
    if (m_recordResumeButton != nullptr) {
        m_recordResumeButton->setVisible(paused);
        m_recordResumeButton->setEnabled(paused && !m_recordingBusy);
    }
    if (m_recordMicrophoneButton != nullptr) {
        const bool microphoneControlEnabled = idle && !m_recordingBusy;
        const auto scheme = snow_shot::presentation::styles::generateThemeColorScheme();
        const QColor microphoneIconColor =
            m_recordingMicrophoneEnabled ? scheme.map.colorSuccess : scheme.map.colorTextQuaternary;

        // Match the original recording toolbar: this is always a text button,
        // with a green microphone icon when the setting is enabled and a
        // disabled-text icon when it is not.  Disabling interaction while
        // recording must not alter that visual state.
        m_recordMicrophoneButton->setButtonStyle(adqt::widgets::AdButton::ButtonStyle::Text);
        m_recordMicrophoneButton->setAccentRole(adqt::widgets::AdButton::AccentRole::Neutral);
        m_recordMicrophoneButton->setIconRef(snow_shot::presentation::icons::withPrimaryColor(
            custom_outlined_icons::RecordingMicrophone(), microphoneIconColor));
        m_recordMicrophoneButton->setEnabled(microphoneControlEnabled);
    }
    if (m_recordSystemAudioButton != nullptr) {
        const bool systemAudioControlEnabled = idle && !m_recordingBusy;
        const auto scheme = snow_shot::presentation::styles::generateThemeColorScheme();
        const QColor systemAudioIconColor = m_recordingSystemAudioEnabled
                                                ? scheme.map.colorSuccess
                                                : scheme.map.colorTextQuaternary;

        // Keep the system-audio toggle visually consistent with the
        // microphone toggle: no filled state, and disabling interaction must
        // not change the icon color selected by the setting.
        m_recordSystemAudioButton->setButtonStyle(adqt::widgets::AdButton::ButtonStyle::Text);
        m_recordSystemAudioButton->setAccentRole(adqt::widgets::AdButton::AccentRole::Neutral);
        m_recordSystemAudioButton->setIconRef(snow_shot::presentation::icons::withPrimaryColor(
            outlined_icons::Sound(), systemAudioIconColor));
        m_recordSystemAudioButton->setEnabled(systemAudioControlEnabled);
    }
    if (m_recordCloseButton != nullptr) {
        m_recordCloseButton->setEnabled(!m_recordingBusy);
    }
    if (m_recordCopyAnimatedImageButton != nullptr) {
        const bool copyGifEnabled = active && !m_recordingBusy;
        const auto scheme = snow_shot::presentation::styles::generateThemeColorScheme();

        // The original toolbar keeps this as a text button and highlights the
        // GIF glyph in the primary color only while an active recording can be
        // copied.  Leave the icon untinted when disabled so the button's
        // disabled palette supplies the correct gray.
        m_recordCopyAnimatedImageButton->setButtonStyle(
            adqt::widgets::AdButton::ButtonStyle::Text);
        m_recordCopyAnimatedImageButton->setAccentRole(
            adqt::widgets::AdButton::AccentRole::Neutral);
        m_recordCopyAnimatedImageButton->setIconRef(
            copyGifEnabled
                ? snow_shot::presentation::icons::withPrimaryColor(outlined_icons::Gif(),
                                                                    scheme.map.colorPrimary)
                : outlined_icons::Gif());
        m_recordCopyAnimatedImageButton->setEnabled(copyGifEnabled);
    }
    if (m_recordCopyVideoButton != nullptr) {
        const bool copyVideoEnabled = active && !m_recordingBusy;
        const auto scheme = snow_shot::presentation::styles::generateThemeColorScheme();

        // Keep the regular-copy action in lockstep with the reference toolbar
        // and the GIF-copy action: use the primary glyph only while copying is
        // possible, while the disabled button retains the normal gray glyph.
        m_recordCopyVideoButton->setButtonStyle(adqt::widgets::AdButton::ButtonStyle::Text);
        m_recordCopyVideoButton->setAccentRole(adqt::widgets::AdButton::AccentRole::Neutral);
        m_recordCopyVideoButton->setIconRef(
            copyVideoEnabled ? snow_shot::presentation::icons::withPrimaryColor(
                                   outlined_icons::Copy(), scheme.map.colorPrimary)
                             : outlined_icons::Copy());
        m_recordCopyVideoButton->setEnabled(copyVideoEnabled);
    }

    if (visibilityChanged) {
        updateToolbarGeometry();
        emit visibleContentChanged();
    }
}

void ScreenshotToolPalette::updateRecordingControlMetrics() {
    if (m_recordDurationLabel == nullptr) {
        return;
    }
    m_recordDurationLabel->setFixedSize(scaledMetric(RECORDING_DURATION_WIDTH),
                                        m_mainPanel != nullptr ? m_mainPanel->buttonSize()
                                                               : scaledMetric(32));
    QFont font = m_recordDurationLabel->font();
    font.setPixelSize(scaledMetric(RECORDING_DURATION_FONT_SIZE));
    font.setWeight(QFont::Normal);
    m_recordDurationLabel->setFont(font);
    const auto scheme = snow_shot::presentation::styles::generateThemeColorScheme();
    m_recordDurationLabel->setStyleSheet(
        QStringLiteral("color: %1;").arg(scheme.map.colorTextSecondary.name(QColor::HexArgb)));
}

QPoint ScreenshotToolPalette::contentOffset() const {
    return QPoint(m_shadowMargins.left(), m_shadowMargins.top());
}

QSize ScreenshotToolPalette::contentSizeForVisibleRows() const {
    int width = 0;
    int height = 0;
    int visibleRows = 0;
    const auto appendPanel = [&](const QWidget* panel, bool visible) {
        if (panel == nullptr || !visible) {
            return;
        }

        QSize panelSize = panel->size();
        if (panelSize.isEmpty()) {
            panelSize = panel->sizeHint();
        }
        if (panelSize.isEmpty()) {
            return;
        }

        width = std::max(width, panelSize.width());
        height += panelSize.height();
        ++visibleRows;
    };

    appendPanel(m_mainPanel, m_mainPanel != nullptr);
    const QWidget* secondaryPanel = m_actionToolbarTargetVisible
                                        ? m_selectActionPanel
                                        : m_styleToolbarTargetVisible
                                              ? m_rectangleStylePanel
                                              : nullptr;
    appendPanel(secondaryPanel, secondaryPanel != nullptr);

    if (visibleRows > 1) {
        height += scaledMetric(TOOLBAR_ROW_SPACING) * (visibleRows - 1);
    }

    return QSize(width, height);
}

QSize ScreenshotToolPalette::fullContentSize() const {
    const auto panelSize = [](const QWidget* panel) {
        if (panel == nullptr) {
            return QSize();
        }
        QSize size = panel->size();
        if (size.isEmpty()) {
            size = panel->sizeHint();
        }
        return size;
    };

    const QSize mainSize = panelSize(m_mainPanel);
    QSize maximumSecondarySize = panelSize(m_selectActionPanel);
    maximumSecondarySize = maximumSecondarySize.expandedTo(maximumSecondaryToolbarSizeHint());
    if (maximumSecondarySize.isEmpty()) {
        return mainSize;
    }

    return QSize(std::max(mainSize.width(), maximumSecondarySize.width()),
                 mainSize.height() + scaledMetric(TOOLBAR_ROW_SPACING) +
                     maximumSecondarySize.height());
}

QRect ScreenshotToolPalette::panelContentRect(const QWidget* panel) const {
    if (panel == nullptr) {
        return QRect();
    }
    return panel->geometry().translated(-contentOffset());
}

ScreenshotToolbarPlacementSnapshot ScreenshotToolPalette::buildPlacementSnapshot() const {
    ScreenshotToolbarPlacementSnapshot snapshot;
    snapshot.contentOffset = m_layoutResult.contentOffset;
    snapshot.contentSize = m_layoutResult.contentSize;

    if (m_mainPanel == nullptr) {
        return snapshot;
    }

    QSize mainSize = m_mainPanel->size();
    if (mainSize.isEmpty()) {
        mainSize = m_mainPanel->sizeHint();
    }
    if (mainSize.isEmpty()) {
        return snapshot;
    }

    const QWidget* secondaryPanel = nullptr;
    if (m_actionToolbarTargetVisible) {
        secondaryPanel = m_selectActionPanel;
    } else if (m_styleToolbarTargetVisible) {
        secondaryPanel = m_rectangleStylePanel;
    }

    QSize secondarySize;
    if (secondaryPanel != nullptr) {
        secondarySize = secondaryPanel->size();
        if (secondarySize.isEmpty()) {
            secondarySize = secondaryPanel->sizeHint();
        }
    }

    const bool hasSecondary = !secondarySize.isEmpty();
    const int visibleWidth = std::max(mainSize.width(), hasSecondary ? secondarySize.width() : 0);
    const int rowSpacing = hasSecondary ? scaledMetric(TOOLBAR_ROW_SPACING) : 0;
    const int visibleHeight = mainSize.height() + rowSpacing +
                              (hasSecondary ? secondarySize.height() : 0);
    snapshot.visibleContentSize = QSize(visibleWidth, visibleHeight);

    // Both rows share the content area's right edge.  Derive it from the
    // visible extent rather than a child geometry that may still be pending a
    // parent-layout activation during a first display.
    const int right = visibleWidth - 1;
    const QRect bottomMain(QPoint(right - mainSize.width() + 1, 0), mainSize);
    const int topMainY = hasSecondary ? secondarySize.height() + rowSpacing : 0;
    const QRect topMain(QPoint(right - mainSize.width() + 1, topMainY), mainSize);
    QRect bottomSecondary;
    QRect topSecondary;
    if (hasSecondary) {
        const int secondaryX = right - secondarySize.width() + 1;
        bottomSecondary = QRect(QPoint(secondaryX, mainSize.height() + rowSpacing), secondarySize);
        topSecondary = QRect(QPoint(secondaryX, topMain.top() - rowSpacing - secondarySize.height()),
                             secondarySize);
    }

    const auto occupied = [](const QRect& mainRect, const QRect& secondaryRect) {
        return secondaryRect.isEmpty() ? mainRect : mainRect.united(secondaryRect);
    };
    snapshot.bottom = ScreenshotToolbarPlacementGeometry{
        bottomMain, bottomSecondary, occupied(bottomMain, bottomSecondary)};
    snapshot.top = ScreenshotToolbarPlacementGeometry{
        topMain, topSecondary, occupied(topMain, topSecondary)};
    return snapshot;
}
