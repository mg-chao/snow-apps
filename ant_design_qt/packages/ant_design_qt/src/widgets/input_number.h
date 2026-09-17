#pragma once

#include <QAbstractSpinBox>
#include <QColor>
#include <QPointer>
#include <QSet>
#include <QVariant>

#include <memory>
#include <optional>

#include "icon_core.h"

class QEnterEvent;
class QHideEvent;
class QLabel;
class QLineEdit;
class QMouseEvent;
class QMoveEvent;
class QPaintEvent;
class QResizeEvent;
class QShowEvent;
class QToolButton;
class QVBoxLayout;

namespace adqt::widgets::detail {
class InputNumberValueModel;
struct InputNumberVisualStyle;
}  // namespace adqt::widgets::detail

namespace adqt::widgets {

class AdInputNumberTextPolicy : public QObject {
  Q_OBJECT

 public:
  explicit AdInputNumberTextPolicy(QObject* parent = nullptr) : QObject(parent) {}
  ~AdInputNumberTextPolicy() override = default;

  virtual QString formatText(const QString& canonicalText, bool editing,
                             const QString& inputText) const {
    Q_UNUSED(canonicalText)
    Q_UNUSED(editing)
    Q_UNUSED(inputText)
    return QString();
  }

  virtual QString parseText(const QString& text) const { return text; }
};

class AdInputNumber final : public QAbstractSpinBox {
  Q_OBJECT

  Q_PROPERTY(
      ControlSize controlSize READ controlSize WRITE setControlSize NOTIFY controlSizeChanged)
  Q_PROPERTY(Variant variant READ variant WRITE setVariant NOTIFY variantChanged)
  Q_PROPERTY(Status status READ status WRITE setStatus NOTIFY statusChanged)
  Q_PROPERTY(StepButtonLayout stepButtonLayout READ stepButtonLayout WRITE setStepButtonLayout
                 NOTIFY stepButtonLayoutChanged)
  Q_PROPERTY(ValueMode valueMode READ valueMode WRITE setValueMode NOTIFY valueModeChanged)
  Q_PROPERTY(RangeMode rangeMode READ rangeMode WRITE setRangeMode NOTIFY rangeModeChanged)
  Q_PROPERTY(bool readOnly READ readOnly WRITE setReadOnly NOTIFY readOnlyChanged)
  Q_PROPERTY(bool stepButtonsVisible READ stepButtonsVisible WRITE setStepButtonsVisible NOTIFY
                 stepButtonsVisibleChanged)
  Q_PROPERTY(bool stepKeysEnabled READ stepKeysEnabled WRITE setStepKeysEnabled NOTIFY
                 stepKeysEnabledChanged)
  Q_PROPERTY(bool wheelStepEnabled READ wheelStepEnabled WRITE setWheelStepEnabled NOTIFY
                 wheelStepEnabledChanged)
  Q_PROPERTY(bool hasValue READ hasValue NOTIFY hasValueChanged)
  Q_PROPERTY(double value READ value WRITE setValue NOTIFY valueChanged)
  Q_PROPERTY(QString exactValue READ exactValue WRITE setExactValue NOTIFY exactValueChanged)
  Q_PROPERTY(double minimum READ minimum WRITE setMinimum NOTIFY minimumChanged)
  Q_PROPERTY(double maximum READ maximum WRITE setMaximum NOTIFY maximumChanged)
  Q_PROPERTY(double singleStep READ singleStep WRITE setSingleStep NOTIFY singleStepChanged)
  Q_PROPERTY(int decimals READ decimals WRITE setDecimals NOTIFY decimalsChanged)
  Q_PROPERTY(
      QString exactMinimum READ exactMinimum WRITE setExactMinimum NOTIFY exactMinimumChanged)
  Q_PROPERTY(
      QString exactMaximum READ exactMaximum WRITE setExactMaximum NOTIFY exactMaximumChanged)
  Q_PROPERTY(QString exactSingleStep READ exactSingleStep WRITE setExactSingleStep NOTIFY
                 exactSingleStepChanged)
  Q_PROPERTY(QString placeholderText READ placeholderText WRITE setPlaceholderText NOTIFY
                 placeholderTextChanged)
  Q_PROPERTY(QString prefixText READ prefixText WRITE setPrefixText NOTIFY prefixTextChanged)
  Q_PROPERTY(QString suffixText READ suffixText WRITE setSuffixText NOTIFY suffixTextChanged)
  Q_PROPERTY(Qt::Alignment textAlignment READ textAlignment WRITE setTextAlignment NOTIFY
                 textAlignmentChanged)
  Q_PROPERTY(bool joinedLeft READ joinedLeft WRITE setJoinedLeft NOTIFY joinedLeftChanged)
  Q_PROPERTY(bool joinedRight READ joinedRight WRITE setJoinedRight NOTIFY joinedRightChanged)
  Q_PROPERTY(adqt::widgets::AdInputNumberTextPolicy* textPolicy READ textPolicy WRITE setTextPolicy
                 NOTIFY textPolicyChanged)

