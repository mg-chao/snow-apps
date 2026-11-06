// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#include "snow_shot/image/imageimportlimits.h"

#include <QString>
#include <QtGlobal>

#include <memory>

namespace snow_shot::presentation::image_stitching_detail {
inline constexpr qint64 maximumRasterBytes = image::maximumImageImportBytes;
inline constexpr qint64 maximumWindowPreviewBytes = qint64{64} * 1024 * 1024;
inline constexpr qint64 maximumWorkingBytes = qint64{512} * 1024 * 1024;

class ResourceReservation final {
  public:
    explicit ResourceReservation(bool working = false, bool preview = false)
        : m_working(working), m_preview(preview) {}
    ~ResourceReservation();
    ResourceReservation(const ResourceReservation&) = delete;
    ResourceReservation& operator=(const ResourceReservation&) = delete;
    bool resize(qint64 bytes, QString* error = nullptr);

  private:
    const bool m_working;
    const bool m_preview;
};

[[nodiscard]] std::shared_ptr<ResourceReservation> reserve(qint64 bytes, QString* error = nullptr);
[[nodiscard]] std::shared_ptr<ResourceReservation> reserveWorking(qint64 bytes,
                                                                  QString* error = nullptr);
[[nodiscard]] std::shared_ptr<ResourceReservation> reservePreview(qint64 bytes,
                                                                  QString* error = nullptr);
[[nodiscard]] qint64 reservedBytes();
[[nodiscard]] qint64 budgetLimit();
} // namespace snow_shot::presentation::image_stitching_detail
