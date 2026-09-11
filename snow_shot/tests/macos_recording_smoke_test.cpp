#include "snow_capture.h"

#include <QApplication>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QPainter>
#include <QProcess>
#include <QScreen>
#include <QTimer>
#include <QWidget>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <future>
#include <thread>

namespace {
class RecordingFixture final : public QWidget {
  public:
    RecordingFixture() {
        setWindowTitle(QStringLiteral("Snow Shot recording validation"));
        setWindowFlags(Qt::Window | Qt::FramelessWindowHint | Qt::WindowStaysOnTopHint);
        resize(480, 320);
        connect(&timer, &QTimer::timeout, this, [this] {
            frame = (frame + 1) % 20;
            update();
        });
        timer.start(100);
    }

  protected:
    void paintEvent(QPaintEvent*) override {
        QPainter painter(this);
        painter.fillRect(rect(), QColor(20, 50, 100));
        painter.fillRect(QRect(frame * 18, 60, 90, 90), QColor(230, 90, 40));
        painter.setPen(Qt::white);
        QFont font = painter.font();
        font.setPixelSize(28);
        painter.setFont(font);
        painter.drawText(QRect(20, 180, 440, 100), Qt::AlignCenter,
                         QStringLiteral("macOS Recording %1").arg(frame));
    }

  private:
    int frame = 0;
    QTimer timer;
};

bool record(const QJsonObject& region, const QByteArray& output, bool audio, bool hardware) {
    SnowCaptureDirectRecordingConfig config{};
    config.version = SNOW_CAPTURE_DIRECT_RECORDING_CONFIG_VERSION;
    config.struct_size = sizeof(config);
    config.x = region.value(QStringLiteral("x")).toInt();
    config.y = region.value(QStringLiteral("y")).toInt();
    config.width = static_cast<uint32_t>(region.value(QStringLiteral("width")).toInt());
    config.height = static_cast<uint32_t>(region.value(QStringLiteral("height")).toInt());
    config.capture_backend = SNOW_CAPTURE_BACKEND_AUTO;
    config.output_file_utf8 = output.constData();
    config.output_format = output.endsWith(".gif")    ? SNOW_CAPTURE_RECORDING_OUTPUT_FORMAT_GIF
                           : output.endsWith(".apng") ? SNOW_CAPTURE_RECORDING_OUTPUT_FORMAT_APNG
                           : output.endsWith(".webp") ? SNOW_CAPTURE_RECORDING_OUTPUT_FORMAT_WEBP
                                                      : SNOW_CAPTURE_RECORDING_OUTPUT_FORMAT_MP4;
    config.capture_fps = 30;
    config.output_fps = 15;
    config.codec = SNOW_CAPTURE_VIDEO_CODEC_H264;
    config.preset = SNOW_CAPTURE_VIDEO_ENCODING_PRESET_VERYFAST;
    config.encoder_preference = hardware ? SNOW_CAPTURE_ENCODER_PREFERENCE_H264_HARDWARE
                                         : SNOW_CAPTURE_ENCODER_PREFERENCE_SOFTWARE;
    config.enable_system_audio = audio ? 1 : 0;
    config.show_cursor = 1;
    config.mouse_trail_duration_ms = 500;
    config.keyboard_size = 64;
    SnowCaptureRecordingSession* session = nullptr;
    bool ok =
        snow_capture_recording_session_create_direct(&config, &session) == SNOW_CAPTURE_RESULT_OK;
    if (ok) {
        ok = snow_capture_recording_session_start(session) != 0;
    }
    if (ok) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1600));
        ok = snow_capture_recording_session_pause(session) != 0;
        std::this_thread::sleep_for(std::chrono::milliseconds(600));
        ok = snow_capture_recording_session_resume(session) != 0 && ok;
        std::this_thread::sleep_for(std::chrono::milliseconds(1600));
        ok = snow_capture_recording_session_stop(session) == SNOW_CAPTURE_RESULT_OK && ok;
    }
    if (!ok) {
        std::fprintf(stderr, "Recording failed: %s\n", snow_capture_last_error_message());
    }
    if (session != nullptr) {
        snow_capture_recording_session_destroy(session);
    }
    return ok && QFileInfo(QString::fromUtf8(output)).size() > 1024;
}
} // namespace

// Interactive, explicit invocation: a child process draws a known fixture so capture never
// stores unrelated desktop content. The parent runs the same public recording FFI as the app.
int main(int argc, char** argv) {
    QApplication app(argc, argv);
    if (app.arguments().contains(QStringLiteral("--fixture"))) {
        RecordingFixture fixture;
        const QScreen* screen = app.arguments().contains(QStringLiteral("--secondary"))
                                    ? app.screens().last()
                                    : app.primaryScreen();
        fixture.move(screen->availableGeometry().center() - fixture.rect().center());
        fixture.show();
        QTimer::singleShot(500, &app, [&] {
            const QScreen* screen = fixture.screen();
            const QRect geometry = screen->geometry();
            const qreal scale = screen->devicePixelRatio();
            qreal desktopScale = 1.0;
            for (const QScreen* candidate : QGuiApplication::screens()) {
                desktopScale = std::max(desktopScale, candidate->devicePixelRatio());
            }
            const QJsonObject region{
                {QStringLiteral("x"),
                 qRound(geometry.x() * desktopScale + (fixture.x() - geometry.x()) * scale)},
                {QStringLiteral("y"),
                 qRound(geometry.y() * desktopScale + (fixture.y() - geometry.y()) * scale)},
                {QStringLiteral("width"), qRound(fixture.width() * scale)},
                {QStringLiteral("height"), qRound(fixture.height() * scale)},
            };
            const QByteArray json = QJsonDocument(region).toJson(QJsonDocument::Compact);
            std::printf("%s\n", json.constData());
            std::fflush(stdout);
        });
        QTimer::singleShot(60000, &app, &QCoreApplication::quit);
        return app.exec();
    }
    if (app.arguments().size() < 2) {
        std::fprintf(stderr, "Usage: macos-recording-smoke-test OUTPUT.{mp4,gif,apng,webp} "
                             "[--audio] [--hardware]\n");
        return 2;
    }
    const QByteArray output = app.arguments().at(1).toUtf8();
    QProcess fixture;
    QStringList fixtureArguments{QStringLiteral("--fixture")};
    if (app.arguments().contains(QStringLiteral("--secondary"))) {
        fixtureArguments.append(QStringLiteral("--secondary"));
    }
    fixture.start(QCoreApplication::applicationFilePath(), fixtureArguments);
    if (!fixture.waitForStarted() || !fixture.waitForReadyRead(10000)) {
        std::fprintf(stderr, "Cannot launch recording fixture\n");
        return 1;
    }
    const auto region = QJsonDocument::fromJson(fixture.readLine()).object();
    if (region.isEmpty()) {
        std::fprintf(stderr, "Fixture did not provide a capture region\n");
        return 1;
    }
    auto task = std::async(std::launch::async, record, region, output,
                           app.arguments().contains(QStringLiteral("--audio")),
                           app.arguments().contains(QStringLiteral("--hardware")));
    QTimer poll;
    QObject::connect(&poll, &QTimer::timeout, &app, [&] {
        if (task.wait_for(std::chrono::milliseconds(0)) == std::future_status::ready) {
            app.exit(task.get() ? 0 : 1);
        }
    });
    poll.start(30);
    const int result = app.exec();
    fixture.terminate();
    if (!fixture.waitForFinished(3000)) {
        fixture.kill();
        fixture.waitForFinished();
    }
    return result;
}
