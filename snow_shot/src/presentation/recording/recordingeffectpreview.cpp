#include "recordingeffectpreview.h"
#include "recordingeffectstyle.h"
#include "recordingeffectgeometry.h"
#include "snow_shot/presentation/canvasstatusreadout.h"
#include "snow_shot/presentation/screenrecordingareawindow.h"
#include "snow_shot/presentation/styles/thememanager.h"
#include "snow_draw_engine_qt/snow_canvas_widget.h"
#include <QApplication>
#include <QDialog>
#include <QPainter>
#ifdef Q_OS_WIN
#include <qt_windows.h>
#include <dwmapi.h>
#endif

namespace {
uint32_t rgba(const QColor& color) {
    return (static_cast<uint32_t>(color.red()) << 24) |
           (static_cast<uint32_t>(color.green()) << 16) |
           (static_cast<uint32_t>(color.blue()) << 8) | static_cast<uint32_t>(color.alpha());
}

class NativeRecordingEffectsSource final : public RecordingEffectsSource {
  public:
    ~NativeRecordingEffectsSource() override {
        stop();
    }
    bool start(const SnowRecordingEffectsConfig& config, std::function<void()> notify,
               QString& error) override {
        stop();
        m_notify = std::move(notify);
        m_handle = snow_recording_effects_create(
            &config,
            [](void* context) {
                auto* self = static_cast<NativeRecordingEffectsSource*>(context);
                self->m_notify();
            },
            this);
        if (m_handle != nullptr && snow_recording_effects_set_active(m_handle, 1) != 0) {
            return true;
        }
        error = QString::fromUtf8(snow_recording_effects_last_error());
        stop();
        return false;
    }
    bool configure(const SnowRecordingEffectsConfig& config, QString& error) override {
        if (snow_recording_effects_configure(m_handle, &config) != 0) {
            return true;
        }
        error = QString::fromUtf8(snow_recording_effects_last_error());
        return false;
    }
    void stop() override {
        snow_recording_effects_destroy(m_handle);
        m_handle = nullptr;
        m_notify = {};
    }
    std::shared_ptr<RecordingEffectsFrame> acquire() override {
        auto* native = snow_recording_effects_acquire_frame(m_handle);
        if (native == nullptr) {
            return {};
        }
        auto frame = std::make_shared<RecordingEffectsFrame>();
        frame->lease = std::shared_ptr<void>(native, [](void* value) {
            snow_recording_effects_release_frame(static_cast<SnowRecordingEffectsFrame*>(value));
        });
        SnowRecordingEffectsFrameInfo info{};
        if (snow_recording_effects_frame_info(native, &info) == 0) {
            return {};
        }
        frame->generation = info.generation;
        frame->revision = info.revision;
        frame->output = QSize(static_cast<int>(info.width), static_cast<int>(info.height));
        if (info.error_utf8 != nullptr) {
            frame->error = QString::fromUtf8(info.error_utf8);
        }
        const auto appendLayer = [&frame](const SnowRecordingEffectsFrameInfo& layer) {
            frame->tiles.reserve(frame->tiles.size() + layer.tile_count);
            for (uint32_t i = 0; i < layer.tile_count; ++i) {
                const auto& tile = layer.tiles[i];
                const QRect rect(static_cast<int>(tile.x), static_cast<int>(tile.y),
                                 static_cast<int>(tile.width), static_cast<int>(tile.height));
                frame->tiles.push_back(
                    {rect,
                     QImage(tile.rgba_premultiplied, rect.width(), rect.height(),
                            static_cast<qsizetype>(tile.stride),
                            QImage::Format_RGBA8888_Premultiplied),
                     QSize(static_cast<int>(layer.width), static_cast<int>(layer.height))});
            }
        };
        appendLayer(info);
        if (snow_recording_effects_frame_keyboard_info(native, &info) == 0) {
            return {};
        }
        appendLayer(info);
        return frame;
    }

  private:
    SnowRecordingEffects* m_handle = nullptr;
    std::function<void()> m_notify;
};
} // namespace

RecordingEffectPreview::RecordingEffectPreview(ScreenRecordingAreaWindow& area,
                                               std::unique_ptr<RecordingEffectsSource> source)
    : QObject(&area), m_area(area),
      m_source(source ? std::move(source) : std::make_unique<NativeRecordingEffectsSource>()),
      m_readout(new CanvasStatusReadout(&area)) {
    m_readout->setObjectName(QStringLiteral("screenRecordingMotionPreviewLabel"));
    area.canvas()->setCustomRenderer(this);
    qApp->installEventFilter(this);
    m_configurationTimer.setSingleShot(true);
    m_configurationTimer.setInterval(0);
    connect(&m_configurationTimer, &QTimer::timeout, this, [this]() { synchronize(); });
    connect(&snow_shot::presentation::styles::ThemeManager::instance(),
            &snow_shot::presentation::styles::ThemeManager::themeChanged, this, [this]() {
                m_configurationDirty = true;
                m_failed = false;
                m_configurationTimer.start();
            });
}

