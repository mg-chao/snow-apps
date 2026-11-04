#pragma once
#include "../src/presentation/recording/recordingeffectpreview.h"

struct RecordingEffectTestState {
    bool active = false;
    int starts = 0;
    int stops = 0;
    int configurations = 0;
    int acquisitions = 0;
    QRect capture;
    double canvasScale = 0;
    uint32_t trail = 0;
    uint32_t click = 0;
    quint64 generation = 0;
    quint64 revision = 0;
    QSize output;
    uint32_t keyboardSize = 0;
    QByteArray keyboardFontFamily;
    QByteArray keyboardCjkFontFamily;
    uint32_t keyboardFontWeight = 0;
    uint32_t highlight = 0;
    uint32_t highlightRadius = 0;
    bool recordMouseClicks = false;
    bool recordMouseScrolling = false;
    bool showKeyboard = false;
    bool recordNumberKeys = true;
    bool recordLettersAndSymbols = true;
    bool recordControlKeys = true;
    bool preserveKeyboardShortcuts = true;
    uint32_t trailDurationMs = 0;
    uint32_t keyboardBackground = 0;
    uint32_t keyboardForeground = 0;
    uint32_t keyboardBorder = 0;
    const SnowWebcam* webcam = nullptr;
    uint32_t webcamSize = 0;
    uint32_t webcamShape = 0;
    uint32_t webcamOrientation = 0;
    QPoint webcamOffset;
    bool fail = false;
    bool autoPublish = true;
    std::function<void()> notify;
    std::shared_ptr<RecordingEffectsFrame> frame;
    void publish(bool visible = true) {
        frame = std::make_shared<RecordingEffectsFrame>();
        frame->generation = generation;
        frame->revision = ++revision;
        frame->output = output;
        if (visible) {
            QImage image(12, 12, QImage::Format_RGBA8888_Premultiplied);
            image.fill(QColor(255, 0, 0, 128));
            frame->tiles.push_back({QRect(24, 24, 12, 12), image, {}});
        }
        if (notify) {
            notify();
        }
    }
};

class RecordingEffectTestSource final : public RecordingEffectsSource {
  public:
    explicit RecordingEffectTestSource(std::shared_ptr<RecordingEffectTestState> state)
        : state(std::move(state)) {}
    bool start(const SnowRecordingEffectsConfig& config, std::function<void()> callback,
               QString& error) override {
        ++state->starts;
        state->active = true;
        state->notify = std::move(callback);
        return configure(config, error);
    }
    bool configure(const SnowRecordingEffectsConfig& config, QString& error) override {
        ++state->configurations;
        if (state->fail) {
            error = QStringLiteral("test preview failure");
            return false;
        }
        state->generation = config.generation;
        state->capture = QRect(config.x, config.y, static_cast<int>(config.width),
                               static_cast<int>(config.height));
        state->canvasScale = config.canvas_scale;
        state->trail = config.trail_rgba;
        state->click = config.click_rgba;
        state->keyboardSize = config.keyboard_size;
        state->keyboardFontFamily = config.keyboard_font_family_utf8;
        state->keyboardCjkFontFamily = config.keyboard_cjk_font_family_utf8;
        state->keyboardFontWeight = config.keyboard_font_weight;
        state->highlight = config.highlight_rgba;
        state->highlightRadius = config.highlight_radius;
        state->recordMouseClicks = config.record_mouse_clicks != 0;
        state->recordMouseScrolling = config.record_mouse_scrolling != 0;
        state->showKeyboard = config.show_keyboard != 0;
        state->recordNumberKeys = config.record_number_keys != 0;
        state->recordLettersAndSymbols = config.record_letters_and_symbols != 0;
        state->recordControlKeys = config.record_control_keys != 0;
        state->preserveKeyboardShortcuts = config.preserve_keyboard_shortcuts != 0;
        state->trailDurationMs = config.trail_duration_ms;
        state->keyboardBackground = config.keyboard_background_rgba;
        state->keyboardForeground = config.keyboard_text_rgba;
        state->keyboardBorder = config.keyboard_border_rgba;
        state->webcam = config.webcam_source;
        state->webcamSize = config.webcam_size;
        state->webcamShape = config.webcam_shape;
        state->webcamOrientation = config.webcam_orientation;
        state->webcamOffset = QPoint(static_cast<int>(config.webcam_offset_left),
                                     static_cast<int>(config.webcam_offset_bottom));
        state->output = QSize(qRound(config.width * config.canvas_scale),
                              qRound(config.height * config.canvas_scale));
        if (state->autoPublish)
            state->publish();
        return true;
    }
    void stop() override {
        if (state->active) {
            ++state->stops;
        }
        state->active = false;
        state->notify = {};
        state->frame.reset();
    }
    std::shared_ptr<RecordingEffectsFrame> acquire() override {
        ++state->acquisitions;
        return state->frame;
    }
    std::shared_ptr<RecordingEffectTestState> state;
};
