#pragma once

#include <QAbstractSlider>
#include <QBrush>
#include <QColor>
#include <QMap>
#include <QList>
#include <QString>
#include <QWidget>

#include <functional>
#include <memory>
#include <optional>

#include "control_scale.h"

class QHideEvent;
class QStyleOptionSlider;
class QWheelEvent;

namespace adqt::widgets {

class AdTooltip;

class AdMultiSlider : public QWidget, public AdControlScaleParticipant {
  Q_OBJECT

  Q_PROPERTY(double minimum READ minimum WRITE setMinimum NOTIFY minimumChanged)
  Q_PROPERTY(double maximum READ maximum WRITE setMaximum NOTIFY maximumChanged)
  Q_PROPERTY(double singleStep READ singleStep WRITE setSingleStep NOTIFY singleStepChanged)
  Q_PROPERTY(double pageStep READ pageStep WRITE setPageStep NOTIFY pageStepChanged)
  Q_PROPERTY(bool tracking READ tracking WRITE setTracking NOTIFY trackingChanged)
  Q_PROPERTY(bool invertedAppearance READ invertedAppearance WRITE setInvertedAppearance NOTIFY
                 invertedAppearanceChanged)
  Q_PROPERTY(bool invertedControls READ invertedControls WRITE setInvertedControls NOTIFY
                 invertedControlsChanged)
  Q_PROPERTY(bool markSnapEnabled READ markSnapEnabled WRITE setMarkSnapEnabled NOTIFY
                 markSnapEnabledChanged)
  Q_PROPERTY(bool markIndicatorsVisible READ markIndicatorsVisible WRITE setMarkIndicatorsVisible
                 NOTIFY markIndicatorsVisibleChanged)
  Q_PROPERTY(bool markStepSnapEnabled READ markStepSnapEnabled WRITE setMarkStepSnapEnabled NOTIFY
                 markStepSnapEnabledChanged)
  Q_PROPERTY(bool selectionHighlightVisible READ selectionHighlightVisible WRITE
                 setSelectionHighlightVisible NOTIFY selectionHighlightVisibleChanged)
  Q_PROPERTY(
      Qt::Orientation orientation READ orientation WRITE setOrientation NOTIFY orientationChanged)
  Q_PROPERTY(bool disabled READ disabled WRITE setDisabled NOTIFY disabledChanged)
  Q_PROPERTY(bool keyboardEnabled READ keyboardEnabled WRITE setKeyboardEnabled NOTIFY
                 keyboardEnabledChanged)
  Q_PROPERTY(bool wheelEnabled READ wheelEnabled WRITE setWheelEnabled NOTIFY wheelEnabledChanged)
  Q_PROPERTY(double value READ value WRITE setValue NOTIFY valueChanged)
  Q_PROPERTY(double sliderPosition READ sliderPosition WRITE setSliderPosition NOTIFY
                 sliderPositionChanged)
  Q_PROPERTY(bool sliderDown READ sliderDown WRITE setSliderDown NOTIFY sliderDownChanged)
  Q_PROPERTY(
      QList<double> handleValues READ handleValues WRITE setHandleValues NOTIFY handleValuesChanged)
  Q_PROPERTY(
      int currentHandle READ currentHandle WRITE setCurrentHandle NOTIFY currentHandleChanged)
  Q_PROPERTY(bool selectionDragEnabled READ selectionDragEnabled WRITE setSelectionDragEnabled
                 NOTIFY selectionDragEnabledChanged)
  Q_PROPERTY(bool handleEditingEnabled READ handleEditingEnabled WRITE setHandleEditingEnabled
                 NOTIFY handleEditingEnabledChanged)
  Q_PROPERTY(int minimumHandleCount READ minimumHandleCount WRITE setMinimumHandleCount NOTIFY
                 minimumHandleCountChanged)
  Q_PROPERTY(int maximumHandleCount READ maximumHandleCount WRITE setMaximumHandleCount NOTIFY
                 maximumHandleCountChanged)
  Q_PROPERTY(
      bool tooltipEnabled READ tooltipEnabled WRITE setTooltipEnabled NOTIFY tooltipEnabledChanged)
  Q_PROPERTY(TooltipVisibleMode tooltipVisibleMode READ tooltipVisibleMode WRITE
                 setTooltipVisibleMode NOTIFY tooltipVisibleModeChanged)
  Q_PROPERTY(TooltipPlacement tooltipPlacement READ tooltipPlacement WRITE setTooltipPlacement
                 NOTIFY tooltipPlacementChanged)