RecordingEffectPreview::~RecordingEffectPreview() {
    qApp->removeEventFilter(this);
    stopAndClear();
    m_area.canvas()->setCustomRenderer(nullptr);
    delete m_readout;
}

void RecordingEffectPreview::configure(const QRect& capture, const QSize& output,
                                       const QColor& trail, const QColor& click, bool keyboard,
                                       int trailDurationMs, const QColor& keyboardBackground,
                                       const QColor& keyboardForeground, int keyboardSize) {
    if (m_capture == capture && m_output == output && m_trail == trail && m_click == click &&
        m_keyboard == keyboard && m_trailDurationMs == trailDurationMs &&
        m_keyboardBackground == keyboardBackground && m_keyboardForeground == keyboardForeground &&
        m_keyboardSize == keyboardSize) {
        return;
    }
    // Clear with the old transform before changing geometry or disabling an effect.
    clearFrame();
    ++m_generation;
    m_capture = capture;
    m_output = output;
    m_trail = trail;
    m_click = click;
    m_keyboard = keyboard;
    m_keyboardSize = keyboardSize;
    m_trailDurationMs = trailDurationMs;
    m_keyboardBackground = keyboardBackground;
    m_keyboardForeground = keyboardForeground;
    m_configurationDirty = true;
    m_failed = false;
    if (trail.alpha() == 0 && click.alpha() == 0 && !keyboard) {
        stopAndClear();
    } else {
        m_configurationTimer.start();
    }
}

void RecordingEffectPreview::setEligible(bool eligible) {
    if (m_eligible == eligible) {
        return;
    }
    m_eligible = eligible;
    m_failed = false;
    if (!eligible) {
        stopAndClear();
    } else {
        synchronize();
    }
}

void RecordingEffectPreview::synchronize() {
    const bool enabled = m_eligible && m_area.isVisible() && !m_windowBlocked &&
                         QApplication::activeModalWidget() == nullptr && m_capture.isValid() &&
                         m_output.isValid() &&
                         (m_trail.alpha() != 0 || m_click.alpha() != 0 || m_keyboard);
    if (!enabled) {
        stopAndClear();
        return;
    }
    if (m_failed || (m_running && !m_configurationDirty)) {
        updateReadout();
        return;
    }
    ++m_generation;
    clearFrame();
    const RecordingKeyboardLabels labels(m_keyboard);
    const RecordingKeyboardTheme theme(m_keyboardBackground, m_keyboardForeground);
    const SnowRecordingEffectsConfig config{SNOW_RECORDING_EFFECTS_CONFIG_VERSION,
                                            sizeof(SnowRecordingEffectsConfig),
                                            m_capture.x(),
                                            m_capture.y(),
                                            static_cast<uint32_t>(m_capture.width()),
                                            static_cast<uint32_t>(m_capture.height()),
                                            static_cast<uint32_t>(m_output.width()),
                                            static_cast<uint32_t>(m_output.height()),
                                            rgba(m_trail),
                                            rgba(m_click),
                                            static_cast<uint32_t>(m_keyboard),
                                            rgba(theme.background),
                                            rgba(theme.text),
                                            rgba(theme.border),
                                            labels.previewEntries.constData(),
                                            static_cast<uint32_t>(labels.previewEntries.size()),
                                            static_cast<uint32_t>(m_trailDurationMs),
                                            m_generation,
                                            static_cast<uint32_t>(m_keyboardSize)};
    QString error;
    const bool success = m_running ? m_source->configure(config, error)
                                   : m_source->start(
                                         config,
                                         [this]() {
                                             if (!m_notificationPending.exchange(true)) {
                                                 QMetaObject::invokeMethod(
                                                     this,
                                                     [this]() {
                                                         m_notificationPending = false;
                                                         receiveFrame();
                                                     },
                                                     Qt::QueuedConnection);
                                             }
                                         },
                                         error);
    m_running = success;
    m_configurationDirty = false;
    if (!success) {
        stopAndClear();
        m_failed = true;
        if (reportError) {
            reportError(error);
        }
    }
}

void RecordingEffectPreview::stopAndClear(bool present) {
    m_configurationTimer.stop();
    ++m_generation;
    m_running = false;
    m_source->stop();
    clearFrame();
    m_readout->hide();
    if (present && m_area.isVisible()) {
        m_area.repaint();
        m_area.canvas()->repaint();
#ifdef Q_OS_WIN
        if (QGuiApplication::platformName() == QStringLiteral("windows")) {
            static_cast<void>(DwmFlush());
        }
#endif
    }
}

