#include "snow_shot/presentation/canvasstatusreadout.h"

#include <QApplication>
#include <QEvent>
#include <QImage>
#include <QPainter>
#include <QTimer>
#include <QTranslator>
#include <QtMath>

#include <array>
#include <iostream>
#include <limits>
#include <stdexcept>

namespace {
using Message = CanvasStatusReadout::Message;
using Corner = CanvasStatusReadout::Corner;
using DisplayMode = CanvasStatusReadout::DisplayMode;

void require(bool condition, const char* message) {
    if (!condition)
        throw std::runtime_error(message);
}

class TransparentCanvas final : public QWidget {
  public:
    TransparentCanvas() {
        setAttribute(Qt::WA_TranslucentBackground);
        setAttribute(Qt::WA_NoSystemBackground);
        resize(320, 240);
    }

  protected:
    void paintEvent(QPaintEvent*) override {}
};

class ReadoutTranslator final : public QTranslator {
  public:
    bool isEmpty() const override {
        return false;
    }

    QString translate(const char* context, const char* source, const char*, int) const override {
        if (qstrcmp(context, "CanvasStatusReadout") != 0)
            return {};
        if (qstrcmp(source, "Scale: %1%") == 0)
            return QStringLiteral("Translated scale: %1%");
        if (qstrcmp(source, "Locked") == 0)
            return QStringLiteral("Translated locked");
        return {};
    }
};

QImage render(TransparentCanvas& canvas, CanvasStatusReadout& readout, qreal dpr, bool overlay) {
    QImage result(QSize(qCeil(canvas.width() * dpr), qCeil(canvas.height() * dpr)),
                  QImage::Format_ARGB32_Premultiplied);
    result.setDevicePixelRatio(dpr);
    result.fill(Qt::transparent);
    QPainter painter(&result);
    if (overlay)
        readout.paintOverlay(painter);
    else
        canvas.render(&painter, QPoint(), QRegion(), QWidget::DrawChildren);
    return result;
}

void paintingUsesOneRoundedBackground() {
    TransparentCanvas canvas;
    CanvasStatusReadout readout(&canvas);
    readout.setMessage(Message::Scale, 125);
    readout.layoutIn(QRectF(canvas.rect()));
    readout.showPersistent();
    canvas.show();
    QApplication::processEvents();
    for (const qreal dpr : std::array{1.0, 1.25, 1.5, 1.75, 2.0}) {
        const QImage ordinary = render(canvas, readout, dpr, false);
        const QImage overlay = render(canvas, readout, dpr, true);
        require(ordinary == overlay,
                "ordinary child painting and overlay composition must produce the same pixels");
        const QPoint corner(qFloor((readout.x() + 0.2) * dpr), qFloor((readout.y() + 0.2) * dpr));
        const QPoint padding(qFloor((readout.x() + readout.width() / 2.0) * dpr),
                             qFloor((readout.y() + readout.height() - 2.0) * dpr));
        require(overlay.pixelColor(corner).alpha() == 0,
                "rounded corners must remain transparent at every device pixel ratio");
        require(overlay.pixelColor(padding) == QColor(0, 0, 0, 150),
                "the background must contain exactly one alpha-150 black paint pass");
    }
    readout.dismiss();
    const QImage empty = render(canvas, readout, 1.5, true);
    require(empty.pixelColor(qRound(readout.x() * 1.5), qRound(readout.y() * 1.5)).alpha() == 0,
            "dismissed hints must not be composed");
}

void messagesRemainPassiveAndAccessible() {
    TransparentCanvas canvas;
    canvas.resize(800, 240);
    CanvasStatusReadout readout(&canvas);
    require(readout.testAttribute(Qt::WA_TransparentForMouseEvents) &&
                !readout.testAttribute(Qt::WA_NativeWindow) &&
                readout.focusPolicy() == Qt::NoFocus && readout.textFormat() == Qt::PlainText &&
                readout.isHidden(),
            "readouts must begin hidden and accept neither pointer nor keyboard input");
    const std::array messages{Message::Scale,         Message::Opacity,
                              Message::Locked,        Message::MotionPreview,
                              Message::ResultPreview, Message::RenderingPreview};
    const std::array expected{QStringLiteral("Scale: 125%"),
                              QStringLiteral("Opacity: 125%"),
                              QStringLiteral("Locked"),
                              QStringLiteral("Motion preview in progress"),
                              QStringLiteral("Result preview in progress"),
                              QStringLiteral("Rendering preview")};
    for (size_t i = 0; i < messages.size(); ++i) {
        readout.setMessage(messages[i], 125);
        require(readout.message() == messages[i] && readout.text() == expected[i] &&
                    readout.toolTip() == expected[i] && readout.accessibleName() == expected[i],
                "all messages must share formatting and complete tooltip/accessibility text");
    }
    readout.setMessage(Message::Scale, 8.75, 1);
    require(readout.text() == QStringLiteral("Scale: 8.8%"),
            "export scale messages must retain requested decimal precision");
    readout.setMessage(Message::Scale, qRound(82.5));
    require(readout.text() == QStringLiteral("Scale: 83%"),
            "pinned/original consumers must retain their integer percentage rounding");
    readout.setMessage(Message::MotionPreview);
    readout.layoutIn(QRectF(10.25, 20.25, 80.5, 28.5));
    require(readout.x() == 19 && readout.width() <= 63 && readout.text() != readout.toolTip() &&
                readout.toolTip() == QStringLiteral("Motion preview in progress") &&
                readout.accessibleName() == readout.toolTip(),
            "fractional content bounds must round inward and elide only visible text");
    readout.layoutIn(QRectF(25, 30, 210, 150), Corner::TopLeft);
    require(readout.pos() == QPoint(33, 38),
            "top-left notices must use the shared eight-pixel inset");
    readout.layoutIn(QRectF(25, 30, 210, 150));
    require(readout.x() == 33 && readout.y() + readout.height() == 172,
            "bottom-left notices must use the shared eight-pixel inset");
    readout.layoutIn(QRectF(-10, -10, 400, 300));
    require(readout.x() == 8 && readout.y() + readout.height() == 232,
            "anchors must stay inside their parent bounds");
    readout.layoutIn(QRectF(25.1, 30.1, 3.7, 3.7));
    require(QRect(26, 31, 2, 2).contains(readout.geometry()),
            "tiny content bounds must contain the complete readout geometry");
    readout.showPersistent();
    readout.layoutIn(QRectF());
    require(readout.isHidden() && !readout.dismissalTimer()->isActive(),
            "empty anchors must dismiss immediately");
    readout.layoutIn(QRectF(std::numeric_limits<qreal>::quiet_NaN(), 0, 80, 80));
    require(readout.isHidden(), "nonfinite anchors must dismiss safely");
    readout.layoutIn(QRectF(canvas.rect()));
    require(readout.isHidden(), "layout changes must not resurrect a dismissed readout");
}

void deadlinesAndSnapshotsAreDeterministic() {
    TransparentCanvas canvas;
    CanvasStatusReadout readout(&canvas);
    readout.setMessage(Message::Scale, 110);
    readout.showTransient();
    auto state = readout.captureState();
    require(state.visible && state.mode == DisplayMode::Transient &&
                readout.dismissalTimer()->parent() == &readout &&
                readout.dismissalTimer()->isSingleShot() && readout.dismissalTimer()->isActive() &&
                readout.dismissalTimer()->interval() == 1000,
            "the shared readout must own its one-second single-shot timer");
    state.deadline = QDeadlineTimer(700, Qt::PreciseTimer);
    readout.restoreState(state);
    const auto shortened = readout.captureState().deadline;
    require(readout.dismissalTimer()->interval() <= 700 && readout.dismissalTimer()->interval() > 0,
            "snapshot restoration must retain remaining lifetime");
    readout.setMessage(Message::Opacity, 85);
    readout.layoutIn(QRectF(20, 30, 160, 100));
    require(readout.captureState().deadline == shortened && !readout.isHidden(),
            "message and layout updates must preserve the transient deadline");
    readout.showTransient();
    require(readout.dismissalTimer()->interval() == 1000 &&
                readout.captureState().deadline > shortened,
            "each explicit transient show must restart the full one-second lifetime");
    require(QMetaObject::invokeMethod(readout.dismissalTimer(), "timeout", Qt::DirectConnection),
            "the timer must expose its timeout signal");
    require(readout.isHidden() && !readout.dismissalTimer()->isActive(),
            "timer expiry must dismiss synchronously");
    readout.setMessage(Message::Locked);
    require(readout.isHidden(), "updating a hidden readout must preserve explicit invisibility");
    readout.showPersistent();
    const auto persistent = readout.captureState();
    require(persistent.mode == DisplayMode::Persistent && persistent.deadline.isForever() &&
                !readout.dismissalTimer()->isActive(),
            "persistent notices must not schedule dismissal");
    readout.setMessage(Message::Scale, 200);
    readout.layoutIn(QRectF(1, 2, 250, 180), Corner::TopLeft);
    readout.dismiss();
    readout.restoreState(persistent);
    require(!readout.isHidden() && readout.message() == Message::Locked &&
                readout.text() == QStringLiteral("Locked") &&
                readout.captureState().anchor == persistent.anchor &&
                readout.captureState().corner == persistent.corner &&
                !readout.dismissalTimer()->isActive(),
            "restoration must recover message, anchor, placement, visibility, and display mode");
    state.deadline = QDeadlineTimer(0, Qt::PreciseTimer);
    readout.restoreState(state);
    require(readout.isHidden() && !readout.dismissalTimer()->isActive() &&
                readout.captureState().mode == DisplayMode::Hidden,
            "expired transient snapshots must remain dismissed");
    state.visible = false;
    state.deadline = QDeadlineTimer(500, Qt::PreciseTimer);
    readout.restoreState(state);
    require(readout.isHidden() && !readout.dismissalTimer()->isActive(),
            "hidden snapshots must not schedule or reveal a readout");
}

void presentationChangesPreserveLifetimeAndVisibility() {
    TransparentCanvas canvas;
    CanvasStatusReadout readout(&canvas);
    readout.setMessage(Message::Scale, 115);
    readout.showTransient();
    const auto deadline = readout.captureState().deadline;
    int changes = 0;
    QObject::connect(&readout, &CanvasStatusReadout::presentationChanged, [&] { ++changes; });
    ReadoutTranslator translator;
    require(QApplication::installTranslator(&translator), "the test translator must install");
    QEvent languageChange(QEvent::LanguageChange);
    QApplication::sendEvent(&readout, &languageChange);
    require(readout.toolTip() == QStringLiteral("Translated scale: 115%") &&
                readout.accessibleName() == readout.toolTip() &&
                readout.captureState().deadline == deadline && !readout.isHidden() && changes > 0,
            "retranslation must refresh complete copy without restarting or hiding the hint");
    const int oldHeight = readout.height();
    QFont larger = readout.font();
    larger.setPixelSize(30);
    readout.setFont(larger);
    require(readout.height() > oldHeight && readout.captureState().deadline == deadline,
            "font changes must relayout without restarting the timer");
    QEvent styleChange(QEvent::StyleChange);
    QApplication::sendEvent(&readout, &styleChange);
    require(readout.captureState().deadline == deadline,
            "style changes must preserve the dismissal deadline");
    readout.dismiss();
    QApplication::removeTranslator(&translator);
    QApplication::sendEvent(&readout, &languageChange);
    require(readout.isHidden() && !readout.dismissalTimer()->isActive() &&
                readout.toolTip() == QStringLiteral("Scale: 115%"),
            "hidden readouts must retranslate without being revealed");
    larger.setPixelSize(20);
    readout.setFont(larger);
    QApplication::sendEvent(&readout, &styleChange);
    require(readout.isHidden() && !readout.dismissalTimer()->isActive(),
            "font and style changes must also preserve explicit invisibility");
}
} // namespace

int main(int argc, char* argv[]) {
    if (qEnvironmentVariableIsEmpty("QT_QPA_PLATFORM"))
        qputenv("QT_QPA_PLATFORM", QByteArrayLiteral("offscreen"));
    QApplication application(argc, argv);
    try {
        paintingUsesOneRoundedBackground();
        messagesRemainPassiveAndAccessible();
        deadlinesAndSnapshotsAreDeterministic();
        presentationChangesPreserveLifetimeAndVisibility();
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
    std::cout << "Canvas status readout tests passed\n";
    return 0;
}
