#pragma once

#include <QColor>
#include <QGradient>
#include <QWidget>
#include <functional>
#include <memory>
#include <optional>

class QPainter;

namespace adqt::widgets {

// A passive, theme-aware progress indicator. All dimensions use Qt logical pixels,
// except circular strokeWidth, which is a percentage of the circle diameter.
class AdProgress final : public QWidget {
  Q_OBJECT
  Q_PROPERTY(Type type READ type WRITE setType NOTIFY typeChanged)
  Q_PROPERTY(Status status READ status WRITE setStatus NOTIFY statusChanged)
  Q_PROPERTY(Status effectiveStatus READ effectiveStatus NOTIFY effectiveStatusChanged)
  Q_PROPERTY(qreal percent READ percent WRITE setPercent NOTIFY percentChanged)
  Q_PROPERTY(qreal displayedPercent READ displayedPercent NOTIFY displayedPercentChanged)
  Q_PROPERTY(
      qreal successPercent READ successPercent WRITE setSuccessPercent NOTIFY successPercentChanged)
  Q_PROPERTY(bool showInfo READ showInfo WRITE setShowInfo NOTIFY showInfoChanged)
  Q_PROPERTY(Size sizeClass READ sizeClass WRITE setSizeClass NOTIFY sizeClassChanged)
  Q_PROPERTY(QSize progressSize READ progressSize WRITE setProgressSize NOTIFY progressSizeChanged)
  Q_PROPERTY(qreal strokeWidth READ strokeWidth WRITE setStrokeWidth NOTIFY strokeWidthChanged)
  Q_PROPERTY(StrokeLinecap strokeLinecap READ strokeLinecap WRITE setStrokeLinecap NOTIFY
                 strokeLinecapChanged)
  Q_PROPERTY(QColor strokeColor READ strokeColor WRITE setStrokeColor NOTIFY strokeColorChanged)
  Q_PROPERTY(QColor railColor READ railColor WRITE setRailColor NOTIFY railColorChanged)
  Q_PROPERTY(QColor successColor READ successColor WRITE setSuccessColor NOTIFY successColorChanged)
  Q_PROPERTY(int steps READ steps WRITE setSteps NOTIFY stepsChanged)
  Q_PROPERTY(qreal stepGap READ stepGap WRITE setStepGap NOTIFY stepGapChanged)
  Q_PROPERTY(qreal gapDegree READ gapDegree WRITE setGapDegree NOTIFY gapDegreeChanged)
  Q_PROPERTY(
      GapPlacement gapPlacement READ gapPlacement WRITE setGapPlacement NOTIFY gapPlacementChanged)
  Q_PROPERTY(PercentAlignment percentAlignment READ percentAlignment WRITE setPercentAlignment
                 NOTIFY percentAlignmentChanged)
  Q_PROPERTY(PercentPlacement percentPlacement READ percentPlacement WRITE setPercentPlacement
                 NOTIFY percentPlacementChanged)
  Q_PROPERTY(bool animationEnabled READ animationEnabled WRITE setAnimationEnabled NOTIFY
                 animationEnabledChanged)

 public:
  enum class Type { Line, Circle, Dashboard };
  Q_ENUM(Type)
  enum class Status { Automatic, Normal, Active, Exception, Success };
  Q_ENUM(Status)
  enum class Size { Small, Middle, Large };
  Q_ENUM(Size)
  enum class StrokeLinecap { Round, Butt, Square };
  Q_ENUM(StrokeLinecap)
  enum class GapPlacement { Top, Bottom, Start, End };
  Q_ENUM(GapPlacement)
  enum class PercentAlignment { Start, Center, End };
  Q_ENUM(PercentAlignment)
  enum class PercentPlacement { Outer, Inner };
  Q_ENUM(PercentPlacement)

  struct ColorTokens {
    std::optional<QColor> defaultColor;
    std::optional<QColor> remainingColor;
    std::optional<QColor> circleTextColor;
    std::optional<QColor> successColor;
    std::optional<QColor> exceptionColor;
  };
  struct MetricTokens {
    std::optional<qreal> lineBorderRadius;
    std::optional<qreal> circleTextFontSize;
    std::optional<qreal> circleIconFontSize;
    std::optional<int> animationDurationMs;
  };
  struct ComponentTokens {
    ColorTokens colors;
    MetricTokens metrics;
  };
  struct SemanticSlotStyle {
    std::optional<QColor> backgroundColor;
    std::optional<QColor> textColor;
    std::optional<QFont> font;
  };
  struct SemanticStyles {
    SemanticSlotStyle root;
    SemanticSlotStyle body;
    SemanticSlotStyle rail;
    SemanticSlotStyle track;
    SemanticSlotStyle indicator;
  };
  using Format = std::function<QString(qreal percent, qreal successPercent)>;
  using StepRounding = std::function<int(qreal filledSteps)>;

