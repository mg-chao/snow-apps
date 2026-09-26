#pragma once

#include <QObject>
#include <QPointer>

namespace snow_shot::presentation {
class FullscreenCanvasWindow;

class FullscreenCanvasController final : public QObject {
    Q_OBJECT

  public:
    explicit FullscreenCanvasController(QObject* parent = nullptr);
    ~FullscreenCanvasController() override;
    void activate();
    void shutdown();
    [[nodiscard]] FullscreenCanvasWindow* window() const;

  signals:
    void operationFailed(const QString& message, bool warning);

  private:
    QPointer<FullscreenCanvasWindow> m_window;
};
} // namespace snow_shot::presentation
