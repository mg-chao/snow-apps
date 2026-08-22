#pragma once

#include <QColor>
#include <QPointer>
#include <QWidget>
#include <QVector>

#include <functional>
#include <memory>
#include <optional>

#include "color_selection.h"
#include "popover.h"

class QAbstractButton;
class QButtonGroup;
class QFrame;
class QHBoxLayout;
class QLabel;
class QMoveEvent;
class QResizeEvent;
class QShowEvent;
class QStackedLayout;
class QVBoxLayout;

namespace adqt::widgets {

namespace detail {
struct ColorPickerMetrics;
struct ColorPickerVisualStyle;
}  // namespace detail

class AdLineEdit;
class AdInputNumber;
class AdComboBox;
class AdMultiSlider;
class AdSlider;
class ColorSaturationPanel;
class AdColorPickerState;

class AdColorPickerPanel;

class AdColorPicker final : public QWidget {
  Q_OBJECT

  Q_PROPERTY(Size size READ size WRITE setSize NOTIFY sizeChanged)
  Q_PROPERTY(Mode mode READ mode WRITE setMode NOTIFY modeChanged)
  Q_PROPERTY(
      QVector<Mode> modeOptions READ modeOptions WRITE setModeOptions NOTIFY modeOptionsChanged)
  Q_PROPERTY(Format format READ format WRITE setFormat NOTIFY formatChanged)
  Q_PROPERTY(bool allowClear READ allowClear WRITE setAllowClear NOTIFY allowClearChanged)
  Q_PROPERTY(bool triggerTextVisible READ triggerTextVisible WRITE setTriggerTextVisible NOTIFY
                 triggerTextVisibleChanged)
  Q_PROPERTY(bool popupVisible READ popupVisible WRITE setPopupVisible NOTIFY popupVisibleChanged)
  Q_PROPERTY(bool alphaChannelEnabled READ alphaChannelEnabled WRITE setAlphaChannelEnabled NOTIFY
                 alphaChannelEnabledChanged)
  Q_PROPERTY(bool formatSelectorEnabled READ formatSelectorEnabled WRITE setFormatSelectorEnabled
                 NOTIFY formatSelectorEnabledChanged)
  Q_PROPERTY(Trigger trigger READ trigger WRITE setTrigger NOTIFY triggerChanged)
  Q_PROPERTY(Placement placement READ placement WRITE setPlacement NOTIFY placementChanged)
  Q_PROPERTY(adqt::widgets::AdPopupLayerMode popupLayerMode READ popupLayerMode WRITE
                 setPopupLayerMode NOTIFY popupLayerModeChanged)
  Q_PROPERTY(adqt::widgets::AdColorPickerState* state READ state WRITE setState NOTIFY stateChanged)
  Q_PROPERTY(QWidget* triggerContent READ triggerContent WRITE setTriggerContent NOTIFY
                 triggerContentChanged)
  Q_PROPERTY(QWidget* previewContent READ previewContent WRITE setPreviewContent NOTIFY
                 previewContentChanged)
  Q_PROPERTY(
      QWidget* popupContent READ popupContent WRITE setPopupContent NOTIFY popupContentChanged)
  Q_PROPERTY(PopupContentPlacement popupContentPlacement READ popupContentPlacement WRITE
                 setPopupContentPlacement NOTIFY popupContentPlacementChanged)
  Q_PROPERTY(QVector<PresetItem> presets READ presets WRITE setPresets NOTIFY presetsChanged)
  Q_PROPERTY(adqt::widgets::AdColorValue value READ value WRITE setValue NOTIFY valueChanged)

 public:
  enum class Size {
    Small,
    Middle,
    Large,
  };
  Q_ENUM(Size)

  enum class Mode {
    Solid,
    Gradient,
  };
  Q_ENUM(Mode)

  enum class Format {
    Hex,
    Rgb,
    Hsb,
  };
  Q_ENUM(Format)

