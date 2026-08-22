#ifndef SNOW_SHOT_PRESENTATION_SCREENSHOTTOOLPALETTEBUTTONS_H
#define SNOW_SHOT_PRESENTATION_SCREENSHOTTOOLPALETTEBUTTONS_H

#include "icon_core.h"
#include "snow_draw_engine_qt/snow_canvas_types.h"
#include "widgets/button.h"

#include <QColor>
#include <QFont>
#include <QString>
#include <QStringList>
#include <QtGlobal>
#include <QVector>

#include <functional>

class QPaintEvent;
class QPainter;
class QBoxLayout;
class QLabel;
class QObject;
class QStandardItem;
class QWidget;

namespace adqt::widgets {
class AdPopover;
class AdRadio;
class AdRadioButtonGroup;
class AdSelect;
class AdSlider;
} // namespace adqt::widgets

struct ScreenshotToolPaletteButtonMetrics {
    int buttonSize = 0;
    int iconSize = 0;
    qreal physicalScale = 1.0;
    const QWidget* scope = nullptr;
};

struct ScreenshotToolPaletteTranslationText {
    ScreenshotToolPaletteTranslationText() = default;
    ScreenshotToolPaletteTranslationText(const char* sourceText);
    ScreenshotToolPaletteTranslationText(const QString& sourceText);

    [[nodiscard]] QString translated() const;
    [[nodiscard]] ScreenshotToolPaletteTranslationText arg(const QString& value) const;
    [[nodiscard]] ScreenshotToolPaletteTranslationText
    arg(double value, int fieldWidth = 0, char format = 'g', int precision = 6) const;

    QString source;
    QStringList arguments;
};

struct ScreenshotToolPaletteSliderEditor {
    QLabel* icon = nullptr;
    adqt::widgets::AdSlider* slider = nullptr;
    adqt::icons::IconRef iconRef;
    int baseIconSize = 0;
    int baseSliderWidth = 0;
};

struct ScreenshotToolPaletteSliderEditorConfig {
    QString iconObjectName;
    QString sliderObjectName;
    QString accessibleName;
    QString sliderTooltip;
    adqt::icons::IconRef iconRef;
    int initialValue = 0;
    int baseIconSize = 0;
    int baseSliderWidth = 96;
};

struct ScreenshotToolPaletteSelectEditor {
    adqt::widgets::AdSelect* select = nullptr;
    int baseWidth = 128;
};

struct ScreenshotToolPaletteSelectEditorConfig {
    QString objectName;
    QString accessibleName;
    QString tooltip;
    QString placeholder;
    int baseWidth = 128;
    bool searchEnabled = false;
};

struct ScreenshotToolPaletteRadioOption {
    int id = -1;
    ScreenshotToolPaletteTranslationText tooltip;
    adqt::icons::IconRef iconRef;
};

struct ScreenshotToolPaletteRadioEditor {
    QWidget* container = nullptr;
    adqt::widgets::AdRadioButtonGroup* group = nullptr;
    QVector<adqt::widgets::AdRadio*> buttons;
};

struct ScreenshotToolPaletteRadioEditorConfig {
    QString objectName;
    QVector<ScreenshotToolPaletteRadioOption> options;
    int initialId = -1;
    bool useButtonMetrics = false;
};

struct ScreenshotToolPaletteOptionPopoverOption {
    int value = -1;
    ScreenshotToolPaletteTranslationText tooltip;
    adqt::icons::IconRef iconRef;
};

struct ScreenshotToolPaletteOptionPopoverEditor {
    adqt::widgets::AdPopover* popover = nullptr;
    QVector<adqt::widgets::AdButton*> buttons;
    QVector<int> values;
};

struct ScreenshotToolPaletteOptionPopoverEditorConfig {
    QString accessibleName;
    QString contentObjectName;
    QVector<ScreenshotToolPaletteOptionPopoverOption> options;
    int optionSpacing = 0;
};

