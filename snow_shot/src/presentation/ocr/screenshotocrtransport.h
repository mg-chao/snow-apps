#pragma once

#include "screenshotocrprotocol.h"
#include "screenshotocrtransferbuffer.h"
#include "snow_shot/presentation/screenshotocrassets.h"
#include "snow_shot/diagnostics/diagnostics.h"
#include <QImage>
#include <QObject>
#include <QMetaObject>
#include <QProcess>
#include <QProcessEnvironment>
#include <QTemporaryFile>
#include <QTimer>
#include <atomic>
#include <functional>
#include <memory>

namespace snow_shot::ocr {
using namespace protocol;

// Private transport: every method and all Qt/file objects belong to its I/O thread.
// The service retains scheduling and receiver ownership on the application thread.
class ScreenshotOcrTransport final : public QObject {
  public:
    struct Callbacks {
        std::function<void(qint64)> started;
        std::function<void(QByteArray)> output;
        std::function<void(QByteArray)> errorOutput;
        std::function<void(int, QProcess::ExitStatus)> finished;
        std::function<void(QString)> failed;
    };

    ScreenshotOcrTransport(QObject* receiver, Callbacks callbacks)
        : m_receiver(receiver), m_callbacks(std::move(callbacks)) {}

    ~ScreenshotOcrTransport() override {
        if (m_process != nullptr) {
            m_process->disconnect(this);
            if (m_process->state() != QProcess::NotRunning) {
                const auto pid = m_process->processId();
                m_process->kill();
                m_process->waitForFinished(1000);
                snow_shot::diagnostics::logEvent(
                    QStringLiteral("snow_shot.ocr"), QStringLiteral("ocr.process_exit"),
                    {{QStringLiteral("exit_code"), m_process->exitCode()},
                     {QStringLiteral("outcome"), QStringLiteral("shutdown")},
                     {QStringLiteral("child_pid"), pid}});
            }
        }
        releaseBuffer();
    }

    void start(const ScreenshotOcrResolvedAssets& assets, const QProcessEnvironment& environment) {
        QByteArray hello;
        appendString(hello, assets.stateDirectory);
        m_process = std::make_unique<QProcess>();
        connect(m_process.get(), &QProcess::started, this, [this, hello]() {
            post(m_callbacks.started, m_process->processId());
            send(makeFrame(kHello, 0, hello));
        });
        connect(m_process.get(), &QProcess::readyReadStandardOutput, this,
                [this]() { post(m_callbacks.output, m_process->readAllStandardOutput()); });
        connect(m_process.get(), &QProcess::readyReadStandardError, this,
                [this]() { post(m_callbacks.errorOutput, m_process->readAllStandardError()); });
        connect(m_process.get(), &QProcess::errorOccurred, this,
                [this](QProcess::ProcessError error) {
                    if (error == QProcess::FailedToStart)
                        post(m_callbacks.failed, QStringLiteral("start_failed"));
                });
        connect(m_process.get(), qOverload<int, QProcess::ExitStatus>(&QProcess::finished), this,
                [this](int code, QProcess::ExitStatus status) {
                    post(m_callbacks.output, m_process->readAllStandardOutput());
                    post(m_callbacks.errorOutput, m_process->readAllStandardError());
                    post(m_callbacks.finished, code, status);
                });
        m_process->setWorkingDirectory(assets.runtimeDirectory);
        m_process->setProcessEnvironment(environment);
        m_process->start(assets.processPath);
    }

    void allocateBuffer(qsizetype bytes, quint64 generation) {
        if (!m_buffer.allocate(bytes, generation)) {
            post(m_callbacks.failed, QStringLiteral("shared_memory"));
            return;
        }
        QByteArray payload;
        appendString(payload, m_buffer.path());
        appendU64(payload, static_cast<quint64>(bytes));
        send(makeFrame(kAttachBuffer, generation, payload));
    }

    void releaseBuffer() {
        m_buffer.release();
    }

    void submit(QImage image, quint64 sequence, quint64 token) {
        if (m_process == nullptr || m_process->state() != QProcess::Running)
            return;
        image = image.convertToFormat(QImage::Format_RGBA8888);
        const qsizetype stride = static_cast<qsizetype>(image.width()) * 4;
        if (image.isNull() || stride * image.height() > m_buffer.capacity() - kSlotHeaderBytes) {
            post(m_callbacks.failed, QStringLiteral("image_transfer"));
            return;
        }
        uchar* header = m_buffer.data();
        writeU32(header + kSlotStateOffset, kSlotFree);
        for (int row = 0; row < image.height(); ++row)
            std::memcpy(header + kSlotHeaderBytes + row * stride, image.constScanLine(row),
                        static_cast<std::size_t>(stride));
        writeU64(header + kSlotSequenceOffset, sequence);
        writeU32(header + kSlotWidthOffset, static_cast<quint32>(image.width()));
        writeU32(header + kSlotHeightOffset, static_cast<quint32>(image.height()));
        writeU32(header + kSlotStrideOffset, static_cast<quint32>(stride));
        writeU32(header + kSlotBytesOffset, static_cast<quint32>(stride * image.height()));
        writeU32(header + kSlotMagicOffset, kSlotMagic);
        QByteArray payload;
        appendU64(payload, m_buffer.generation());
        appendU32(payload, static_cast<quint32>(image.width()));
        appendU32(payload, static_cast<quint32>(image.height()));
        appendU32(payload, static_cast<quint32>(stride));
        appendU64(payload, sequence);
        std::atomic_thread_fence(std::memory_order_release);
        writeU32(header + kSlotStateOffset, kSlotReady);
        send(makeFrame(kSubmit, token, payload));
    }

    void send(const QByteArray& frame) {
        if (m_process != nullptr && m_process->state() == QProcess::Running)
            m_process->write(frame);
    }

    void stop(bool force) {
        if (m_process == nullptr || m_process->state() == QProcess::NotRunning)
            return;
        if (force) {
            m_process->kill();
        } else {
            send(makeFrame(kShutdown, 0));
            m_process->closeWriteChannel();
            QTimer::singleShot(1000, this, [this]() { stop(true); });
        }
    }

  private:
    template <typename Callback, typename... Args> void post(Callback callback, Args... args) {
        QMetaObject::invokeMethod(
            m_receiver, [callback, args...]() mutable { callback(args...); }, Qt::QueuedConnection);
    }
    QObject* m_receiver;
    Callbacks m_callbacks;
    std::unique_ptr<QProcess> m_process;
    ScreenshotOcrTransferBuffer m_buffer;
};

} // namespace snow_shot::ocr
