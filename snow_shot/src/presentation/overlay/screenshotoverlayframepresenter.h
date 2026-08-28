#ifndef SNOW_SHOT_PRESENTATION_SCREENSHOTOVERLAYFRAMEPRESENTER_H
#define SNOW_SHOT_PRESENTATION_SCREENSHOTOVERLAYFRAMEPRESENTER_H

#include <QByteArray>

class QWidget;

enum class ScreenshotOverlayRevealStrategy {
    SingleRepaint,
    PostedUpdate,
    NativeUpdate,
    NativeInvalidate,
    NativeInvalidateSuppressed,
    Legacy,
    ShowOnly,
};

struct ScreenshotOverlayRevealPlan {
    bool suppressShowPaint = false;
    bool repaint = false;
    bool sendPostedUpdate = false;
    bool nativeUpdate = false;
    bool nativeInvalidate = false;
};

class ScreenshotOverlayFramePresenter final {
  public:
    explicit ScreenshotOverlayFramePresenter(QWidget& window);

    void presentPreparedFrame();

    [[nodiscard]] ScreenshotOverlayRevealStrategy strategy() const;
    [[nodiscard]] static ScreenshotOverlayRevealStrategy
    strategyForName(const QByteArray& name, ScreenshotOverlayRevealStrategy fallback);
    [[nodiscard]] static const char* strategyName(ScreenshotOverlayRevealStrategy strategy);
    [[nodiscard]] static ScreenshotOverlayRevealPlan
    planFor(ScreenshotOverlayRevealStrategy strategy);

#if defined(SNOW_SHOT_BENCH_INTERNALS)
    void setStrategyForTesting(ScreenshotOverlayRevealStrategy strategy);
#endif

  private:
    void commitPreparedSurface(const ScreenshotOverlayRevealPlan& plan);
    void repaintSurface();
    void sendPostedUpdate();
    void updateNativeSurface(bool invalidate);

    QWidget& m_window;
    ScreenshotOverlayRevealStrategy m_strategy;
};

#endif // SNOW_SHOT_PRESENTATION_SCREENSHOTOVERLAYFRAMEPRESENTER_H
