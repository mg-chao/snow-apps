#pragma once

#include <QBrush>
#include <QColor>
#include <QFont>

#include "slider.h"

namespace adqt::theme {
struct ResolvedTheme;
}

namespace adqt::widgets::detail {

struct SliderMetrics {
  int controlSize = 20;
  int railSize = 4;
  int handleSize = 10;
  int handleSizeHover = 12;
  qreal handleLineWidth = 2.0;
  qreal handleLineWidthHover = 2.5;
  int dotSize = 8;
  int marginMain = 8;
  int marginCross = 10;
  int markGap = 10;
  qreal focusOutlineSize = 6.0;
  int tooltipPaddingH = 8;
  int tooltipPaddingV = 4;
  int tooltipRadius = 6;
  int tooltipOffset = 8;
  int tooltipArrowSize = 5;
  QFont font;
};

struct SliderVisualStyle {
  qreal backgroundOpacity = 1.0;
  QColor rootBg;
  QColor railBg;
  QColor railHoverBg;
  QBrush railBrush;
  bool useRailBrush = false;
  QColor trackBg;
  QColor trackHoverBg;
  QColor handleColor;
  QColor handleHoverColor;
  QColor handleActiveColor;
  QColor handleActiveOutlineColor;
  QColor handleShadowColor;
  QColor handleActiveShadowColor;
  QColor handleColorDisabled;
  QColor surfaceBg;
  QBrush handleBrush;
  bool useHandleBrush = false;
  QColor dotBorderColor;
  QColor dotHoverBorderColor;
  QColor dotActiveBorderColor;
  QColor trackBgDisabled;
  QColor markColor;
  QColor markActiveColor;
  QColor tooltipBg;
  QColor tooltipText;
  QBrush tracksBrush;
  bool useTracksBrush = false;
  SliderMetrics metrics;
};

struct SliderStyleInput {
  AdMultiSlider::Mode mode = AdMultiSlider::Mode::Single;
  Qt::Orientation orientation = Qt::Horizontal;
  bool hovered = false;
  bool dragging = false;
  bool focused = false;
  bool disabled = false;
  bool reverse = false;
  bool deferSemanticStyles = false;
  QFont baseFont;
  AdMultiSlider::ComponentTokens componentTokens;
  AdMultiSlider::SemanticStyles semanticStyles;
};

SliderVisualStyle resolveSliderVisualStyle(const SliderStyleInput& input,
                                           const adqt::theme::ResolvedTheme& resolvedTheme);
SliderVisualStyle resolveSliderVisualStyle(const SliderStyleInput& input);
SliderVisualStyle applySliderSemanticStyles(const SliderVisualStyle& baseStyle,
                                            const AdMultiSlider::SemanticStyles& semanticStyles,
                                            bool disabled);

}  // namespace adqt::widgets::detail
