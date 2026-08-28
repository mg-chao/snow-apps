#ifndef SNOW_SHOT_SCREENSHOTCAPTUREPERFINSTRUMENTATION_H
#define SNOW_SHOT_SCREENSHOTCAPTUREPERFINSTRUMENTATION_H

#include <QtGlobal>
#include <QString>

#include <chrono>

namespace snow_shot::presentation::capture_perf {
class Sink {
  public:
    virtual ~Sink() = default;
    virtual void recordScope(const char* name, qint64 nanoseconds) = 0;
    virtual void recordMilestone(const char* name, qint64 nanoseconds) = 0;
    virtual void recordCounter(const char* name, qint64 value) = 0;
    virtual void finish(bool success) = 0;
};

void setSink(Sink* sink);
void configureTrace(const QString& path);
void beginSample(const char* scenario, qint64 width, qint64 height);
void milestone(const char* name);
void counter(const char* name, qint64 value = 1);
// Blocks until the desktop compositor has presented the next frame. Used to
// timestamp when a revealed overlay window actually reached the screen; an
// empty stub unless SNOW_SHOT_CAPTURE_PERF_INSTRUMENTATION is defined.
void flushDesktopComposition();

class Scope final {
  public:
    explicit Scope(const char* name) : m_name(name), m_started(std::chrono::steady_clock::now()) {}
    ~Scope();

  private:
    const char* m_name;
    std::chrono::steady_clock::time_point m_started;
};

void finish(bool success);
} // namespace snow_shot::presentation::capture_perf

#define SNOW_SHOT_CAPTURE_PERF_CONCAT_IMPL(a, b) a##b
#define SNOW_SHOT_CAPTURE_PERF_CONCAT(a, b) SNOW_SHOT_CAPTURE_PERF_CONCAT_IMPL(a, b)
#if defined(SNOW_SHOT_CAPTURE_PERF_INSTRUMENTATION)
#define SNOW_SHOT_CAPTURE_PERF_SCOPE(name) \
    ::snow_shot::presentation::capture_perf::Scope SNOW_SHOT_CAPTURE_PERF_CONCAT(snowShotCapturePerfScope, __LINE__)(name)
#define SNOW_SHOT_CAPTURE_PERF_BEGIN(scenario, width, height) \
    ::snow_shot::presentation::capture_perf::beginSample(scenario, width, height)
#define SNOW_SHOT_CAPTURE_PERF_MILESTONE(name) \
    ::snow_shot::presentation::capture_perf::milestone(name)
#define SNOW_SHOT_CAPTURE_PERF_COUNTER(name, value) \
    ::snow_shot::presentation::capture_perf::counter(name, value)
#define SNOW_SHOT_CAPTURE_PERF_FINISH(success) \
    ::snow_shot::presentation::capture_perf::finish(success)
#define SNOW_SHOT_CAPTURE_PERF_FLUSH_COMPOSITION() \
    ::snow_shot::presentation::capture_perf::flushDesktopComposition()
#else
#define SNOW_SHOT_CAPTURE_PERF_SCOPE(name) ((void)0)
#define SNOW_SHOT_CAPTURE_PERF_BEGIN(scenario, width, height) ((void)0)
#define SNOW_SHOT_CAPTURE_PERF_MILESTONE(name) ((void)0)
#define SNOW_SHOT_CAPTURE_PERF_COUNTER(name, value) ((void)0)
#define SNOW_SHOT_CAPTURE_PERF_FINISH(success) ((void)0)
#define SNOW_SHOT_CAPTURE_PERF_FLUSH_COMPOSITION() ((void)0)
#endif

#endif