  enum class Trigger {
    Click,
    Hover,
  };
  Q_ENUM(Trigger)

  enum class Placement {
    Top,
    TopLeft,
    TopRight,
    Bottom,
    BottomLeft,
    BottomRight,
    Left,
    LeftTop,
    LeftBottom,
    Right,
    RightTop,
    RightBottom,
  };
  Q_ENUM(Placement)

  enum class PopupContentPlacement {
    Top,
    Bottom,
  };
  Q_ENUM(PopupContentPlacement)

  struct PresetItem {
    QString label;
    QVector<AdColorValue> colors;
    bool defaultOpen = true;
    QString key;
  };

  struct ComponentTokens {
    std::optional<int> controlHeight;
    std::optional<int> triggerMinWidth;
    std::optional<int> triggerRadius;
    std::optional<int> panelWidth;
    std::optional<int> swatchSize;
    std::optional<int> presetSwatchSize;
    std::optional<int> panelPadding;
    std::optional<int> inputHeight;
    std::optional<QColor> triggerBackground;
    std::optional<QColor> triggerBorderColor;
    std::optional<QColor> triggerBorderHoverColor;
    std::optional<QColor> triggerTextColor;
    std::optional<QColor> panelBackground;
    std::optional<QColor> panelBorderColor;
  };

  using ShowTextFormatter = std::function<QString(const AdColorValue&, Format, int activeIndex)>;
  using PopupLayerMode = AdPopupLayerMode;

  explicit AdColorPicker(QWidget* parent = nullptr);
  ~AdColorPicker() override;

  Size size() const;
  void setSize(Size value);

  Mode mode() const;
  void setMode(Mode value);

  QVector<Mode> modeOptions() const;
  void setModeOptions(const QVector<Mode>& options);

  Format format() const;
  void setFormat(Format value);

  bool allowClear() const;
  void setAllowClear(bool value);

  bool triggerTextVisible() const;
  void setTriggerTextVisible(bool value);

  bool popupVisible() const;
  void setPopupVisible(bool value);

  bool disabled() const;
  void setDisabled(bool value);

  bool alphaChannelEnabled() const;
  void setAlphaChannelEnabled(bool value);

  bool formatSelectorEnabled() const;
  void setFormatSelectorEnabled(bool value);

  Trigger trigger() const;
  void setTrigger(Trigger value);

  Placement placement() const;
  void setPlacement(Placement value);

  PopupLayerMode popupLayerMode() const;
  void setPopupLayerMode(PopupLayerMode value);

  QString cssText() const;
  void setCssText(const QString& value);

  QString displayText() const;

  AdColorValue value() const;
  void setValue(const AdColorValue& value);
  void commitValue(const AdColorValue& value);

  QVector<PresetItem> presets() const;
  void setPresets(const QVector<PresetItem>& presets);

  AdColorPickerState* state() const;
  void setState(AdColorPickerState* state);

  // Installs a custom popover trigger. Passing nullptr, or destroying the
  // custom widget, restores the built-in trigger.
  QWidget* triggerContent() const;
  void setTriggerContent(QWidget* widget);

  // Replaces the trailing preview swatch in the popup's slider row. Passing
  // nullptr, or destroying the custom widget, restores the built-in swatch.
  QWidget* previewContent() const;
  void setPreviewContent(QWidget* widget);

  QWidget* popupContent() const;
  void setPopupContent(QWidget* widget);

  PopupContentPlacement popupContentPlacement() const;
  void setPopupContentPlacement(PopupContentPlacement placement);

  ShowTextFormatter showTextFormatter() const;
  void setShowTextFormatter(ShowTextFormatter formatter);

  ComponentTokens componentTokens() const;
  void setComponentTokens(const ComponentTokens& tokens);
  void resetComponentTokens();