 public:
  enum class Mode {
    Single,
    Range,
  };
  Q_ENUM(Mode)

  enum class TooltipVisibleMode {
    Auto,
    Always,
    Never,
  };
  Q_ENUM(TooltipVisibleMode)

  enum class TooltipPlacement {
    Top,
    Bottom,
    Left,
    Right,
  };
  Q_ENUM(TooltipPlacement)

  struct Mark {
    QString label;
    std::optional<QColor> color;
    std::optional<QFont> font;

    bool operator==(const Mark& other) const {
      return label == other.label && color == other.color && font == other.font;
    }
  };
  using MarkMap = QMap<double, Mark>;

  struct ComponentTokens {
    std::optional<int> controlSize;
    std::optional<int> railSize;
    std::optional<int> handleSize;
    std::optional<int> handleSizeHover;
    std::optional<int> handleLineWidth;
    std::optional<int> handleLineWidthHover;
    std::optional<int> marginMain;
    std::optional<int> marginCross;
    std::optional<int> markGap;
    std::optional<int> focusOutlineSize;
    std::optional<int> dotSize;
    std::optional<QColor> railBg;
    std::optional<QColor> railHoverBg;
    std::optional<QColor> trackBg;
    std::optional<QColor> trackHoverBg;
    std::optional<QColor> handleColor;
    std::optional<QColor> handleActiveColor;
    std::optional<QColor> handleActiveOutlineColor;
    std::optional<QColor> handleShadowColor;
    std::optional<QColor> handleActiveShadowColor;
    std::optional<QColor> handleColorDisabled;
    std::optional<QColor> dotBorderColor;
    std::optional<QColor> dotActiveBorderColor;
    std::optional<QColor> trackBgDisabled;
  };

  struct SemanticSlotStyle {
    std::optional<QColor> textColor;
    std::optional<QColor> backgroundColor;
    std::optional<QColor> borderColor;
    std::optional<QBrush> brush;
  };

  struct SemanticStyles {
    SemanticSlotStyle root;
    SemanticSlotStyle rail;
    SemanticSlotStyle track;
    SemanticSlotStyle tracks;
    SemanticSlotStyle handle;
    SemanticSlotStyle mark;
    SemanticSlotStyle markActive;
  };

  struct StyleContext {
    Mode mode = Mode::Single;
    Qt::Orientation orientation = Qt::Horizontal;
    bool reverse = false;
    bool disabled = false;
    bool dragging = false;
    bool hovered = false;
    bool focused = false;
    QList<double> values;
  };

  using TooltipFormatter = std::function<QString(double value)>;
  using SemanticStyleResolver = std::function<SemanticStyles(const StyleContext&)>;

  explicit AdMultiSlider(QWidget* parent = nullptr);
  ~AdMultiSlider() override;

  double minimum() const;
  void setMinimum(double value);

  double maximum() const;
  void setMaximum(double value);

  double singleStep() const;
  void setSingleStep(double value);

  double pageStep() const;
  void setPageStep(double value);

  bool tracking() const;
  void setTracking(bool value);

  bool invertedAppearance() const;
  void setInvertedAppearance(bool value);

  bool invertedControls() const;
  void setInvertedControls(bool value);

  bool markSnapEnabled() const;
  void setMarkSnapEnabled(bool value);

