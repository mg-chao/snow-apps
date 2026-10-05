#include "snow_shot/presentation/screenshotprintservice.h"
#include "../src/presentation/services/nativeprintbackend.h"

#include <QApplication>
#include <QLabel>
#include <QPainter>
#include <QPushButton>
#include <QVBoxLayout>
#include <QWidget>

#ifdef Q_OS_WIN
#include <qt_windows.h>

class NativePrintFixtureWindow final : public QWidget {
  public:
    void beginPrint() {
        m_topmostPreserved = isNativeTopmost();
        m_observingPrint = true;
    }
    bool finishPrint() {
        m_topmostPreserved = m_topmostPreserved && isNativeTopmost();
        m_observingPrint = false;
        return m_topmostPreserved;
    }

  protected:
    bool nativeEvent(const QByteArray& eventType, void* message, qintptr* result) override {
        if (m_observingPrint)
            m_topmostPreserved = m_topmostPreserved && isNativeTopmost();
        return QWidget::nativeEvent(eventType, message, result);
    }

  private:
    bool isNativeTopmost() const {
        return (GetWindowLongPtrW(reinterpret_cast<HWND>(internalWinId()), GWL_EXSTYLE) &
                WS_EX_TOPMOST) != 0;
    }
    bool m_observingPrint = false;
    bool m_topmostPreserved = true;
};
#else
using NativePrintFixtureWindow = QWidget;
#endif

// Interactive fixture: no printer job starts until the tester clicks Print and
// accepts the native dialog. Backend selection is intentionally confined to tests.
int main(int argc, char** argv) {
    QApplication app(argc, argv);
    auto primary =
        screenshotNativePrintBackend(app.arguments().contains(QStringLiteral("--legacy")));
#ifdef Q_OS_WIN
    if (app.arguments().contains(QStringLiteral("--classic")))
        primary = screenshotClassicWindowsPrintBackend();
#endif
    if (app.arguments().contains(QStringLiteral("--modern-unavailable")))
        primary = [](QWidget*, QImage, auto done) {
            done({ScreenshotPrintService::Status::Unavailable, {}});
        };
    ScreenshotPrintService printer(std::move(primary), screenshotNativePrintBackend(true));
    NativePrintFixtureWindow window;
    window.setWindowTitle(QStringLiteral("SnowShot native print fixture"));
    window.setWindowFlag(Qt::WindowStaysOnTopHint);
    auto* layout = new QVBoxLayout(&window);
    QImage image(1200, 600, QImage::Format_ARGB32_Premultiplied);
    image.fill(Qt::transparent);
    {
        QPainter painter(&image);
        painter.setRenderHint(QPainter::Antialiasing);
        painter.fillRect(QRect(40, 40, 1120, 520), QColor(220, 240, 255, 180));
        painter.setPen(QPen(Qt::red, 8));
        painter.drawRect(QRect(60, 60, 1080, 480));
        painter.setPen(Qt::black);
        painter.setFont(QFont(QStringLiteral("Arial"), 32));
        painter.drawText(
            QRect(80, 80, 1040, 440), Qt::AlignCenter,
            QStringLiteral("SnowShot\nNative one-page image print\nTop left → bottom right"));
    }
    auto* preview = new QLabel(&window);
    preview->setPixmap(
        QPixmap::fromImage(ScreenshotPrintService::opaqueImage(image)).scaledToWidth(600));
    layout->addWidget(preview);
    auto* status = new QLabel(QStringLiteral("Ready"), &window);
    layout->addWidget(status);
    auto* print = new QPushButton(QStringLiteral("Print"), &window);
    layout->addWidget(print);
    QObject::connect(print, &QPushButton::clicked, &window, [&] {
        print->setEnabled(false);
#ifdef Q_OS_WIN
        window.beginPrint();
#endif
        if (!printer.printImage(&window, &window, image, [&](auto result) {
                print->setEnabled(true);
                const QString outcome = result.status == ScreenshotPrintService::Status::Submitted
                                            ? QStringLiteral("Submitted")
                                        : result.status == ScreenshotPrintService::Status::Cancelled
                                            ? QStringLiteral("Cancelled")
                                        : result.status == ScreenshotPrintService::Status::HandedOff
                                            ? QStringLiteral("Photo dialog closed")
                                            : QStringLiteral("Failed");
                status->setText(outcome + QLatin1Char(' ') + result.error);
#ifdef Q_OS_WIN
                status->setText(status->text() + (window.finishPrint()
                                                      ? QStringLiteral(" | Topmost preserved")
                                                      : QStringLiteral(" | Topmost changed")));
#endif
            })) {
#ifdef Q_OS_WIN
            window.finishPrint();
#endif
            print->setEnabled(true);
        }
    });
    window.show();
    return app.exec();
}
