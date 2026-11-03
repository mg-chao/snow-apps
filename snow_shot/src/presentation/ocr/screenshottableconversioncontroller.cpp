#include "snow_shot/presentation/screenshottableconversioncontroller.h"
#if SNOW_SHOT_ENABLE_TABLE_RECOGNITION
#include "snow_shot/platform/applicationqos.h"
#include "snow_shot/runtime/runtimeactivitytracker.h"
#include <QCoreApplication>
#include <QThreadPool>
#include <utility>

ScreenshotTableConversionController::ScreenshotTableConversionController(QObject* parent)
    : QObject(parent) {
    m_previewTimer.setInterval(100);
    m_previewTimer.setSingleShot(true);
    connect(&m_previewTimer, &QTimer::timeout, this, &ScreenshotTableConversionController::parse);
}

ScreenshotTableConversionController::~ScreenshotTableConversionController() {
    cancel();
}

void ScreenshotTableConversionController::cancel() {
    ++m_generation;
    m_previewTimer.stop();
    if (m_parseCancellation)
        m_parseCancellation->store(true, std::memory_order_relaxed);
    // Keep the worker slot until its completion is delivered. A replacement conversion
    // can collect text in the meantime, but must not start a second concurrent parser.
    m_busy = false;
    disconnect(m_providerDestroyed);
    const auto token = std::exchange(m_request, 0);
    m_conversation.reset();
    m_source.clear();
    if (m_api)
        m_api->cancel(token);
}

void ScreenshotTableConversionController::start(SnowShotApiClient* api, const QImage& image,
                                                const QString& model) {
    const QPointer<ScreenshotTableConversionController> guard(this);
    cancel();
    if (!guard)
        return;
    m_api = api;
    m_preview = {};
    m_correcting = false;
    m_finished = false;
    m_revision = 0;
    m_busy = true;
    m_conversation = std::make_shared<SnowShotTableVisionConversation>();
    if (api) {
        m_providerDestroyed = connect(api, &QObject::destroyed, this, [this] {
            if (m_busy)
                fail(tr("Table recognition service is unavailable"));
        });
    }
    sendAttempt(image, model);
}

void ScreenshotTableConversionController::sendAttempt(const QImage& image, const QString& model,
                                                      const QString& failedSource) {
    const QPointer<ScreenshotTableConversionController> guard(this);
    const quint64 generation = m_generation;
    emit changed();
    if (!guard || generation != m_generation || !m_busy)
        return;
    if (!m_api) {
        fail(tr("Table recognition service is unavailable"));
        return;
    }
    const auto delta = [this, generation](const QString& chunk) {
        if (generation != m_generation || !m_busy)
            return;
        m_source += chunk;
        ++m_revision;
        if (!m_previewTimer.isActive())
            m_previewTimer.start();
    };
    const auto completion = [this, generation](SnowShotTranslationResult result) {
        if (generation != m_generation || !m_busy)
            return;
        m_request = 0;
        if (!result.succeeded()) {
            fail(result.error.isEmpty() ? tr("Table recognition failed") : result.error);
            return;
        }
        // An empty successful stream is the prompt's no-table result, not malformed HTML.
        // Retrying it as a format error would ask the model to invent a table.
        if (m_source.trimmed().isEmpty()) {
            fail(tr("No table cells were recognized"));
            return;
        }
        m_finished = true;
        m_previewTimer.stop();
        parse();
    };
    const auto token =
        m_correcting
            ? m_api->correctTableVision(m_conversation, failedSource, this, delta, completion)
            : m_api->streamTableVision(image, model, m_conversation, this, delta, completion);
    if (!guard || generation != m_generation || !m_busy)
        return;
    m_request = token;
    if (!token)
        fail(tr("Table recognition request could not be prepared"));
}

void ScreenshotTableConversionController::parse() {
    if (!m_busy || m_parsing)
        return;
    m_parsing = true;
    const quint64 generation = m_generation;
    const quint64 revision = m_revision;
    const QString source = m_source;
    const auto cancelled = std::make_shared<std::atomic_bool>(false);
    m_parseCancellation = cancelled;
    const QPointer<ScreenshotTableConversionController> guard(this);
    QThreadPool::globalInstance()->start(
        snow_shot::runtime::trackRuntimeWork([guard, generation, revision, source, cancelled] {
            snow_shot::platform::applyApplicationQoSToCurrentThread();
            auto document = cancelled->load(std::memory_order_relaxed)
                                ? ScreenshotTableDocument{}
                                : ScreenshotTableDocument::fromModelOutput(source);
            QMetaObject::invokeMethod(
                QCoreApplication::instance(),
                snow_shot::runtime::trackRuntimeWork([guard, generation, revision, cancelled,
                                                      document = std::move(document)]() mutable {
                    if (!guard)
                        return;
                    guard->m_parsing = false;
                    guard->m_parseCancellation.reset();
                    if (!guard->m_busy)
                        return;
                    if (cancelled->load(std::memory_order_relaxed) ||
                        generation != guard->m_generation) {
                        if (guard->m_finished)
                            guard->parse();
                        else if (!guard->m_source.isEmpty() && !guard->m_previewTimer.isActive())
                            guard->m_previewTimer.start();
                        return;
                    }
                    if (revision != guard->m_revision) {
                        // This is still a valid prefix of this attempt. Show it while the
                        // next snapshot waits, otherwise a fast stream can starve previews.
                        if (!guard->m_finished && !document.empty()) {
                            guard->m_preview = std::move(document);
                            emit guard->changed();
                        }
                        if (!guard || generation != guard->m_generation || !guard->m_busy)
                            return;
                        if (guard->m_finished)
                            guard->parse();
                        else if (!guard->m_previewTimer.isActive())
                            guard->m_previewTimer.start();
                        return;
                    }
                    if (!guard->m_finished) {
                        if (!document.empty())
                            guard->m_preview = std::move(document);
                        emit guard->changed();
                        return;
                    }
                    if (!document.empty()) {
                        guard->m_busy = false;
                        guard->m_conversation.reset();
                        guard->m_source.clear();
                        emit guard->completed(std::move(document), {});
                    } else if (!guard->m_correcting) {
                        const QString failedSource = std::move(guard->m_source);
                        guard->m_source.clear();
                        guard->m_preview = {};
                        guard->m_correcting = true;
                        guard->m_finished = false;
                        ++guard->m_revision;
                        guard->sendAttempt({}, {}, failedSource);
                    } else {
                        guard->fail(tr("The model returned no usable table after correction"));
                    }
                }),
                Qt::QueuedConnection);
        }));
}

void ScreenshotTableConversionController::fail(const QString& error) {
    const QPointer<ScreenshotTableConversionController> guard(this);
    cancel();
    if (guard)
        emit completed({}, error);
}
#endif
