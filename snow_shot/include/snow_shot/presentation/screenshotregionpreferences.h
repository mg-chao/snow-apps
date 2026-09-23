#pragma once

#include "snow_shot/image/screenshotregiongeometry.h"
#include "snow_shot/storage/applicationstorage.h"
#include "snow_shot/storage/configurationstore.h"

inline ScreenshotRegionType screenshotRegionPreference() {
    auto& storage = snow_shot::storage::ApplicationStorage::instance();
    if (!storage.isInitialized())
        return ScreenshotRegionType::Rectangle;
    return screenshotRegionTypeFromId(storage.configuration()
                                          .value(QStringLiteral("screenshot_selection/region_type"))
                                          .toString());
}

inline void setScreenshotRegionPreference(ScreenshotRegionType type) {
    auto& storage = snow_shot::storage::ApplicationStorage::instance();
    if (storage.isInitialized()) {
        static_cast<void>(storage.configuration().setValue(
            QStringLiteral("screenshot_selection/region_type"), screenshotRegionTypeId(type)));
    }
}
