#include "timing_hub.h"

#include "theme/theme.h"

#include <QElapsedTimer>
#include <QHash>
#include <QThread>
#include <QTimer>

#include <algorithm>
#include <limits>
#include <queue>
#include <utility>
#include <vector>

namespace adqt::widgets::detail {

namespace {

struct ContextKey {
  const QObject* context = nullptr;
  QString key;
};

bool operator==(const ContextKey& lhs, const ContextKey& rhs) {
  return lhs.context == rhs.context && lhs.key == rhs.key;
}

size_t qHash(const ContextKey& value, size_t seed) {
  return qHashMulti(seed, value.context, value.key);
}

class TimingHub final : public QObject {
 public:
  static TimingHub& instance() {
    static TimingHub hub;
    return hub;
  }

  qint64 nowMs() const { return clock_.isValid() ? clock_.elapsed() : 0; }

  TimingConfig timingConfig() const { return timingConfig_; }

  void scheduleTask(QObject* context, const QString& key, int delayMs, DeadlineCallback callback) {
    if (!ensureUiThread() || !context || key.isEmpty() || !callback) {
      return;
    }

    registerContext(context);

    const int normalizedDelay = std::max(0, delayMs);
    const qint64 deadlineMs = nowMs() + normalizedDelay;
    const ContextKey taskKey{context, key};

    if (const auto existing = keyedTaskIds_.constFind(taskKey);
        existing != keyedTaskIds_.constEnd()) {
      taskPayloads_.remove(existing.value());
      keyedTaskIds_.erase(existing);
    }

    const quint64 taskId = ++nextTaskId_;
    TaskPayload payload;
    payload.context = context;
    payload.key = key;
    payload.callback = std::move(callback);
    taskPayloads_[taskId] = std::move(payload);

    keyedTaskIds_.insert(taskKey, taskId);
    taskHeap_.push(TaskRef{deadlineMs, ++nextTaskOrder_, taskId});
    armDeadlineTimer();
  }

  void cancelTask(QObject* context, const QString& key) {
    if (!ensureUiThread() || !context || key.isEmpty()) {
      return;
    }

    const ContextKey taskKey{context, key};
    const auto taskIt = keyedTaskIds_.find(taskKey);
    if (taskIt == keyedTaskIds_.end()) {
      return;
    }

    taskPayloads_.remove(taskIt.value());
    keyedTaskIds_.erase(taskIt);
    armDeadlineTimer();
  }

  void setFrameSubscription(QObject* context, const QString& key, bool enabled,
                            FrameCallback callback) {
    if (!ensureUiThread() || !context || key.isEmpty()) {
      return;
    }

    const ContextKey frameKey{context, key};
    if (!enabled) {
      frameCallbacks_.remove(frameKey);
      updateFrameTimerState();
      return;
    }
    if (!callback) {
      frameCallbacks_.remove(frameKey);
      updateFrameTimerState();
      return;
    }

    registerContext(context);
    frameCallbacks_[frameKey] = std::move(callback);
    updateFrameTimerState();
  }

 private:
  struct TaskPayload {
    QPointer<QObject> context;
    QString key;
    DeadlineCallback callback;
  };

  struct TaskRef {
    qint64 deadlineMs = 0;
    quint64 order = 0;
    quint64 id = 0;
  };

  struct TaskRefLater {
    bool operator()(const TaskRef& lhs, const TaskRef& rhs) const {
      if (lhs.deadlineMs != rhs.deadlineMs) {
        return lhs.deadlineMs > rhs.deadlineMs;
      }
      return lhs.order > rhs.order;
    }
  };

  TimingHub() {
    clock_.start();
    deadlineTimer_.setSingleShot(true);
    deadlineTimer_.setTimerType(Qt::PreciseTimer);
    frameTimer_.setTimerType(Qt::PreciseTimer);

    connect(&deadlineTimer_, &QTimer::timeout, this, [this]() { dispatchDueTasks(); });
    connect(&frameTimer_, &QTimer::timeout, this, [this]() { dispatchFrameCallbacks(); });
    connect(&theme::ThemeManager::instance(), &theme::ThemeManager::themeChanged, this,
            [this]() { refreshTimingConfigFromTheme(); });

    refreshTimingConfigFromTheme();
  }

