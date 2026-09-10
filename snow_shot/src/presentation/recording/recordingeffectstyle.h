#ifndef SNOW_SHOT_RECORDINGEFFECTSTYLE_H
#define SNOW_SHOT_RECORDINGEFFECTSTYLE_H
#include "snow_capture.h"
#include "snow_recording_effects.h"
#include "snow_shot/presentation/styles/themecolorscheme.h"
#include <QCoreApplication>
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
        const auto keyText = [](const char* source) {
            return QCoreApplication::translate("RecordingKeyboard", source);
        };
        const std::pair<uint32_t, const char*> names[] = {
            {0x08, QT_TRANSLATE_NOOP("RecordingKeyboard", "Backspace")},
            {0x09, QT_TRANSLATE_NOOP("RecordingKeyboard", "Tab")},
            {0x0C, QT_TRANSLATE_NOOP("RecordingKeyboard", "Clear")},
            {0x0D, QT_TRANSLATE_NOOP("RecordingKeyboard", "Enter")},
            {0x10, QT_TRANSLATE_NOOP("RecordingKeyboard", "Shift")},
            {0x11, QT_TRANSLATE_NOOP("RecordingKeyboard", "Ctrl")},
            {0x12, QT_TRANSLATE_NOOP("RecordingKeyboard", "Alt")},
            {0x13, QT_TRANSLATE_NOOP("RecordingKeyboard", "Pause")},
            {0x14, QT_TRANSLATE_NOOP("RecordingKeyboard", "Caps Lock")},
            {0x1B, QT_TRANSLATE_NOOP("RecordingKeyboard", "Esc")},
            {0x20, QT_TRANSLATE_NOOP("RecordingKeyboard", "Space")},
            {0x21, QT_TRANSLATE_NOOP("RecordingKeyboard", "Page Up")},
            {0x22, QT_TRANSLATE_NOOP("RecordingKeyboard", "Page Down")},
            {0x23, QT_TRANSLATE_NOOP("RecordingKeyboard", "End")},
            {0x24, QT_TRANSLATE_NOOP("RecordingKeyboard", "Home")},
            {0x25, QT_TRANSLATE_NOOP("RecordingKeyboard", "Left")},
            {0x26, QT_TRANSLATE_NOOP("RecordingKeyboard", "Up")},
            {0x27, QT_TRANSLATE_NOOP("RecordingKeyboard", "Right")},
            {0x28, QT_TRANSLATE_NOOP("RecordingKeyboard", "Down")},
            {0x2C, QT_TRANSLATE_NOOP("RecordingKeyboard", "Print Screen")},
            {0x2D, QT_TRANSLATE_NOOP("RecordingKeyboard", "Insert")},
            {0x2E, QT_TRANSLATE_NOOP("RecordingKeyboard", "Delete")},
            {0x5B, QT_TRANSLATE_NOOP("RecordingKeyboard", "Win")},
            {0x5D, QT_TRANSLATE_NOOP("RecordingKeyboard", "Menu")},
            {0x6A, QT_TRANSLATE_NOOP("RecordingKeyboard", "Num *")},
            {0x6B, QT_TRANSLATE_NOOP("RecordingKeyboard", "Num +")},
            {0x6C, QT_TRANSLATE_NOOP("RecordingKeyboard", "Num Separator")},
            {0x6D, QT_TRANSLATE_NOOP("RecordingKeyboard", "Num -")},
            {0x6E, QT_TRANSLATE_NOOP("RecordingKeyboard", "Num .")},
            {0x6F, QT_TRANSLATE_NOOP("RecordingKeyboard", "Num /")},
            {0x90, QT_TRANSLATE_NOOP("RecordingKeyboard", "Num Lock")},
            {0x91, QT_TRANSLATE_NOOP("RecordingKeyboard", "Scroll Lock")},
            {0xA5, QT_TRANSLATE_NOOP("RecordingKeyboard", "AltGr")},
            {0xAD, QT_TRANSLATE_NOOP("RecordingKeyboard", "Mute")},
            {0xAE, QT_TRANSLATE_NOOP("RecordingKeyboard", "Volume Down")},
            {0xAF, QT_TRANSLATE_NOOP("RecordingKeyboard", "Volume Up")},
            {0xB0, QT_TRANSLATE_NOOP("RecordingKeyboard", "Next Track")},
            {0xB1, QT_TRANSLATE_NOOP("RecordingKeyboard", "Previous Track")},
            {0xB2, QT_TRANSLATE_NOOP("RecordingKeyboard", "Stop")},
            {0xB3, QT_TRANSLATE_NOOP("RecordingKeyboard", "Play/Pause")},
        };
        text.reserve(64);
        entries.reserve(64);
        for (const auto& [key, label] : names) {
            add(key, keyText(label));
        }
        for (uint32_t key = 0x60; key <= 0x69; ++key) {
            add(key, keyText(QT_TRANSLATE_NOOP("RecordingKeyboard", "Num %1")).arg(key - 0x60));
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
    RecordingKeyboardTheme() {
        const auto scheme = snow_shot::presentation::styles::generateThemeColorScheme();
        background = scheme.map.colorBgElevated;
        background.setAlpha(204);
        text = scheme.map.colorText;
        border = scheme.map.colorBorder;
        border.setAlpha(100);
    }
};
#endif
