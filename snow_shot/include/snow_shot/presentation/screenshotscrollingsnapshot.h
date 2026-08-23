#ifndef SNOW_SHOT_PRESENTATION_SCREENSHOTSCROLLINGSNAPSHOT_H
#define SNOW_SHOT_PRESENTATION_SCREENSHOTSCROLLINGSNAPSHOT_H

#include "snow_shot/presentation/screenshotimagerowsource.h"

#include <QImage>
#include <QSize>

#include <functional>
#include <memory>

class ScreenshotScrollingSnapshot final {
  public:
    ScreenshotScrollingSnapshot() = default;

    [[nodiscard]] static ScreenshotScrollingSnapshot adoptNative(void* snapshot, QSize size);
    [[nodiscard]] bool isValid() const;
    [[nodiscard]] QSize size() const;
    [[nodiscard]] ScreenshotImageRowSource
    rowSource(std::function<bool()> cancellationRequested = {}) const;
    [[nodiscard]] QImage materialize() const;

  private:
    std::shared_ptr<void> m_snapshot;
    QSize m_size;
};

#endif // SNOW_SHOT_PRESENTATION_SCREENSHOTSCROLLINGSNAPSHOT_H