  bool ensureUiThread() const {
    if (!thread() || QThread::currentThread() != thread()) {
      Q_ASSERT_X(false, "TimingHub", "TimingHub must be used from the UI thread");
      return false;
    }
    return true;
  }

  void registerContext(QObject* context) {
    if (!context || contextConnections_.contains(context)) {
      return;
    }

    contextConnections_.insert(context,
                               connect(context, &QObject::destroyed, this,
                                       [this](QObject* destroyed) { clearContext(destroyed); }));
  }

  void clearContext(QObject* context) {
    if (!context) {
      return;
    }

    const auto taskKeys = keyedTaskIds_.keys();
    for (const ContextKey& key : taskKeys) {
      if (key.context != context) {
        continue;
      }
      const auto taskIt = keyedTaskIds_.find(key);
      if (taskIt == keyedTaskIds_.end()) {
        continue;
      }
      taskPayloads_.remove(taskIt.value());
      keyedTaskIds_.erase(taskIt);
    }

    const auto frameKeys = frameCallbacks_.keys();
    for (const ContextKey& key : frameKeys) {
      if (key.context == context) {
        frameCallbacks_.remove(key);
      }
    }

    const auto connIt = contextConnections_.find(context);
    if (connIt != contextConnections_.end()) {
      disconnect(connIt.value());
      contextConnections_.erase(connIt);
    }

    updateFrameTimerState();
    armDeadlineTimer();
  }

  void refreshTimingConfigFromTheme() {
    const theme::AdThemeMotion& motion = theme::ThemeManager::instance().theme().motion;
    TimingConfig next;
    if (motion.motion) {
      next.frameIntervalMs = std::max(0, motion.timingFrameIntervalMs);
      next.spinnerCycleMs = std::max(0, motion.timingSpinnerCycleMs);
      next.waveDurationMs = std::max(0, motion.timingWaveDurationMs);
      next.menuOpenDelayMs = std::max(0, motion.timingMenuOpenDelayMs);
      next.menuCloseDelayMs = std::max(0, motion.timingMenuCloseDelayMs);
      next.loadingDelayMs = std::max(0, motion.timingLoadingDelayMs);
    } else {
      next.frameIntervalMs = 0;
      next.spinnerCycleMs = 0;
      next.waveDurationMs = 0;
      next.menuOpenDelayMs = 0;
      next.menuCloseDelayMs = 0;
      next.loadingDelayMs = 0;
    }

    timingConfig_ = next;
    updateFrameTimerState();
  }

  void updateFrameTimerState() {
    const int interval = timingConfig_.frameIntervalMs;
    if (interval <= 0 || frameCallbacks_.isEmpty()) {
      if (frameTimer_.isActive()) {
        frameTimer_.stop();
      }
      lastFrameTickMs_ = -1;
      return;
    }

    if (frameTimer_.interval() != interval) {
      frameTimer_.setInterval(interval);
      lastFrameTickMs_ = -1;
    }
    if (!frameTimer_.isActive()) {
      frameTimer_.start();
    }
  }

  void armDeadlineTimer() {
    while (!taskHeap_.empty() && !taskPayloads_.contains(taskHeap_.top().id)) {
      taskHeap_.pop();
    }

    if (taskHeap_.empty()) {
      if (deadlineTimer_.isActive()) {
        deadlineTimer_.stop();
      }
      return;
    }

    const qint64 delay = std::max<qint64>(0, taskHeap_.top().deadlineMs - nowMs());
    const int boundedDelay =
        static_cast<int>(std::min<qint64>(delay, std::numeric_limits<int>::max()));
    deadlineTimer_.start(boundedDelay);
  }

  void dispatchDueTasks() {
    if (!ensureUiThread()) {
      return;
    }

    const qint64 now = nowMs();
    std::vector<quint64> dueIds;
    while (!taskHeap_.empty() && taskHeap_.top().deadlineMs <= now) {
      dueIds.push_back(taskHeap_.top().id);
      taskHeap_.pop();
    }

    // Keep not-yet-dispatched tasks registered: an earlier callback may cancel,
    // replace, or destroy the context of another task in this same batch.
    for (quint64 id : dueIds) {
      auto payloadIt = taskPayloads_.find(id);
      if (payloadIt == taskPayloads_.end()) {
        continue;
      }
      TaskPayload payload = std::move(payloadIt.value());
      taskPayloads_.erase(payloadIt);
      const ContextKey key{payload.context.data(), payload.key};
      auto keyIt = keyedTaskIds_.find(key);
      if (keyIt == keyedTaskIds_.end() || keyIt.value() != id) {
        continue;
      }
      keyedTaskIds_.erase(keyIt);
      if (payload.context && payload.callback) {
        payload.callback();
      }
    }

    armDeadlineTimer();
  }

