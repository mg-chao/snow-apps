#ifndef SNOW_SHOT_PLATFORM_MACOS_IMAGECLIPBOARD_H
#define SNOW_SHOT_PLATFORM_MACOS_IMAGECLIPBOARD_H

class QByteArray;
class QClipboard;

namespace snow_shot::platform::macos {
void initializeImageClipboardConverter();
// Cocoa only. The pasteboard owns encoded data; the app retains no image payload.
[[nodiscard]] bool publishImageClipboard(QClipboard* clipboard, const QByteArray& png,
                                         const QByteArray& placement, const QByteArray& appearance);
// True only for this process's current successful publication: its TIFF is
// derived from its PNG, so a reader retaining PNG needs no bitmap fallback.
// Callers must check the clipboard revision around the complete snapshot.
[[nodiscard]] bool imageClipboardBitmapIsDerivedFromPng();
} // namespace snow_shot::platform::macos

#endif
