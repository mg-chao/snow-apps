# Install Snow Shot and take your first screenshot

[All guides](README.md) · [简体中文](zh-CN/getting-started.md)

Snow Shot combines capture, annotation, local OCR, pinning and screen recording.
Start with the full edition unless you specifically want the smaller Mini package.

## Choose a package

Download from [official GitHub Releases](https://github.com/mg-chao/snow-apps/releases/latest)
or the [official download selector](https://snowshot.top/download.html).

| Your computer or workflow | Package to choose |
| --- | --- |
| Windows PC with an Intel or AMD x64 processor | `windows-x64` |
| Windows PC with an ARM64 processor | `windows-arm64` |
| Mac with Apple silicon | `macos-arm64.dmg` |
| Intel Mac | Full edition `macos-x86_64.dmg` |
| Windows installation without network access | Full edition `offline.exe` |
| Windows without an installation wizard | Extract the complete `portable.zip` archive |

On Windows, inspect **Settings → System → About → System type**. On macOS, check
**About This Mac** for the chip or processor. macOS packages require version 15 or
later. Mini supports both Windows architectures and Apple silicon Macs; it has no
Intel Mac build or offline installer.

The full Windows offline package includes the default OCR resources. Online and
Mini packages can download components on first use. Mini omits table and QR
recognition; enable manual text recognition in its settings when needed.

Package-manager commands are in the [project README](../../README.md#install-snow-shot-on-windows).
Keep using your package manager for upgrades if that is how you installed the app.

## Capture and annotate

1. Launch Snow Shot. On macOS, grant screen recording permission when requested;
   if prompted to restart the application after changing permission, do so.
2. Press **F1**. Select the region or window you want to capture.
3. Add arrows, shapes, text, highlights or blur with the capture toolbar.
4. Review the result, then copy, save or pin it using the toolbar.

Use **Ctrl+F1** (**Command+F1** on macOS) when you want to confirm a selection and copy it directly.
If you need more time to arrange a menu before capture, configure the delayed
screenshot action in the application's shortcut settings.

## Make the shortcuts work on your keyboard

The default capture shortcut is **F1** and clipboard pinning is **F3**.
Some keyboards require **Fn** to send a function key rather than a media key.
Another application may also register the same shortcut. Change the binding in
Snow Shot's shortcut settings if it conflicts with your existing workflow.

Actions such as text recognition and scrolling capture can have their own custom
global bindings. An action shown as unset has no assigned shortcut; do not assume
every toolbar action already has one.

## History and privacy

Use screenshot history to find previous captures. Review the history retention
and storage settings if you work with sensitive material: history can retain the
original screen image beyond the crop you export. Deleting an exported image does
not necessarily remove its history entry.

Capture, annotation and standard OCR run locally. Online translation, configured
AI connections and other network features have their own data handling. Read the
[privacy policy](https://snowshot.top/privacy.html) before enabling those features.

## Next steps

- [Capture a scrolling page](scrolling-screenshots.md).
- [Extract text with local OCR](offline-ocr.md).
- [Pin a reference or record a demonstration](pin-and-record.md).

For a blank or incomplete capture, first check OS screen permissions, the target
application and display configuration. Include those details when
[reporting a reproducible issue](https://github.com/mg-chao/snow-apps/issues).
