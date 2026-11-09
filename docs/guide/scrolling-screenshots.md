# Take scrolling screenshots with Snow Shot

[All guides](README.md) · [简体中文](zh-CN/scrolling-screenshots.md)

Scrolling capture combines overlapping screen images into one longer image.
Use it for a webpage, a conversation or another scrollable desktop view that does
not fit on one screen. It captures the displayed application; it does not export
the underlying webpage document.

## Capture a long page

1. Open the target application and position the content at the start you need.
2. Press **F1** and select the scrollable content area. Avoid fixed navigation,
   unrelated sidebars and other regions that will not move with the content.
3. Choose **Scrolling screenshot** in the capture toolbar. You can also assign
   the scrolling screenshot action a global shortcut in settings.
4. Scroll the target content in steady, overlapping steps and inspect the stitched
   preview. Keep the window position, page zoom and display scaling unchanged.
5. When the needed content is captured, finish scrolling capture using its controls,
   inspect the complete result, and copy or save it.

The scrolling controls also support automatic scrolling and direction selection.
Start with manual scrolling when diagnosing an unfamiliar application so you can
see whether the correct pane receives the scroll input.

## Improve a failed or misaligned capture

| Symptom | What to check |
| --- | --- |
| Repeated header or overlapping toolbar | Make the selection exclude fixed-position content |
| Wrong pane scrolls | Place the pointer over the intended scrollable content and test a small manual scroll |
| Missing or repeated rows | Scroll more slowly and leave enough overlap between frames |
| Images shift while scrolling | Let lazy-loaded images finish loading; avoid moving or animated content |
| Capture stops after changing monitors | Select the region again after finishing display or scaling changes |

Virtualized lists can replace their contents as you scroll, and animation can
change pixels between captures. If stitching remains unreliable, capture smaller
sections or use the application's own export function. A browser's full-page
export and a desktop scrolling screenshot solve different problems.

Review the top, middle and bottom before sharing a long capture. An apparently
successful preview does not guarantee every line is present.

## Related tasks

- [Extract text from the resulting image](offline-ocr.md).
- [Pin a reference](pin-and-record.md).
- [Download the current release](https://github.com/mg-chao/snow-apps/releases/latest).

For a reproducible stitching issue, report the app/version, display scaling,
scroll direction and a non-private sample at
[GitHub Issues](https://github.com/mg-chao/snow-apps/issues).
