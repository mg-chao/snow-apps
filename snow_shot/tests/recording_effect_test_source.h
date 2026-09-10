#pragma once
#include "../src/presentation/recording/recordingeffectpreview.h"

struct RecordingEffectTestState {
    bool active = false;
    int starts = 0;
    int stops = 0;
    quint64 generation = 0;
    quint64 revision = 0;
    QSize output;
    uint32_t keyboardSize = 0;
    uint32_t trailDurationMs = 0;
    uint32_t keyboardBackground = 0;
    uint32_t keyboardForeground = 0;
    uint32_t keyboardBorder = 0;
    bool fail = false;
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
        if (state->fail) {
            error = QStringLiteral("test preview failure");
            return false;
        }
        state->generation = config.generation;
        state->keyboardSize = config.keyboard_size;
        state->trailDurationMs = config.trail_duration_ms;
        state->keyboardBackground = config.keyboard_background_rgba;
        state->keyboardForeground = config.keyboard_text_rgba;
        state->keyboardBorder = config.keyboard_border_rgba;
        state->output =
            QSize(static_cast<int>(config.output_width), static_cast<int>(config.output_height));
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
        return state->frame;
    }
    std::shared_ptr<RecordingEffectTestState> state;
};
