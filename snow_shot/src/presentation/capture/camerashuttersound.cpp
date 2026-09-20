#include "camerashuttersound.h"
#include <QCoreApplication>
#include <QDir>
#include <QFileInfo>
#include <QTimer>
#include <QDebug>
#if defined(Q_OS_WIN)
#include <windows.h>
#include <mmsystem.h>
#endif

#if defined(Q_OS_WIN) || defined(_WIN32)
namespace {
QString cameraShutterAudioPath() {
    const QString installedPath = QDir(QCoreApplication::applicationDirPath())
                                      .filePath(QStringLiteral("audios/camera_shutter.mp3"));
    if (QFileInfo(installedPath).isFile()) {
        return installedPath;
    }
    return QStringLiteral(SNOW_SHOT_CAMERA_SHUTTER_AUDIO_SOURCE);
}

QString mediaControlError(MCIERROR error) {
    wchar_t message[256]{};
    if (mciGetErrorStringW(error, message, 256) != FALSE) {
        return QString::fromWCharArray(message);
    }
    return QString::number(error);
}

MCIERROR sendMediaControlCommand(const QString& command) {
    const std::wstring nativeCommand = command.toStdWString();
    return mciSendStringW(nativeCommand.c_str(), nullptr, 0, nullptr);
}
} // namespace

void playCameraShutterSound() {
    const QString audioPath = cameraShutterAudioPath();
    if (!QFileInfo(audioPath).isFile()) {
        qWarning("Camera shutter audio is unavailable: %s", qPrintable(audioPath));
        return;
    }

    static quint64 playbackId = 0;
    const QString alias = QStringLiteral("snow_shot_camera_shutter_%1").arg(++playbackId);
    const MCIERROR openError =
        sendMediaControlCommand(QStringLiteral("open \"%1\" type mpegvideo alias %2")
                                    .arg(QDir::toNativeSeparators(audioPath), alias));
    if (openError != 0) {
        qWarning("Failed to open camera shutter audio: %s",
                 qPrintable(mediaControlError(openError)));
        return;
    }

    const MCIERROR playError = sendMediaControlCommand(QStringLiteral("play %1 from 0").arg(alias));
    if (playError != 0) {
        qWarning("Failed to play camera shutter audio: %s",
                 qPrintable(mediaControlError(playError)));
        static_cast<void>(sendMediaControlCommand(QStringLiteral("close %1").arg(alias)));
        return;
    }
    QTimer::singleShot(5000, QCoreApplication::instance(), [alias]() {
        static_cast<void>(sendMediaControlCommand(QStringLiteral("close %1").arg(alias)));
    });
}
#elif !defined(Q_OS_MACOS)
void playCameraShutterSound() {}
#endif
