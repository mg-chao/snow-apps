#ifndef SNOW_SHOT_PRESENTATION_SCREENSHOTTOOLPALETTESTYLECOMPONENTS_H
#define SNOW_SHOT_PRESENTATION_SCREENSHOTTOOLPALETTESTYLECOMPONENTS_H

#include "screenshottoolpalettebuttons.h"
#include "snow_draw_engine_qt/snow_canvas_types.h"

#include <QColor>
#include <QString>
#include <QStringList>
#include <QVector>

#include <functional>

class QBoxLayout;
class QObject;
class QWidget;

namespace adqt::widgets {
class AdButton;
class AdColorPicker;
class AdPopover;
class AdSelect;
} // namespace adqt::widgets

namespace snow_shot::presentation {

// Cross-editor services injected by the style-controls owner. Components stay
// palette-agnostic: canvas sampling and popup-lifecycle bookkeeping are
// forwarded through these callbacks instead of reaching into the palette.
struct ScreenshotToolPaletteEditorServices {
    std::function<void(adqt::widgets::AdColorPicker*)> canvasColorSamplingRequested;
    std::function<void(QObject*)> popupInteractionBegan;
    std::function<void(QObject*)> popupInteractionEnded;
};

// Base protocol shared by every reusable sub-toolbar editor component. A
// component owns the widgets of one logical editor (built lazily into a row
// layout), applies inbound style state through update(), and refreshes its
// own metrics. Widget lifetimes stay owned by the row widget; release()
// drops non-owning pointers when the row is evicted.
class ScreenshotToolPaletteStyleEditorComponent {
  public:
    virtual ~ScreenshotToolPaletteStyleEditorComponent() = default;

    virtual void refreshMetrics(const ScreenshotToolPaletteButtonMetrics& metrics) = 0;
    // Popup content owns its window DPR and is intentionally excluded from
    // toolbar scaling; this pass resets popup buttons after a DPI commit.
    virtual void resetPopupMetrics(const ScreenshotToolPaletteButtonMetrics& metrics) {
        static_cast<void>(metrics);
    }
    // True while the user is actively editing through this component (for
    // example dragging a color); inbound state sync queries it to keep the
    // in-flight value authoritative.
    [[nodiscard]] virtual bool isInteracting() const {
        return false;
    }
    virtual void release() = 0;

  protected:
    ScreenshotToolPaletteStyleEditorComponent() = default;
};

struct ScreenshotToolPaletteColorEditorConfig {
    QString accessibleName;
    QString pickerObjectName;
    QString triggerObjectName;
    QVector<QColor> presetValues;
    std::function<ScreenshotToolPaletteTranslationText(const QColor& color)> presetTooltip;
    bool alphaEnabled = false;
    bool observePopup = false;
};

// Color picker trigger plus preset swatches. Shared by the highlight,
// pen-highlight, text, serial number, watermark and spotlight editors.
class ScreenshotToolPaletteColorEditor final : public ScreenshotToolPaletteStyleEditorComponent {
  public:
    void build(QBoxLayout* layout, QWidget* parent, QObject* receiver,
               const ScreenshotToolPaletteColorEditorConfig& config, const QColor& initialColor,
               const std::function<void(const QColor&)>& commitColor,
               const std::function<void(const QColor&)>& previewColor,
               const ScreenshotToolPaletteEditorServices& services,
               const ScreenshotToolPaletteButtonMetrics& metrics);
    void update(const QColor& color, bool mixed);
    void refreshMetrics(const ScreenshotToolPaletteButtonMetrics& metrics) override;
    [[nodiscard]] bool isInteracting() const override {
        return m_handlingChange;
    }
    void resetPopupMetrics(const ScreenshotToolPaletteButtonMetrics& metrics) override;
    void release() override;

