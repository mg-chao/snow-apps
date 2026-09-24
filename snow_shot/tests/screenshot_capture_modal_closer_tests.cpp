#include "../src/presentation/capture/screenshotcapturemodalcloser.h"

#include <QApplication>
#include <QCloseEvent>
#include <QWidget>

#include <cstdlib>
#include <iostream>

namespace {
void require(bool condition, const char* message) {
    if (!condition) {
        std::cerr << message << '\n';
        std::exit(1);
    }
}

class IndirectlyClosingModal final : public QWidget {
  protected:
    void closeEvent(QCloseEvent* event) override {
        event->ignore();
        hide();
    }
};

class RefusingModal final : public QWidget {
  protected:
    void closeEvent(QCloseEvent* event) override {
        event->ignore();
    }
};
} // namespace

int main(int argc, char* argv[]) {
    QApplication application(argc, argv);

    QWidget first;
    first.setWindowModality(Qt::ApplicationModal);
    first.show();
    IndirectlyClosingModal second;
    second.setWindowModality(Qt::ApplicationModal);
    second.show();
    QApplication::processEvents();
    require(QApplication::activeModalWidget() == &second,
            "the last dialog should block the screenshot overlay");

    require(snow_shot::presentation::closeActiveCaptureModalWindows(),
            "capture should close every active modal window");
    require(!first.isVisible() && !second.isVisible() &&
                QApplication::activeModalWidget() == nullptr,
            "closed dialogs must no longer block screenshot input");

    RefusingModal refusing;
    refusing.setWindowModality(Qt::ApplicationModal);
    refusing.show();
    QApplication::processEvents();
    require(!snow_shot::presentation::closeActiveCaptureModalWindows(),
            "capture must not start when a dialog refuses to close");
    require(refusing.isVisible() && QApplication::activeModalWidget() == &refusing,
            "a refused close must leave the dialog usable");
    refusing.hide();
    return 0;
}
