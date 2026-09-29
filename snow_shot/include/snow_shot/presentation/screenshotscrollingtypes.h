#ifndef SNOW_SHOT_PRESENTATION_SCREENSHOTSCROLLINGTYPES_H
#define SNOW_SHOT_PRESENTATION_SCREENSHOTSCROLLINGTYPES_H

inline constexpr int kScreenshotScrollingAutoScrollIntervalDefault = 200;
inline constexpr int kScreenshotScrollingAutoScrollIntervalMinimum = 128;
inline constexpr int kScreenshotScrollingAutoScrollIntervalMaximum = 1000;

enum class ScreenshotScrollingRecognitionMode {
    Vertical,
    Horizontal,
};

enum class ScreenshotScrollingStitchChange {
    Initial,
    AppendedDown,
    PrependedUp,
    AppendedRight,
    PrependedLeft,
    Replaced,
};

struct ScreenshotScrollingTrimRange {
    int top = 0;
    int bottom = 0;

    [[nodiscard]] bool isValid() const {
        return bottom > top;
    }
};

#endif // SNOW_SHOT_PRESENTATION_SCREENSHOTSCROLLINGTYPES_H
