#pragma once

#include <QFuture>
#include <QPromise>
#include <QThreadPool>
#include <exception>
#include <type_traits>
#include <utility>

namespace snow_shot::app::mcp {
// Keep bounded application work on the caller's pool. QtCore supplies promises
// and completion signals even in the application's minimal static Qt build.
template <typename Work> auto runMcpWork(QThreadPool* pool, Work work) {
    using Result = std::invoke_result_t<Work>;
    QPromise<Result> promise;
    auto future = promise.future();
    promise.start();
    pool->start([promise = std::move(promise), work = std::move(work)]() mutable {
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
    });
    return future;
}
} // namespace snow_shot::app::mcp
