#ifndef SNOW_SHOT_PRESENTATION_PINNEDWINDOWSTACKINGCOORDINATOR_H
#define SNOW_SHOT_PRESENTATION_PINNEDWINDOWSTACKINGCOORDINATOR_H

#include <QObject>
#include <QStringList>
#include <memory>

class QWidget;
namespace snow_shot::storage {
class PinnedWindowRepository;
}
namespace snow_shot::presentation {
struct PinnedWindowStackingPlatform;

class PinnedWindowStackingCoordinator final : public QObject {
  public:
    explicit PinnedWindowStackingCoordinator(storage::PinnedWindowRepository* repository,
                                             QObject* parent = nullptr);
    PinnedWindowStackingCoordinator(storage::PinnedWindowRepository* repository,
                                    PinnedWindowStackingPlatform platform,
                                    QObject* parent = nullptr);
    ~PinnedWindowStackingCoordinator() override;

    void track(QWidget* window, const QString& id, const QString& group);
    void presented(QWidget* window, bool preserveFocus);
    void retiring(QWidget* window);
    void nativeOrderChanged(QWidget* window);
    void promote(QWidget* window);
    void checkpoint();
    void shutdown();
    // Checkpoint once, then suspend native ordering across nested lifecycle mutations.
    // Presentations and restore completions remain tracked and reconcile after the outermost end.
    void beginMutation();
    void endMutation();
    [[nodiscard]] quint64 beginRestore(const QString& group, const QStringList& ids);
    void settleRestore(quint64 batch, const QString& id);
    void cancelGroup(const QString& group);

  protected:
    bool eventFilter(QObject* object, QEvent* event) override;

  private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;
};
} // namespace snow_shot::presentation
#endif
