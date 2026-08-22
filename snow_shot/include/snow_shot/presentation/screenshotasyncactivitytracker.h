#ifndef SNOW_SHOT_PRESENTATION_SCREENSHOTASYNCACTIVITYTRACKER_H
#define SNOW_SHOT_PRESENTATION_SCREENSHOTASYNCACTIVITYTRACKER_H

#include <QMetaObject>
#include <QMutex>
#include <QMutexLocker>
#include <QObject>
#include <QPointer>

#include <algorithm>
#include <atomic>
#include <functional>
#include <memory>
#include <utility>
#include <vector>

class ScreenshotAsyncActivityTracker;

class ScreenshotAsyncActivityLease final {
  public:
    ScreenshotAsyncActivityLease() = default;

    [[nodiscard]] bool isValid() const noexcept {
        return m_token != nullptr;
    }

    void reset() noexcept {
        m_token.reset();
    }

  private:
    friend class ScreenshotAsyncActivityTracker;
    explicit ScreenshotAsyncActivityLease(std::shared_ptr<void> token)
        : m_token(std::move(token)) {}

    std::shared_ptr<void> m_token;
};

// Tracks asynchronous allocations through their true terminal callback, not merely until a
// cancellation request or worker return. The process-wide idle observer lets memory reclamation
// run after the final result payload and callback captures have been released.
class ScreenshotAsyncActivityTracker final {
  public:
    using IdleCallback = std::function<void()>;

    [[nodiscard]] static ScreenshotAsyncActivityTracker& shared() {
        // Activity leases may be released during application teardown. Keeping this small tracker
        // alive until process exit avoids a cross-thread singleton destruction race.
        static auto* tracker = new ScreenshotAsyncActivityTracker();
        return *tracker;
    }

    [[nodiscard]] ScreenshotAsyncActivityLease acquire() {
        m_activeCount.fetch_add(1, std::memory_order_acq_rel);
        return ScreenshotAsyncActivityLease(std::make_shared<ActivityToken>(*this));
    }

    [[nodiscard]] int activeActivityCount() const noexcept {
        return m_activeCount.load(std::memory_order_acquire);
    }

    void observeIdle(QObject* receiver, IdleCallback callback) {
        if (receiver == nullptr || !callback) {
            return;
        }
        QMutexLocker lock(&m_observerMutex);
        m_observers.erase(
            std::remove_if(m_observers.begin(), m_observers.end(),
                           [](const Observer& observer) { return observer.receiver.isNull(); }),
            m_observers.end());
        m_observers.push_back(Observer{QPointer<QObject>(receiver), std::move(callback)});
    }

  private:
    struct ActivityToken final {
        explicit ActivityToken(ScreenshotAsyncActivityTracker& tracker) : owner(tracker) {}
        ~ActivityToken() {
            owner.release();
        }

        ScreenshotAsyncActivityTracker& owner;
    };

    struct Observer final {
        QPointer<QObject> receiver;
        IdleCallback callback;
    };

    ScreenshotAsyncActivityTracker() = default;

    void release() {
        const int previous = m_activeCount.fetch_sub(1, std::memory_order_acq_rel);
        if (previous != 1) {
            return;
        }

        std::vector<Observer> observers;
        {
            QMutexLocker lock(&m_observerMutex);
            m_observers.erase(
                std::remove_if(m_observers.begin(), m_observers.end(),
                               [](const Observer& observer) { return observer.receiver.isNull(); }),
                m_observers.end());
            observers = m_observers;
        }
        for (const Observer& observer : observers) {
            const QPointer<QObject> receiver = observer.receiver;
            if (receiver.isNull()) {
                continue;
            }
            static_cast<void>(QMetaObject::invokeMethod(
                receiver.data(),
                [receiver, callback = observer.callback]() {
                    if (!receiver.isNull() && callback) {
                        callback();
                    }
                },
                Qt::QueuedConnection));
        }
    }

    std::atomic_int m_activeCount{0};
    QMutex m_observerMutex;
    std::vector<Observer> m_observers;
};

#endif // SNOW_SHOT_PRESENTATION_SCREENSHOTASYNCACTIVITYTRACKER_H
