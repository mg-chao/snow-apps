// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <QImage>
#include <CoreGraphics/CoreGraphics.h>

namespace snow_shot::platform::macos {
// Retained result; the provider owns immutable RGB32 pixels independently of the source window.
[[nodiscard]] CGImageRef createPrintImage(QImage prepared);
} // namespace snow_shot::platform::macos
