#include "icon_registry.h"

#include <QApplication>
#include <QColor>
#include <QImage>

#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <iostream>
#include <limits>
#include <mutex>
#include <new>
#include <stdexcept>
#include <thread>
#include <type_traits>
#include <vector>

namespace {

std::atomic_bool gTrackAllocations{false};
std::atomic_size_t gAllocationCount{0};

void recordAllocation() noexcept {
  if (gTrackAllocations.load(std::memory_order_relaxed))
    gAllocationCount.fetch_add(1, std::memory_order_relaxed);
}

void require(bool condition, const char* message) {
  if (!condition) throw std::runtime_error(message);
}

using adqt::icons::IconColorModel;
using adqt::icons::IconColors;
using adqt::icons::IconDescriptor;
using adqt::icons::IconFit;
using adqt::icons::IconPack;
using adqt::icons::IconRef;
using adqt::icons::IconRenderRequest;
using adqt::icons::IconRenderer;
using adqt::icons::IconStaticColors;

constexpr IconDescriptor kEntries[] = {{
    std::string_view("cache-test"),
    std::string_view("outlined"),
    std::string_view("square"),
    std::string_view(
        R"(<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 16 16"><rect x="1" y="1" width="14" height="14" fill="__ADQT_SLOT_PRIMARY__"/></svg>)"),
    std::string_view("cache-test-square-v1"),
    IconColorModel::Monochrome,
    IconFit::Contain,
    IconStaticColors{},
    false,
}};

constexpr IconPack kPack{std::string_view("cache-test"), std::string_view("unit test"),
                         std::string_view("cache-test-pack-v1"), kEntries,
                         std::size(kEntries)};

static_assert(std::is_trivially_copyable_v<IconColors>);
static_assert(std::is_trivially_copyable_v<IconRef>);

IconRef coloredRef(const QColor& color) {
  return kPack.icon(0, IconColors::primary(color));
}

IconRenderRequest requestFor(const QSize& logicalSize, qreal dpr = 1.0) {
  IconRenderRequest request;
  request.logicalSize = logicalSize;
  request.devicePixelRatio = dpr;
  return request;
}

void defaultsAndExactByteAccounting() {
  IconRenderer renderer;
  const auto defaults = renderer.cacheStatistics();
  require(defaults.limitBytes == 2 * 1024 * 1024 && defaults.maxEntries == 512 &&
              defaults.maxRasterBytes == 256 * 1024,
          "the renderer should expose the documented production cache limits");

  const QImage image =
      renderer.renderIconImage(coloredRef(QColor(Qt::black)), requestFor(QSize(10, 7)));
  const auto populated = renderer.cacheStatistics();
  require(!image.isNull() && image.format() == QImage::Format_ARGB32_Premultiplied,
          "the renderer should produce a premultiplied ARGB raster");
  require(populated.entryCount == 1 &&
              populated.costBytes == static_cast<qint64>(image.sizeInBytes()) &&
              populated.costBytes == 10 * 7 * 4 && populated.costKB == 1,
          "cache cost should use the exact QImage byte allocation");

  static_cast<void>(
      renderer.renderIconImage(coloredRef(QColor(Qt::black)), requestFor(QSize(10, 7))));
  const auto hit = renderer.cacheStatistics();
  require(hit.hitCount == 1 && hit.missCount == 1 && hit.rasterizationCount == 1,
          "an identical request should reuse its cached raster");
}

void entryLimitAndLruOrderAreEnforced() {
  IconRenderer renderer;
  for (int index = 0; index <= IconRenderer::kDefaultMaxCacheEntries; ++index) {
    const QColor color(index & 0xff, (index >> 8) & 0xff, 73);
    static_cast<void>(renderer.renderIconImage(coloredRef(color), requestFor(QSize(1, 1))));
  }
  const auto capped = renderer.cacheStatistics();
  require(capped.entryCount == IconRenderer::kDefaultMaxCacheEntries &&
              capped.costBytes == IconRenderer::kDefaultMaxCacheEntries * 4 &&
              capped.evictionCount == 1,
          "the entry cap should evict the least-recently-used raster independently of bytes");

  renderer.clearCache();
  renderer.setCacheLimits(1024 * 1024, 2, IconRenderer::kDefaultMaxRasterBytes);
  const IconRef red = coloredRef(QColor(Qt::red));
  const IconRef green = coloredRef(QColor(Qt::green));
  const IconRef blue = coloredRef(QColor(Qt::blue));
  const IconRenderRequest request = requestFor(QSize(8, 8));
  static_cast<void>(renderer.renderIconImage(red, request));
  static_cast<void>(renderer.renderIconImage(green, request));
  static_cast<void>(renderer.renderIconImage(red, request));
  static_cast<void>(renderer.renderIconImage(blue, request));
  auto statistics = renderer.cacheStatistics();
  require(statistics.entryCount == 2 && statistics.hitCount == 1 &&
              statistics.missCount == 3 && statistics.evictionCount == 1,
          "a cache hit should promote the entry before the next LRU eviction");

  static_cast<void>(renderer.renderIconImage(red, request));
  statistics = renderer.cacheStatistics();
  require(statistics.hitCount == 2 && statistics.missCount == 3,
          "the promoted LRU entry should remain resident");
  static_cast<void>(renderer.renderIconImage(green, request));
  statistics = renderer.cacheStatistics();
  require(statistics.hitCount == 2 && statistics.missCount == 4 &&
              statistics.evictionCount == 2,
          "the untouched entry should be the one reclaimed");
}

void oversizedRastersAreNeverAllocatedToTheCache() {
  IconRenderer renderer;
  const IconRef ref = coloredRef(QColor(Qt::black));
  const QImage atThreshold =
      renderer.renderIconImage(ref, requestFor(QSize(256, 256)));
  auto statistics = renderer.cacheStatistics();
  require(!atThreshold.isNull() &&
              static_cast<qint64>(atThreshold.sizeInBytes()) == 256 * 1024 &&
              statistics.entryCount == 1 && statistics.costBytes == 256 * 1024,
          "a raster exactly at the individual limit should remain cacheable");

  renderer.clearCache();
  const QImage overThreshold =
      renderer.renderIconImage(ref, requestFor(QSize(257, 256)));
  statistics = renderer.cacheStatistics();
  require(!overThreshold.isNull() &&
              static_cast<qint64>(overThreshold.sizeInBytes()) > 256 * 1024 &&
              statistics.entryCount == 0 && statistics.costBytes == 0 &&
              statistics.rasterizationCount == 1,
          "a raster above the individual limit should bypass cache storage");

  renderer.clearCache();
  renderer.setCacheLimits(1024, 512, 256 * 1024);
  static_cast<void>(renderer.renderIconImage(ref, requestFor(QSize(32, 32))));
  statistics = renderer.cacheStatistics();
  require(statistics.entryCount == 0 && statistics.costBytes == 0 &&
              statistics.evictionCount == 0,
          "a raster larger than the total budget should not allocate a transient LRU entry");
}

void pathologicalRasterRequestsAreRejectedBeforeAllocation() {
  IconRenderer renderer;
  const QImage image = renderer.renderIconImage(
      coloredRef(QColor(Qt::black)),
      requestFor(QSize(std::numeric_limits<int>::max(), std::numeric_limits<int>::max())));
  const auto statistics = renderer.cacheStatistics();
  require(image.isNull() && statistics.entryCount == 0 && statistics.costBytes == 0 &&
              statistics.rasterizationCount == 0,
          "pathological raster dimensions should be rejected before QImage allocation");
}

void loweringRasterLimitReclaimsExistingOversizedEntries() {
  IconRenderer renderer;
  const IconRef ref = coloredRef(QColor(Qt::black));
  static_cast<void>(renderer.renderIconImage(ref, requestFor(QSize(256, 256))));
  auto statistics = renderer.cacheStatistics();
  require(statistics.entryCount == 1 && statistics.costBytes == 256 * 1024,
          "the raster-limit fixture should begin with one threshold-sized entry");

  renderer.setCacheLimits(2 * 1024 * 1024, 512, 64 * 1024);
  statistics = renderer.cacheStatistics();
  require(statistics.entryCount == 0 && statistics.costBytes == 0 &&
              statistics.evictionCount == 1,
          "lowering the per-raster limit should reclaim existing oversized entries");
}

void trimmingReportsExactReclamationAndPreservesCallers() {
  IconRenderer renderer;
  const IconRenderRequest request = requestFor(QSize(64, 64));
  const IconRef first = coloredRef(QColor(Qt::red));
  const IconRef second = coloredRef(QColor(Qt::green));
  const IconRef third = coloredRef(QColor(Qt::blue));
  static_cast<void>(renderer.renderIconImage(first, request));
  static_cast<void>(renderer.renderIconImage(second, request));
  const QImage held = renderer.renderIconImage(third, request);
  const auto before = renderer.cacheStatistics();
  require(before.entryCount == 3 && before.costBytes == 3 * 64 * 64 * 4,
          "the trim fixture should begin with three exact-size entries");

  const auto partial = renderer.trimCacheToBytes(64 * 64 * 4);
  require(partial.bytesBefore == 3 * 64 * 64 * 4 &&
              partial.bytesAfter == 64 * 64 * 4 &&
              partial.reclaimedBytes == 2 * 64 * 64 * 4 && partial.entriesBefore == 3 &&
              partial.entriesAfter == 1 && partial.generation == before.generation + 1,
          "trim reports should describe the exact whole-entry reclamation");

  const auto all = renderer.trimCacheToBytes(0);
  const auto empty = renderer.cacheStatistics();
  require(all.bytesBefore == 64 * 64 * 4 && all.bytesAfter == 0 &&
              all.reclaimedBytes == 64 * 64 * 4 && empty.entryCount == 0 &&
              empty.costBytes == 0,
          "trimming to zero should release all cache-owned bytes");
  require(!held.isNull() && held.pixelColor(held.width() / 2, held.height() / 2).alpha() > 0,
          "a caller-held image should survive eviction while leaving cache accounting");
}

void equivalentPhysicalSizesShareOneRaster() {
  IconRenderer renderer;
  const IconRef ref = coloredRef(QColor(Qt::black));
  const QImage highDpi = renderer.renderIconImage(ref, requestFor(QSize(16, 16), 2.0));
  const QImage standardDpi = renderer.renderIconImage(ref, requestFor(QSize(32, 32), 1.0));
  const auto statistics = renderer.cacheStatistics();
  require(highDpi.size() == QSize(32, 32) && standardDpi.size() == QSize(32, 32) &&
              qFuzzyCompare(highDpi.devicePixelRatio(), 2.0) &&
              qFuzzyCompare(standardDpi.devicePixelRatio(), 1.0),
          "each caller should receive the requested DPR metadata");
  require(statistics.entryCount == 1 && statistics.rasterizationCount == 1 &&
              statistics.missCount == 1 && statistics.hitCount == 1,
          "equal physical dimensions should share a single cached raster");
}

void concurrentRequestsRendezvousOnOneRasterization() {
  IconRenderer renderer;
  const IconRef ref = coloredRef(QColor(Qt::black));
  const IconRenderRequest request = requestFor(QSize(96, 96));
  constexpr int threadCount = 8;
  std::atomic_int ready{0};
  std::atomic_bool start{false};
  std::array<QImage, threadCount> images;
  std::vector<std::thread> threads;
  threads.reserve(threadCount);
  for (int index = 0; index < threadCount; ++index) {
    threads.emplace_back([&, index] {
      ready.fetch_add(1, std::memory_order_release);
      while (!start.load(std::memory_order_acquire)) std::this_thread::yield();
      images[static_cast<std::size_t>(index)] = renderer.renderIconImage(ref, request);
    });
  }
  while (ready.load(std::memory_order_acquire) != threadCount) std::this_thread::yield();
  start.store(true, std::memory_order_release);
  for (auto& thread : threads) thread.join();

  for (const QImage& image : images)
    require(!image.isNull(), "every concurrent caller should receive the rendered image");
  const auto statistics = renderer.cacheStatistics();
  require(statistics.rasterizationCount == 1 && statistics.missCount == 1 &&
              statistics.hitCount == threadCount - 1 && statistics.entryCount == 1,
          "concurrent identical requests should rendezvous on one rasterization");
}

void clearingAnActiveRenderPreventsStaleRepopulation() {
  IconRenderer renderer;
  std::mutex mutex;
  std::condition_variable enteredCondition;
  std::condition_variable releaseCondition;
  bool entered = false;
  bool release = false;
  renderer.setPaletteResolver([&] {
    std::unique_lock lock(mutex);
    entered = true;
    enteredCondition.notify_one();
    releaseCondition.wait(lock, [&] { return release; });
    return adqt::icons::IconPalette();
  });

  QImage result;
  std::thread renderThread([&] {
    result = renderer.renderIconImage(coloredRef(QColor(Qt::black)),
                                      requestFor(QSize(64, 64)));
  });
  {
    std::unique_lock lock(mutex);
    const bool started = enteredCondition.wait_for(
        lock, std::chrono::seconds(5), [&] { return entered; });
    if (!started) {
      release = true;
      lock.unlock();
      releaseCondition.notify_one();
      renderThread.join();
      require(false, "the render should enter the blocking palette resolver");
    }
  }

  renderer.clearCache();
  {
    std::lock_guard lock(mutex);
    release = true;
  }
  releaseCondition.notify_one();
  renderThread.join();

  const auto statistics = renderer.cacheStatistics();
  require(!result.isNull(), "clearing the cache should not cancel the caller's raster");
  require(statistics.entryCount == 0 && statistics.costBytes == 0 &&
              statistics.rasterizationCount == 1 && statistics.staleRenderCount == 1,
          "a render begun before clear should not repopulate the new cache generation");
  renderer.clearPaletteResolver();
}

void staticFactoriesRemainAllocationFree() {
  std::array<IconRef, 1024> references;
  gAllocationCount.store(0, std::memory_order_relaxed);
  gTrackAllocations.store(true, std::memory_order_release);
  for (IconRef& reference : references) reference = kPack.icon(0);
  gTrackAllocations.store(false, std::memory_order_release);

  require(gAllocationCount.load(std::memory_order_relaxed) == 0,
          "static descriptor factories should not allocate");
  for (const IconRef& reference : references)
    require(reference.descriptor() == &kEntries[0],
            "static factories should retain descriptor identity without owning key text");
}

}  // namespace

