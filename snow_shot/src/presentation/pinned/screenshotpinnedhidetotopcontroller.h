#ifndef SNOW_SHOT_PRESENTATION_SCREENSHOTPINNEDHIDETOTOPCONTROLLER_H
#define SNOW_SHOT_PRESENTATION_SCREENSHOTPINNEDHIDETOTOPCONTROLLER_H

#include <QColor>
#include <QObject>
#include <QPointer>
#include <QRect>
#include <QVariantAnimation>
#include <QVector>

#include <functional>
#include <memory>
#include <optional>

class QScreen;
class QWidget;
class ScreenshotPinnedPointerPresence;

namespace screenshot_pinned_hide_to_top {
struct Screen {
    QPointer<QScreen> screen;
    QRect workArea;
    qreal dpi = 1.0;
};

[[nodiscard]] Screen screenGeometry(QScreen* screen);
[[nodiscard]] QRect placeHandle(const QRect& workArea, int preferredLeft, qreal dpi,
                                const QVector<QRect>& occupied);
[[nodiscard]] QRect hitGeometry(const QRect& handle, const QRect& workArea, qreal dpi);
} // namespace screenshot_pinned_hide_to_top

class ScreenshotPinnedHideToTopController final : public QObject {
  public:
    enum class State { Normal, Entering, Hidden, Revealed };
    struct Hooks {
        std::function<QRect()> geometry;
        std::function<QRect()> frameGeometry;
        std::function<bool(const QRect&)> applyGeometry;
        std::function<int()> opacity;
        std::function<int()> allocateAccent;
        std::function<void()> activate;
        std::function<void()> changed;
        std::function<std::optional<QPoint>()> cursor;
        std::function<void(const QPoint&)> contextMenu;
        std::function<void(bool)> mouseAction;
    };

    ScreenshotPinnedHideToTopController(QWidget* owner, Hooks hooks);
    ~ScreenshotPinnedHideToTopController() override;
    [[nodiscard]] State state() const {
        return m_state;
    }
    [[nodiscard]] bool active() const {
        return m_state != State::Normal;
    }
    [[nodiscard]] QRect handleGeometry() const {
        return m_handleGeometry;
    }
    [[nodiscard]] QRect shownGeometry() const;
    [[nodiscard]] int accentIndex() const {
        return m_accentIndex;
    }
    void setAccentIndex(int index);
    [[nodiscard]] QColor handleColor() const;
    [[nodiscard]] QVariantAnimation& animation() {
        return m_animation;
    }
    [[nodiscard]] QWidget* handleWidget() const {
        return m_handle.get();
    }
    bool enter(const screenshot_pinned_hide_to_top::Screen& screen);
    bool prepareRestore(const screenshot_pinned_hide_to_top::Screen& screen,
                        const QRect& savedHandle);
    void finishRestore();
    void exit(bool cancelEntry = false);
    void shutdown();
    void setSuppressed(bool suppressed);
    void refreshOpacity();
    void refreshPointer();
    void updatePointer(const QPoint& nativeCursor);
    void reconcileScreen(const screenshot_pinned_hide_to_top::Screen& screen);
    void recoverToScreen(const screenshot_pinned_hide_to_top::Screen& screen);

  protected:
    bool eventFilter(QObject* watched, QEvent* event) override;

  private:
    friend class ScreenshotPinnedHideToTopControllerTestAccess;
    bool pointerInside(const QPoint& nativeCursor) const;
    bool reserve(const screenshot_pinned_hide_to_top::Screen& screen, int preferredLeft);
    void release();
    void showHandle();
    void hideWindow();
    void reveal();
    void changed();
    void retranslate();
    void watchScreen();

    QPointer<QWidget> m_owner;
    Hooks m_hooks;
    State m_state = State::Normal;
    screenshot_pinned_hide_to_top::Screen m_screen;
    std::unique_ptr<QWidget> m_handle;
    std::unique_ptr<ScreenshotPinnedPointerPresence> m_pointerPresence;
    QVariantAnimation m_animation;
    QRect m_handleGeometry;
    QPoint m_entryPosition;
    int m_accentIndex = -1;
    bool m_restoring = false;
    bool m_suppressed = false;
    bool m_shutdown = false;
    bool m_changing = false;
    QVector<QMetaObject::Connection> m_screenConnections;
};

#endif // SNOW_SHOT_PRESENTATION_SCREENSHOTPINNEDHIDETOTOPCONTROLLER_H
