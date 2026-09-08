#ifndef SNOW_SHOT_PRESENTATION_SELECTEDTEXTTRANSLATIONCONTROLLER_H
#define SNOW_SHOT_PRESENTATION_SELECTEDTEXTTRANSLATIONCONTROLLER_H

#include <QObject>
#include <QString>
#include <QTimer>

#include <memory>

namespace snow_shot::presentation {
enum class SelectedTextStatus {
    Pending,
    Selected,
    NoSelection,
    Unsupported,
    Busy,
    TimedOut,
    Failed
};

struct SelectedTextCaptureResult {
    SelectedTextStatus status = SelectedTextStatus::Pending;
    QString text;
};

// Capture must begin before the host activates any window. Implementations never block on a result.
class SelectedTextCaptureBackend {
  public:
    virtual ~SelectedTextCaptureBackend() = default;
    virtual SelectedTextCaptureResult start() = 0;
    virtual SelectedTextCaptureResult poll() = 0;
    virtual void cancel() = 0;
};

class SelectedTextTranslationController final : public QObject {
    Q_OBJECT

  public:
    explicit SelectedTextTranslationController(QObject* parent = nullptr);
    explicit SelectedTextTranslationController(std::unique_ptr<SelectedTextCaptureBackend> backend,
                                               QObject* parent = nullptr);
    ~SelectedTextTranslationController() override;
    void capture();
    void shutdown();

  signals:
    void textReady(const QString& text);
    void operationFailed(const QString& message);

  private:
    void acceptResult(const SelectedTextCaptureResult& result);

    std::unique_ptr<SelectedTextCaptureBackend> m_backend;
    QTimer m_pollTimer;
    bool m_pending = false;
    bool m_shutdown = false;
};
} // namespace snow_shot::presentation

#endif