  bool markIndicatorsVisible() const;
  void setMarkIndicatorsVisible(bool value);

  bool markStepSnapEnabled() const;
  void setMarkStepSnapEnabled(bool value);

  bool selectionHighlightVisible() const;
  void setSelectionHighlightVisible(bool value);

  Qt::Orientation orientation() const;
  void setOrientation(Qt::Orientation value);

  bool disabled() const;
  void setDisabled(bool value);

  bool keyboardEnabled() const;
  void setKeyboardEnabled(bool value);

  bool wheelEnabled() const;
  void setWheelEnabled(bool value);

  double value() const;
  void setValue(double value);

  double sliderPosition() const;
  void setSliderPosition(double value);

  bool sliderDown() const;
  void setSliderDown(bool value);

  QList<double> handleValues() const;
  void setHandleValues(const QList<double>& values);
  int activeHandleIndex() const;
  int currentHandle() const;
  void setCurrentHandle(int index);

  bool selectionDragEnabled() const;
  void setSelectionDragEnabled(bool value);

  bool handleEditingEnabled() const;
  void setHandleEditingEnabled(bool value);

  int minimumHandleCount() const;
  void setMinimumHandleCount(int value);

  int maximumHandleCount() const;
  void setMaximumHandleCount(int value);

  bool tooltipEnabled() const;
  void setTooltipEnabled(bool value);

  TooltipVisibleMode tooltipVisibleMode() const;
  void setTooltipVisibleMode(TooltipVisibleMode value);

  TooltipPlacement tooltipPlacement() const;
  void setTooltipPlacement(TooltipPlacement value);

  MarkMap marks() const;
  void setMarks(const MarkMap& marks);
  void clearMarks();

  TooltipFormatter tooltipFormatter() const;
  void setTooltipFormatter(TooltipFormatter formatter);

  void setRange(double minimum, double maximum);
  void triggerAction(QAbstractSlider::SliderAction action);

  ComponentTokens componentTokens() const;
  void setComponentTokens(const ComponentTokens& tokens);
  void resetComponentTokens();

  SemanticStyles semanticStyles() const;
  void setSemanticStyles(const SemanticStyles& styles);
  void setSemanticStyleResolver(SemanticStyleResolver resolver);

  QSize sizeHint() const override;
  QSize minimumSizeHint() const override;
  void prepareControlScale(const AdControlScaleContext& context) override;
  void commitControlScale(const AdControlScaleContext& context) override;

 signals:
  void minimumChanged(double value);
  void maximumChanged(double value);
  void singleStepChanged(double value);
  void pageStepChanged(double value);
  void rangeChanged(double minimum, double maximum);
  void trackingChanged(bool value);
  void invertedAppearanceChanged(bool value);
  void invertedControlsChanged(bool value);
  void markSnapEnabledChanged(bool value);
  void markIndicatorsVisibleChanged(bool value);
  void markStepSnapEnabledChanged(bool value);
  void selectionHighlightVisibleChanged(bool value);
  void orientationChanged(Qt::Orientation value);
  void disabledChanged(bool value);
  void keyboardEnabledChanged(bool value);
  void wheelEnabledChanged(bool value);
  void valueChanged(double value);
  void sliderPositionChanged(double value);
  void sliderDownChanged(bool value);
  void handleValuesChanged(const QList<double>& values);
  void selectionDragEnabledChanged(bool value);
  void handleEditingEnabledChanged(bool value);
  void minimumHandleCountChanged(int value);
  void maximumHandleCountChanged(int value);
  void tooltipEnabledChanged(bool value);
  void tooltipVisibleModeChanged(TooltipVisibleMode value);
  void tooltipPlacementChanged(TooltipPlacement value);
  void marksChanged();
  void componentTokensChanged();
  void semanticStylesChanged();
  void activeHandleIndexChanged(int index);
  void currentHandleChanged(int index);
  void handlePressed(int index);
  void handleMoved(int index, double value);
  void handleReleased(int index);
  void handleActionTriggered(QAbstractSlider::SliderAction action, int index);
  void editingFinished();

