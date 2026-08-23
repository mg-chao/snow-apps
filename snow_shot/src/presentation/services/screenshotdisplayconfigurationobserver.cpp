#include "snow_shot/presentation/screenshotdisplayconfigurationobserver.h"

#include <QGuiApplication>
#include <QRect>
#include <QScreen>

#include <utility>

ScreenshotDisplayConfigurationObserver::ScreenshotDisplayConfigurationObserver(
    ChangeHandler onChanged, QObject* parent)
    : QObject(parent), m_onChanged(std::move(onChanged)) {}

void ScreenshotDisplayConfigurationObserver::connectApplicationSignals(
    QGuiApplication* application) {
    if (application == nullptr) {
        return;
    }

    QObject::connect(application, &QGuiApplication::screenAdded, this,
                     [this](QScreen* screen) { handleScreenAdded(screen); });
    QObject::connect(application, &QGuiApplication::screenRemoved, this,
                     [this](QScreen* screen) { handleScreenRemoved(screen); });
    QObject::connect(application, &QGuiApplication::applicationStateChanged, this,
                     [this](Qt::ApplicationState state) { handleApplicationStateChanged(state); });
}

void ScreenshotDisplayConfigurationObserver::observeCurrentScreens() {
    observeScreens(QGuiApplication::screens());
}

void ScreenshotDisplayConfigurationObserver::observeScreens(const QList<QScreen*>& screens) {
    for (QScreen* screen : screens) {
        observeScreen(screen);
    }
}

void ScreenshotDisplayConfigurationObserver::observeScreen(QScreen* screen) {
    if (screen == nullptr || m_observedScreens.contains(screen)) {
        return;
    }

    m_observedScreens.insert(screen);
    QObject::connect(screen, &QObject::destroyed, this,
                     [this, screen]() { m_observedScreens.remove(screen); });
    QObject::connect(screen, &QScreen::geometryChanged, this,
                     [this](const QRect&) { handleObservedScreenChanged(); });
    QObject::connect(screen, &QScreen::virtualGeometryChanged, this,
                     [this](const QRect&) { handleObservedScreenChanged(); });
    QObject::connect(screen, &QScreen::logicalDotsPerInchChanged, this,
                     [this](qreal) { handleObservedScreenChanged(); });
    QObject::connect(screen, &QScreen::physicalDotsPerInchChanged, this,
                     [this](qreal) { handleObservedScreenChanged(); });
}

int ScreenshotDisplayConfigurationObserver::observedScreenCount() const {
    return static_cast<int>(m_observedScreens.size());
}

void ScreenshotDisplayConfigurationObserver::handleScreenAdded(QScreen* screen) {
    notifyChanged();
    observeScreen(screen);
}

void ScreenshotDisplayConfigurationObserver::handleScreenRemoved(QScreen* screen) {
    m_observedScreens.remove(screen);
    notifyChanged();
}

void ScreenshotDisplayConfigurationObserver::handleApplicationStateChanged(
    Qt::ApplicationState state) {
    if (state == Qt::ApplicationHidden || state == Qt::ApplicationSuspended) {
        notifyChanged();
    }
}

void ScreenshotDisplayConfigurationObserver::handleObservedScreenChanged() {
    notifyChanged();
}

void ScreenshotDisplayConfigurationObserver::notifyChanged() {
    if (m_onChanged) {
        m_onChanged();
    }
}