 signals:
  void sizeChanged(Size value);
  void modeChanged(Mode value);
  void modeOptionsChanged(const QVector<Mode>& options);
  void formatChanged(Format value);
  void allowClearChanged(bool value);
  void triggerTextVisibleChanged(bool value);
  // Emitted before opening transfers keyboard focus into the popup.
  void popupOpening();
  void popupVisibleChanged(bool value);
  void disabledChanged(bool value);
  void alphaChannelEnabledChanged(bool value);
  void formatSelectorEnabledChanged(bool value);
  void triggerChanged(Trigger value);
  void placementChanged(Placement value);
  void popupLayerModeChanged(PopupLayerMode value);
  void cssTextChanged(const QString& value);
  void displayTextChanged(const QString& value);
  void valueChanged(const adqt::widgets::AdColorValue& value);
  void presetsChanged();
  void stateChanged(adqt::widgets::AdColorPickerState* state);
  void triggerContentChanged(QWidget* widget);
  void previewContentChanged(QWidget* widget);
  void popupContentChanged(QWidget* widget);
  void popupContentPlacementChanged(PopupContentPlacement placement);
  void showTextFormatterChanged();
  void componentTokensChanged();
  void cleared();
  void editingFinished(const adqt::widgets::AdColorValue& value);

 protected:
  void changeEvent(QEvent* event) override;
  void moveEvent(QMoveEvent* event) override;
  void resizeEvent(QResizeEvent* event) override;
  void showEvent(QShowEvent* event) override;
  bool eventFilter(QObject* watched, QEvent* event) override;

 private:
  friend class AdColorPickerPanel;

  enum class HostMode {
    WithTrigger,
    PanelOnly,
  };

  enum class LivePanelSyncSource {
    None,
    SaturationPanel,
    HueSlider,
    AlphaSlider,
    FormatInputs,
    AlphaInput,
    GradientStops,
  };

  explicit AdColorPicker(HostMode hostMode, QWidget* parent);

  struct InternalGradientStop {
    QColor color;
    int percent = 0;
  };

  struct SelectionState {
    AdColorSelection selection;
    Mode mode = Mode::Solid;
    QVector<Mode> modeOptions = {Mode::Solid};
    int activeStopIndex = 0;
  };

  static QString modeName(Mode value);
  static Mode parseModeName(const QString& value, Mode fallback);
  static QString formatName(Format value);
  static Format parseFormatName(const QString& value, Format fallback);
  static AdPopover::Placement toPopoverPlacement(Placement value);
  static Placement fromPopoverPlacement(AdPopover::Placement value);
  static AdPopover::Triggers toPopoverTriggers(Trigger value);
  static Trigger fromPopoverTriggers(AdPopover::Triggers value, Trigger fallback);
  const QWidget* themeLogicalOwner() const;

  void ensureRootLayout();
  void ensureTriggerHost();
  void ensureTriggerUi();
  void ensurePopover();
  void ensureEditorUi();
  void ensureOperationUi();
  void ensureClearButtonUi();
  void ensurePresetsUi();
  void ensureFormatInputUi(Format format);
  void ensureRgbInputUi();
  void ensureHsbInputUi();
  void ensureGradientUi();
  void commitFormatInputs();
  void previewFormatInputs();
  void previewAlphaInput();
  QWidget* currentTriggerWidget() const;
  void setActiveTriggerWidget(QWidget* widget);
  void syncPopoverSourceWidget();
  void setHostedRootWidget(QWidget* widget);
  void attachPopupContentToPanel();
  QWidget* detachPanelHost(QWidget* newParent);
  void attachPanelHostToPopover();
  detail::ColorPickerVisualStyle visualStyle() const;
  detail::ColorPickerMetrics metrics() const;
  void invalidateStyleCache() const;

