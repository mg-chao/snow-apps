#ifndef SNOW_SHOT_PRESENTATION_WINDOWGEOMETRYMEMORY_H
#define SNOW_SHOT_PRESENTATION_WINDOWGEOMETRYMEMORY_H

#include <QEvent>
#include <QObject>
#include <QRect>
#include <QSize>
#include <QWidget>

#include <optional>

class QTimer;

namespace snow_shot::presentation {

// Visible-space capture: maximized/fullscreen/minimized widgets report placeholder
// geometry(), so persist the restore rectangle instead.
[[nodiscard]] QRect persistableNormalGeometry(const QWidget& widget);
[[nodiscard]] QSize persistableWindowSize(const QWidget& widget);

// Owns the main-window restore/save cycle so hidden top-level geometry is never
// persisted: restore is applied on the first Show, saves read live geometry while
// the window is visible and fall back to the snapshot captured at close once it is
// hidden, and the destructor flushes after WA_DeleteOnClose or a shutdown delete.
class WindowGeometryMemory final : public QObject {
    Q_OBJECT

  public:
    explicit WindowGeometryMemory(QWidget* widget);
    ~WindowGeometryMemory() override;

    void restoreMainWindow(const QSize& minimumSize);
    void captureAcceptedClose();

  protected:
    bool eventFilter(QObject* watched, QEvent* event) override;

  private:
    void applyPendingRestore();
    void scheduleSave();
    void save();

    QWidget* m_widget = nullptr;
    QTimer* m_saveTimer = nullptr;
    std::optional<QRect> m_pendingRestoreGeometry;
    bool m_restoreMaximized = false;
    QRect m_closeGeometry;
    bool m_closeGeometryMaximized = false;
    bool m_hasCloseGeometry = false;
    bool m_wasShown = false;
};

} // namespace snow_shot::presentation

#endif // SNOW_SHOT_PRESENTATION_WINDOWGEOMETRYMEMORY_H