 protected:
  Mode mode() const;
  void setMode(Mode value);

  void paintEvent(QPaintEvent* event) override;
  bool event(QEvent* event) override;
  void enterEvent(QEnterEvent* event) override;
  void leaveEvent(QEvent* event) override;
  void mousePressEvent(QMouseEvent* event) override;
  void mouseMoveEvent(QMouseEvent* event) override;
  void mouseReleaseEvent(QMouseEvent* event) override;
  void wheelEvent(QWheelEvent* event) override;
  void keyPressEvent(QKeyEvent* event) override;
  void keyReleaseEvent(QKeyEvent* event) override;
  void focusInEvent(QFocusEvent* event) override;
  void focusOutEvent(QFocusEvent* event) override;
  void changeEvent(QEvent* event) override;
  void resizeEvent(QResizeEvent* event) override;
  void showEvent(QShowEvent* event) override;
  void hideEvent(QHideEvent* event) override;

 private:
  class TooltipHost;

  enum class DragMode {
    None,
    Handle,
    Track,
  };

  struct LayoutInfo;

  MarkMap effectiveMarks() const;
  SemanticStyles resolvedSemanticStyles() const;
  QList<double> normalizedValues(const QList<double>& values, bool forceRangeMode) const;
  QList<double> snapPoints() const;
  double normalizeValue(double value) const;
  void setHandlesInternal(const QList<double>& handles, bool emitValueChangedSignal,
                          bool fromUserAction, bool commitResult = true,
                          bool valueOnlyLayoutChange = false);
  void setInversionState(bool invertedAppearance, bool invertedControls);
  void setFocusHandleIndex(int index);
  void setSliderDownInternal(bool value);
  void setPendingChangeStateFromVisuals();
  void commitPendingChanges();
  void emitChangedSignalsForCurrentMode();
  void emitCompletedSignalsForCurrentMode();
  void refreshAfterPropertyChange(bool updateGeometryHint = true);
  void emitActiveHandleIndexChangedIfNeeded(int previousIndex);
  void emitSliderPositionChangedIfNeeded(double previousPosition);
  void syncAccessibleState();
  void syncInteractionCursor();
  double effectiveSingleStep() const;
  double effectivePageStep() const;
  void notifyAccessibleValueChange() const;
  void notifyAccessibleDescriptionChange() const;
  void notifyAccessibleFocusChange() const;
  void initStyleOption(QStyleOptionSlider* option, int handleIndex = -1) const;
  int sliderHandleIndex() const;
  void emitHandleActionTriggered(QAbstractSlider::SliderAction action, int index);

  LayoutInfo buildLayout() const;
  int hitTestHandle(const QPoint& pos, const LayoutInfo& layout) const;
  int nearestHandleIndex(double value) const;
  double valueFromHandlePosition(const QPoint& pos, const LayoutInfo& layout) const;
  int handlePositionFromValue(double value, const LayoutInfo& layout) const;
  double clampTrackDelta(double delta) const;
  int handleRailAction(const QPoint& pos, const LayoutInfo& layout, bool emitLiveSignals);
  bool deleteHandleAt(int index, bool emitLiveSignals = true);
  bool addHandleAt(double value, int* insertedIndex = nullptr, bool emitLiveSignals = true);
  bool isMarkActive(double markValue) const;
  QList<int> tooltipHandleIndexes() const;
  QString tooltipText(double value) const;
  void ensureTooltipHosts(int count);
  void clearTooltipHosts();
  void requestTooltipSync();
  void syncTooltipHosts(const LayoutInfo* layout = nullptr);
  void invalidateLayoutCache();
  void invalidateHandleLayoutCache();
  void invalidateSemanticStyleCache();
  void refreshHandleRects(LayoutInfo* layout) const;
  void setHoverHandleIndex(int index);
  void setDragHandleIndex(int index);
  int indexForHandleId(qint64 id) const;
  void syncTrackedHandleIndexes();

