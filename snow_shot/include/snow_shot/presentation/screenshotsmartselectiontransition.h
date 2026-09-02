#ifndef SNOW_SHOT_PRESENTATION_SCREENSHOTSMARTSELECTIONTRANSITION_H
#define SNOW_SHOT_PRESENTATION_SCREENSHOTSMARTSELECTIONTRANSITION_H

#include <QEasingCurve>
#include <QRectF>
#include <QVariantAnimation>

#include <functional>

class ScreenshotSmartSelectionTransition final {
  public:
    static constexpr int kDurationMs = 101;
    static constexpr QEasingCurve::Type kEasingCurve = QEasingCurve::OutQuad;

    using UpdateCallback = std::function<void(const QRectF&)>;

    explicit ScreenshotSmartSelectionTransition(UpdateCallback update);

    void setEnabled(bool enabled);
    [[nodiscard]] bool enabled() const;
    [[nodiscard]] bool update(const QRectF& selection, bool smartFraming);

    [[nodiscard]] bool isRunning() const;
    [[nodiscard]] QRectF displayedSelection() const;

  private:
    void presentDirectly(const QRectF& selection);
    void notifyUpdate();

    UpdateCallback m_update;
    QVariantAnimation m_animation;
    QRectF m_displayedSelection;
    QRectF m_targetSelection;
    bool m_hasPresentedSmartSelection = false;
    bool m_enabled = true;
};

#endif // SNOW_SHOT_PRESENTATION_SCREENSHOTSMARTSELECTIONTRANSITION_H
