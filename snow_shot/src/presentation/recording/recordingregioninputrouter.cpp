#include "recordingregioninputrouter.h"

#include <QCursor>
#include <QGuiApplication>
#include <QObject>
#include <QThread>
#include <atomic>
#include <bit>
#include <utility>

#ifdef Q_OS_MACOS
#import <AppKit/AppKit.h>
#endif

class RecordingRegionInputRouter::Impl final {
  public:
#ifdef Q_OS_WIN
    Impl(Handler changed, RecordingRegionMouseApi api)
        : changed(std::move(changed)), api(std::move(api)) {
#else
    explicit Impl(Handler changed) : changed(std::move(changed)) {
#endif
#ifdef Q_OS_WIN
        if (!(this->api.supported ? this->api.supported()
                                  : QGuiApplication::platformName() == QStringLiteral("windows")))
            return;
        worker = new QObject;
        worker->moveToThread(&thread);
        QObject::connect(&thread, &QThread::finished, worker, &QObject::deleteLater);
        QObject::connect(&thread, &QThread::started, worker, [this] {
            current = this;
            hook = this->api.installHook(WH_MOUSE_LL, mouseCallback, GetModuleHandleW(nullptr), 0);
            if (!hook)
                qWarning("Failed to observe recording region pointer input: %lu", GetLastError());
        });
        thread.setObjectName(QStringLiteral("RecordingRegionMouseObserver"));
        thread.start();
#elif defined(Q_OS_MACOS)
        if (QGuiApplication::platformName() != QStringLiteral("cocoa"))
            return;
        constexpr NSEventMask mask = NSEventMaskMouseMoved | NSEventMaskLeftMouseDragged |
                                     NSEventMaskRightMouseDragged | NSEventMaskOtherMouseDragged |
                                     NSEventMaskLeftMouseDown | NSEventMaskRightMouseDown |
                                     NSEventMaskOtherMouseDown | NSEventMaskLeftMouseUp |
                                     NSEventMaskRightMouseUp | NSEventMaskOtherMouseUp;
        localMonitor = [NSEvent addLocalMonitorForEventsMatchingMask:mask
                                                             handler:^NSEvent*(NSEvent* event) {
                                                               this->changed(QCursor::pos());
                                                               return event;
                                                             }];
        globalMonitor = [NSEvent addGlobalMonitorForEventsMatchingMask:mask
                                                               handler:^(NSEvent*) {
                                                                 this->changed(QCursor::pos());
                                                               }];
#endif
    }

    ~Impl() {
#ifdef Q_OS_WIN
        if (worker) {
            QMetaObject::invokeMethod(
                worker,
                [this] {
                    if (hook)
                        api.removeHook(hook);
                    hook = nullptr;
                    current = nullptr;
                },
                Qt::BlockingQueuedConnection);
            thread.quit();
            thread.wait();
        }
        // delivery is destroyed after the worker stops posting. QObject removes
        // its queued updates, so none can reach a hidden or destroyed recording area.
#elif defined(Q_OS_MACOS)
        if (localMonitor)
            [NSEvent removeMonitor:localMonitor];
        if (globalMonitor)
            [NSEvent removeMonitor:globalMonitor];
#endif
    }

  private:
    Handler changed;
#ifdef Q_OS_WIN
    void observe(const POINT& point) {
        latest.store((quint64(quint32(point.x)) << 32) | quint32(point.y));
        if (pending.exchange(true))
            return;
        QMetaObject::invokeMethod(
            &delivery,
            [this] {
                pending.store(false);
                const quint64 point = latest.load();
                const auto handler = changed;
                handler(QPoint(std::bit_cast<qint32>(quint32(point >> 32)),
                               std::bit_cast<qint32>(quint32(point))));
            },
            Qt::QueuedConnection);
    }

    static LRESULT CALLBACK mouseCallback(int code, WPARAM message, LPARAM data) {
        if (code == HC_ACTION && current)
            current->observe(reinterpret_cast<const MSLLHOOKSTRUCT*>(data)->pt);
        // No QWidget access, native style changes, or synchronous UI calls here.
        return current ? current->api.nextHook(nullptr, code, message, data)
                       : CallNextHookEx(nullptr, code, message, data);
    }

    static thread_local Impl* current;
    RecordingRegionMouseApi api;
    QObject delivery;
    QThread thread;
    QObject* worker = nullptr;
    HHOOK hook = nullptr;
    std::atomic<quint64> latest{0};
    std::atomic<bool> pending{false};
#elif defined(Q_OS_MACOS)
    id localMonitor = nil;
    id globalMonitor = nil;
#endif
};

#ifdef Q_OS_WIN
thread_local RecordingRegionInputRouter::Impl* RecordingRegionInputRouter::Impl::current = nullptr;
#endif

#ifdef Q_OS_WIN
RecordingRegionInputRouter::RecordingRegionInputRouter(Handler changed)
    : RecordingRegionInputRouter(std::move(changed), RecordingRegionMouseApi{}) {}

RecordingRegionInputRouter::RecordingRegionInputRouter(Handler changed, RecordingRegionMouseApi api)
    : m_impl(std::make_unique<Impl>(std::move(changed), std::move(api))) {}
#else
RecordingRegionInputRouter::RecordingRegionInputRouter(Handler changed)
    : m_impl(std::make_unique<Impl>(std::move(changed))) {}
#endif

RecordingRegionInputRouter::~RecordingRegionInputRouter() = default;
