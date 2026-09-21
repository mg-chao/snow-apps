#include "widgets/platform_compatibility.h"

#import <AppKit/AppKit.h>

#include <QAccessible>
#include <QApplication>
#include <QFileDialog>
#include <QTimer>
#include <QWidget>
#include <cstdlib>
#include <iostream>
#include <new>

namespace {
void require(bool condition, const char* message) {
    if (!condition) {
        std::cerr << message << '\n';
        std::exit(EXIT_FAILURE);
    }
}

QAccessible::Id leaveExpiredInterface(QWidget* widget) {
    auto* accessible = QAccessible::queryAccessibleInterface(widget);
    require(accessible && accessible->isValid(), "the initial accessible interface must be valid");
    const auto id = QAccessible::uniqueId(accessible);
    // Fault injection, not an assertion about the original source of the orphan:
    // suppress Qt's destruction cleanup to reproduce the expired cached interface
    // seen in the Save dialog crash. Placement new makes address reuse deterministic.
    QObject::disconnect(widget, nullptr, nullptr, nullptr);
    return id;
}

void repeatedFileDialogsRecoverExpiredInterfaces() {
    alignas(QFileDialog) unsigned char memory[sizeof(QFileDialog)];
    for (int cycle = 0; cycle != 12; ++cycle) {
        auto* base = new (memory) QWidget;
        const auto baseId = leaveExpiredInterface(base);
        base->~QWidget();
        auto* old = new (memory) QFileDialog;
        const auto dialogId = leaveExpiredInterface(old);
        old->~QFileDialog();
        require(QAccessible::accessibleInterface(dialogId) &&
                    !QAccessible::accessibleInterface(dialogId)->isValid(),
                "the test must retain an expired dialog interface");

        auto* dialog = new (memory) QFileDialog;
        const bool nativePanel =
            QApplication::platformName() == QStringLiteral("cocoa") && cycle >= 6;
        dialog->setOption(QFileDialog::DontUseNativeDialog, !nativePanel);
        dialog->setWindowTitle(QStringLiteral("File dialog accessibility regression"));
        dialog->setAccessibleName(QStringLiteral("Accessible file picker"));
        dialog->setAcceptMode(cycle % 3 == 0 ? QFileDialog::AcceptSave : QFileDialog::AcceptOpen);
        dialog->setFileMode(cycle % 3 == 2 ? QFileDialog::Directory : QFileDialog::AnyFile);
        bool inspectedNativePanel = false;
        QTimer cancel;
        QObject::connect(&cancel, &QTimer::timeout, dialog, [&] {
            if (nativePanel) {
                for (NSWindow* window in NSApp.windows) {
                    if (window.visible && [window isKindOfClass:[NSSavePanel class]]) {
                        inspectedNativePanel = true;
                        dialog->reject();
                        return;
                    }
                }
            } else {
                dialog->reject();
            }
        });
        cancel.start(0);
        QTimer::singleShot(5000, dialog,
                           [] { require(false, "file dialog did not become cancellable"); });
        require(dialog->exec() == QDialog::Rejected, "cancel must reject the file dialog");
        cancel.stop();
        require(!nativePanel || inspectedNativePanel,
                "the Cocoa run must exercise real file panels");
        require(!QAccessible::accessibleInterface(baseId) &&
                    !QAccessible::accessibleInterface(dialogId),
                "recovery must remove every construction-time interface for the reused address");
        auto* recovered = QAccessible::queryAccessibleInterface(dialog);
        require(recovered && recovered->isValid() && recovered->object() == dialog,
                "the reopened dialog must have a live accessible interface");
        require(recovered->text(QAccessible::Name) == dialog->accessibleName(),
                "recovery must preserve the dialog's accessible name");
        const auto recoveredId = QAccessible::uniqueId(recovered);
        dialog->~QFileDialog();
        require(!QAccessible::accessibleInterface(recoveredId),
                "the recovered interface must follow normal destruction cleanup");
    }
}

class RecreatedWidget final : public QWidget {
  public:
    void recreateSurface() {
        hide();
        destroy();
        show();
    }
};

void validInterfacesSurviveNativeCreation() {
    RecreatedWidget widget;
    widget.setWindowTitle(QStringLiteral("Live window"));
    auto* accessible = QAccessible::queryAccessibleInterface(&widget);
    const auto id = QAccessible::uniqueId(accessible);
    widget.show();
    require(QAccessible::queryAccessibleInterface(&widget) == accessible &&
                QAccessible::uniqueId(accessible) == id,
            "native creation must not invalidate a live accessibility client");
    widget.recreateSurface();
    require(QAccessible::queryAccessibleInterface(&widget) == accessible &&
                QAccessible::uniqueId(accessible) == id,
            "surface recreation must preserve a live widget's accessible interface");
    widget.setWindowTitle(QStringLiteral("Renamed window"));
    require(accessible->text(QAccessible::Name) == widget.windowTitle(),
            "normal accessibility title updates must keep working");
}
} // namespace

int main(int argc, char** argv) {
    QApplication app(argc, argv);
    app.setQuitOnLastWindowClosed(false);
    QAccessible::setActive(true);
    // Repair must work without applying any AdQt theme.
    adqt::widgets::initializePlatformCompatibility(app);
    // Repeated startup initialization must not install duplicate handlers.
    adqt::widgets::initializePlatformCompatibility(app);
    QTimer::singleShot(0, &app, [&app] {
        validInterfacesSurviveNativeCreation();
        repeatedFileDialogsRecoverExpiredInterfaces();
        app.quit();
    });
    return app.exec();
}