 public:
  enum class ControlSize {
    Large,
    Medium,
    Small,
  };
  Q_ENUM(ControlSize)

  enum class Variant {
    Outlined,
    Filled,
    Borderless,
    Underlined,
  };
  Q_ENUM(Variant)

  enum class Status {
    None,
    Error,
    Warning,
  };
  Q_ENUM(Status)

  enum class StepButtonLayout {
    Compact,
    Split,
  };
  Q_ENUM(StepButtonLayout)

  enum class ValueMode {
    Number,
    ExactDecimal,
  };
  Q_ENUM(ValueMode)

  enum class RangeMode {
    Strict,
    Permissive,
  };
  Q_ENUM(RangeMode)

  enum class FocusSelection {
    Preserve,
    Start,
    End,
    SelectAll,
  };
  Q_ENUM(FocusSelection)

  enum class StepType {
    Up,
    Down,
  };
  Q_ENUM(StepType)

  enum class StepEmitter {
    Handler,
    KeyDown,
    Wheel,
  };
  Q_ENUM(StepEmitter)

  struct AppearanceMetrics {
    std::optional<int> controlWidth;
    std::optional<int> controlHeight;
    std::optional<int> borderRadius;
    std::optional<int> borderWidth;
    std::optional<int> horizontalPadding;
    std::optional<int> iconSize;
    std::optional<int> inputFontSize;
    std::optional<int> inputFontSizeSM;
    std::optional<int> inputFontSizeLG;
    std::optional<int> largeHorizontalPadding;
    std::optional<int> handleWidth;
    std::optional<int> handleVisibleWidth;
  };

  struct AppearanceColors {
    std::optional<QColor> backgroundColor;
    std::optional<QColor> borderColor;
    std::optional<QColor> hoverBorderColor;
    std::optional<QColor> activeBorderColor;
    std::optional<QColor> textColor;
    std::optional<QColor> placeholderColor;
    std::optional<QColor> prefixColor;
    std::optional<QColor> suffixColor;
    std::optional<QColor> actionBackgroundColor;
    std::optional<QColor> actionPressedBackgroundColor;
    std::optional<QColor> actionBorderColor;
    std::optional<QColor> actionHoverColor;
    std::optional<QColor> actionIconColor;
  };

  struct AppearanceRole {
    std::optional<QColor> textColor;
    std::optional<QColor> backgroundColor;
    std::optional<QColor> borderColor;
  };

  struct AppearanceOverrides {
    AppearanceMetrics metrics;
    AppearanceColors colors;
    AppearanceRole frame;
    AppearanceRole input;
    AppearanceRole prefix;
    AppearanceRole suffix;
    AppearanceRole actions;
    AppearanceRole action;
  };

  explicit AdInputNumber(QWidget* parent = nullptr);
  ~AdInputNumber() override;

  ControlSize controlSize() const;
  void setControlSize(ControlSize value);

  Variant variant() const;
  void setVariant(Variant value);

  Status status() const;
  void setStatus(Status value);

  StepButtonLayout stepButtonLayout() const;
  void setStepButtonLayout(StepButtonLayout value);

  ValueMode valueMode() const;
  void setValueMode(ValueMode value);

  RangeMode rangeMode() const;
  void setRangeMode(RangeMode value);

  bool readOnly() const;
  void setReadOnly(bool value);

  bool stepButtonsVisible() const;
  void setStepButtonsVisible(bool value);

  bool stepKeysEnabled() const;
  void setStepKeysEnabled(bool value);

  bool wheelStepEnabled() const;
  void setWheelStepEnabled(bool value);

  void setKeyboardTracking(bool value);

  bool hasValue() const;

  double value() const;
  void setValue(double value);

  QString exactValue() const;
  void setExactValue(const QString& value);

  double minimum() const;
  void setMinimum(double value);

  double maximum() const;
  void setMaximum(double value);

  void setRange(double minimum, double maximum);

  double singleStep() const;
  void setSingleStep(double value);

  int decimals() const;
  void setDecimals(int value);

  QString exactMinimum() const;
  void setExactMinimum(const QString& value);

  QString exactMaximum() const;
  void setExactMaximum(const QString& value);

  void setExactRange(const QString& minimum, const QString& maximum);

  QString exactSingleStep() const;
  void setExactSingleStep(const QString& value);

  QString placeholderText() const;
  void setPlaceholderText(const QString& value);

  QString prefixText() const;
  void setPrefixText(const QString& value);

  QString suffixText() const;
  void setSuffixText(const QString& value);

