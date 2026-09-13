#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <new>
#include <thread>

namespace {
std::atomic_bool workerPaused{false};
std::atomic_bool releaseWorker{false};
thread_local bool failAllocation = false;

void beforeNotify() {
    workerPaused.store(true);
    while (!releaseWorker.load()) {
        std::this_thread::yield();
    }
}

void require(bool condition, const char* message) {
    if (!condition) {
        std::fprintf(stderr, "%s\n", message);
        std::exit(1);
    }
}
} // namespace

// Keep allocation injection isolated from the application and other test executables.
void* operator new(std::size_t size) {
    if (failAllocation) {
        throw std::bad_alloc();
    }
    if (void* pointer = std::malloc(size == 0 ? 1 : size)) {
        return pointer;
    }
    throw std::bad_alloc();
}
void operator delete(void* pointer) noexcept {
    std::free(pointer);
}
void operator delete(void* pointer, std::size_t) noexcept {
    std::free(pointer);
}

// Exercise the actual private dispatch and cache implementation, without source snapshots.
#define SNOW_CANVAS_FILTER_TEST_BEFORE_NOTIFY() beforeNotify()
#include "../src/rendering/snow_canvas_filter_render.cpp"
#undef SNOW_CANVAS_FILTER_TEST_BEFORE_NOTIFY
#include "../src/rendering/snow_canvas_filter_tile_cache.cpp"

namespace {
void completionProtectsBarrierUntilNotification() {
    using namespace snow_canvas_filter_render;
    FilterWorkerPool pool;
    FilterWorkerPool::Barrier barrier;
    barrier.remaining = 1;
    pool.submit({nullptr, [](void*, int, int) {}, 0, 1, &barrier});
    while (!workerPaused.load()) {
        std::this_thread::yield();
    }
    // The worker is paused at its last barrier access. A caller must not be able
    // to acquire the completion mutex and destroy the barrier at this point.
    const bool callerCanAcquire = barrier.mutex.try_lock();
    if (callerCanAcquire) {
        barrier.mutex.unlock();
    }
    releaseWorker.store(true);
    pool.wait(barrier);
    require(!callerCanAcquire, "completion must hold the barrier mutex through notification");
}

void cacheHitsPreserveLruWithoutAllocating() {
    using namespace snow_canvas_filter_tile_cache;
    int namespaceToken = 0;
    Key first{};
    first.canvasNamespace = &namespaceToken;
    Key second = first;
    second.tile = QPoint(1, 0);
    QImage image(16, 16, QImage::Format_ARGB32_Premultiplied);
    image.fill(Qt::black);
    require(store(first, image, image.rect()) && store(second, image, image.rect()),
            "cache setup must succeed");
    const auto retained = find(first);
    require(retained && find(second), "cache entries must be readable before injection");
    const auto bytes = retainedBytes();
    bool threw = false;
    bool sameEntry = false;
    failAllocation = true;
    try {
        sameEntry = find(first) == retained && find(first) == retained;
    } catch (const std::bad_alloc&) {
        threw = true;
    }
    failAllocation = false;
    require(!threw, "cache hits must not allocate or throw under allocation failure");
    require(sameEntry, "cache hits must retain the original entry");
    require(cache().entries.size() == 2 && cache().lru.size() == 2,
            "each cached entry must retain one live LRU node");
    require(cache().lru.front() == first && cache().lru.back() == second,
            "cache hits must promote both old and already-most-recent entries correctly");
    require(retainedBytes() == bytes && find(second) && find(first),
            "lookups and byte accounting must survive allocation recovery");
    invalidateNamespace(&namespaceToken);
    require(!find(first) && !find(second) && retainedBytes() == 0,
            "invalidation after cache hits must safely remove all retained nodes");
    require(!retained->image.isNull(), "existing readers must survive invalidation");
}
struct KernelError final : std::exception {};

constexpr int kExceptionRowCount = 1000;
constexpr int kExceptionWidth = 1000;

int exceptionJobCount(const snow_canvas_filter_render::FilterWorkerPool& pool) {
    using namespace snow_canvas_filter_render;
    constexpr std::size_t pixels =
        static_cast<std::size_t>(kExceptionRowCount) * static_cast<std::size_t>(kExceptionWidth);
    const int usefulJobs =
        static_cast<int>((pixels + kTargetPixelsPerJob - 1) / kTargetPixelsPerJob);
    return std::min<int>(kExceptionRowCount,
                         std::min(usefulJobs, static_cast<int>(pool.workerCount()) + 1));
}

void callerFailureJoinsSubmittedJobsBeforePropagating() {
    using namespace snow_canvas_filter_render;
    // Neutralize the beforeNotify() parking hook: these modes drive many jobs
    // through the barrier and only the barrier mode coordinates a pause.
    releaseWorker.store(true);
    FilterWorkerPool pool;
    const int jobCount = exceptionJobCount(pool);
    require(jobCount >= 2, "exception tests need a parallel job split");
    const int rowsPerJob = (kExceptionRowCount + jobCount - 1) / jobCount;
    const int callerBegin = (jobCount - 1) * rowsPerJob;
    std::atomic<int> completedRanges{0};
    bool threw = false;
    try {
        static_cast<void>(
            parallelRows(kExceptionRowCount, kExceptionWidth, false, [&](int begin, int) {
                if (begin == callerBegin) {
                    throw KernelError{};
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(5));
                ++completedRanges;
            }));
    } catch (const KernelError&) {
        threw = true;
    }
    require(threw, "the caller chunk's failure must propagate to the caller");
    require(completedRanges.load() == jobCount - 1,
            "parallelRows must join every submitted job before propagating a caller failure");
}

void workerFailurePropagatesAfterRemainingJobsFinish() {
    using namespace snow_canvas_filter_render;
    releaseWorker.store(true);
    FilterWorkerPool pool;
    const int jobCount = exceptionJobCount(pool);
    require(jobCount >= 2, "exception tests need a parallel job split");
    std::atomic<int> completedRanges{0};
    bool threw = false;
    try {
        static_cast<void>(
            parallelRows(kExceptionRowCount, kExceptionWidth, false, [&](int begin, int) {
                if (begin == 0) {
                    throw KernelError{};
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(5));
                ++completedRanges;
            }));
    } catch (const KernelError&) {
        threw = true;
    }
    require(threw, "a worker kernel failure must propagate to the joining caller");
    require(completedRanges.load() == jobCount - 1,
            "non-failing ranges, including the caller chunk, must complete when a worker fails");
}
} // namespace

int main(int argc, char** argv) {
    require(argc == 2, "expected barrier, cache, caller-failure, or worker-failure test mode");
    if (std::strcmp(argv[1], "barrier") == 0) {
        completionProtectsBarrierUntilNotification();
    } else if (std::strcmp(argv[1], "cache") == 0) {
        cacheHitsPreserveLruWithoutAllocating();
    } else if (std::strcmp(argv[1], "caller-failure") == 0) {
        callerFailureJoinsSubmittedJobsBeforePropagating();
    } else if (std::strcmp(argv[1], "worker-failure") == 0) {
        workerFailurePropagatesAfterRemainingJobsFinish();
    } else {
        require(false, "unknown test mode");
    }
    return 0;
}