  private:
    adqt::widgets::AdColorPicker* m_picker = nullptr;
    ColorSwatchButton* m_trigger = nullptr;
    QVector<adqt::widgets::AdButton*> m_presets;
    QVector<QColor> m_presetValues;
    bool m_handlingChange = false;
};

struct ScreenshotToolPaletteStrokeEditorConfig {
    QString accessibleName;
    QString popupObjectName;
    QString styleRowObjectName;
    QVector<QColor> colorValues;
    std::function<ScreenshotToolPaletteTranslationText(const QColor& color)> colorTooltip;
    std::function<ScreenshotToolPaletteTranslationText(SnowCanvasStrokeStyle style)> styleTooltip;
};

// Stroke color plus stroke style (solid/dashed/dotted). The one stroke editor
// shared verbatim by the shape/line/free-draw row and the arrow row.
class ScreenshotToolPaletteStrokeEditor final : public ScreenshotToolPaletteStyleEditorComponent {
  public:
    void build(QBoxLayout* layout, QWidget* parent, QObject* receiver,
               const ScreenshotToolPaletteStrokeEditorConfig& config, const QColor& initialColor,
               SnowCanvasStrokeStyle initialStyle,
               const std::function<void(const QColor&)>& setColor,
               const std::function<void(SnowCanvasStrokeStyle)>& setStyle,
               const ScreenshotToolPaletteEditorServices& services,
               const ScreenshotToolPaletteButtonMetrics& metrics);
    void update(const QColor& color, SnowCanvasStrokeStyle style, bool colorMixed, bool styleMixed);
    void refreshMetrics(const ScreenshotToolPaletteButtonMetrics& metrics) override;
    [[nodiscard]] bool isInteracting() const override {
        return m_handlingChange;
    }
    void resetPopupMetrics(const ScreenshotToolPaletteButtonMetrics& metrics) override;
    void release() override;

  private:
    adqt::widgets::AdColorPicker* m_picker = nullptr;
    StrokeStylePreviewTrigger* m_trigger = nullptr;
    QVector<adqt::widgets::AdButton*> m_colorPresets;
    QVector<QColor> m_colorValues;
    QVector<StrokeStylePreviewButton*> m_styleButtons;
    QVector<SnowCanvasStrokeStyle> m_styleValues;
    bool m_handlingChange = false;
};

struct ScreenshotToolPaletteFillEditorConfig {
    QString accessibleName;
    QString popupObjectName;
    QString presetRowObjectName;
    QVector<QColor> colorValues;
    std::function<ScreenshotToolPaletteTranslationText(const QColor& color)> colorTooltip;
    std::function<ScreenshotToolPaletteTranslationText(SnowCanvasFillStyle style)> styleTooltip;
    bool observePopup = false;
};

// Fill color plus fill style (solid/line/cross-line). Shared by the shape,
// text and serial number fill editors.
class ScreenshotToolPaletteFillEditor final : public ScreenshotToolPaletteStyleEditorComponent {
  public:
    void build(QBoxLayout* layout, QWidget* parent, QObject* receiver,
               const ScreenshotToolPaletteFillEditorConfig& config, const QColor& initialColor,
               SnowCanvasFillStyle initialStyle, const std::function<void(const QColor&)>& setColor,
               const std::function<void(SnowCanvasFillStyle)>& setStyle,
               const ScreenshotToolPaletteEditorServices& services,
               const ScreenshotToolPaletteButtonMetrics& metrics);
    void update(const QColor& color, SnowCanvasFillStyle style, bool colorMixed, bool styleMixed);
    void refreshMetrics(const ScreenshotToolPaletteButtonMetrics& metrics) override;
    [[nodiscard]] bool isInteracting() const override {
        return m_handlingChange;
    }
    void resetPopupMetrics(const ScreenshotToolPaletteButtonMetrics& metrics) override;
    void release() override;