  Qt::Alignment textAlignment() const;
  void setTextAlignment(Qt::Alignment value);

  bool joinedLeft() const;
  void setJoinedLeft(bool value);

  bool joinedRight() const;
  void setJoinedRight(bool value);

  adqt::icons::IconRef prefixIconRef() const;
  void setPrefixIconRef(const adqt::icons::IconRef& token);

  adqt::icons::IconRef suffixIconRef() const;
  void setSuffixIconRef(const adqt::icons::IconRef& token);

  adqt::icons::IconRef upIconRef() const;
  void setUpIconRef(const adqt::icons::IconRef& token);

  adqt::icons::IconRef downIconRef() const;
  void setDownIconRef(const adqt::icons::IconRef& token);

  AdInputNumberTextPolicy* textPolicy() const;
  void setTextPolicy(AdInputNumberTextPolicy* value);

  QString displayText() const;
  void clear() override;

  AppearanceOverrides appearanceOverrides() const;
  void setAppearanceOverrides(const AppearanceOverrides& overrides);
  void resetAppearanceOverrides();

  QSize sizeHint() const override;
  QSize minimumSizeHint() const override;

  void focusEditor(FocusSelection selection = FocusSelection::Preserve);
  void blurInput();

 signals:
  void controlSizeChanged(ControlSize value);
  void variantChanged(Variant value);
  void statusChanged(Status value);
  void stepButtonLayoutChanged(StepButtonLayout value);
  void valueModeChanged(ValueMode value);
  void rangeModeChanged(RangeMode value);
  void readOnlyChanged(bool value);
  void stepButtonsVisibleChanged(bool value);
  void stepKeysEnabledChanged(bool value);
  void wheelStepEnabledChanged(bool value);
  void keyboardTrackingChanged(bool value);
  void hasValueChanged(bool value);
  void valueChanged(double value);
  void exactValueChanged(const QString& value);
  void minimumChanged(double value);
  void maximumChanged(double value);
  void singleStepChanged(double value);
  void decimalsChanged(int value);
  void exactMinimumChanged(const QString& value);
  void exactMaximumChanged(const QString& value);
  void exactSingleStepChanged(const QString& value);
  void textChanged(const QString& text);
  void placeholderTextChanged(const QString& value);
  void prefixTextChanged(const QString& value);
  void suffixTextChanged(const QString& value);
  void textAlignmentChanged(Qt::Alignment value);
  void joinedLeftChanged(bool value);
  void joinedRightChanged(bool value);
  void prefixIconRefChanged(const adqt::icons::IconRef& token);
  void suffixIconRefChanged(const adqt::icons::IconRef& token);
  void upIconRefChanged(const adqt::icons::IconRef& token);
  void downIconRefChanged(const adqt::icons::IconRef& token);
  void textPolicyChanged(AdInputNumberTextPolicy* value);
  void appearanceOverridesChanged();
  void stepped(int offset, StepType type, StepEmitter emitter);

 protected:
  bool eventFilter(QObject* watched, QEvent* event) override;
  void focusInEvent(QFocusEvent* event) override;
  void focusOutEvent(QFocusEvent* event) override;
  void enterEvent(QEnterEvent* event) override;
  void leaveEvent(QEvent* event) override;
  void paintEvent(QPaintEvent* event) override;
  void changeEvent(QEvent* event) override;
  void moveEvent(QMoveEvent* event) override;
  void resizeEvent(QResizeEvent* event) override;
  void showEvent(QShowEvent* event) override;
  void hideEvent(QHideEvent* event) override;
  void wheelEvent(QWheelEvent* event) override;
  void mousePressEvent(QMouseEvent* event) override;
  QValidator::State validate(QString& text, int& pos) const override;
  void fixup(QString& text) const override;
  void stepBy(int steps) override;
  StepEnabled stepEnabled() const override;

 private:
  struct StyleContext {
    ControlSize controlSize = ControlSize::Medium;
    Variant variant = Variant::Outlined;
    Status status = Status::None;
    StepButtonLayout stepButtonLayout = StepButtonLayout::Compact;
    ValueMode valueMode = ValueMode::Number;
    bool disabled = false;
    bool readOnly = false;
    bool focused = false;
    bool hovered = false;
    bool stepButtonsVisible = true;
    bool outOfRange = false;
  };

  struct ResolvedVisualState;

  QVariant normalizeBoundaryValue(const QVariant& value) const;
  QVariant normalizeSingleStepValue(const QVariant& value) const;
  void applyRangeValues(const QVariant& minimumValue, const QVariant& maximumValue);
  void emitRangeSignals(bool minimumDidChange, bool maximumDidChange);
  void applySingleStepValue(const QVariant& value);

  StyleContext buildStyleContext(bool interactiveControls, const QVariant& visualValue) const;
  ResolvedVisualState resolvedVisualState() const;

