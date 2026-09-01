#ifndef SNOW_SHOT_SCREENSHOTPINTOOSCREENPERFINSTRUMENTATION_H
#define SNOW_SHOT_SCREENSHOTPINTOOSCREENPERFINSTRUMENTATION_H

#include <QtGlobal>
#include <QString>

#include <chrono>

namespace snow_shot::presentation::pin_perf {
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
void setSampleDescriptor(const char* scenario, qint64 width, qint64 height);
void milestone(const char* name);
void counter(const char* name, qint64 value = 1);

class Scope final {
  public:
    explicit Scope(const char* name) : m_name(name), m_started(std::chrono::steady_clock::now()) {}
    ~Scope();

  private:
    const char* m_name;
    std::chrono::steady_clock::time_point m_started;
};

void finish(bool success);
} // namespace snow_shot::presentation::pin_perf

#define SNOW_SHOT_PIN_PERF_CONCAT_IMPL(a, b) a##b
#define SNOW_SHOT_PIN_PERF_CONCAT(a, b) SNOW_SHOT_PIN_PERF_CONCAT_IMPL(a, b)
#if defined(SNOW_SHOT_PIN_PERF_INSTRUMENTATION)
#define SNOW_SHOT_PIN_PERF_SCOPE(name) \
    ::snow_shot::presentation::pin_perf::Scope SNOW_SHOT_PIN_PERF_CONCAT(snowShotPinPerfScope, __LINE__)(name)
#define SNOW_SHOT_PIN_PERF_BEGIN(scenario, width, height) \
    ::snow_shot::presentation::pin_perf::beginSample(scenario, width, height)
#define SNOW_SHOT_PIN_PERF_DESCRIPTOR(scenario, width, height) \
    ::snow_shot::presentation::pin_perf::setSampleDescriptor(scenario, width, height)
#define SNOW_SHOT_PIN_PERF_MILESTONE(name) ::snow_shot::presentation::pin_perf::milestone(name)
#define SNOW_SHOT_PIN_PERF_COUNTER(name, value) ::snow_shot::presentation::pin_perf::counter(name, value)
#define SNOW_SHOT_PIN_PERF_FINISH(success) ::snow_shot::presentation::pin_perf::finish(success)
#else
// The inert forms still consume their value arguments so call sites can pass
// local variables without triggering unused-parameter diagnostics when the
// instrumentation is compiled out.
#define SNOW_SHOT_PIN_PERF_SCOPE(name) ((void)(name))
#define SNOW_SHOT_PIN_PERF_BEGIN(scenario, width, height) ((void)(scenario))
#define SNOW_SHOT_PIN_PERF_DESCRIPTOR(scenario, width, height) ((void)(scenario))
#define SNOW_SHOT_PIN_PERF_MILESTONE(name) ((void)(name))
#define SNOW_SHOT_PIN_PERF_COUNTER(name, value) ((void)(value))
#define SNOW_SHOT_PIN_PERF_FINISH(success) ((void)(success))
#endif

#endif