  void rebuildPresetsPanel();
  void refreshPresetSelectionState();
  void refreshStyle(bool preserveCurrentTriggerWidth = true);
  void refreshTriggerDisplay(bool deferTextUpdate = false);
  void refreshPanelControlsFromState(bool minimal = false);
  void refreshPanelControlsForOpenInteraction(bool commit);
  void refreshInteractiveEditorsFromState();
  void scheduleInteractiveEditorRefresh();
  void refreshChannelVisuals(LivePanelSyncSource source = LivePanelSyncSource::None);
  void refreshPreviewSwatch();
  void syncPreviewContentWidget();
  void updateFormatInputText();
  void updateFormatInputVisibility();
  void updateModeSegmentedOptions();
  void updateTriggerFocusOverlay();
  void refreshTriggerInteractionStyle();
  void suppressTriggerUpdatesDuringInteraction();
  void resumeTriggerUpdatesAfterInteraction();
  void syncTriggerWidthLock(bool preserveCurrentWidth = true);
  void refreshTriggerWidthAfterDisplayChange();

  QColor currentEditableColor() const;
  void setCurrentEditableColor(const QColor& color, bool fromUser, bool emitCompleted,
                               bool emitCssTextSignal = true);
  void setCurrentFromControls(bool emitCompleted, LivePanelSyncSource source);
  void setGradientStopsFromSlider(const QList<double>& values, bool emitCompleted);
  QColor parseColorString(const QString& value, bool* ok) const;
  QString colorToString(const QColor& color, Format format) const;
  QString formattedColorString(const QColor& color) const;
  QString colorToCss(const QColor& color) const;
  QString colorValueToCss(const AdColorSelection& value) const;
  static SelectionState createSelectionState(const AdColorSelection& selection, Mode mode,
                                             const QVector<Mode>& modeOptions, int activeStopIndex);
  SelectionState selectionState() const;
  void applySelectionState(const SelectionState& state);
  AdColorSelection exportColorValue() const;
  void importColorValue(const AdColorSelection& value, bool fromUser, bool emitCompleted,
                        bool emitCssTextSignal = true);
  QVector<InternalGradientStop> normalizeGradientStops(
      const QVector<InternalGradientStop>& stops) const;

  void emitChangeSignals(bool emitCompleted, bool emitCssTextSignal);
  void applyPreset(const AdColorValue& value);
  void applyStateObject();
  void syncStateObject();
  void syncStateObject(const AdColorSelection& selection, const QString& cssText,
                       const QString& displayText);

  const HostMode hostMode_;

  Size size_ = Size::Middle;
  Mode mode_ = Mode::Solid;
  QVector<Mode> modeOptions_ = {Mode::Solid};
  Format format_ = Format::Hex;
  bool allowClear_ = false;
  bool triggerTextVisible_ = false;
  bool disabledAlpha_ = false;
  bool disabledFormat_ = false;
  bool cleared_ = true;
  Trigger trigger_ = Trigger::Click;
  Placement placement_ = Placement::BottomLeft;
  PopupContentPlacement popupContentPlacement_ = PopupContentPlacement::Top;
  PopupLayerMode popupLayerMode_ = PopupLayerMode::InWindow;

  QColor solidColor_ = QColor(0, 0, 0, 0);
  QVector<InternalGradientStop> gradientStops_;
  int activeStopIndex_ = 0;

  QVector<PresetItem> presets_;
  ShowTextFormatter showTextFormatter_;
  ComponentTokens componentTokens_;
  QPointer<AdColorPickerState> state_;
  std::unique_ptr<AdColorPickerState> ownedState_;

  bool syncingControls_ = false;
  bool syncingStateObject_ = false;
  bool applyingStateObject_ = false;
  bool triggerHovered_ = false;
  bool triggerUpdatesSuppressed_ = false;
  int lockedTriggerWidth_ = 0;
  LivePanelSyncSource livePanelSyncSource_ = LivePanelSyncSource::None;
  bool interactiveEditorRefreshPending_ = false;
  bool editorPrewarmScheduled_ = false;
  bool pendingEditingFinished_ = false;
  AdColorSelection pendingFinishedValue_;

