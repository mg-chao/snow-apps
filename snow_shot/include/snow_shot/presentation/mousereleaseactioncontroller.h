#ifndef SNOW_SHOT_PRESENTATION_MOUSERELEASEACTIONCONTROLLER_H
#define SNOW_SHOT_PRESENTATION_MOUSERELEASEACTIONCONTROLLER_H

#include <QObject>
#include <QPointer>
#include <QWidget>

#include <functional>

namespace snow_shot::presentation {

// Owns a dismissing mouse gesture until its release has finished dispatching.
// The scope stays visible; interruptions discard the action rather than closing.
class MouseReleaseActionController final : public QObject {
  public:
    explicit MouseReleaseActionController(QObject* parent = nullptr);
    ~MouseReleaseActionController() override;

    [[nodiscard]] bool arm(QWidget* scopeWindow, Qt::MouseButton button,
                           std::function<void()> action);
    void cancel();
    [[nodiscard]] bool pending() const;
    // Forward from the scope's nativeEvent, including directly sent non-client
    // messages that do not pass through an application native event filter.
    bool handleNativeEvent(void* message, qintptr* result);

  protected:
    bool eventFilter(QObject* watched, QEvent* event) override;

  private:
    void finish();
    void releaseCapture();

    QPointer<QWidget> m_scope;
    QPointer<QWidget> m_capture;
    QMetaObject::Connection m_scopeDestroyed;
    std::function<void()> m_action;
    Qt::MouseButton m_button = Qt::NoButton;
    quint64 m_revision = 0;
    bool m_ownsCapture = false;
    bool m_finishing = false;
};

} // namespace snow_shot::presentation

#endif
