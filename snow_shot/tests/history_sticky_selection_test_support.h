#pragma once

#include "snow_shot/presentation/styles/thememanager.h"
#include "theme/theme_manager.h"
#include "widgets/button.h"
#include "widgets/checkbox.h"
#include "widgets/scroll_area.h"

#include <QApplication>
#include <QDir>
#include <QEvent>
#include <QImage>
#include <QScrollBar>

namespace history_sticky_test {

inline void settleLayout() {
    for (int index = 0; index < 4; ++index) {
        QCoreApplication::sendPostedEvents(nullptr, QEvent::LayoutRequest);
        QCoreApplication::processEvents();
    }
}

template <typename Require>
void verifySelection(QWidget& page, const QString& prefix, adqt::widgets::AdCheckbox* checkbox,
                     Require require) {
    auto* area = page.findChild<adqt::widgets::AdScrollArea*>();
    auto* bar = page.findChild<QWidget*>(prefix + QStringLiteral("SelectionBar"));
    auto* panel = page.findChild<QWidget*>(prefix + QStringLiteral("SelectionPanel"));
    auto* entries = page.findChild<QWidget*>(prefix + QStringLiteral("Entries"));
    auto* deselect =
        page.findChild<adqt::widgets::AdButton*>(prefix + QStringLiteral("DeselectAll"));
    require(area && bar && panel && entries && checkbox && deselect,
            "sticky selection fixture exposes the page controls");
    auto* viewport = area->viewport();
    auto* scroll = area->verticalScrollBar();
    const int topMargin = snow_shot::presentation::styles::ThemeManager::instance()
                              .themeColorScheme()
                              .metricAlias.marginSM;
    const auto saveReview = [&](const QString& suffix) {
        const QString directory = qEnvironmentVariable("SNOW_STICKY_SELECTION_REVIEW_DIR");
        if (!directory.isEmpty()) {
            require(QDir().mkpath(directory) &&
                        page.grab().save(QDir(directory).filePath(prefix + suffix + ".png")),
                    "save sticky selection review");
        }
    };
    require(!bar->isVisible() && scroll->maximum() > 500,
            "a scrollable page starts without a selection panel");
    const int entriesWithoutSelection = entries->y();
    checkbox->setChecked(true);
    settleLayout();
    const int normalTop = bar->mapTo(viewport, QPoint()).y();
    const int contentEntryTop = entries->y();
    const int maximum = scroll->maximum();
    const int wideHeight = bar->height();
    require(bar->isVisible() && topMargin > 0 && normalTop > topMargin &&
                contentEntryTop - entriesWithoutSelection == bar->height(),
            "selection reserves exactly one panel height at its normal position");

    scroll->setValue(normalTop / 2);
    settleLayout();
    require(bar->mapTo(viewport, QPoint()).y() == normalTop - scroll->value(),
            "the selection panel scrolls normally before reaching the top");
    scroll->setValue(normalTop + 80);
    settleLayout();
    require(bar->mapTo(viewport, QPoint()).y() == topMargin && entries->y() == contentEntryTop &&
                scroll->maximum() == maximum,
            "the panel preserves its top margin without changing content layout or scroll range");
    const QPoint hitPoint = deselect->mapTo(viewport, deselect->rect().center());
    QWidget* hit = viewport->childAt(hitPoint);
    require(hit && (hit == deselect || deselect->isAncestorOf(hit)),
            "sticky actions receive pointer input above the scrolling records");
    deselect->setFocus();
    scroll->setValue(normalTop + 180);
    settleLayout();
    require(deselect->hasFocus() && bar->mapTo(viewport, QPoint()).y() == topMargin,
            "scrolling preserves focus on the original selection controls");

    adqt::theme::ThemeOverride transparent;
    transparent.backgroundOpacity = 0.0;
    auto& themes = adqt::theme::ThemeManager::instance();
    themes.setScopeOverride(&page, transparent);
    settleLayout();
    QImage panelImage(panel->size(), QImage::Format_ARGB32_Premultiplied);
    panelImage.fill(Qt::transparent);
    panel->render(&panelImage, QPoint(), QRegion(), QWidget::DrawChildren);
    require(panelImage.pixelColor(panel->width() / 2, 3).alpha() == 255,
            "a sticky panel blocks underlying records even with a transparent skin");
    themes.clearScopeOverride(&page);
    settleLayout();
    saveReview(QStringLiteral("-wide"));

    page.resize(600, 640);
    settleLayout();
    require(bar->mapTo(viewport, QPoint()).y() == topMargin && bar->width() == entries->width() &&
                bar->height() > wideHeight && deselect->mapTo(panel, QPoint()).y() >= 0 &&
                deselect->mapTo(panel, deselect->rect().bottomRight()).y() < panel->height(),
            "a sticky panel follows narrow layout width and reserves its wrapped height");
    saveReview(QStringLiteral("-narrow"));
    page.resize(980, 640);
    settleLayout();
    require(bar->height() == wideHeight && bar->width() == entries->width(),
            "widening restores the original sticky panel size");
    page.hide();
    page.show();
    settleLayout();
    require(bar->isVisible() && bar->mapTo(viewport, QPoint()).y() == topMargin,
            "page revisits preserve the sticky selection");

    scroll->setValue(scroll->maximum());
    settleLayout();
    require(bar->mapTo(viewport, QPoint()).y() == topMargin,
            "selection actions remain available at the end of the page");
    scroll->setValue(0);
    settleLayout();
    require(bar->mapTo(viewport, QPoint()).y() == normalTop && entries->y() == contentEntryTop,
            "scrolling back restores the normal panel position without layout drift");
    scroll->setValue(normalTop + 80);
    settleLayout();
    deselect->click();
    settleLayout();
    require(!bar->isVisible() && !checkbox->isChecked(),
            "clearing a sticky selection hides the panel and unchecks the entry");
    require(entries->y() == entriesWithoutSelection,
            "clearing a sticky selection removes its reserved space");
    // Hiding the focused action can scroll Qt's next focus target into view.
    // Start the next selection explicitly below the sticky threshold.
    scroll->setValue(normalTop + 80);
    checkbox->setChecked(true);
    settleLayout();
    require(bar->isVisible() && bar->mapTo(viewport, QPoint()).y() == topMargin &&
                entries->y() == contentEntryTop,
            "selecting while already scrolled immediately restores the sticky panel");
}

} // namespace history_sticky_test
