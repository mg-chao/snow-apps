#ifndef SNOW_SHOT_PRESENTATION_SCREENSHOTLATEXRENDERER_H
#define SNOW_SHOT_PRESENTATION_SCREENSHOTLATEXRENDERER_H

#include <QColor>
#include <QImage>
#include <QObject>
#include <QSize>
#include <QString>

#include <memory>

// MicroTeX's mutable registries belong to one process-wide worker. Each preview keeps only
// its latest request; completed images are implicitly shared and contain no renderer objects.
class ScreenshotLatexRenderer final : public QObject {
    Q_OBJECT

  public:
    enum class Error { None, InvalidFormula, LimitExceeded, InitializationFailed };
    Q_ENUM(Error)

    explicit ScreenshotLatexRenderer(QObject* parent = nullptr);
    ~ScreenshotLatexRenderer() override;

    // viewportSize is in logical pixels. The returned transparent image has the requested DPR.
    [[nodiscard]] quint64 request(const QString& source, QSize viewportSize, qreal devicePixelRatio,
                                  QColor foreground);
    void cancel();
    // Cancel pending work and release the per-preview raster cache on the worker.
    void clearCache();

  signals:
    void rendered(quint64 token, const QImage& image, ScreenshotLatexRenderer::Error error);

  private:
    struct State;
    std::shared_ptr<State> m_state;
};

#endif // SNOW_SHOT_PRESENTATION_SCREENSHOTLATEXRENDERER_H
