// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "snow_shot/app/mcp/mcpdocumentservice.h"
#include "snow_shot/storage/pinnedwindowtypes.h"

namespace snow_shot::storage {
class CaptureHistoryRepository;
}

namespace snow_shot::app::mcp {
// Non-file document sources have their own admission, snapshots and worker lifetime.
// All public calls and completions run on the owning GUI thread. Borrowed ports must
// outlive stopAndDrain(); repository reads and raster work run on the bounded pool.
class McpSourceResolver final : public QObject {
  public:
    using Source = McpDocumentService::Source;
    using Completion = McpDocumentService::Ports::SourceCompletion;
    using Budget = McpDocumentService::Ports::SourceBudget;
    using CaptureCompletion = std::function<void(QImage, QJsonObject, QString)>;
    struct LivePin {
        storage::PinnedWindowRecord record;
        ScreenshotRecognitionResults recognition;
        QString tool;
    };
    struct Ports {
        // Fresh final admission, after a delayed request becomes runnable. Empty
        // means allowed; otherwise return a protocol error such as busy/permission_required.
        std::function<QString()> captureAllowed;
        std::function<bool(const QJsonObject&, CaptureCompletion, Budget)> capture;
        // Cancellation requests native stop; its owner retains the acquisition
        // lease until the worker finishes, even when result delivery is suppressed.
        std::function<void()> cancelCapture;
        std::function<std::optional<ScreenshotClipboardContentSnapshot>(Budget)> clipboard;
        // GUI snapshot: reserve raster bytes plus 64 MiB session overhead before
        // constructing the owning pin/recognition snapshot. No live pin returns nullopt.
        std::function<std::optional<LivePin>(const QString&, Budget)> livePin;
        // Worker read: admission must precede payload allocation.
        std::function<std::optional<storage::PinnedWindowRecord>(const QString&, Budget)> loadPin;
        storage::CaptureHistoryRepository* history = nullptr;
    };

    explicit McpSourceResolver(Ports ports, QObject* parent = nullptr);
    ~McpSourceResolver() override;
    // Invalid/rejected sources complete with an error. Accepted work completes at
    // most once; cancellation, disconnect and destruction suppress delivery.
    void resolve(const ScreenshotMcpRequest& request, Completion completion, Budget budget);
    bool cancel(quint64 owner, const QString& requestId);
    void disconnected(quint64 owner);
    // Occupied source slots include GUI snapshots and canceled workers that have not finished.
    [[nodiscard]] int workerCount() const;
    [[nodiscard]] bool hasPendingSources() const;
    // Suspension closes source admission, including the final check after delay.
    // Running workers keep their reservations until actual completion.
    void setSuspended(bool suspended);
    // Terminal: close admission, cancel timers/capture, drain owned workers and
    // discard queued results. A subsequent listener run creates a fresh resolver.
    void stopAndDrain();

  private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;
};
} // namespace snow_shot::app::mcp