  private:
    adqt::widgets::AdColorPicker* m_picker = nullptr;
    FillStylePreviewTrigger* m_trigger = nullptr;
    QVector<adqt::widgets::AdButton*> m_colorPresets;
    QVector<QColor> m_colorValues;
    QVector<FillStylePreviewButton*> m_styleButtons;
    QVector<SnowCanvasFillStyle> m_styleValues;
    bool m_handlingChange = false;
};

struct ScreenshotToolPaletteWidthColorEditorConfig {
    QString accessibleName;
    QString triggerTooltip;
    QString popupObjectName;
    QString widthRowObjectName;
    QString colorRowObjectName;
    QVector<double> widthValues;
    QVector<QColor> colorValues;
    std::function<ScreenshotToolPaletteTranslationText(double value)> widthTooltip;
    std::function<ScreenshotToolPaletteTranslationText(const QColor& color)> colorTooltip;
    bool observePopup = false;
};

// Combined width and color editor: a stroke-width preview trigger whose
// picker popup carries a width preset row and a color preset row. Shared by
// the rectangle-highlight and text stroke editors.
class ScreenshotToolPaletteWidthColorEditor final
    : public ScreenshotToolPaletteStyleEditorComponent {
  public:
    void build(QBoxLayout* layout, QWidget* parent, QObject* receiver,
               const ScreenshotToolPaletteWidthColorEditorConfig& config, double initialWidth,
               const QColor& initialColor, const std::function<void(double)>& setWidth,
               const std::function<void(const QColor&)>& setColor,
               const ScreenshotToolPaletteEditorServices& services,
               const ScreenshotToolPaletteButtonMetrics& metrics);
    void update(double width, const QColor& color, bool widthMixed, bool colorMixed);
    void refreshMetrics(const ScreenshotToolPaletteButtonMetrics& metrics) override;
    [[nodiscard]] bool isInteracting() const override {
        return m_handlingChange;
    }
    void resetPopupMetrics(const ScreenshotToolPaletteButtonMetrics& metrics) override;
    [[nodiscard]] adqt::widgets::AdColorPicker* picker() const {
        return m_picker;
    }
    void release() override;

  private:
    adqt::widgets::AdColorPicker* m_picker = nullptr;
    StrokeWidthPreviewButton* m_trigger = nullptr;
    QVector<adqt::widgets::AdButton*> m_widthButtons;
    QVector<double> m_widthValues;
    QVector<adqt::widgets::AdButton*> m_colorButtons;
    QVector<QColor> m_colorValues;
    bool m_handlingChange = false;
};

struct ScreenshotToolPaletteNumericPresetEditorConfig {
    QString summaryTooltip;
    QString summaryObjectName;
    QString suffix;
    QVector<double> values;
    std::function<ScreenshotToolPaletteTranslationText(int index, double value)> presetTooltip;
    std::function<QString(double value)> presetObjectName;
    std::function<adqt::icons::IconRef(int index)> presetIcon;
    bool strokePreview = false;
};

// The shared size editor: a summary button plus value presets. Renders as a
// stroke-width preview row or an S/M/L/XL icon row depending on the config.
// Shared by the shape/arrow stroke widths and the pen-highlight/pen-filter
// widths.
class ScreenshotToolPaletteNumericPresetEditor final
    : public ScreenshotToolPaletteStyleEditorComponent {
  public:
    void build(QBoxLayout* layout, QWidget* parent, QObject* receiver,
               const ScreenshotToolPaletteNumericPresetEditorConfig& config, double initialValue,
               const std::function<void()>& cycleValue, const std::function<void(double)>& setValue,
               const ScreenshotToolPaletteButtonMetrics& metrics);
    void update(double value, bool mixed);
    void refreshMetrics(const ScreenshotToolPaletteButtonMetrics& metrics) override;
    void release() override;

  private:
    adqt::widgets::AdButton* m_summary = nullptr;
    QVector<adqt::widgets::AdButton*> m_presets;
    QVector<double> m_values;
    bool m_strokePreview = false;
};

struct ScreenshotToolPaletteFontEditorConfig {
    QString accessibleName;
    QString summaryTooltip;
    QString summaryObjectName;
    QVector<double> sizeValues;
    std::function<ScreenshotToolPaletteTranslationText(int index, double value)> presetTooltip;
    bool observePopup = false;
};

// Font editor: size summary button, S/M/L/XL size presets and a searchable
// family select. Shared by the text, watermark and serial number editors.
class ScreenshotToolPaletteFontEditor final : public ScreenshotToolPaletteStyleEditorComponent {
  public:
    void build(QBoxLayout* layout, QWidget* parent, QObject* receiver,
               const ScreenshotToolPaletteFontEditorConfig& config, double initialSize,
               const QString& initialFamily, const std::function<void()>& cycleSize,
               const std::function<void(double)>& setSize,
               const std::function<void(const QString&)>& setFamily,
               const ScreenshotToolPaletteEditorServices& services,
               const ScreenshotToolPaletteButtonMetrics& metrics);
    void update(double size, const QString& family, bool sizeMixed, bool familyMixed,
                quint32 groups, quint32 sizeGroup, quint32 familyGroup);
    void refreshMetrics(const ScreenshotToolPaletteButtonMetrics& metrics) override;
    [[nodiscard]] NumericValuePreviewButton* sizeSummary() const {
        return m_sizeSummary;
    }
    [[nodiscard]] adqt::widgets::AdSelect* familySelect() const {
        return m_familySelect;
    }
    void release() override;

