#pragma once

#include <QImage>
#include <QObject>
#include <QPointer>
#include <QRect>

#include <functional>
#include <utility>

namespace snow_shot::capture_detail {

// Owns the temporary pause and a bounded, latest-position image request queue. Callbacks run
// on the owner's GUI thread; source shutdown and extraction remain asynchronous.
class ScrollingHoverPreview final : public QObject {
  public:
    struct Context {
        std::function<void(std::function<void()>)> pause;
        std::function<bool(const QRect&, std::function<void(QImage)>)> request;
        std::function<void(const QImage&, bool)> present;
        std::function<void()> clear;
        std::function<void()> resume;
    };

    explicit ScrollingHoverPreview(Context context) : m_context(std::move(context)) {}

    void setEnabled(bool enabled, bool hoverEnabled = true) {
        m_enabled = enabled;
        m_hoverEnabled = hoverEnabled;
        reconcile();
    }

    void setHoverRect(const QRect& rect, bool cropping = false) {
        if (m_cropping != cropping)
            m_completed = {};
        m_cropping = cropping;
        m_desired = rect;
        reconcile();
    }

    void contentChanged() {
        ++m_contentRevision;
        m_completed = {};
        dispatch();
    }

    // Session teardown cancels even a request whose pipeline will no longer deliver a callback.
    void reset() {
        m_enabled = false;
        m_desired = {};
        m_cropping = false;
        reconcile();
        ++m_requestSerial;
        m_pending = false;
        m_completed = {};
        ++m_contentRevision;
    }

    [[nodiscard]] bool paused() const {
        return m_paused;
    }

  private:
    void reconcile() {
        const bool wanted = m_enabled && (m_hoverEnabled || m_cropping) && !m_desired.isEmpty();
        if (!wanted) {
            if (!m_paused)
                return;
            ++m_epoch;
            m_paused = false;
            m_ready = false;
            m_completed = {};
            m_context.clear();
            m_context.resume();
            return;
        }
        if (m_paused) {
            dispatch();
            return;
        }
        m_paused = true;
        m_ready = false;
        const quint64 epoch = ++m_epoch;
        const QPointer<ScrollingHoverPreview> receiver(this);
        m_context.pause([receiver, epoch] {
            if (!receiver || receiver->m_epoch != epoch || !receiver->m_paused)
                return;
            receiver->m_ready = true;
            receiver->dispatch();
        });
    }

    void dispatch() {
        if (!m_paused || !m_ready || m_pending || m_desired.isEmpty() || m_desired == m_completed)
            return;
        const QRect rect = m_desired;
        const quint64 epoch = m_epoch;
        const quint64 contentRevision = m_contentRevision;
        const quint64 serial = ++m_requestSerial;
        m_pending = true;
        const QPointer<ScrollingHoverPreview> receiver(this);
        const bool accepted =
            m_context.request(rect, [receiver, rect, epoch, contentRevision, serial](QImage image) {
                if (!receiver || serial != receiver->m_requestSerial)
                    return;
                receiver->m_pending = false;
                if (receiver->m_paused && receiver->m_ready && epoch == receiver->m_epoch &&
                    contentRevision == receiver->m_contentRevision && rect == receiver->m_desired) {
                    receiver->m_completed = rect;
                    if (image.isNull())
                        receiver->m_context.clear();
                    else
                        receiver->m_context.present(image, receiver->m_cropping);
                }
                receiver->dispatch();
            });
        if (!accepted && serial == m_requestSerial) {
            m_pending = false;
            m_completed = rect;
            m_context.clear();
        }
    }

    Context m_context;
    QRect m_desired;
    QRect m_completed;
    quint64 m_epoch = 0;
    quint64 m_contentRevision = 0;
    quint64 m_requestSerial = 0;
    bool m_enabled = false;
    bool m_hoverEnabled = true;
    bool m_paused = false;
    bool m_ready = false;
    bool m_pending = false;
    bool m_cropping = false;
};

} // namespace snow_shot::capture_detail
