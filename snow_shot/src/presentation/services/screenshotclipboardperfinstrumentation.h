#ifndef SNOW_SHOT_SCREENSHOTCLIPBOARDPERFINSTRUMENTATION_H
#define SNOW_SHOT_SCREENSHOTCLIPBOARDPERFINSTRUMENTATION_H

#include <QtGlobal>

#include <chrono>

#if defined(SNOW_SHOT_CLIPBOARD_PERF_INSTRUMENTATION)
#include <atomic>
#endif

namespace snow_shot::presentation::clipboard_perf {
class Sink {
  public:
    virtual ~Sink() = default;
    virtual void recordDuration(const char* name, qint64 elapsedNanoseconds) = 0;
    virtual void recordCounter(const char* name, qint64 value) = 0;
};

#if defined(SNOW_SHOT_CLIPBOARD_PERF_INSTRUMENTATION)
inline std::atomic<Sink*> activeSink = nullptr;

inline void setSink(Sink* sink) {
    activeSink.store(sink, std::memory_order_release);
}

inline void duration(const char* name, qint64 elapsedNanoseconds) {
    if (Sink* sink = activeSink.load(std::memory_order_acquire); sink != nullptr) {
        sink->recordDuration(name, elapsedNanoseconds);
    }
}

inline void counter(const char* name, qint64 value = 1) {
    if (Sink* sink = activeSink.load(std::memory_order_acquire); sink != nullptr) {
        sink->recordCounter(name, value);
    }
}

class Stopwatch final {
  public:
    Stopwatch() : m_started(std::chrono::steady_clock::now()) {}

    [[nodiscard]] qint64 elapsedNanoseconds() const {
        return std::chrono::duration_cast<std::chrono::nanoseconds>(
                   std::chrono::steady_clock::now() - m_started)
            .count();
    }

  private:
    std::chrono::steady_clock::time_point m_started;
};

class Scope final {
  public:
    explicit Scope(const char* name) : m_name(name) {}
    ~Scope() {
        duration(m_name, m_timer.elapsedNanoseconds());
    }

  private:
    const char* m_name;
    Stopwatch m_timer;
};
#else
inline void setSink(Sink*) {}
inline void duration(const char*, qint64) {}
inline void counter(const char*, qint64 = 1) {}

class Stopwatch final {
  public:
    [[nodiscard]] qint64 elapsedNanoseconds() const {
        return 0;
    }
};

class Scope final {
  public:
    explicit Scope(const char*) {}
};
#endif
} // namespace snow_shot::presentation::clipboard_perf

#define SNOW_SHOT_CLIPBOARD_PERF_CONCAT_IMPL(a, b) a##b
#define SNOW_SHOT_CLIPBOARD_PERF_CONCAT(a, b) SNOW_SHOT_CLIPBOARD_PERF_CONCAT_IMPL(a, b)
#define SNOW_SHOT_CLIPBOARD_PERF_SCOPE(name)                                                       \
    ::snow_shot::presentation::clipboard_perf::Scope SNOW_SHOT_CLIPBOARD_PERF_CONCAT(              \
        snowShotClipboardPerfScope, __LINE__)(name)
#define SNOW_SHOT_CLIPBOARD_PERF_COUNTER(name, value)                                              \
    ::snow_shot::presentation::clipboard_perf::counter(name, value)

#endif // SNOW_SHOT_SCREENSHOTCLIPBOARDPERFINSTRUMENTATION_H