  QPointer<QHBoxLayout> rootLayout_;
  QPointer<QWidget> hostedRootWidget_;
  QPointer<QWidget> triggerHost_;
  QPointer<QHBoxLayout> triggerHostLayout_;
  QPointer<QWidget> activeTriggerWidget_;
  QPointer<AdPopover> popover_;
  QPointer<QAbstractButton> triggerFrame_;
  QPointer<QWidget> defaultTrigger_;
  QPointer<QWidget> triggerContent_;
  QMetaObject::Connection triggerContentDestroyedConnection_;
  QPointer<QWidget> previewContent_;
  QMetaObject::Connection previewContentDestroyedConnection_;
  QPointer<QWidget> popupContent_;
  QPointer<QWidget> popoverContentStub_;
  QPointer<QWidget> panelHost_;
  QPointer<QWidget> pickerPanel_;
  QPointer<QWidget> presetsPanel_;

  QPointer<QWidget> triggerSwatch_;
  QPointer<QLabel> triggerTextLabel_;

  QPointer<QWidget> operationRow_;
  QPointer<QWidget> operationGap_;
  QPointer<QWidget> modeSegmented_;
  QPointer<QButtonGroup> modeButtonGroup_;
  QPointer<QAbstractButton> clearButton_;
  QPointer<QWidget> gradientSection_;
  QPointer<QWidget> gradientGap_;
  QPointer<AdMultiSlider> gradientSlider_;
  QPointer<ColorSaturationPanel> saturationPanel_;
  QPointer<QWidget> saturationGap_;
  QPointer<QWidget> sliderContainer_;
  QPointer<QWidget> sliderGap_;
  QPointer<QWidget> sliderGroup_;
  QPointer<QWidget> alphaSection_;
  QPointer<AdSlider> hueSlider_;
  QPointer<AdSlider> alphaSlider_;
  QPointer<QWidget> previewSwatch_;

  QPointer<AdComboBox> formatCombo_;
  QPointer<QWidget> formatInputHost_;
  QPointer<QStackedLayout> formatInputStack_;
  QPointer<QWidget> rgbInputHost_;
  QPointer<QWidget> hsbInputHost_;
  QPointer<AdLineEdit> hexInput_;
  QPointer<AdInputNumber> rgbInputR_;
  QPointer<AdInputNumber> rgbInputG_;
  QPointer<AdInputNumber> rgbInputB_;
  QPointer<AdInputNumber> hsbInputH_;
  QPointer<AdInputNumber> hsbInputS_;
  QPointer<AdInputNumber> hsbInputB_;
  QPointer<AdInputNumber> alphaInput_;

  QPointer<QVBoxLayout> presetsLayout_;
  struct StyleCache;
  mutable std::unique_ptr<StyleCache> styleCache_;
};

class AdColorPickerState final : public QObject {
  Q_OBJECT

  Q_PROPERTY(adqt::widgets::AdColorPicker::Mode mode READ mode WRITE setMode NOTIFY modeChanged)
  Q_PROPERTY(QVector<adqt::widgets::AdColorPicker::Mode> modeOptions READ modeOptions WRITE
                 setModeOptions NOTIFY modeOptionsChanged)
  Q_PROPERTY(
      adqt::widgets::AdColorPicker::Format format READ format WRITE setFormat NOTIFY formatChanged)
  Q_PROPERTY(bool allowClear READ allowClear WRITE setAllowClear NOTIFY allowClearChanged)
  Q_PROPERTY(bool alphaChannelEnabled READ alphaChannelEnabled WRITE setAlphaChannelEnabled NOTIFY
                 alphaChannelEnabledChanged)
  Q_PROPERTY(bool formatSelectorEnabled READ formatSelectorEnabled WRITE setFormatSelectorEnabled
                 NOTIFY formatSelectorEnabledChanged)
  Q_PROPERTY(int activeStopIndex READ activeStopIndex WRITE setActiveStopIndex NOTIFY
                 activeStopIndexChanged)
  Q_PROPERTY(QVector<adqt::widgets::AdColorPicker::PresetItem> presets READ presets WRITE setPresets
                 NOTIFY presetsChanged)
  Q_PROPERTY(QString cssText READ cssText WRITE setCssText NOTIFY cssTextChanged)
  Q_PROPERTY(QString displayText READ displayText NOTIFY displayTextChanged)
  Q_PROPERTY(adqt::widgets::AdColorValue value READ value WRITE setValue NOTIFY valueChanged)