  void syncValueModelConfig();
  void updateEditorTextFromValue(bool userTyping);
  void emitTextChangedIfNeeded(const QString& text);
  void commitFromEditor();
  bool performStep(int steps, StepEmitter emitter);
  int consumeWheelSteps(int angleDelta);
  QVariant effectiveUserInputValue(bool committed) const;
  QVariant visualValue() const;
  QString canonicalTextForValue(const QVariant& value) const;
  void setCommittedValueInternal(const QVariant& value, bool emitSignal, bool preserveEditorText,
                                 bool clampToRange);
  void emitCommittedValueSignals(const QVariant& previousValue, bool previousHasValue);
  QString displayTextForValue(const QVariant& value, bool editing, const QString& input) const;
  void refreshVisualState(bool geometryChanged);
  void layoutChildren(const ResolvedVisualState& state);
  void updateInteractionFocusOverlay();
  void updateActionIcons(const ResolvedVisualState& state);
  bool shouldShowInputActions(const ResolvedVisualState& state) const;
  void updateActionVisibility();
  void updateAffixVisual();
  void updateSuffixSpinnerState();
  void updateReadOnlyState();
  void updateInteractiveCursor();
  void updateInputActionsGeometry(const ResolvedVisualState& state);
  bool isHoverTrackedChild(const QObject* watched) const;
  void setChildHovered(const QObject* watched, bool hovered);
  void syncHoveredState();
  void focusFromMouseGlobalPos(const QPoint& globalPos, Qt::FocusReason reason);
  void bumpJoinedZOrder();
  void syncAccessibleState();
  void notifyAccessibleValueChange() const;
  void notifyAccessibleDescriptionChange() const;
  void notifyAccessibleFocusChange() const;

  ControlSize controlSize_ = ControlSize::Medium;
  Variant variant_ = Variant::Outlined;
  Status status_ = Status::None;
  StepButtonLayout stepButtonLayout_ = StepButtonLayout::Compact;
  ValueMode valueMode_ = ValueMode::Number;
  RangeMode rangeMode_ = RangeMode::Strict;
  bool readOnly_ = false;
  bool stepButtonsVisible_ = true;
  bool stepKeysEnabled_ = true;
  bool wheelStepEnabled_ = false;
  QVariant value_;
  QVariant minimum_;
  QVariant maximum_;
  QVariant singleStep_ = QVariant(1.0);
  int decimals_ = -1;
  QString prefixText_;
  QString suffixText_;
  Qt::Alignment textAlignment_ = Qt::AlignLeft | Qt::AlignVCenter;
  bool customTextAlignment_ = false;
  bool joinedLeft_ = false;
  bool joinedRight_ = false;
  adqt::icons::IconRef prefixIconRef_;
  adqt::icons::IconRef suffixIconRef_;
  adqt::icons::IconRef upIconRef_;
  adqt::icons::IconRef downIconRef_;
  QPointer<AdInputNumberTextPolicy> textPolicy_;
  AppearanceOverrides appearanceOverrides_;

  QLineEdit* editor_ = nullptr;
  QLabel* prefixIconLabel_ = nullptr;
  QLabel* prefixLabel_ = nullptr;
  QLabel* suffixLabel_ = nullptr;
  QLabel* suffixIconLabel_ = nullptr;
  QWidget* inputActionsWidget_ = nullptr;
  QWidget* inputActionsBorderOverlay_ = nullptr;
  QVBoxLayout* inputActionsLayout_ = nullptr;
  QToolButton* inputUpButton_ = nullptr;
  QToolButton* inputDownButton_ = nullptr;
  QToolButton* splitDownButton_ = nullptr;
  QToolButton* splitUpButton_ = nullptr;

  bool hovered_ = false;
  bool selfHovered_ = false;
  bool suffixSpinnerSubscribed_ = false;
  QSet<const QObject*> hoveredChildren_;
  bool focused_ = false;
  bool userTyping_ = false;
  bool internalTextUpdate_ = false;
  int wheelStepRemainder_ = 0;
  StepEmitter currentStepEmitter_ = StepEmitter::Handler;
  QString lastEmittedText_;

  std::unique_ptr<detail::InputNumberValueModel> valueModel_;
};

}  // namespace adqt::widgets

Q_DECLARE_METATYPE(adqt::widgets::AdInputNumber::StepButtonLayout)
Q_DECLARE_METATYPE(adqt::widgets::AdInputNumber::ValueMode)
Q_DECLARE_METATYPE(adqt::widgets::AdInputNumber::RangeMode)
Q_DECLARE_METATYPE(adqt::widgets::AdInputNumber::StepType)
Q_DECLARE_METATYPE(adqt::widgets::AdInputNumber::StepEmitter)