void RecordingEffectPreview::receiveFrame() {
    if (!m_running) {
        return;
    }
    auto frame = m_source->acquire();
    if (!frame || frame->generation != m_generation ||
        (m_frame && frame->revision == m_frame->revision)) {
        return;
    }
    if (!frame->error.isEmpty()) {
        const QString error = frame->error;
        stopAndClear();
        m_failed = true;
        if (reportError) {
            reportError(error);
        }
        return;
    }
    QRegion damage = frameRegion(*frame);
    if (m_frame) {
        damage += frameRegion(*m_frame);
    }
    m_frame = std::move(frame);
    if (!damage.isEmpty()) {
        m_area.canvas()->update(damage);
    }
    updateReadout();
}

void RecordingEffectPreview::clearFrame() {
    if (m_frame) {
        const QRegion damage = frameRegion(*m_frame);
        m_frame.reset();
        if (!damage.isEmpty()) {
            m_area.canvas()->update(damage);
        }
    }
}

QTransform RecordingEffectPreview::outputToCanvas(const QSize& output) const {
    return recordingEffectsOutputTransform(
        m_capture, m_area.physicalRegion(), m_area.selectionRect(),
        m_area.canvasGeometry().topLeft(), m_area.devicePixelRatioF(), output);
}

QRegion RecordingEffectPreview::frameRegion(const RecordingEffectsFrame& frame) const {
    if (frame.output.isEmpty()) {
        return {};
    }
    QRegion result;
    for (const auto& tile : frame.tiles) {
        const QTransform transform =
            outputToCanvas(tile.coordinateSize.isEmpty() ? frame.output : tile.coordinateSize);
        result += transform.mapRect(QRectF(tile.rect)).toAlignedRect().adjusted(-1, -1, 1, 1);
    }
    return result.intersected(m_area.canvas()->rect());
}

void RecordingEffectPreview::updateReadout() {
    if (!m_running || !m_frame) {
        m_readout->hide();
        return;
    }
    m_readout->setText(
        QCoreApplication::translate("RecordingEffectPreview", "Motion Preview in Progress"));
    m_readout->layoutIn(m_area.selectionRect().toAlignedRect());
    m_readout->show();
    m_readout->raise();
}

bool RecordingEffectPreview::eventFilter(QObject* watched, QEvent* event) {
    if (watched == &m_area) {
        switch (event->type()) {
        case QEvent::Hide:
            stopAndClear();
            break;
        case QEvent::WindowBlocked:
            m_windowBlocked = true;
            stopAndClear();
            break;
        case QEvent::WindowUnblocked:
            m_windowBlocked = false;
            m_configurationTimer.start();
            break;
        case QEvent::Show:
            m_failed = false;
            m_configurationTimer.start();
            break;
        case QEvent::Resize:
        case QEvent::DevicePixelRatioChange:
        case QEvent::ScreenChangeInternal:
            updateReadout();
            break;
        case QEvent::LanguageChange:
            m_configurationDirty = true;
            m_failed = false;
            updateReadout();
            m_configurationTimer.start();
            break;
        default:
            break;
        }
    }
    if (auto* dialog = qobject_cast<QDialog*>(watched); dialog != nullptr && dialog->isModal()) {
        if (event->type() == QEvent::Show) {
            stopAndClear();
        }
        if (event->type() == QEvent::Show || event->type() == QEvent::Hide) {
            m_configurationTimer.start();
        }
    }
    return QObject::eventFilter(watched, event);
}

void RecordingEffectPreview::renderBeforeCanvas(QPainter& painter,
                                                const SnowCanvasRenderContext& context) {
    painter.setCompositionMode(QPainter::CompositionMode_Source);
    painter.fillRect(context.exposedRegion.boundingRect(), Qt::transparent);
    // The child canvas shares the layered window's backing store. Retain the area's
    // hit-test coverage while clearing old tiles, even when preview is stopped.
    const QColor inputColor = m_area.inputSurfaceColor();
    if (inputColor.alpha() != 0) {
        painter.fillRect(m_area.selectionRect().translated(-m_area.canvasGeometry().topLeft()),
                         inputColor);
    }
}

void RecordingEffectPreview::renderAfterCanvas(QPainter& painter, const SnowCanvasRenderContext&) {
    if (!m_running || !m_frame || m_frame->output.isEmpty()) {
        return;
    }
    painter.setClipRect(m_area.selectionRect().translated(-m_area.canvasGeometry().topLeft()),
                        Qt::IntersectClip);
    const QTransform canvasTransform = painter.transform();
    QSize coordinateSize;
    for (const auto& tile : m_frame->tiles) {
        const QSize next = tile.coordinateSize.isEmpty() ? m_frame->output : tile.coordinateSize;
        if (next != coordinateSize) {
            painter.setTransform(canvasTransform);
            painter.setTransform(outputToCanvas(next), true);
            coordinateSize = next;
        }
        painter.drawImage(tile.rect.topLeft(), tile.image);
    }
}

bool RecordingEffectPreview::active() const {
    return m_running;
}
quint64 RecordingEffectPreview::generation() const {
    return m_generation;
}
bool RecordingEffectPreview::hasFrame() const {
    return m_frame && !m_frame->tiles.empty();
}