[[nodiscard]] ScreenshotToolPaletteOptionPopoverEditor
createScreenshotToolPaletteOptionPopoverEditor(
    adqt::widgets::AdButton* trigger, QObject* receiver,
    const ScreenshotToolPaletteOptionPopoverEditorConfig& config,
    const std::function<void(int)>& activateValue,
    const ScreenshotToolPaletteButtonMetrics& metrics);

void configureScreenshotToolPaletteOptionPopoverEditor(
    adqt::widgets::AdPopover* popover, const QVector<adqt::widgets::AdButton*>& buttons,
    int optionSpacing, const ScreenshotToolPaletteButtonMetrics& metrics);

void updateScreenshotToolPaletteOptionPopoverEditor(
    const QVector<adqt::widgets::AdButton*>& buttons, const QVector<int>& values, int activeValue);

[[nodiscard]] ScreenshotToolPaletteRadioEditor
createScreenshotToolPaletteRadioEditor(QWidget* parent,
                                       const ScreenshotToolPaletteRadioEditorConfig& config,
                                       const ScreenshotToolPaletteButtonMetrics& metrics);

[[nodiscard]] ScreenshotToolPaletteSelectEditor
createScreenshotToolPaletteSelectEditor(QWidget* parent,
                                        const ScreenshotToolPaletteSelectEditorConfig& config,
                                        const ScreenshotToolPaletteButtonMetrics& metrics);

void configureScreenshotToolPaletteSelectEditor(ScreenshotToolPaletteSelectEditor& editor,
                                                const ScreenshotToolPaletteButtonMetrics& metrics);

[[nodiscard]] ScreenshotToolPaletteSliderEditor
createScreenshotToolPaletteSliderEditor(QBoxLayout* layout, QWidget* parent,
                                        const ScreenshotToolPaletteSliderEditorConfig& config,
                                        const ScreenshotToolPaletteButtonMetrics& metrics);

void configureScreenshotToolPaletteSliderEditor(ScreenshotToolPaletteSliderEditor& editor,
                                                const ScreenshotToolPaletteButtonMetrics& metrics);

[[nodiscard]] bool
screenshotToolPaletteMetricsApplyTo(const ScreenshotToolPaletteButtonMetrics& metrics,
                                    const QWidget* widget);

class StylePreviewButton : public adqt::widgets::AdButton {
  public:
    explicit StylePreviewButton(QWidget* parent = nullptr);

    void setOutlined(bool outlined);
};

class StrokeWidthPreviewButton final : public StylePreviewButton {
  public:
    explicit StrokeWidthPreviewButton(QWidget* parent = nullptr);

    void setStrokeWidth(double strokeWidth);
    void setActiveStrokeWidth(bool active);
    void setMixed(bool mixed);
    void setTextFallbackEnabled(bool enabled);
    void setPhysicalScale(qreal scale);
    void commitControlScale(const adqt::widgets::AdControlScaleContext& context) override;

  protected:
    void paintEvent(QPaintEvent* event) override;

  private:
    [[nodiscard]] QString strokeWidthText() const;
    void drawStrokeWidthText(QPainter* painter, const QColor& color) const;

    double m_strokeWidth;
    qreal m_physicalScale = 1.0;
    bool m_active = false;
    bool m_mixed = false;
    bool m_textFallbackEnabled = false;
};

class NumericValuePreviewButton final : public StylePreviewButton {
  public:
    explicit NumericValuePreviewButton(QWidget* parent = nullptr);

    void setValue(double value);
    void setSuffix(const QString& suffix);
    void setMixed(bool mixed);
    void setIntegerDisplay(bool integerDisplay);
    void setStrokeWidthPreviewEnabled(bool enabled);
    void setPhysicalScale(qreal scale);
    void commitControlScale(const adqt::widgets::AdControlScaleContext& context) override;

  protected:
    void paintEvent(QPaintEvent* event) override;

  private:
    [[nodiscard]] QString valueText() const;
    void updateAccessibleValue();