 public:
  using Mode = AdColorPicker::Mode;
  using Format = AdColorPicker::Format;
  using PresetItem = AdColorPicker::PresetItem;

  explicit AdColorPickerState(QObject* parent = nullptr);
  ~AdColorPickerState() override;

  Mode mode() const;
  void setMode(Mode value);

  QVector<Mode> modeOptions() const;
  void setModeOptions(const QVector<Mode>& options);

  Format format() const;
  void setFormat(Format value);

  bool allowClear() const;
  void setAllowClear(bool value);

  bool alphaChannelEnabled() const;
  void setAlphaChannelEnabled(bool value);

  bool formatSelectorEnabled() const;
  void setFormatSelectorEnabled(bool value);

  int activeStopIndex() const;
  void setActiveStopIndex(int value);

  QString cssText() const;
  void setCssText(const QString& value);

  QString displayText() const;

  AdColorValue value() const;
  void setValue(const AdColorValue& value);

  QVector<PresetItem> presets() const;
  void setPresets(const QVector<PresetItem>& presets);

  QColor editableColor() const;
  void setEditableColor(const QColor& color);
  void setGradientStopPositions(const QList<double>& values);
  void clearSelection();

 signals:
  void modeChanged(Mode value);
  void modeOptionsChanged(const QVector<Mode>& options);
  void formatChanged(Format value);
  void allowClearChanged(bool value);
  void alphaChannelEnabledChanged(bool value);
  void formatSelectorEnabledChanged(bool value);
  void activeStopIndexChanged(int value);
  void cssTextChanged(const QString& value);
  void displayTextChanged(const QString& value);
  void valueChanged(const adqt::widgets::AdColorValue& value);
  void presetsChanged();
  void stateChanged();
  void cleared();

 private:
  friend class AdColorPicker;

  void applyState(const AdColorSelection& selection, Mode mode, const QVector<Mode>& modeOptions,
                  Format format, bool allowClear, bool alphaChannelEnabled,
                  bool formatSelectorEnabled, int activeStopIndex,
                  const QVector<PresetItem>& presets, bool emitClearedSignal = true);
  void applyNormalizedState(const AdColorSelection& selection, Mode mode,
                            const QVector<Mode>& modeOptions, Format format, bool allowClear,
                            bool alphaChannelEnabled, bool formatSelectorEnabled,
                            int activeStopIndex, const QVector<PresetItem>& presets,
                            const QString& cssText, const QString& displayText,
                            bool emitClearedSignal = true);

  Mode mode_ = Mode::Solid;
  QVector<Mode> modeOptions_ = {Mode::Solid};
  Format format_ = Format::Hex;
  bool allowClear_ = false;
  bool alphaChannelEnabled_ = true;
  bool formatSelectorEnabled_ = true;
  int activeStopIndex_ = 0;
  AdColorSelection colorValue_ = AdColorSelection::empty(QColor(0, 0, 0, 0));
  QVector<PresetItem> presets_;
  QString cachedCssText_;
  QString cachedDisplayText_;
};

class AdColorPickerPanel final : public QWidget {
  Q_OBJECT

