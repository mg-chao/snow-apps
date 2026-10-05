#ifndef SNOW_SHOT_PRESENTATION_PINNEDWINDOWSELECTIONCONTROLLER_H
#define SNOW_SHOT_PRESENTATION_PINNEDWINDOWSELECTIONCONTROLLER_H

#include "snow_draw_engine_qt/snow_canvas_types.h"
#include "snow_shot/storage/storageresult.h"

#include <QObject>
#include <QList>
#include <QPointer>
#include <QPointF>
#include <QVector>
#include <QSet>
#include <functional>
#include <memory>
#include <optional>

class ScreenshotPinnedWindow;
class QWidget;
class QEvent;
namespace adqt::widgets {
class AdContextMenu;
class AdModal;
} // namespace adqt::widgets

namespace snow_shot::presentation {
// Selection belongs to a pin session, independently of persistent window groups.
class PinnedWindowSelectionController final : public QObject {
    Q_OBJECT
  public:
    using DestroyRecords = std::function<storage::StorageResult(const QVector<QString>&)>;
    explicit PinnedWindowSelectionController(QObject* parent = nullptr,
                                             DestroyRecords destroyRecords = {});
    ~PinnedWindowSelectionController() override;

    void registerWindow(ScreenshotPinnedWindow* window);
    void unregisterWindow(ScreenshotPinnedWindow* window);
    void windowStateChanged(ScreenshotPinnedWindow* window);
    [[nodiscard]] bool isSelectable(const ScreenshotPinnedWindow* window) const;
    [[nodiscard]] bool isSelected(const ScreenshotPinnedWindow* window) const;
    [[nodiscard]] int selectedCount() const;
    [[nodiscard]] QList<QPointer<ScreenshotPinnedWindow>> selectedWindows() const;
    void toggleSelection(ScreenshotPinnedWindow* window);
    void deselectWindow(ScreenshotPinnedWindow* window);
    void clearSelection();
    // Returns true when selection routing owns this context menu event.
    bool showContextMenu(ScreenshotPinnedWindow* window, const QPoint& globalPosition);

    bool handlePointer(ScreenshotPinnedWindow* window, QObject* watched, QEvent* event);
    [[nodiscard]] bool routesPointerToClient(const ScreenshotPinnedWindow& window,
                                             const QPoint& position) const;
    bool beginGeometry(ScreenshotPinnedWindow* window, const QPointF& desktopPress,
                       std::optional<int> handle = {}, std::optional<QPoint> nativePress = {});
    void updateGeometry(const QPointF& desktopPosition);
    void endGeometry(bool cancel);
    void cancelGeometry();
    // Native capture loss can precede the drag threshold or interrupt shared geometry.
    bool cancelPointerInteraction(ScreenshotPinnedWindow* window);
    bool scaleBy(ScreenshotPinnedWindow* window, double requestedLeaderScale);
    [[nodiscard]] bool geometryActive() const;
    [[nodiscard]] bool usesSharedGeometry(const ScreenshotPinnedWindow* window) const;
    bool alignSelection(SnowCanvasSelectionAlignment alignment);

  signals:
    void selectionChanged();

  protected:
    bool eventFilter(QObject* watched, QEvent* event) override;

  private:
    void notifySelectionChanged();
    void buildContextMenu(ScreenshotPinnedWindow* owner);
    void showFailure(QWidget* owner, const QString& text);
    void confirmDestroy(ScreenshotPinnedWindow* owner,
                        const QList<QPointer<ScreenshotPinnedWindow>>& targets);
    void retranslateUi();

    QList<QPointer<ScreenshotPinnedWindow>> m_windows;
    QList<QPointer<ScreenshotPinnedWindow>> m_selected;
    QSet<const ScreenshotPinnedWindow*> m_windowIndex;
    QSet<const ScreenshotPinnedWindow*> m_selectedIndex;
    QPointer<adqt::widgets::AdContextMenu> m_contextMenu;
    QPointer<adqt::widgets::AdModal> m_destroyConfirmation;
    DestroyRecords m_destroyRecords;
    quint64 m_selectionRevision = 0;
    struct GeometryState;
    std::shared_ptr<GeometryState> m_geometryState;
};
} // namespace snow_shot::presentation

#endif // SNOW_SHOT_PRESENTATION_PINNEDWINDOWSELECTIONCONTROLLER_H