    double m_value = 0.0;
    QString m_suffix;
    qreal m_physicalScale = 1.0;
    bool m_mixed = false;
    bool m_integerDisplay = false;
    bool m_strokeWidthPreviewEnabled = false;
};

class ColorSwatchButton : public adqt::widgets::AdButton {
  public:
    explicit ColorSwatchButton(QWidget* parent = nullptr);

    void setSwatchColor(const QColor& color);
    void setSwatchBorderVisible(bool visible);
    void setPhysicalScale(qreal scale);
    void commitControlScale(const adqt::widgets::AdControlScaleContext& context) override;

  protected:
    void paintEvent(QPaintEvent* event) override;

    [[nodiscard]] QColor swatchColor() const;
    [[nodiscard]] qreal swatchPhysicalScale() const;

  private:
    QColor m_color;
    qreal m_physicalScale = 1.0;
    bool m_swatchBorderVisible = true;
};

class ColorPickerSamplerButton final : public ColorSwatchButton {
  public:
    explicit ColorPickerSamplerButton(QWidget* parent = nullptr);

  protected:
    void paintEvent(QPaintEvent* event) override;
};

class IconValuePreviewTrigger final : public StylePreviewButton {
  public:
    explicit IconValuePreviewTrigger(QWidget* parent = nullptr);

    void setValueIconRef(const adqt::icons::IconRef& iconRef);
    void setMixed(bool mixed);
    void setPhysicalScale(qreal scale);
    void commitControlScale(const adqt::widgets::AdControlScaleContext& context) override;
    void setIconSize(int size);

  protected:
    void paintEvent(QPaintEvent* event) override;

  private:
    adqt::icons::IconRef m_valueIconRef;
    int m_iconSize = 18;
    qreal m_physicalScale = 1.0;
    bool m_mixed = false;
};

class StrokeStylePreviewTrigger final : public StylePreviewButton {
  public:
    explicit StrokeStylePreviewTrigger(QWidget* parent = nullptr);

    void setStrokeColor(const QColor& color);
    void setStrokeStyle(SnowCanvasArrowStrokeStyle strokeStyle);
    void setMixed(bool mixed);
    void setPhysicalScale(qreal scale);
    void commitControlScale(const adqt::widgets::AdControlScaleContext& context) override;

  protected:
    void paintEvent(QPaintEvent* event) override;

  private:
    QColor m_color;
    SnowCanvasArrowStrokeStyle m_strokeStyle = SnowCanvasArrowStrokeStyle::Solid;
    qreal m_physicalScale = 1.0;
    bool m_mixed = false;
};

class StrokeStylePreviewButton final : public adqt::widgets::AdButton {
  public:
    explicit StrokeStylePreviewButton(QWidget* parent = nullptr);

    void setStrokeStyle(SnowCanvasArrowStrokeStyle strokeStyle);
    void setPhysicalScale(qreal scale);
    void commitControlScale(const adqt::widgets::AdControlScaleContext& context) override;

  protected:
    void paintEvent(QPaintEvent* event) override;

  private:
    SnowCanvasArrowStrokeStyle m_strokeStyle = SnowCanvasArrowStrokeStyle::Solid;
    qreal m_physicalScale = 1.0;
};

class FillStylePreviewTrigger final : public StylePreviewButton {
  public:
    explicit FillStylePreviewTrigger(QWidget* parent = nullptr);

    void setFillColor(const QColor& color);
    void setFillStyle(SnowCanvasFillStyle fillStyle);
    void setMixed(bool mixed);
    void setPhysicalScale(qreal scale);
    void commitControlScale(const adqt::widgets::AdControlScaleContext& context) override;

  protected:
    void paintEvent(QPaintEvent* event) override;

  private:
    QColor m_color;
    SnowCanvasFillStyle m_fillStyle = SnowCanvasFillStyle::Solid;
    qreal m_physicalScale = 1.0;
    bool m_mixed = false;
};

class FillStylePreviewButton final : public adqt::widgets::AdButton {
  public:
    explicit FillStylePreviewButton(QWidget* parent = nullptr);