  Q_PROPERTY(adqt::widgets::AdColorPicker::Size size READ size WRITE setSize NOTIFY sizeChanged)
  Q_PROPERTY(adqt::widgets::AdColorPicker::Mode mode READ mode WRITE setMode NOTIFY modeChanged)
  Q_PROPERTY(QVector<adqt::widgets::AdColorPicker::Mode> modeOptions READ modeOptions WRITE
                 setModeOptions NOTIFY modeOptionsChanged)
  Q_PROPERTY(
      adqt::widgets::AdColorPicker::Format format READ format WRITE setFormat NOTIFY formatChanged)
  Q_PROPERTY(bool allowClear READ allowClear WRITE setAllowClear NOTIFY allowClearChanged)
  Q_PROPERTY(bool alphaChannelEnabled READ alphaChannelEnabled WRITE setAlphaChannelEnabled NOTIFY
                 alphaChannelEnabledChanged)
  Q_PROPERTY(bool formatSelectorEnabled READ formatSelectorEnabled WRITE setFormatSelectorEnabled
                 NOTIFY formatSelectorEnabledChanged)
  Q_PROPERTY(adqt::widgets::AdColorPickerState* state READ state WRITE setState NOTIFY stateChanged)
  Q_PROPERTY(QVector<adqt::widgets::AdColorPicker::PresetItem> presets READ presets WRITE setPresets
                 NOTIFY presetsChanged)
  Q_PROPERTY(adqt::widgets::AdColorValue value READ value WRITE setValue NOTIFY valueChanged)

 public:
  using Size = AdColorPicker::Size;
  using Mode = AdColorPicker::Mode;
  using Format = AdColorPicker::Format;
  using PresetItem = AdColorPicker::PresetItem;
  using ComponentTokens = AdColorPicker::ComponentTokens;

  explicit AdColorPickerPanel(QWidget* parent = nullptr);
  ~AdColorPickerPanel() override;

  Size size() const;
  void setSize(Size value);

  Mode mode() const;
  void setMode(Mode value);

  QVector<Mode> modeOptions() const;
  void setModeOptions(const QVector<Mode>& options);

  Format format() const;
  void setFormat(Format value);

  bool allowClear() const;
  void setAllowClear(bool value);

  bool disabled() const;
  void setDisabled(bool value);

  bool alphaChannelEnabled() const;
  void setAlphaChannelEnabled(bool value);

  bool formatSelectorEnabled() const;
  void setFormatSelectorEnabled(bool value);

  QString cssText() const;
  void setCssText(const QString& value);

  QString displayText() const;

  AdColorValue value() const;
  void setValue(const AdColorValue& value);

  QVector<PresetItem> presets() const;
  void setPresets(const QVector<PresetItem>& presets);

  AdColorPickerState* state() const;
  void setState(AdColorPickerState* state);

  ComponentTokens componentTokens() const;
  void setComponentTokens(const ComponentTokens& tokens);
  void resetComponentTokens();

 signals:
  void sizeChanged(Size value);
  void modeChanged(Mode value);
  void modeOptionsChanged(const QVector<Mode>& options);
  void formatChanged(Format value);
  void allowClearChanged(bool value);
  void disabledChanged(bool value);
  void alphaChannelEnabledChanged(bool value);
  void formatSelectorEnabledChanged(bool value);
  void cssTextChanged(const QString& value);
  void displayTextChanged(const QString& value);
  void valueChanged(const adqt::widgets::AdColorValue& value);
  void presetsChanged();
  void stateChanged(adqt::widgets::AdColorPickerState* state);
  void componentTokensChanged();
  void cleared();
  void editingFinished(const adqt::widgets::AdColorValue& value);

 private:
  QPointer<AdColorPicker> core_;
};

}  // namespace adqt::widgets

Q_DECLARE_METATYPE(adqt::widgets::AdColorPicker::Mode)
Q_DECLARE_METATYPE(QVector<adqt::widgets::AdColorPicker::Mode>)
Q_DECLARE_METATYPE(adqt::widgets::AdColorPicker::Format)
Q_DECLARE_METATYPE(adqt::widgets::AdColorPicker::PresetItem)
Q_DECLARE_METATYPE(QVector<adqt::widgets::AdColorPicker::PresetItem>)