  void dispatchFrameCallbacks() {
    if (!ensureUiThread()) {
      return;
    }

    if (frameCallbacks_.isEmpty()) {
      updateFrameTimerState();
      return;
    }

    const qint64 now = nowMs();
    qint64 delta = 0;
    if (lastFrameTickMs_ < 0) {
      delta = timingConfig_.frameIntervalMs;
    } else {
      delta = std::max<qint64>(0, now - lastFrameTickMs_);
    }
    lastFrameTickMs_ = now;

    struct FrameEntry {
      ContextKey key;
      FrameCallback callback;
    };
    QVector<FrameEntry> snapshot;
    snapshot.reserve(frameCallbacks_.size());
    for (auto it = frameCallbacks_.cbegin(); it != frameCallbacks_.cend(); ++it) {
      snapshot.push_back(FrameEntry{it.key(), it.value()});
    }

    for (const FrameEntry& entry : snapshot) {
      const auto liveIt = frameCallbacks_.find(entry.key);
      if (liveIt == frameCallbacks_.end()) {
        continue;
      }
      if (!entry.key.context || !entry.callback) {
        frameCallbacks_.erase(liveIt);
        continue;
      }
      entry.callback(now, delta);
    }

    updateFrameTimerState();
  }

  QElapsedTimer clock_;
  TimingConfig timingConfig_;

  QTimer deadlineTimer_;
  QTimer frameTimer_;

  qint64 lastFrameTickMs_ = -1;
  quint64 nextTaskId_ = 0;
  quint64 nextTaskOrder_ = 0;

  QHash<quint64, TaskPayload> taskPayloads_;
  QHash<ContextKey, quint64> keyedTaskIds_;
  std::priority_queue<TaskRef, std::vector<TaskRef>, TaskRefLater> taskHeap_;
  QHash<ContextKey, FrameCallback> frameCallbacks_;
  QHash<const QObject*, QMetaObject::Connection> contextConnections_;
};

TimingHub& hub() { return TimingHub::instance(); }

int resolveComponentDelay(int componentDelayMs, int themeDelayMs) {
  if (componentDelayMs >= 0) {
    return componentDelayMs;
  }
  return std::max(0, themeDelayMs);
}

}  // namespace

void scheduleTimingTask(QObject* context, const QString& key, int delayMs,
                        DeadlineCallback callback) {
  hub().scheduleTask(context, key, delayMs, std::move(callback));
}

void deferTimingTask(QObject* context, const QString& key, DeadlineCallback callback) {
  hub().scheduleTask(context, key, 0, std::move(callback));
}

void cancelTimingTask(QObject* context, const QString& key) { hub().cancelTask(context, key); }

void setFrameSubscription(QObject* context, const QString& key, bool enabled,
                          FrameCallback callback) {
  hub().setFrameSubscription(context, key, enabled, std::move(callback));
}

void clearFrameSubscription(QObject* context, const QString& key) {
  hub().setFrameSubscription(context, key, false, {});
}

qint64 timingNowMs() { return hub().nowMs(); }

TimingConfig currentTimingConfig() { return hub().timingConfig(); }

int resolveLoadingDelayMs(int componentDelayMs) {
  return resolveComponentDelay(componentDelayMs, hub().timingConfig().loadingDelayMs);
}

int resolveMenuOpenDelayMs(int componentDelayMs) {
  return resolveComponentDelay(componentDelayMs, hub().timingConfig().menuOpenDelayMs);
}

int resolveMenuCloseDelayMs(int componentDelayMs) {
  return resolveComponentDelay(componentDelayMs, hub().timingConfig().menuCloseDelayMs);
}

int spinnerCycleDurationMs() { return std::max(0, hub().timingConfig().spinnerCycleMs); }

int waveDurationMs() { return std::max(0, hub().timingConfig().waveDurationMs); }

}  // namespace adqt::widgets::detail