  Mode mode_ = Mode::Range;
  double minimum_ = 0.0;
  double maximum_ = 100.0;
  double pageStep_ = 10.0;
  bool tracking_ = true;
  bool invertedAppearance_ = false;
  bool invertedControls_ = false;
  double step_ = 1.0;
  bool marksOnly_ = false;
  bool markIndicatorsVisible_ = true;
  bool markStepSnapEnabled_ = false;
  bool included_ = true;
  Qt::Orientation orientation_ = Qt::Horizontal;
  bool keyboardEnabled_ = true;
  bool wheelEnabled_ = false;
  bool draggableTrack_ = false;
  bool editableHandles_ = false;
  int minHandleCount_ = 0;
  int maxHandleCount_ = -1;
  bool tooltipEnabled_ = true;
  TooltipVisibleMode tooltipVisibleMode_ = TooltipVisibleMode::Auto;
  TooltipPlacement tooltipPlacement_ = TooltipPlacement::Top;

  QList<double> handles_;
  QList<double> committedHandles_;
  MarkMap marks_;
  TooltipFormatter tooltipFormatter_;
  ComponentTokens componentTokens_;
  SemanticStyles semanticStyles_;
  SemanticStyleResolver semanticStyleResolver_;

  bool focusVisible_ = false;
  bool dragging_ = false;
  bool sliderDown_ = false;
  bool dragChanged_ = false;
  bool pendingPrimaryValueChange_ = false;
  bool pendingValuesChange_ = false;
  bool keyboardInteractionActive_ = false;
  DragMode dragMode_ = DragMode::None;
  int pressedHandleIndex_ = -1;
  qint64 dragHandleId_ = -1;
  int dragHandleIndex_ = -1;
  qint64 hoverHandleId_ = -1;
  int hoverHandleIndex_ = -1;
  qint64 focusHandleId_ = -1;
  int focusHandleIndex_ = -1;
  QPoint dragStartPos_;
  QList<double> dragStartValues_;
  QList<qint64> handleIds_;
  qint64 nextHandleId_ = 1;
  QList<TooltipHost*> tooltipHosts_;
  bool tooltipSyncPending_ = false;
  bool lastDisabledState_ = false;
  mutable std::unique_ptr<LayoutInfo> layoutCache_;
  mutable QSize layoutCacheSize_;
  mutable bool layoutCacheDirty_ = true;
  mutable bool layoutStyleDirty_ = true;
  mutable bool handleLayoutDirty_ = false;
  mutable bool semanticStyleDirty_ = false;
  AdControlScaleContext controlScale_;
  QFont referenceFont_;
  bool referenceFontCaptured_ = false;
};

using AdSliderMark = AdMultiSlider::Mark;
using AdSliderMarkMap = AdMultiSlider::MarkMap;
using AdSliderComponentTokens = AdMultiSlider::ComponentTokens;
using AdSliderSemanticSlotStyle = AdMultiSlider::SemanticSlotStyle;
using AdSliderSemanticStyles = AdMultiSlider::SemanticStyles;
using AdSliderStyleContext = AdMultiSlider::StyleContext;
using AdSliderTooltipFormatter = AdMultiSlider::TooltipFormatter;
using AdSliderSemanticStyleResolver = AdMultiSlider::SemanticStyleResolver;

class AdSlider final : public AdMultiSlider {
  Q_OBJECT

 public:
  using Mark = AdMultiSlider::Mark;
  using MarkMap = AdMultiSlider::MarkMap;
  using ComponentTokens = AdMultiSlider::ComponentTokens;
  using SemanticSlotStyle = AdMultiSlider::SemanticSlotStyle;
  using SemanticStyles = AdMultiSlider::SemanticStyles;
  using StyleContext = AdMultiSlider::StyleContext;
  using TooltipFormatter = AdMultiSlider::TooltipFormatter;
  using SemanticStyleResolver = AdMultiSlider::SemanticStyleResolver;
  using TooltipVisibleMode = AdMultiSlider::TooltipVisibleMode;
  using TooltipPlacement = AdMultiSlider::TooltipPlacement;

