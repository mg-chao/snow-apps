#ifndef SNOW_SHOT_PLATFORM_MACOS_WINDOWCURSORCOORDINATOR_P_H
#define SNOW_SHOT_PLATFORM_MACOS_WINDOWCURSORCOORDINATOR_P_H

#include <QCursor>
#include <QObject>
#include <QPointer>
#include <QSet>
#include <QWidget>
#include <QWindow>
#include <functional>
#include <optional>

namespace snow_shot::platform::macos::detail {
// Qt owns cursor selection. This controller only arbitrates when/where the native
// backend may apply it. Keeping the boundary injectable makes ownership testable
// without a desktop, pointer movement, or permission to post native input.
class WindowCursorCoordinator final : public QObject {
  public:
    struct Backend {
        std::function<bool()> active;
        std::function<bool()> suspended;
        std::function<QWidget*()> hoverTarget;
        std::function<QWidget*()> mouseGrabber;
        std::function<bool(QWidget*)> inputEnabled;
        std::function<void(QWidget*, bool)> apply;
        // Native tracking loops can consume a release before Qt sees it. An
        // optional physical snapshot reconciles that boundary without polling.
        std::function<bool()> buttonsPressed;
    };

    explicit WindowCursorCoordinator(Backend backend, QObject* parent = nullptr);
    void addWindow(QWidget* window);
    void invalidate();

  protected:
    bool eventFilter(QObject* watched, QEvent* event) override;

  private:
    bool manages(QWidget* widget) const;
    void observeWindow(QWindow* window);
    void refresh();
    void observeOverrideCursor();
    void clearInteraction();

    Backend m_backend;
    QList<QPointer<QWidget>> m_roots;
    QSet<QWindow*> m_observed;
    QSet<QWindow*> m_blocked;
    QSet<QWindow*> m_destroying;
    QPointer<QWidget> m_pressedWindow;
    QPointer<QWindow> m_pressedSurface;
    Qt::MouseButtons m_buttons = Qt::NoButton;
    std::optional<QCursor> m_override;
    bool m_queued = false;
};
} // namespace snow_shot::platform::macos::detail

#endif
