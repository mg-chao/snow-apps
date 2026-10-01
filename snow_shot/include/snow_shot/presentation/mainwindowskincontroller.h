#pragma once

#include <QImage>
#include <QObject>
#include <QPixmap>
#include <QRectF>
#include <QSize>
#include <QString>

#include <memory>

class SnowCanvasRegionFilterScratch;

namespace snow_shot::presentation {

enum class MainWindowSkinDisplayMode { Overlay, Contain };
enum class SkinSurface { MainWindow, Toolbar, TrayMenu };
enum class SkinPosition {
    TopLeft,
    TopCenter,
    TopRight,
    CenterLeft,
    Center,
    CenterRight,
    BottomLeft,
    BottomCenter,
    BottomRight
};

[[nodiscard]] SkinPosition skinPositionFromString(const QString& position);

struct MainWindowSkinFrame {
    QImage image;
    // Placement is relative to the viewport, so the last frame remains usable during resize.
    QRectF normalizedPlacement;
};

struct MainWindowSkinDiagnostics {
    quint64 decodeJobs = 0;
    quint64 preparationJobs = 0;
    quint64 staleResults = 0;
    quint64 cacheHits = 0;
    quint64 pixmapConversions = 0;
    qsizetype retainedBytes = 0;
    qsizetype idleFrameBytes = 0;
    int idleFrameCount = 0;
    qsizetype scratchRetainedBytes = 0;
    int executorCount = 0;
    bool executorAllocated = false;
    bool busy = false;
};

// Pure image preparation shared by the controller, deterministic tests and benchmark.
[[nodiscard]] MainWindowSkinFrame
prepareMainWindowSkin(const QImage& source, const QSize& logicalSize, qreal devicePixelRatio,
                      MainWindowSkinDisplayMode mode, int blurLevel,
                      SnowCanvasRegionFilterScratch* scratch = nullptr,
                      SkinPosition position = SkinPosition::Center);

// Application-owned: deleting a window never waits for the image executor.
class MainWindowSkinController final : public QObject {
    Q_OBJECT

  public:
    explicit MainWindowSkinController(QObject* parent = nullptr);
    ~MainWindowSkinController() override;
    [[nodiscard]] static MainWindowSkinController& instance();
    [[nodiscard]] static MainWindowSkinController* existingInstance() noexcept;

    void attach(QObject* view, const QSize& size, qreal devicePixelRatio);
    void attach(QObject* view, SkinSurface surface, const QSize& size, qreal devicePixelRatio);
    void detach(QObject* view);
    void setViewport(QObject* view, const QSize& size, qreal devicePixelRatio,
                     bool immediate = false);
    void reload();
    void reload(SkinSurface surface);
    // Explicit settings validation stays asynchronous and creates no hidden frame.
    void validate(SkinSurface surface);

    [[nodiscard]] MainWindowSkinFrame frame() const;
    [[nodiscard]] MainWindowSkinFrame frame(QObject* view) const;
    [[nodiscard]] QPixmap pixmap(QObject* view) const;
    [[nodiscard]] bool skinActive() const;
    [[nodiscard]] bool skinActive(QObject* view) const;
    [[nodiscard]] qreal opacity() const;
    [[nodiscard]] qreal maskOpacity() const;
    [[nodiscard]] QString statusText() const;
    [[nodiscard]] QString statusText(SkinSurface surface) const;
    [[nodiscard]] bool hasError() const;
    [[nodiscard]] bool hasError(SkinSurface surface) const;
    [[nodiscard]] MainWindowSkinDiagnostics diagnostics() const;

  signals:
    void frameChanged();
    void viewFrameChanged(QObject* view);
    void appearanceChanged();
    void statusChanged();

  private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;
};
} // namespace snow_shot::presentation