  private:
    NumericValuePreviewButton* m_sizeSummary = nullptr;
    QVector<adqt::widgets::AdButton*> m_sizePresets;
    QVector<double> m_sizeValues;
    adqt::widgets::AdSelect* m_familySelect = nullptr;
};

struct ScreenshotToolPaletteIconOption {
    int value = 0;
    ScreenshotToolPaletteTranslationText tooltip;
    adqt::icons::IconRef icon;
};

struct ScreenshotToolPaletteIconOptionEditorConfig {
    QString accessibleName;
    QString triggerTooltip;
    QVector<ScreenshotToolPaletteIconOption> options;
    int gridColumnCount = 0;
    bool minimizeTitleWidth = false;
};

// Icon-valued option editor: preview trigger plus popover grid. Shared by the
// arrowhead editors and the text alignment editor.
class ScreenshotToolPaletteIconOptionEditor final
    : public ScreenshotToolPaletteStyleEditorComponent {
  public:
    void build(QBoxLayout* layout, QWidget* parent, QObject* receiver,
               const ScreenshotToolPaletteIconOptionEditorConfig& config, int initialValue,
               const std::function<void(int)>& setValue,
               const ScreenshotToolPaletteButtonMetrics& metrics);
    void update(int value, bool mixed);
    void refreshMetrics(const ScreenshotToolPaletteButtonMetrics& metrics) override;
    void resetPopupMetrics(const ScreenshotToolPaletteButtonMetrics& metrics) override;
    void release() override;

  private:
    IconValuePreviewTrigger* m_trigger = nullptr;
    adqt::widgets::AdPopover* m_popover = nullptr;
    QVector<adqt::widgets::AdButton*> m_buttons;
    QVector<ScreenshotToolPaletteIconOption> m_options;
};

// Assembles the shared S/M/L/XL numeric preset config used by the
// pen-highlight and pen-filter width editors.
[[nodiscard]] ScreenshotToolPaletteNumericPresetEditorConfig
screenshotToolPaletteSizePresetEditorConfig(const QString& summaryTooltip,
                                            const QString& summaryObjectName,
                                            const char* presetTooltipPattern);

// System font families offered by the font family editors: trimmed,
// de-duplicated, sorted. Lazily enumerated once per process; fonts installed
// while the app is running appear after a restart.
[[nodiscard]] const QStringList& screenshotToolPaletteFontFamilies();

} // namespace snow_shot::presentation

#endif // SNOW_SHOT_PRESENTATION_SCREENSHOTTOOLPALETTESTYLECOMPONENTS_H
