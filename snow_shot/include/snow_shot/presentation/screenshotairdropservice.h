// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <QImage>
#include <QObject>
#include <QString>

#include <functional>
#include <memory>

class QWidget;

class ScreenshotAirDropService final : public QObject {
  public:
    // Some AirDrop extensions report didShareItems even when their chooser is
    // cancelled. HandedOff preserves the source when no shared attachment is confirmed.
    enum class Status { Shared, Cancelled, Failed, Unavailable, HandedOff };
    struct Result {
        Status status = Status::Failed;
        QString error;
    };
    using Completion = std::function<void(Result)>;
    // The backend retains completion until native sharing ends. That retention also
    // keeps the temporary PNG alive, even when the source window has been destroyed.
    using Backend = std::function<void(QWidget*, QString, Completion)>;

    explicit ScreenshotAirDropService(Backend backend, QObject* parent = nullptr);
    ~ScreenshotAirDropService() override;
    [[nodiscard]] static ScreenshotAirDropService& shared();
    [[nodiscard]] bool shareImage(QObject* receiver, QWidget* owner, QImage snapshot,
                                  QString filename, Completion completion);
    [[nodiscard]] bool busy() const;

  private:
    struct Request;
    void finish(const std::shared_ptr<Request>& request, Result result);
    Backend m_backend;
    std::shared_ptr<Request> m_request;
};