  explicit AdProgress(QWidget* parent = nullptr);
  ~AdProgress() override;

  Type type() const;
  void setType(Type value);
  Status status() const;
  void setStatus(Status value);
  Status effectiveStatus() const;
  qreal percent() const;
  void setPercent(qreal value);
  qreal displayedPercent() const;
  // -1 means no separate success segment. Setters clamp finite percentages to [0,100].
  qreal successPercent() const;
  void setSuccessPercent(qreal value);
  void clearSuccess();
  bool showInfo() const;
  void setShowInfo(bool value);
  Size sizeClass() const;
  void setSizeClass(Size value);
  // Non-positive dimensions select the natural size. Line width includes outer text;
  // for line steps, width specifies one step's width instead.
  QSize progressSize() const;
  void setProgressSize(const QSize& value);
  // 0 selects the natural width. Line uses logical pixels; circles use percent.
  qreal strokeWidth() const;
  void setStrokeWidth(qreal value);
  StrokeLinecap strokeLinecap() const;
  void setStrokeLinecap(StrokeLinecap value);
  QColor strokeColor() const;
  void setStrokeColor(const QColor& value);
  QColor railColor() const;
  void setRailColor(const QColor& value);
  QColor successColor() const;
  void setSuccessColor(const QColor& value);
  // Gradient positions are fractions [0,1]. Invalid stops are ignored; empty resets.
  QGradientStops strokeGradient() const;
  void setStrokeGradient(const QGradientStops& stops);
  bool gradientReversed() const;
  void setGradientReversed(bool reversed);
  int steps() const;
  // Bounded to [0,10000] to keep geometry allocation predictable.
  void setSteps(int value);
  qreal stepGap() const;
  void setStepGap(qreal value);
  QList<QColor> stepColors() const;
  void setStepColors(const QList<QColor>& colors);
  void setStepRounding(StepRounding rounding);
  void resetStepRounding();
  qreal gapDegree() const;
  void setGapDegree(qreal value);
  GapPlacement gapPlacement() const;
  void setGapPlacement(GapPlacement value);
  PercentAlignment percentAlignment() const;
  void setPercentAlignment(PercentAlignment value);
  PercentPlacement percentPlacement() const;
  void setPercentPlacement(PercentPlacement value);
  void setFormat(Format format);
  void resetFormat();
  QString formattedText() const;
  bool animationEnabled() const;
  void setAnimationEnabled(bool value);
  ComponentTokens componentTokens() const;
  void setComponentTokens(const ComponentTokens& value);
  void resetComponentTokens();
  SemanticStyles semanticStyles() const;
  void setSemanticStyles(const SemanticStyles& value);
  void resetSemanticStyles();

  QSize sizeHint() const override;
  QSize minimumSizeHint() const override;
  bool hasHeightForWidth() const override;
  int heightForWidth(int width) const override;

 signals:
  void typeChanged(Type value);
  void statusChanged(Status value);
  void effectiveStatusChanged(Status value);
  void percentChanged(qreal value);
  void displayedPercentChanged(qreal value);
  void successPercentChanged(qreal value);
  void showInfoChanged(bool value);
  void sizeClassChanged(Size value);
  void progressSizeChanged(const QSize& value);
  void strokeWidthChanged(qreal value);
  void strokeLinecapChanged(StrokeLinecap value);
  void strokeColorChanged(const QColor& value);
  void railColorChanged(const QColor& value);
  void successColorChanged(const QColor& value);
  void stepsChanged(int value);
  void stepGapChanged(qreal value);
  void gapDegreeChanged(qreal value);
  void gapPlacementChanged(GapPlacement value);
  void percentAlignmentChanged(PercentAlignment value);
  void percentPlacementChanged(PercentPlacement value);
  void animationEnabledChanged(bool value);
  void strokeGradientChanged();
  void stepColorsChanged();
  void formatChanged();
  void componentTokensChanged();
  void semanticStylesChanged();

 protected:
  bool event(QEvent* event) override;
  void paintEvent(QPaintEvent* event) override;
  void changeEvent(QEvent* event) override;

 private:
  struct Private;
  void refreshAppearance(bool resolveTheme = false);
  void refreshContent();
  void refreshValue();
  void refreshAnimation();
  void animationFrame(qint64 nowMs);
  void notifyAccessibleValueChange();
  void invalidatePainting();
  void paintProgress(QPainter& painter);
  std::unique_ptr<Private> d_;
};

}  // namespace adqt::widgets

Q_DECLARE_METATYPE(adqt::widgets::AdProgress::Type)
Q_DECLARE_METATYPE(adqt::widgets::AdProgress::Status)
Q_DECLARE_METATYPE(adqt::widgets::AdProgress::Size)
