#pragma once

class QObject;
class QString;
class QWidget;

namespace adqt::widgets::detail {

inline constexpr char kTooltipManagerProperty[] = "adqt.tooltip.manager";
// Opt in when a popup trigger also performs an action whose tooltip remains useful
// while its popup is open. Ordinary editor triggers keep suppressing their tooltip.
inline constexpr char kPopupTriggerTooltipEnabledProperty[] = "adqt.tooltip.popupTriggerEnabled";

void installQtTooltipBridge();

void showQtTooltip(QWidget* target, const QString& text, int displayTimeMs = -1);

void syncTopLevelPopupTooltipRoute(QObject* owner, QWidget* triggerRoot, QWidget* popupSurface,
                                   bool active);

}  // namespace adqt::widgets::detail
