#ifndef SCREENSHOTPINNEDSHAKEGESTURE_H
#define SCREENSHOTPINNEDSHAKEGESTURE_H

#include "screenshotpinnedshakedetector.h"

#include <QElapsedTimer>
#include <QPointer>
#include <QScreen>

struct ScreenshotPinnedShakeGesture final {
    ScreenshotPinnedShakeDetector detector;
    QElapsedTimer clock;
    QPointer<QScreen> display;
    QPointF pointer;
    QPointF logicalPosition;
    qreal scale = 1;
    quint64 generation = 0;
    bool physical = false;
    bool active = false;
    bool pending = false;
};

#endif
