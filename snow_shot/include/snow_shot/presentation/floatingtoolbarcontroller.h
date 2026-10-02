#ifndef SNOW_SHOT_PRESENTATION_FLOATINGTOOLBARCONTROLLER_H
#define SNOW_SHOT_PRESENTATION_FLOATINGTOOLBARCONTROLLER_H

#include "snow_shot/presentation/screenshotclipboardcontent.h"
#include <QObject>
#include <functional>
#include <memory>

class QScreen;

namespace snow_shot::presentation {
class FloatingToolbarController final : public QObject {
    Q_OBJECT
  public:
    using FullscreenDetector = std::function<bool(QScreen*)>;
    explicit FloatingToolbarController(QObject* parent = nullptr, FullscreenDetector detector = {});
    ~FloatingToolbarController() override;
    void refreshConfiguration();
    // Each producer owns a stable key. Restoring is deferred so capture-to-recording
    // transitions and overlapping acquisitions cannot briefly show the desktop surface.
    void setCaptureActive(const QString& source, bool active);
    void shutdown();

  signals:
    void actionRequested(const QString& action);
    void customizeRequested();
    void contentDropped(ScreenshotClipboardContentSnapshot snapshot, QStringList paths);

  protected:
    bool eventFilter(QObject* watched, QEvent* event) override;

  private:
    class Impl;
    std::unique_ptr<Impl> m_impl;
};
} // namespace snow_shot::presentation
#endif
