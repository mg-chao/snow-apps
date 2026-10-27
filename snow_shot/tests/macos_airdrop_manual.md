# AirDrop device acceptance

Use a macOS build and a second Apple device with AirDrop reception enabled.

1. Capture an image, add an annotation and transparent rounded corners, then open the
   Save as file group. Its default popup order is Save as file, Quick save, Print,
   AirDrop, Upload to Cloud.
2. Choose AirDrop. Check that the recipient chooser appears above the screenshot
   and toolbar. Cancel; the screenshot, edits, focus, and shortcuts must remain usable.
3. Retry and select the receiving device. Accept the transfer there. Verify the PNG
   retains dimensions, transparency, and annotations, and that the screenshot closes
   only after the sharing service confirms the shared attachment. Some macOS AirDrop
   extensions report the same callback on cancellation; without a confirmed attachment,
   Snow Shot conservatively keeps the screenshot open.
4. Repeat with a pinned image after rotation, zoom, and opacity changes. Repeat with
   formatted text, a scrolled table, and a playing animation. Confirm that each PNG
   matches the visible content at invocation (one animation frame) and the pin stays open.
5. With AirDrop unavailable, verify an explanatory error and usable source content.
6. Repeat the toolbar/cancellation checks in Snow Shot Mini and in light/dark themes.

The automated `snow-shot-macos-airdrop-native-tests` opens and cancels the native
chooser twice for each source window family. It never selects a recipient or sends
content. It skips when the native service reports AirDrop unavailable. Device receipt
must be verified separately with the steps above.
