// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "snow_shot/presentation/screenshotairdropservice.h"

#include <memory>
#include <utility>

template <typename Finish, typename Shared, typename Restore>
auto screenshotAirDropCompletion(std::shared_ptr<bool> completed, Finish finish, Shared shared,
                                 Restore restore) {
    return
        [completed = std::move(completed), finish = std::move(finish), shared = std::move(shared),
         restore = std::move(restore)](ScreenshotAirDropService::Result result) {
            if (std::exchange(*completed, true) || !finish())
                return;
            if (result.status == ScreenshotAirDropService::Status::Shared)
                shared();
            else
                restore(std::move(result));
        };
}