    void setFillColor(const QColor& color);
    void setFillStyle(SnowCanvasFillStyle fillStyle);
    void setOuterBorderVisible(bool visible);
    void setPhysicalScale(qreal scale);
    void commitControlScale(const adqt::widgets::AdControlScaleContext& context) override;

  protected:
    void paintEvent(QPaintEvent* event) override;

  private:
    QColor m_color;
    SnowCanvasFillStyle m_fillStyle = SnowCanvasFillStyle::Solid;
    qreal m_physicalScale = 1.0;
    bool m_outerBorderVisible = false;
};

class IconNumericValuePreviewButton final : public adqt::widgets::AdButton {
  public:
    explicit IconNumericValuePreviewButton(QWidget* parent = nullptr);

    [[nodiscard]] int value() const;
    void setValue(int value);
    void setCornerRadius(int cornerRadius);
    void setMixed(bool mixed);
    void setIconRef(const adqt::icons::IconRef& iconRef);
    void setCornerIconRef(const adqt::icons::IconRef& iconRef);
    void setValueWidthReference(const QString& value);
    void setPhysicalScale(qreal scale);
    void commitControlScale(const adqt::widgets::AdControlScaleContext& context) override;

  protected:
    void paintEvent(QPaintEvent* event) override;

  private:
    int m_value = 0;
    adqt::icons::IconRef m_iconRef;
    QString m_valueWidthReference = QStringLiteral("88");
    QFont m_baseFont;
    qreal m_physicalScale = 1.0;
    bool m_mixed = false;
};

using CornerRadiusEditorButton = IconNumericValuePreviewButton;

void configureScreenshotToolPaletteTooltip(QWidget* trigger, const char* source);

void configureScreenshotToolPaletteTooltip(QWidget* trigger,
                                           const ScreenshotToolPaletteTranslationText& text);

void configureScreenshotToolPalettePopoverTrigger(QWidget* trigger, const char* source);

void setScreenshotToolPaletteTooltipSource(QWidget* widget, const char* source);

void setScreenshotToolPaletteAccessibleNameSource(QWidget* widget, const char* source);

void setScreenshotToolPalettePlaceholderSource(QWidget* widget, const char* source);

void setScreenshotToolPaletteItemTranslationSource(QStandardItem* item, const char* source);

void setScreenshotToolPaletteItemTranslationSource(
    QStandardItem* item, const ScreenshotToolPaletteTranslationText& text);

void retranslateScreenshotToolPalette(QWidget* root);

void configureScreenshotToolPaletteBaseButton(adqt::widgets::AdButton* button, const char* tooltip,
                                              const ScreenshotToolPaletteButtonMetrics& metrics);

void configureScreenshotToolPaletteStyleButton(adqt::widgets::AdButton* button, const char* tooltip,
                                               const ScreenshotToolPaletteButtonMetrics& metrics);

void configureScreenshotToolPaletteStyleRadioButtonGroup(
    adqt::widgets::AdRadioButtonGroup* group, const ScreenshotToolPaletteButtonMetrics& metrics,
    bool useButtonMetrics = false);

void setScreenshotToolPaletteStyleRadioIcon(adqt::widgets::AdRadio* radio,
                                            const adqt::icons::IconRef& iconRef);

void setScreenshotToolPaletteToolButtonIcon(adqt::widgets::AdButton* button,
                                            const adqt::icons::IconRef& iconRef);

adqt::widgets::AdButton*
createScreenshotToolPaletteToolButton(QWidget* parent, const char* tooltip,
                                      const adqt::icons::IconRef& iconRef,
                                      const ScreenshotToolPaletteButtonMetrics& metrics);

adqt::widgets::AdButton* createScreenshotToolPaletteActionButton(
    QWidget* parent, const char* tooltip, const adqt::icons::IconRef& iconRef, bool danger,
    bool primary, const ScreenshotToolPaletteButtonMetrics& metrics);

