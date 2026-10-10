#ifndef SNOW_SHOT_PRESENTATION_SCREENSHOTPRESENTATIONFRAMESCHEDULER_H
#define SNOW_SHOT_PRESENTATION_SCREENSHOTPRESENTATIONFRAMESCHEDULER_H

#include <QObject>

#include <functional>
#include <memory>

// Retain the wakeup backend while scheduling absolute monotonic deadlines.
// Each deadline invokes the callback once; the callback may arm the next deadline.
class ScreenshotPresentationFrameScheduler final : public QObject {
  public:
    enum class Backend { Automatic, QtTimer };

    ScreenshotPresentationFrameScheduler(std::function<qint64()> monotonicNanoseconds,
                                         std::function<void()> wakeup, QObject* parent = nullptr);
    ScreenshotPresentationFrameScheduler(std::function<qint64()> monotonicNanoseconds,
                                         std::function<void()> wakeup, Backend backend,
                                         QObject* parent = nullptr);
    ~ScreenshotPresentationFrameScheduler() override;

    void setDeadline(qint64 deadlineNs);
    void cancelDeadline();
    void stop();
    [[nodiscard]] bool active() const;
    [[nodiscard]] QObject* wakeupObject() const;

  private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;
};

#endif // SNOW_SHOT_PRESENTATION_SCREENSHOTPRESENTATIONFRAMESCHEDULER_H
