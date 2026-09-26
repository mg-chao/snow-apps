#pragma once

#include "snow_draw_engine_qt/snow_canvas_types.h"

#include <QWidget>
#include <functional>
#include <memory>

class QScreen;
class SnowCanvasWidget;
class ScreenshotToolbarMainPanel;

namespace snow_shot::presentation {
class FullscreenCanvasStylePanel;
class PinnedWindowPlatform;

class FullscreenCanvasWindow final : public QWidget {
    Q_OBJECT

  public:
    using PlatformFactory = std::function<std::unique_ptr<PinnedWindowPlatform>(QWidget*)>;

    explicit FullscreenCanvasWindow(QScreen* screen, PlatformFactory platformFactory = {});
    ~FullscreenCanvasWindow() override;

    [[nodiscard]] bool present();
    [[nodiscard]] bool clickThrough() const;
    [[nodiscard]] bool setClickThrough(bool enabled);
    void activateTool(SnowCanvasTool tool);
    void activateLaser();
    [[nodiscard]] bool laserActive() const;
    void clearCanvas();
    [[nodiscard]] SnowCanvasWidget* canvas() const;
    [[nodiscard]] ScreenshotToolbarMainPanel* toolbar() const;
    [[nodiscard]] FullscreenCanvasStylePanel* stylePanel() const;
    [[nodiscard]] QWidget* recoveryButton() const;
    [[nodiscard]] QColor inputSurfaceColor() const;

  signals:
    void closed();
    void operationFailed(const QString& message, bool warning);

  protected:
    bool eventFilter(QObject* watched, QEvent* event) override;
    bool nativeEvent(const QByteArray& eventType, void* message, qintptr* result) override;
    void resizeEvent(QResizeEvent* event) override;
    void changeEvent(QEvent* event) override;
    void closeEvent(QCloseEvent* event) override;

  private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;
};
} // namespace snow_shot::presentation
