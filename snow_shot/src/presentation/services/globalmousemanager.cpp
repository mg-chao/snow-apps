#include "snow_shot/presentation/globalmousemanager.h"
#include "snow_shot/storage/applicationstorage.h"

#include <QJsonObject>
#include <QCursor>
#include <QGuiApplication>
#include <QMutex>
#include <QMutexLocker>
#include <QScreen>
#include <QTimer>
#include <algorithm>
#include <array>
#include <cmath>
#include <utility>

namespace snow_shot::presentation {
struct GlobalMouseManager::Impl {
    Impl(GlobalMouseManager& owner, std::unique_ptr<GlobalMouseBackend> input)
        : q(owner), backend(input ? std::move(input) : createGlobalMouseBackend()), frameTimer(&q) {
        frameTimer.setObjectName(QStringLiteral("globalMouseFrameTimer"));
        frameTimer.setSingleShot(true);
        frameTimer.setTimerType(Qt::PreciseTimer);
        QObject::connect(&frameTimer, &QTimer::timeout, &q,
                         [this]() { deliver(generation, true); });
    }

    void reload() {
        configuration.bindings.clear();
        using Action = settings::SettingsGlobalMouseAction;
        const std::array pairs{
            std::pair{Action::ScreenshotCopy, "global_mouse/screenshot_copy"},
            std::pair{Action::ScreenshotFixed, "global_mouse/screenshot_fixed"},
            std::pair{Action::ScreenshotOcr, "global_mouse/screenshot_ocr"},
            std::pair{Action::ScreenshotTranslation, "global_mouse/screenshot_translation"},
            std::pair{Action::ScreenshotSave, "global_mouse/screenshot_save"},
            std::pair{Action::ScreenshotQuickSave, "global_mouse/screenshot_quick_save"},
            std::pair{Action::ScreenRecording, "global_mouse/screen_recording"}};
        auto& store = storage::ApplicationStorage::instance().configuration();
        for (const auto& [action, key] : pairs) {
            const auto value = store.value(QString::fromLatin1(key)).toObject();
            const auto binding = globalMouseBinding(
                action, {value.value(QStringLiteral("activation_key")).toVariant().toStringList(),
                         value.value(QStringLiteral("mouse_button")).toString()});
            if (binding) {
                configuration.bindings.push_back(*binding);
            }
        }
        backend->configure(configuration);
    }

    void post(GlobalMouseDragEvent event, quint64 epoch) {
        QMutexLocker lock(&mutex);
        if (epoch != generation) {
            return;
        }
        using Kind = GlobalMouseDragEvent::Kind;
        if (!pending.isEmpty() && event.kind != Kind::Begin &&
            pending.last().kind == Kind::Update && pending.last().id == event.id) {
            // A terminal event carries the final position and supersedes an unpresented move.
            pending.last() = event;
        } else {
            pending.push_back(event);
        }
        if (deliveryQueued || (event.kind == Kind::Update && pacing)) {
            return;
        }
        deliveryQueued = true;
        QMetaObject::invokeMethod(&q, [this, epoch]() { deliver(epoch); }, Qt::QueuedConnection);
    }

    void deliver(quint64 epoch, bool frameDeadline = false) {
        QVector<GlobalMouseDragEvent> events;
        {
            QMutexLocker lock(&mutex);
            if (epoch != generation || !started) {
                return;
            }
            deliveryQueued = false;
            // A timer can overtake a queued boundary wake. That stale wake must not
            // present the next frame's movement early.
            if (!frameDeadline && pacing &&
                std::all_of(pending.cbegin(), pending.cend(), [](const auto& event) {
                    return event.kind == GlobalMouseDragEvent::Kind::Update;
                })) {
                return;
            }
            if (frameDeadline) {
                pacing = false;
            }
            events.swap(pending);
            if (events.isEmpty()) {
                return;
            }
            pacing = true;
        }
        for (const auto& item : events) {
            if (epoch != generation || !started) {
                return;
            }
            if (item.kind == GlobalMouseDragEvent::Kind::Begin) {
                activeId = item.id;
                pendingCaptureId = item.id;
                // QCursor uses logical desktop coordinates, unlike native gesture positions.
                const QScreen* screen = QGuiApplication::screenAt(QCursor::pos());
                const qreal rate = screen ? screen->refreshRate() : 60.0;
                frameTimer.setInterval(
                    std::max(1, static_cast<int>(std::ceil(1000.0 / (rate > 0 ? rate : 60.0)))));
            } else if (item.kind == GlobalMouseDragEvent::Kind::Cancel &&
                       pendingCaptureId == item.id) {
                activeId = 0;
                pendingCaptureId = 0;
            } else if (activeId != item.id) {
                continue;
            } else if (item.kind != GlobalMouseDragEvent::Kind::Update) {
                activeId = 0;
            }
            emit q.dragEvent(item);
        }
        if (epoch != generation || !started) {
            return;
        }
        if (activeId != 0) {
            frameTimer.start();
        } else {
            frameTimer.stop();
            QMutexLocker lock(&mutex);
            pacing = false;
        }
    }

