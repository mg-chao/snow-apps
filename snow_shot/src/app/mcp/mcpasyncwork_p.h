#pragma once

#include "snow_shot/platform/applicationqos.h"
#include "snow_shot/runtime/runtimeactivitytracker.h"

#include <QFuture>
#include <QMetaObject>
#include <QPromise>
#include <QThreadPool>
#include <exception>
#include <type_traits>
#include <utility>

namespace snow_shot::app::mcp {
// Reserve queued work before submission, including work that survives request cancellation.
template <typename Work> bool queueMcpWork(QObject* receiver, Work work) {
    return QMetaObject::invokeMethod(receiver, runtime::trackRuntimeWork(std::move(work)),
                                     Qt::QueuedConnection);
}

// Keep bounded application work on the caller's pool. QtCore supplies promises
// and completion signals even in the application's minimal static Qt build.
template <typename Work> auto runMcpWork(QThreadPool* pool, Work work) {
    using Result = std::invoke_result_t<Work>;
    QPromise<Result> promise;
    auto future = promise.future();
    promise.start();
    pool->start(snow_shot::runtime::trackRuntimeWork(
        [promise = std::move(promise), work = std::move(work)]() mutable {
            platform::applyApplicationQoSToCurrentThread();
            try {
                if (!promise.isCanceled()) {
                    if constexpr (std::is_void_v<Result>)
                        work();
                    else
                        promise.addResult(work());
                }
            } catch (...) {
                promise.setException(std::current_exception());
            }
            promise.finish();
        }));
    return future;
}
} // namespace snow_shot::app::mcp