void* operator new(std::size_t size) {
  recordAllocation();
  if (void* memory = std::malloc(size == 0 ? 1 : size)) return memory;
  throw std::bad_alloc();
}

void* operator new[](std::size_t size) {
  recordAllocation();
  if (void* memory = std::malloc(size == 0 ? 1 : size)) return memory;
  throw std::bad_alloc();
}

void operator delete(void* memory) noexcept { std::free(memory); }
void operator delete[](void* memory) noexcept { std::free(memory); }
void operator delete(void* memory, std::size_t) noexcept { std::free(memory); }
void operator delete[](void* memory, std::size_t) noexcept { std::free(memory); }

int main(int argc, char** argv) {
  QApplication application(argc, argv);
  try {
    defaultsAndExactByteAccounting();
    entryLimitAndLruOrderAreEnforced();
    oversizedRastersAreNeverAllocatedToTheCache();
    pathologicalRasterRequestsAreRejectedBeforeAllocation();
    loweringRasterLimitReclaimsExistingOversizedEntries();
    trimmingReportsExactReclamationAndPreservesCallers();
    equivalentPhysicalSizesShareOneRaster();
    concurrentRequestsRendezvousOnOneRasterization();
    clearingAnActiveRenderPreventsStaleRepopulation();
    staticFactoriesRemainAllocationFree();
    return 0;
  } catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
