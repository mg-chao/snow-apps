#ifndef SNOW_SHOT_PRESENTATION_COMPONENTS_SETTINGSPAGEUTILS_H
#define SNOW_SHOT_PRESENTATION_COMPONENTS_SETTINGSPAGEUTILS_H

#include "snow_shot/presentation/styles/themecolorscheme.h"

#include "widgets/scroll_area.h"

#include <QFont>
#include <QHBoxLayout>
#include <QLabel>
#include <QPalette>
#include <QSizePolicy>
#include <QVBoxLayout>
#include <QWidget>

namespace snow_shot::presentation::components {

inline void configureSettingsScrollArea(adqt::widgets::AdScrollArea* scrollArea,
                                        const styles::ThemeAliasMetricToken& metric) {
    scrollArea->setScrollBarThickness(metric.scrollbarThickness);
    scrollArea->setAutoFillBackground(false);
    if (adqt::widgets::AdScrollBar* scrollBar = scrollArea->overlayVerticalScrollBar();
        scrollBar != nullptr) {
        scrollBar->setOverlayMargins(QMargins(metric.scrollbarMargin, metric.scrollbarMargin,
                                              metric.scrollbarMargin,
                                              metric.scrollbarMargin));
    }

    QWidget* viewport = scrollArea->viewport();
    QPalette viewportPalette = viewport->palette();
    viewportPalette.setColor(QPalette::Window, Qt::transparent);
    viewport->setPalette(viewportPalette);
    viewport->setAutoFillBackground(false);
}

inline int settingsControlWidth(const styles::ThemeAliasMetricToken&) {
    return 230;
}

inline QWidget* createSettingItemRow(QWidget* parent,
                                     const styles::ThemeAliasMetricToken& metric,
                                     QLabel** title, QLabel** description, QWidget* control,
                                     const QString& objectName) {
    auto* row = new QWidget(parent);
    row->setObjectName(objectName);
    row->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);

    auto* rowLayout = new QHBoxLayout(row);
    rowLayout->setContentsMargins(0, 0, 0, 0);
    rowLayout->setSpacing(metric.marginLG);

    auto* copy = new QWidget(row);
    copy->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
    auto* copyLayout = new QVBoxLayout(copy);
    copyLayout->setContentsMargins(0, 0, 0, 0);
    copyLayout->setSpacing(metric.marginXXS);

    *title = new QLabel(copy);
    (*title)->setWordWrap(true);
    (*title)->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
    copyLayout->addWidget(*title);

    *description = new QLabel(copy);
    (*description)->setWordWrap(true);
    (*description)->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
    copyLayout->addWidget(*description);
    rowLayout->addWidget(copy, 1, Qt::AlignVCenter);

    control->setParent(row);
    control->setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Fixed);
    rowLayout->addWidget(control, 0, Qt::AlignVCenter);
    return row;
}

inline void applyCategoryTitleTheme(QLabel* label, const styles::ThemeColorScheme& scheme) {
    QPalette palette = label->palette();
    palette.setColor(QPalette::WindowText, scheme.map.colorText);
    label->setPalette(palette);

    QFont font = label->font();
    font.setPixelSize(scheme.metricAlias.fontSizeXL);
    font.setWeight(QFont::DemiBold);
    label->setFont(font);
}

inline void applySettingItemTheme(QLabel* title, QLabel* description,
                                  const styles::ThemeColorScheme& scheme) {
    QPalette titlePalette = title->palette();
    titlePalette.setColor(QPalette::WindowText, scheme.map.colorText);
    title->setPalette(titlePalette);

    QFont titleFont = title->font();
    titleFont.setPixelSize(scheme.metricAlias.fontSizeLG);
    titleFont.setWeight(QFont::Medium);
    title->setFont(titleFont);

    QPalette descriptionPalette = description->palette();
    descriptionPalette.setColor(QPalette::WindowText, scheme.map.colorTextSecondary);
    description->setPalette(descriptionPalette);

    QFont descriptionFont = description->font();
    descriptionFont.setPixelSize(scheme.metricAlias.fontSize);
    descriptionFont.setWeight(QFont::Normal);
    description->setFont(descriptionFont);
}

} // namespace snow_shot::presentation::components

#endif // SNOW_SHOT_PRESENTATION_COMPONENTS_SETTINGSPAGEUTILS_H
