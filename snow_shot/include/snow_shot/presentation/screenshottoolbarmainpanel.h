#ifndef SNOW_SHOT_PRESENTATION_SCREENSHOTTOOLBARMAINPANEL_H
#define SNOW_SHOT_PRESENTATION_SCREENSHOTTOOLBARMAINPANEL_H

#include "icon_core.h"

#include "widgets/control_scale.h"

#include <QFrame>
#include <QMargins>
#include <QPainterPath>
#include <QVector>

class QBoxLayout;
class QEvent;
class QPaintEvent;
class QSpacerItem;
class QWidget;

namespace adqt::widgets {
class AdButton;
}

// Surface, shadow and separators shared by main and secondary toolbar rows.
class ScreenshotToolbarPanel : public QFrame {
  public:
    explicit ScreenshotToolbarPanel(QWidget* parent = nullptr);
    void setPanelScale(qreal scale);
    [[nodiscard]] QPainterPath surfacePath() const;
    static QString separatorStyleSheet();

  protected:
    void paintEvent(QPaintEvent* event) override;

  private:
    qreal m_panelRadius = 8.0;
    qreal m_panelScale = 0.0;
};

// Shared visual shell for the screenshot and recording toolbars.
class ScreenshotToolbarMainPanel final : public ScreenshotToolbarPanel,
                                         public adqt::widgets::AdControlScaleParticipant {
  public:
    struct Options {
        bool showDragHandle = false;
    };

    explicit ScreenshotToolbarMainPanel(const Options& options, QWidget* parent = nullptr);

    [[nodiscard]] QBoxLayout* contentLayout() const;
    [[nodiscard]] QWidget* dragHandle() const;
    [[nodiscard]] QWidget* trailingDragHandle() const;
    [[nodiscard]] int buttonSize() const;
    [[nodiscard]] QSize sizeHint() const override;

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
    void prepareControlScale(const adqt::widgets::AdControlScaleContext&) override {}
    void commitControlScale(const adqt::widgets::AdControlScaleContext& context) override;

  private:
    void changeEvent(QEvent* event) override;
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
    mutable QSize m_referenceSizeHint;
    qreal m_physicalScale = 1.0;
};

#endif // SNOW_SHOT_PRESENTATION_SCREENSHOTTOOLBARMAINPANEL_H