  explicit AdSlider(QWidget* parent = nullptr);

  void setRange(double minimum, double maximum);

 signals:
  void sliderPressed();
  void sliderMoved(double value);
  void sliderReleased();
  void actionTriggered(QAbstractSlider::SliderAction action);

 private:
  using AdMultiSlider::activeHandleIndex;
  using AdMultiSlider::currentHandle;
  using AdMultiSlider::handleEditingEnabled;
  using AdMultiSlider::handleValues;
  using AdMultiSlider::maximumHandleCount;
  using AdMultiSlider::minimumHandleCount;
  using AdMultiSlider::selectionDragEnabled;
  using AdMultiSlider::setCurrentHandle;
  using AdMultiSlider::setHandleEditingEnabled;
  using AdMultiSlider::setHandleValues;
  using AdMultiSlider::setMaximumHandleCount;
  using AdMultiSlider::setMinimumHandleCount;
  using AdMultiSlider::setSelectionDragEnabled;
};

class AdRangeSlider final : public AdMultiSlider {
  Q_OBJECT

  Q_PROPERTY(double lowerValue READ lowerValue WRITE setLowerValue NOTIFY lowerValueChanged)
  Q_PROPERTY(double upperValue READ upperValue WRITE setUpperValue NOTIFY upperValueChanged)

 public:
  using Mark = AdMultiSlider::Mark;
  using MarkMap = AdMultiSlider::MarkMap;
  using ComponentTokens = AdMultiSlider::ComponentTokens;
  using SemanticSlotStyle = AdMultiSlider::SemanticSlotStyle;
  using SemanticStyles = AdMultiSlider::SemanticStyles;
  using StyleContext = AdMultiSlider::StyleContext;
  using TooltipFormatter = AdMultiSlider::TooltipFormatter;
  using SemanticStyleResolver = AdMultiSlider::SemanticStyleResolver;
  using TooltipVisibleMode = AdMultiSlider::TooltipVisibleMode;
  using TooltipPlacement = AdMultiSlider::TooltipPlacement;

  explicit AdRangeSlider(QWidget* parent = nullptr);

  double lowerValue() const;
  void setLowerValue(double value);

  double upperValue() const;
  void setUpperValue(double value);

  void setValues(double lowerValue, double upperValue);
  void setRange(double minimum, double maximum);

 signals:
  void lowerValueChanged(double value);
  void upperValueChanged(double value);
  void valuesChanged(double lowerValue, double upperValue);
  void sliderPressed(int handleIndex);
  void sliderMoved(int handleIndex, double value);
  void sliderReleased(int handleIndex);
  void actionTriggered(QAbstractSlider::SliderAction action, int handleIndex);

 private:
  double lastLowerValue_ = 0.0;
  double lastUpperValue_ = 0.0;
  using AdMultiSlider::activeHandleIndex;
  using AdMultiSlider::currentHandle;
  using AdMultiSlider::handleEditingEnabled;
  using AdMultiSlider::handleValues;
  using AdMultiSlider::maximumHandleCount;
  using AdMultiSlider::minimumHandleCount;
  using AdMultiSlider::setCurrentHandle;
  using AdMultiSlider::setHandleEditingEnabled;
  using AdMultiSlider::setHandleValues;
  using AdMultiSlider::setMaximumHandleCount;
  using AdMultiSlider::setMinimumHandleCount;
  using AdMultiSlider::setSliderDown;
  using AdMultiSlider::setSliderPosition;
  using AdMultiSlider::setValue;
  using AdMultiSlider::sliderDown;
  using AdMultiSlider::sliderPosition;
  using AdMultiSlider::value;
};

}  // namespace adqt::widgets
