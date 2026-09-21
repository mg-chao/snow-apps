# macOS repeated file-dialog cancellation

The September 22, 2026 crash report for Snow Shot 1.0.9 stops at:

```text
QWidget::accessibleName()                  [null QWidget, address 0x8]
QAccessibleWidget::text(QAccessible::Name)
QWidgetPrivate::setWindowTitle_sys()
QWidget::create()
QDialog::exec()
QFileDialog::getSaveFileName()
ScreenshotController::Impl::saveSelectionToFile()
```

Qt 6.11.1's accessibility lookup can return a cached interface without validating
its object. Native window creation then reads its name without a validity check.
An expired interface retained under a reused widget address reproduces this stack.

The original sequence that orphaned the interface has **not** been established.
Plain native open/cancel loops, including loops using the screenshot stacking
policy, did not reproduce the original intermittent failure. Do not attribute it
to a particular application callback or to the fault-injection method below.

Applications call `adqt::widgets::initializePlatformCompatibility(app)` immediately
after constructing QApplication, before creating widgets. This idempotent,
application-owned integration is independent of theme selection; Snow Shot,
Snow Image Viewer, and the AdQt demo initialize it explicitly.

The macOS AdQt integration repairs an invalid cache association at
`QPlatformSurfaceEvent::SurfaceCreated`, before Qt reads the widget's accessible
name. It leaves valid associations alone. Recovery uses Qt's cache destruction
slot to remove **all** interfaces associated with the address, including those
created under base-class metaobjects during construction. Deleting only the
expired interface through the public API leaves its object-to-ID mapping behind.
Normal Qt lookup subsequently recreates the interface, with accessibility still
enabled. A warning records any recovery for further diagnosis.

This is a compatibility repair for the observed failure mechanism, not proof
that the original source of cache corruption has been eliminated. It depends on
Qt's private cache API, already available through AdQt's macOS `GuiPrivate`
dependency. Qt 6.11.1 is the validated version; CMake warns on other versions
without silently disabling protection. A missing private cleanup method fails
explicitly in release builds as well as debug builds instead of continuing into
the known invalid dereference.

When upgrading Qt, inspect accessible-interface lookup and destruction cleanup,
and rerun the regression offscreen and on Cocoa. Remove the repair when upstream
Qt safely rejects/rebuilds expired associations and the regression passes without
the repair. This removal criterion does not establish the original cause of the
orphaned association; that still requires a production reproduction.

## Focused regression

```sh
scripts/build.sh snow-shot-macos-arm64-debug --target adqt-widget-accessibility-repair-tests
ctest --preset test-snow-shot-macos-arm64-debug -R '^adqt-widget-accessibility-repair-tests$'
QT_QPA_PLATFORM=cocoa build/snow-shot-macos-arm64-debug/ant_design_qt/adqt-widget-accessibility-repair-tests
```

The test deliberately disconnects destruction cleanup and reuses storage to
inject the expired-cache condition deterministically. It covers Save, Open, and
directory dialogs; cancellation and reopening; removal of base-class cache
entries; preservation of live interfaces and accessible names; and cleanup after
recovery. The Cocoa run also opens and cancels real `NSSavePanel`/`NSOpenPanel`
instances. The offscreen run exercises the same cache repair without a desktop.