StrokeWidthPreviewButton*
createScreenshotToolPaletteStrokeWidthButton(QWidget* parent, const char* tooltip,
                                             double strokeWidth, bool summary,
                                             const ScreenshotToolPaletteButtonMetrics& metrics);

ColorSwatchButton*
createScreenshotToolPaletteColorButton(QWidget* parent, const char* tooltip, const QColor& color,
                                       bool summary, bool swatchBorderVisible,
                                       const ScreenshotToolPaletteButtonMetrics& metrics);

ColorPickerSamplerButton* createScreenshotToolPaletteColorPickerSamplerButton(
    QWidget* parent, const QColor& color);

adqt::widgets::AdButton*
createScreenshotToolPaletteStyleActionButton(QWidget* parent, const char* tooltip,
                                             const adqt::icons::IconRef& iconRef,
                                             const ScreenshotToolPaletteButtonMetrics& metrics);

NumericValuePreviewButton*
createScreenshotToolPaletteNumericValueButton(QWidget* parent, const char* tooltip, double value,
                                              const QString& suffix,
                                              const ScreenshotToolPaletteButtonMetrics& metrics);

IconValuePreviewTrigger* createScreenshotToolPaletteIconValuePreviewTrigger(
    QWidget* parent, const char* tooltip, const adqt::icons::IconRef& iconRef,
    const ScreenshotToolPaletteButtonMetrics& metrics);

void configureScreenshotToolPaletteIconValuePreviewTrigger(
    IconValuePreviewTrigger* trigger, const ScreenshotToolPaletteButtonMetrics& metrics);

StrokeStylePreviewTrigger* createScreenshotToolPaletteStrokeStyleTrigger(
    QWidget* parent, const char* tooltip, const QColor& color,
    SnowCanvasArrowStrokeStyle strokeStyle, const ScreenshotToolPaletteButtonMetrics& metrics);

void configureScreenshotToolPaletteStrokeStyleTrigger(
    StrokeStylePreviewTrigger* trigger, const ScreenshotToolPaletteButtonMetrics& metrics);

StrokeStylePreviewButton*
createScreenshotToolPaletteStrokeStyleButton(QWidget* parent, const char* tooltip,
                                             SnowCanvasArrowStrokeStyle strokeStyle,
                                             const ScreenshotToolPaletteButtonMetrics& metrics);

FillStylePreviewTrigger*
createScreenshotToolPaletteFillStyleTrigger(QWidget* parent, const char* tooltip,
                                            const QColor& color, SnowCanvasFillStyle fillStyle,
                                            const ScreenshotToolPaletteButtonMetrics& metrics);

void configureScreenshotToolPaletteFillStyleTrigger(
    FillStylePreviewTrigger* trigger, const ScreenshotToolPaletteButtonMetrics& metrics);

FillStylePreviewButton* createScreenshotToolPaletteFillStyleButton(
    QWidget* parent, const char* tooltip, const QColor& color, SnowCanvasFillStyle fillStyle,
    bool summary, const ScreenshotToolPaletteButtonMetrics& metrics);

CornerRadiusEditorButton*
createScreenshotToolPaletteCornerRadiusEditor(QWidget* parent, const char* tooltip,
                                              const adqt::icons::IconRef& iconRef, int cornerRadius,
                                              const ScreenshotToolPaletteButtonMetrics& metrics);

void configureScreenshotToolPaletteCornerRadiusEditor(
    CornerRadiusEditorButton* button, const ScreenshotToolPaletteButtonMetrics& metrics);

IconNumericValuePreviewButton* createScreenshotToolPaletteIconNumericValueButton(
    QWidget* parent, const char* tooltip, const adqt::icons::IconRef& iconRef, int value,
    const QString& valueWidthReference, const ScreenshotToolPaletteButtonMetrics& metrics);

void configureScreenshotToolPaletteIconNumericValueButton(
    IconNumericValuePreviewButton* button, const ScreenshotToolPaletteButtonMetrics& metrics);

#endif // SNOW_SHOT_PRESENTATION_SCREENSHOTTOOLPALETTEBUTTONS_H
