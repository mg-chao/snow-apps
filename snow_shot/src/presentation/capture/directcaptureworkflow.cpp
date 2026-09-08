#include "snow_shot/presentation/directcaptureworkflow.h"

#include <QCoreApplication>
#include <QPointer>
#include <QTimer>

#include <utility>

namespace snow_shot::presentation {
namespace {
QString queueError() {
    return QCoreApplication::translate("DirectCaptureController",
                                       "The capture operation could not be queued");
}
} // namespace

DirectCaptureWorkflow::DirectCaptureWorkflow(DirectCapturePorts ports, QObject* parent)
    : QObject(parent), m_ports(std::move(ports)) {}

void DirectCaptureWorkflow::enqueue(DirectCaptureRequest request) {
    if (m_phase == Phase::Stopped)
        return;
    m_queue.push_back(std::move(request));
    const QPointer<DirectCaptureWorkflow> self(this);
    if (m_queue.back().shutterSoundNotification && m_ports.captureRequested)
        m_ports.captureRequested();
    if (self && self->m_phase == Phase::Idle)
        self->startNext();
}

qsizetype DirectCaptureWorkflow::pendingCount() const {
    return static_cast<qsizetype>(m_queue.size());
}

void DirectCaptureWorkflow::shutdown() {
    ++m_generation;
    m_phase = Phase::Stopped;
    m_queue.clear();
    m_frame = {};
}

void DirectCaptureWorkflow::report(const QString& error, bool warning) {
    if (!error.isEmpty() && m_ports.report)
        m_ports.report(error, warning);
}

void DirectCaptureWorkflow::startNext() {
    if (m_phase != Phase::Idle || m_queue.empty())
        return;
    m_phase = Phase::Acquiring;
    const quint64 generation = ++m_generation;
    const QPointer<DirectCaptureWorkflow> self(this);
    auto complete = [self, generation](DirectCaptureFrame frame) {
        if (!self || self->m_generation != generation || self->m_phase != Phase::Acquiring)
            return;
        if (!frame.isValid()) {
            self->report(frame.error.isEmpty()
                             ? QCoreApplication::translate("DirectCaptureController",
                                                           "The capture returned an invalid image")
                             : frame.error);
            if (self)
                self->finish();
            return;
        }
        self->m_frame = std::move(frame);
        self->saveOrCopy();
    };
    if (!m_ports.acquire(m_queue.front(), complete))
        complete(DirectCaptureFrame{{}, {}, {}, 0, queueError()});
}

void DirectCaptureWorkflow::saveOrCopy() {
    if (m_phase != Phase::Acquiring)
        return;
    if (!m_queue.front().autoSave && !m_queue.front().copyFile) {
        copy();
        return;
    }
    m_phase = Phase::Saving;
    const quint64 generation = m_generation;
    const QPointer<DirectCaptureWorkflow> self(this);
    auto complete = [self, generation](QString path, QString error) {
        if (!self || self->m_generation != generation || self->m_phase != Phase::Saving)
            return;
        const bool copyFile = self->m_queue.front().copyFile;
        if (path.isEmpty() || !error.isEmpty()) {
            self->report(error.isEmpty() ? queueError() : error, !copyFile);
            if (!self || self->m_phase != Phase::Saving)
                return;
            if (copyFile) {
                self->finish();
                return;
            }
        }
        self->copy(copyFile ? path : QString());
    };
    if (!m_ports.save(m_queue.front(), m_frame, complete))
        complete({}, queueError());
}

void DirectCaptureWorkflow::copy(const QString& path) {
    m_phase = Phase::Copying;
    const quint64 generation = m_generation;
    const QPointer<DirectCaptureWorkflow> self(this);
    auto complete = [self, generation](QString error) {
        if (!self || self->m_generation != generation || self->m_phase != Phase::Copying)
            return;
        if (!error.isEmpty()) {
            self->report(error);
            if (self)
                self->finish();
            return;
        }
        self->publishHistory();
    };
    if (!m_ports.copy(m_queue.front(), m_frame, path, complete))
        complete(queueError());
}

void DirectCaptureWorkflow::publishHistory() {
    if (!m_queue.front().historyEnabled) {
        finish();
        return;
    }
    m_phase = Phase::History;
    const quint64 generation = m_generation;
    const QPointer<DirectCaptureWorkflow> self(this);
    auto complete = [self, generation](QString error) {
        if (!self || self->m_generation != generation || self->m_phase != Phase::History)
            return;
        self->report(error, true);
        if (self)
            self->finish();
    };
    if (!m_ports.history(m_queue.front(), m_frame, complete))
        complete(queueError());
}

void DirectCaptureWorkflow::finish() {
    if (m_phase == Phase::Stopped || m_phase == Phase::Idle)
        return;
    m_queue.pop_front();
    m_frame = {};
    m_phase = Phase::Idle;
    QTimer::singleShot(0, this, [this]() { startNext(); });
}
} // namespace snow_shot::presentation
