#ifndef SNOW_SHOT_RECORDINGEFFECTSTYLE_H
#define SNOW_SHOT_RECORDINGEFFECTSTYLE_H
#include "snow_capture.h"
#include "snow_recording_effects.h"
#include <QString>
#include <QVector>
#include <QByteArray>
#include <QColor>
#include <utility>

struct RecordingKeyboardLabels {
    QVector<QByteArray> text;
    QVector<SnowCaptureKeyboardLabel> entries;
    QVector<SnowRecordingEffectsKeyLabel> previewEntries;

    explicit RecordingKeyboardLabels(bool enabled) {
        if (!enabled) {
            return;
        }
        const auto add = [this](uint32_t key, const QString& label) {
            text.push_back(label.toUtf8());
            entries.push_back({key, reinterpret_cast<const uint8_t*>(text.back().constData()),
                               static_cast<uint32_t>(text.back().size())});
        };
        // Recording key legends are intentionally independent of the application language.
        const std::pair<uint32_t, const char*> names[] = {
            {0x08, "Backspace"},
            {0x09, "Tab"},
            {0x0C, "Clear"},
            {0x0D, "Enter"},
            {0x10, "Shift"},
            {0x11, "Ctrl"},
            {0x12, "Alt"},
            {0x13, "Pause"},
            {0x14, "Caps Lock"},
            {0x1B, "Esc"},
            {0x20, "Space"},
            {0x21, "Page Up"},
            {0x22, "Page Down"},
            {0x23, "End"},
            {0x24, "Home"},
            {0x25, "Left"},
            {0x26, "Up"},
            {0x27, "Right"},
            {0x28, "Down"},
            {0x2C, "Print Screen"},
            {0x2D, "Insert"},
            {0x2E, "Delete"},
            {0x5B, "Win"},
            {0x5D, "Menu"},
            {0x6A, "Num *"},
            {0x6B, "Num +"},
            {0x6C, "Num Separator"},
            {0x6D, "Num -"},
            {0x6E, "Num ."},
            {0x6F, "Num /"},
            {0x90, "Num Lock"},
            {0x91, "Scroll Lock"},
            {0xA5, "AltGr"},
            {0xAD, "Mute"},
            {0xAE, "Volume Down"},
            {0xAF, "Volume Up"},
            {0xB0, "Next Track"},
            {0xB1, "Previous Track"},
            {0xB2, "Stop"},
            {0xB3, "Play/Pause"},
        };
        text.reserve(64);
        entries.reserve(64);
        for (const auto& [key, label] : names) {
            add(key, QString::fromLatin1(label));
        }
        for (uint32_t key = 0x60; key <= 0x69; ++key) {
            add(key, QStringLiteral("Num %1").arg(key - 0x60));
        }
        for (const auto& entry : entries) {
            previewEntries.push_back({entry.key_code, reinterpret_cast<const char*>(entry.utf8)});
        }
    }
};

struct RecordingKeyboardTheme {
    QColor background;
    QColor text;
    QColor border;
    RecordingKeyboardTheme(const QColor& backgroundColor = QColor(0, 0, 0, 204),
                           const QColor& foregroundColor = QColor(Qt::white))
        : background(backgroundColor), text(foregroundColor) {
        const int target = qGray(background.rgb()) < 128 ? 255 : 0;
        const auto blend = [target](int channel) { return qRound(channel * 0.75 + target * 0.25); };
        border = QColor(blend(background.red()), blend(background.green()),
                        blend(background.blue()), background.alpha());
    }
};
#endif
