#ifndef SNOW_SHOT_PRESENTATION_SCREENSHOTTOOLBARMAINPANEL_H
#define SNOW_SHOT_PRESENTATION_SCREENSHOTTOOLBARMAINPANEL_H

#include "icon_core.h"

#include <QFrame>
#include <QMargins>
#include <QVector>

class QBoxLayout;
class QEvent;
class QPaintEvent;
class QSpacerItem;
class QWidget;

namespace adqt::widgets {
class AdButton;
}

// Shared visual shell for the screenshot and recording toolbars.
class ScreenshotToolbarMainPanel final : public QFrame {
  public:
    struct Options {
        bool showDragHandle = false;
    };

    explicit ScreenshotToolbarMainPanel(const Options& options, QWidget* parent = nullptr);

    [[nodiscard]] QBoxLayout* contentLayout() const;
    [[nodiscard]] QWidget* dragHandle() const;
    [[nodiscard]] QWidget* trailingDragHandle() const;
    [[nodiscard]] int buttonSize() const;
    [[nodiscard]] qreal physicalScale() const;

    static QMargins shadowMargins();

    adqt::widgets::AdButton* createToolButton(const char* tooltip,
                                              const adqt::icons::IconRef& iconRef);
    adqt::widgets::AdButton* createActionButton(const char* tooltip,
                                                const adqt::icons::IconRef& iconRef,
                                                bool danger = false, bool primary = false);
    void addSpacing(int baseSpacing);
    void addSeparator();
    void resetContentLayout();
    void addTrailingDragHandle();
    void setPhysicalScale(qreal scale);

  private:
    void changeEvent(QEvent* event) override;
    void paintEvent(QPaintEvent* event) override;
    struct SpacingItem {
        QSpacerItem* item = nullptr;
        int baseSpacing = 0;
    };

    void applyMetrics();
    void updatePanelStyle();
    void updateSeparatorStyle(QFrame* separator);
    void updateDragHandle(QWidget* handle);
    void retranslateUi();

    QBoxLayout* m_layout = nullptr;
    QWidget* m_dragHandle = nullptr;
    QWidget* m_trailingDragHandle = nullptr;
    QVector<adqt::widgets::AdButton*> m_buttons;
    QVector<QFrame*> m_separatorFrames;
    QVector<SpacingItem> m_spacingItems;
    qreal m_physicalScale = 1.0;
};

#endif // SNOW_SHOT_PRESENTATION_SCREENSHOTTOOLBARMAINPANEL_H
