#include "screenshotpinnedshakedetector.h"

#include <array>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <type_traits>

namespace {
using Detector = ScreenshotPinnedShakeDetector;

void require(bool value, const char* message) {
    if (!value)
        throw std::runtime_error(message);
}

void shortShakesAndDragCorrectionsDoNotTrigger() {
    for (const qreal amplitude : {28., 40.}) {
        Detector detector;
        detector.begin({});
        for (int leg = 1; leg <= 12; ++leg)
            require(!detector.sample({(leg % 2) ? amplitude : 0, 0}, leg * 40),
                    "short back-and-forth adjustments must not hide other windows");
    }

    Detector detector;
    detector.begin({});
    for (int leg = 1; leg <= 4; ++leg)
        require(!detector.sample({(leg % 2) ? 96. : 0, 0}, leg * 100),
                "two broad round trips must not be enough to recognize a deliberate shake");
}

void alternatingLegsRecognizeOneShake() {
    for (const QPointF axis : {QPointF(1, 0), QPointF(0, 1), QPointF(1, 1), QPointF(-1, -1)}) {
        Detector detector;
        const QPointF origin(-800, 300);
        detector.begin(origin);
        for (int leg = 1; leg <= 6; ++leg) {
            const auto position = origin + axis * ((leg % 2) ? 64 : 0);
            require(detector.sample(position, leg * 80) == (leg == 6),
                    "six alternating legs must recognize exactly one shake");
            require(!detector.sample(position, leg * 80), "duplicate samples must not retrigger");
        }
        require(!detector.sample(origin + axis * 100, 650), "continued motion remains latched");
        detector.reset();
        require(!detector.sample(origin, 700), "reset must disarm the detector");
        detector.begin(origin);
        for (int leg = 1; leg <= 6; ++leg)
            require(detector.sample(origin + axis * ((leg % 2) ? 64 : 0), leg * 80) == (leg == 6),
                    "a new drag must allow another shake");
    }
}

void distancesAndTimesHaveInclusiveBoundaries() {
    for (const qreal amplitude : {47.99, 48., 48.01}) {
        Detector detector;
        detector.begin({});
        bool recognized = false;
        for (int leg = 1; leg <= 12; ++leg)
            recognized |= detector.sample(QPointF((leg % 2) ? amplitude : 0, 0), leg * 40);
        require(recognized == (amplitude >= 48), "sub-threshold reversals must never accumulate");
    }
    for (const int last : {699, 700, 701}) {
        Detector detector;
        detector.begin({});
        for (int leg = 1; leg <= 5; ++leg)
            require(!detector.sample({(leg % 2) ? 48. : 0, 0}, leg * 100),
                    "five legs cannot complete three round trips");
        require(detector.sample({0, 0}, last) == (last <= 700), "sequence deadline is inclusive");
    }
    for (const int first : {249, 250, 251}) {
        Detector detector;
        detector.begin({});
        require(!detector.sample({48, 0}, first), "first leg is never sufficient");
        require(!detector.sample({0, 0}, first + 50), "second leg is never sufficient");
        require(!detector.sample({48, 0}, first + 100), "third leg is never sufficient");
        require(!detector.sample({0, 0}, first + 150), "fourth leg is never sufficient");
        require(!detector.sample({48, 0}, first + 200), "fifth leg is never sufficient");
        require(detector.sample({0, 0}, first + 250) == (first <= 250),
                "each leg must meet the inclusive duration limit");
    }
}

void ordinaryMovementAndJitterDoNotShake() {
    Detector straight;
    Detector jitter;
    Detector slow;
    straight.begin({});
    jitter.begin({});
    slow.begin({});
    for (int i = 1; i < 2000; ++i) {
        require(!straight.sample(QPointF(i * 3, i * 2), i * 4), "ordinary sweeping movement");
        require(!jitter.sample(QPointF(i % 5 - 2, i % 7 - 3), i * 2), "pointer jitter");
        require(!slow.sample(QPointF((i % 2) ? 80 : 0, 0), i * 300), "slow zigzags");
    }
    Detector mixedAxes;
    mixedAxes.begin({});
    const std::array<QPointF, 6> mixed{{{64, 0}, {0, 64}, {64, 0}, {0, 64}, {64, 0}, {64, 64}}};
    for (std::size_t i = 0; i < mixed.size(); ++i)
        require(!mixedAxes.sample(mixed[i], static_cast<qint64>(i + 1) * 80),
                "reversals on different axes must not add together");
}

void extremaIgnoreSmallCounterMovement() {
    Detector detector;
    detector.begin({});
    const std::array<QPointF, 13> samples{{{64, 0},
                                           {61, 0},
                                           {69, 0},
                                           {5, 0},
                                           {8, 0},
                                           {4, 0},
                                           {68, 0},
                                           {67, 0},
                                           {4, 0},
                                           {7, 0},
                                           {68, 0},
                                           {65, 0},
                                           {4, 0}}};
    for (std::size_t i = 0; i < samples.size(); ++i)
        require(detector.sample(samples[i], static_cast<qint64>(i + 1) * 40) ==
                    (i + 1 == samples.size()),
                "real turning points must absorb sub-threshold counter movement");
}

void pausesAndLongLegsDiscardStaleMovement() {
    Detector detector;
    detector.begin({});
    require(!detector.sample({64, 0}, 50), "first leg");
    require(!detector.sample({0, 0}, 100), "second leg");
    require(!detector.sample({0, 0}, 351), "a pause must reset the sequence");
    require(!detector.sample({64, 0}, 400), "fresh first leg");
    require(!detector.sample({0, 0}, 450), "fresh second leg");
    require(!detector.sample({64, 0}, 500), "fresh third leg");
    require(!detector.sample({0, 0}, 550), "fresh fourth leg");
    require(!detector.sample({64, 0}, 600), "fresh fifth leg");
    require(detector.sample({0, 0}, 650), "a complete shake after a pause must work");

    detector.begin({});
    for (int i = 1; i <= 6; ++i)
        require(!detector.sample(QPointF(i * 64, 0), i * 50), "long forward leg");
    require(!detector.sample({320, 0}, 350), "long leg cannot count toward a shake");
    require(!detector.sample({384, 0}, 400), "second new leg");
    require(!detector.sample({320, 0}, 450), "third new leg");
    require(!detector.sample({384, 0}, 500), "fourth new leg");
    require(!detector.sample({320, 0}, 550), "fifth new leg");
    require(detector.sample({384, 0}, 600), "fresh six legs after a long sweep");
}

void samplingFrequencyDoesNotChangeTheResult() {
    for (const int subdivisions : {1, 2, 4, 20, 100}) {
        Detector detector;
        detector.begin({});
        int recognized = 0;
        for (int leg = 0; leg < 6; ++leg) {
            for (int i = 1; i <= subdivisions; ++i) {
                const qreal portion = qreal(i) / subdivisions;
                const qreal x = (leg % 2) ? 64 * (1 - portion) : 64 * portion;
                recognized += detector.sample({x, 0}, leg * 80 + i * 80 / subdivisions) ? 1 : 0;
            }
        }
        require(recognized == 1, "coarse and high-frequency sampling must agree");
    }
    Detector irregular;
    irregular.begin({});
    require(!irregular.sample({64, 1}, 71), "irregular first leg");
    require(!irregular.sample({0, 2}, 166), "irregular second leg");
    require(!irregular.sample({64, 0}, 249), "irregular third leg");
    require(!irregular.sample({0, 1}, 341), "irregular fourth leg");
    require(!irregular.sample({64, 0}, 424), "irregular fifth leg");
    require(irregular.sample({0, 1}, 503), "irregular sixth leg");
}

void invalidSamplesCannotCompleteAnOldSequence() {
    for (const qreal invalid :
         {std::numeric_limits<qreal>::infinity(), std::numeric_limits<qreal>::quiet_NaN()}) {
        Detector detector;
        detector.begin({});
        require(!detector.sample({64, 0}, 50), "first leg");
        require(!detector.sample({invalid, 0}, 60), "non-finite samples disarm detection");
        require(!detector.sample({0, 0}, 100), "invalid positions cannot become reversals");
    }
    Detector backwards;
    backwards.begin({});
    require(!backwards.sample({64, 0}, 100), "first leg");
    require(!backwards.sample({0, 0}, 90), "backward time starts a new sequence");
    require(!backwards.sample({64, 0}, 150), "new first leg");
    require(!backwards.sample({0, 0}, 200), "new second leg");
    require(!backwards.sample({64, 0}, 250), "new third leg");
    require(!backwards.sample({0, 0}, 300), "new fourth leg");
    require(!backwards.sample({64, 0}, 350), "new fifth leg");
    require(backwards.sample({0, 0}, 400), "new valid sequence after a clock reset");
}
} // namespace

int main() {
    static_assert(std::is_trivially_copyable_v<Detector>);
    static_assert(sizeof(Detector) <= 128);
    try {
        shortShakesAndDragCorrectionsDoNotTrigger();
        alternatingLegsRecognizeOneShake();
        distancesAndTimesHaveInclusiveBoundaries();
        ordinaryMovementAndJitterDoNotShake();
        extremaIgnoreSmallCounterMovement();
        pausesAndLongLegsDiscardStaleMovement();
        samplingFrequencyDoesNotChangeTheResult();
        invalidSamplesCannotCompleteAnOldSequence();
        std::cout << "Pinned shake detector tests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
