#pragma once

#include "screenshotocrprotocol.h"
#include "snow_shot/diagnostics/diagnostics.h"
#include <QTemporaryFile>
#include <memory>

namespace snow_shot::ocr {
// Transport-thread owned. The coordinator permits release only after the child
// has acknowledged detachment, or after the child process has stopped.
class ScreenshotOcrTransferBuffer final {
  public:
    ~ScreenshotOcrTransferBuffer() {
        release();
    }
    bool allocate(qsizetype bytes, quint64 generation) {
        release();
        m_file = std::make_unique<QTemporaryFile>();
        if (!m_file->open() || !m_file->resize(bytes) ||
            (m_mapping = m_file->map(0, bytes)) == nullptr) {
            release();
            return false;
        }
        m_capacity = bytes;
        m_generation = generation;
        m_file->close();
        std::memset(m_mapping, 0, protocol::kSlotHeaderBytes);
        diagnostics::logEvent(QStringLiteral("snow_shot.ocr"),
                              QStringLiteral("ocr.buffer_allocated"),
                              {{QStringLiteral("shared_memory_bytes"), static_cast<qint64>(bytes)},
                               {QStringLiteral("generation"), static_cast<qint64>(generation)}});
        return true;
    }
    void release() {
        if (m_file == nullptr)
            return;
        if (m_mapping != nullptr)
            m_file->unmap(m_mapping);
        m_mapping = nullptr;
        m_file.reset();
        m_capacity = 0;
        diagnostics::logEvent(QStringLiteral("snow_shot.ocr"),
                              QStringLiteral("ocr.buffer_released"),
                              {{QStringLiteral("generation"), static_cast<qint64>(m_generation)}});
    }
    uchar* data() const {
        return m_mapping;
    }
    qsizetype capacity() const {
        return m_capacity;
    }
    quint64 generation() const {
        return m_generation;
    }
    QString path() const {
        return m_file == nullptr ? QString() : m_file->fileName();
    }

  private:
    std::unique_ptr<QTemporaryFile> m_file;
    uchar* m_mapping = nullptr;
    qsizetype m_capacity = 0;
    quint64 m_generation = 0;
};
} // namespace snow_shot::ocr
