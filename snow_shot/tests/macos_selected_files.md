# Finder selected-file pinning

Build `snow_shot`, `snow-shot-macos-selected-files-tests`,
`snow-shot-file-pin-batch-tests`, and `snow-shot-macos-selected-files-native-smoke`
with the macOS debug preset. The deterministic tests never contact Finder or
request privacy permission.

## Interactive backend smoke test

Select two known image files in Finder. From a terminal run the built smoke
bundle's executable with `SNOW_TEST_FINDER_SELECTION=1` and the absolute paths of
both selected files as separate quoted arguments. The executable is inside
`snow-shot-macos-selected-files-native-smoke.app/Contents/MacOS/` under the build
tree. Approve its Finder Automation prompt. It checks the selected paths and
that the clipboard is unchanged. It skips without the environment variable and
expected paths. Repeat with desktop selections and with Finder in the background.
The smoke bundle has a separate permission identity from Snow Shot.

## Application acceptance checks

- Select multiple supported images in Finder and invoke the configured global
  shortcut. Approve the first-use Finder Automation prompt. Each image should
  appear once, using the existing auto-resize preference on the cursor's screen.
- Repeat from Snow Shot's settings quick action and configured menu-bar action,
  including while another app is active. Finder must not activate to read files.
- Select desktop images, Unicode names, spaces, `%`, and `#` in names. Check the
  correct images appear. Mix images with folders, unsupported and corrupt files;
  only supported, readable images should pin.
- Clear Finder's selection: no pin or warning should appear.
- Deny Finder Automation for Snow Shot and invoke the action. The warning must
  explain System Settings > Privacy & Security > Automation. Enable access there
  and retry; pinning should work without restarting Snow Shot.
- Replace a pending pin request with another content-pin action; no stale images
  or warnings may appear. Quitting during a pending request must not crash.
- Verify English, Simplified Chinese, and Traditional Chinese action descriptions
  and warning messages after switching application language.