    GlobalMouseManager& q;
    std::unique_ptr<GlobalMouseBackend> backend;
    GlobalMouseConfiguration configuration;
    QMetaObject::Connection configurationConnection;
    QMutex mutex;
    QVector<GlobalMouseDragEvent> pending;
    QTimer frameTimer;
    quint64 generation = 0;
    quint64 activeId = 0;
    // Release stops frame pacing, but capture preparation may still need cancellation.
    quint64 pendingCaptureId = 0;
    bool pacing = false;
    bool deliveryQueued = false;
    bool started = false;
};

GlobalMouseManager::GlobalMouseManager(QObject* parent) : GlobalMouseManager(nullptr, parent) {}
GlobalMouseManager::GlobalMouseManager(std::unique_ptr<GlobalMouseBackend> backend, QObject* parent)
    : QObject(parent), m_impl(std::make_unique<Impl>(*this, std::move(backend))) {}
GlobalMouseManager::~GlobalMouseManager() {
    shutdown();
}

void GlobalMouseManager::initialize() {
    auto& impl = *m_impl;
    if (impl.started) {
        return;
    }
    impl.started = true;
    const quint64 epoch = impl.generation;
    impl.reload();
    impl.configurationConnection =
        connect(&storage::ApplicationStorage::instance().configuration(),
                &storage::ConfigurationStore::valueChanged, this,
                [this](const QString& key, const QJsonValue&) {
                    if (key.startsWith(QStringLiteral("global_mouse/"))) {
                        m_impl->reload();
                    }
                });
    impl.backend->start(
        [this, epoch](GlobalMouseDragEvent event) { m_impl->post(event, epoch); },
        [this, epoch](quint32 code) {
            QMetaObject::invokeMethod(
                this,
                [this, code, epoch]() {
                    if (m_impl->started && epoch == m_impl->generation) {
                        qWarning("Global mouse input initialization failed: %u", code);
                        emit operationFailed(
                            tr("Global mouse input is unavailable (error %1).").arg(code));
                    }
                },
                Qt::QueuedConnection);
        });
}

void GlobalMouseManager::beginButtonDrag(settings::SettingsGlobalMouseAction action) {
    if (m_impl->started && m_impl->configuration.captureAvailable) {
        m_impl->backend->beginButtonDrag(action);
    }
}

void GlobalMouseManager::shutdown() {
    if (!m_impl->started) {
        return;
    }
    m_impl->started = false;
    disconnect(m_impl->configurationConnection);
    m_impl->backend->stop();
    m_impl->frameTimer.stop();
    QMutexLocker lock(&m_impl->mutex);
    ++m_impl->generation;
    m_impl->activeId = 0;
    m_impl->pendingCaptureId = 0;
    m_impl->pacing = false;
    m_impl->deliveryQueued = false;
    m_impl->pending.clear();
}

void GlobalMouseManager::setCaptureAvailable(bool available) {
    m_impl->configuration.captureAvailable = available;
    m_impl->backend->configure(m_impl->configuration);
}
void GlobalMouseManager::cancelGesture(quint64 id) {
    if (m_impl->pendingCaptureId == id) {
        m_impl->pendingCaptureId = 0;
        m_impl->activeId = 0;
        m_impl->frameTimer.stop();
        QMutexLocker lock(&m_impl->mutex);
        m_impl->pacing = false;
        m_impl->pending.removeIf([id](const auto& event) { return event.id == id; });
    }
    m_impl->backend->cancel(id);
}
} // namespace snow_shot::presentation
