#include "recordingeffectpreview.h"
#include "recordingeffectstyle.h"
#include "recordingeffectgeometry.h"
#include "screenrecordingperfinstrumentation.h"
#include "snow_shot/presentation/canvasstatusreadout.h"
#include "snow_shot/presentation/components/icons/snowshoticons.h"
#include "snow_shot/presentation/screenrecordingareawindow.h"
#include "snow_shot/presentation/styles/thememanager.h"
#include "snow_draw_engine_qt/snow_canvas_widget.h"
#include <QApplication>
#include <QDialog>
#include <QPainter>
#include "antd_icons.h"
#include <algorithm>
#ifdef Q_OS_WIN
#include <qt_windows.h>
#include <dwmapi.h>
#endif

class RecordingStatusPrompt final : public QWidget {
    Q_DECLARE_TR_FUNCTIONS(RecordingStatusPrompt)

  public:
    explicit RecordingStatusPrompt(QWidget* parent) : QWidget(parent) {
        setObjectName(QStringLiteral("screenRecordingStatusPrompt"));
        setAttribute(Qt::WA_TransparentForMouseEvents);
        setAttribute(Qt::WA_NoSystemBackground);
        setFocusPolicy(Qt::NoFocus);
        hide();
        refresh();
        connect(&snow_shot::presentation::styles::ThemeManager::instance(),
                &snow_shot::presentation::styles::ThemeManager::themeChanged, this,
                [this] { update(); });
    }

    void setStatus(const QString& format, bool microphone, bool speaker) {
        if (m_format == format && m_microphone == microphone && m_speaker == speaker)
            return;
        m_format = format;
        m_microphone = microphone;
        m_speaker = speaker;
        refresh();
    }

    void present(const QRectF& bounds, bool visible) {
        m_bounds = bounds;
        m_visible = visible;
        updateLayout();
    }

  protected:
    void paintEvent(QPaintEvent*) override {
        QPainter painter(this);
        painter.setRenderHint(QPainter::Antialiasing);
        painter.setPen(Qt::NoPen);
        painter.setBrush(QColor(0, 0, 0, 150));
        painter.drawRoundedRect(rect(), 6, 6);
        painter.setPen(Qt::white);
        const int textWidth = fontMetrics().horizontalAdvance(m_text);
        painter.drawText(QRect(kPadding, 0, textWidth, height()), Qt::AlignCenter, m_text);
        namespace custom = snow_shot::presentation::icons::custom::outlined;
        const auto colors =
            snow_shot::presentation::styles::ThemeManager::instance().themeColorScheme().map;
        const auto microphoneColor = adqt::icons::IconColors::primary(
            m_microphone ? colors.colorSuccess : colors.colorError);
        const auto speakerColor =
            adqt::icons::IconColors::primary(m_speaker ? colors.colorSuccess : colors.colorError);
        const QRect microphone(kPadding + textWidth + kGap, (height() - kIconSize) / 2, kIconSize,
                               kIconSize);
        adqt::icons::paintIcon(&painter,
                               m_microphone ? custom::RecordingMicrophone(microphoneColor)
                                            : custom::RecordingMicrophoneOff(microphoneColor),
                               microphone);
        adqt::icons::paintIcon(&painter,
                               m_speaker ? adqt::icons::antd::outlined::Sound(speakerColor)
                                         : custom::RecordingSpeakerOff(speakerColor),
                               microphone.translated(kIconSize + kGap, 0));
    }

    void changeEvent(QEvent* event) override {
        QWidget::changeEvent(event);
        if (event->type() == QEvent::LanguageChange || event->type() == QEvent::FontChange ||
            event->type() == QEvent::ApplicationFontChange)
            refresh();
    }

  private:
    void refresh() {
        m_text = m_format == QStringLiteral("gif")    ? tr("GIF")
                 : m_format == QStringLiteral("webp") ? tr("WebP")
                                                      : tr("MP4");
        setAccessibleName(
            tr("Format: %1; %2; %3")
                .arg(m_text, m_microphone ? tr("Microphone enabled") : tr("Microphone disabled"),
                     m_speaker ? tr("Speaker enabled") : tr("Speaker disabled")));
        updateLayout();
        update();
    }

    void updateLayout() {
        const QSize desired(fontMetrics().horizontalAdvance(m_text) +
                                2 * (kPadding + kGap + kIconSize),
                            std::max(fontMetrics().height(), kIconSize) + 16);
        const QRectF bounds = m_bounds.intersected(parentWidget()->rect());
        const bool fits = bounds.width() >= desired.width() && bounds.height() >= desired.height();
        resize(desired);
        move(qRound(bounds.center().x() - width() / 2.0),
             qRound(bounds.center().y() - height() / 2.0));
        setVisible(m_visible && fits);
        if (isVisible())
            raise();
    }

    static constexpr int kPadding = 12;
    static constexpr int kGap = 12;
    static constexpr int kIconSize = 20;
    QString m_format = QStringLiteral("mp4");
    QString m_text;
    bool m_microphone = false;
    bool m_speaker = false;
    bool m_visible = false;
    QRectF m_bounds;
};

namespace {
uint32_t rgba(const QColor& color) {
    return (static_cast<uint32_t>(color.red()) << 24) |
           (static_cast<uint32_t>(color.green()) << 16) |
           (static_cast<uint32_t>(color.blue()) << 8) | static_cast<uint32_t>(color.alpha());
}

} // namespace

RecordingEffectPreview::RecordingEffectPreview(ScreenRecordingAreaWindow& area,
                                               std::unique_ptr<RecordingEffectsSource> source)
    : QObject(&area), m_area(area),
      m_source(source ? std::move(source) : createNativeRecordingEffectsSource()),
      m_readout(new CanvasStatusReadout(&area)), m_statusPrompt(new RecordingStatusPrompt(&area)) {
    m_readout->setObjectName(QStringLiteral("screenRecordingMotionPreviewLabel"));
    m_readout->setMessage(CanvasStatusReadout::Message::MotionPreview);
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
    delete m_statusPrompt;
}

void RecordingEffectPreview::configure(
    const QRect& capture, const QSize& output, const QColor& trail, const QColor& click,
    bool keyboard, int trailDurationMs, const QColor& keyboardBackground,
    const QColor& keyboardForeground, int keyboardSize, const QColor& highlight,
    bool recordMouseClicks, int highlightRadius, uint32_t playbackOverlay,
    const QColor& progressBarColor, SnowWebcam* webcam, int webcamSize, uint32_t webcamShape,
    const QPoint& webcamOffset, bool recordMouseScrolling, uint32_t webcamOrientation,
    bool recordNumberKeys, bool recordLettersAndSymbols, bool recordControlKeys,
    bool preserveKeyboardShortcuts) {
    if (m_sessionEnding)
        return;
    const bool sameScene =
        m_capture == capture && m_output == output && m_trail == trail && m_click == click &&
        m_keyboard == keyboard && m_trailDurationMs == trailDurationMs &&
        m_keyboardBackground == keyboardBackground && m_keyboardForeground == keyboardForeground &&
        m_keyboardSize == keyboardSize && m_highlight == highlight &&
        m_recordMouseClicks == recordMouseClicks &&
        m_recordMouseScrolling == recordMouseScrolling && m_highlightRadius == highlightRadius &&
        m_playbackOverlay == playbackOverlay && m_progressBarColor == progressBarColor &&
        m_webcam == webcam && m_recordNumberKeys == recordNumberKeys &&
        m_recordLettersAndSymbols == recordLettersAndSymbols &&
        m_recordControlKeys == recordControlKeys &&
        m_preserveKeyboardShortcuts == preserveKeyboardShortcuts;
    if (sameScene && m_webcamSize == webcamSize && m_webcamShape == webcamShape &&
        m_webcamOffset == webcamOffset && m_webcamOrientation == webcamOrientation) {
        return;
    }
    if (sameScene && webcam) {
        // Layout edits share the active stream's generation. Keep its displayed frame
        // until the worker replaces it; rapid gestures must not starve presentation.
        m_webcamLayoutDirty = true;
    } else {
        // Clear with the old transform before changing the scene or disabling an effect.
        clearFrame();
        clearPlaybackFrame();
        ++m_generation;
        m_configurationDirty = true;
    }
    m_playbackOverlay = playbackOverlay;
    m_progressBarColor = progressBarColor;
    m_capture = capture;
    m_output = output;
    m_trail = trail;
    m_click = click;
    m_highlight = highlight;
    m_highlightRadius = highlightRadius;
    m_recordMouseClicks = recordMouseClicks;
    m_recordMouseScrolling = recordMouseScrolling;
    m_keyboard = keyboard;
    m_recordNumberKeys = recordNumberKeys;
    m_recordLettersAndSymbols = recordLettersAndSymbols;
    m_recordControlKeys = recordControlKeys;
    m_preserveKeyboardShortcuts = preserveKeyboardShortcuts;
    m_webcam = webcam;
    m_webcamSize = webcamSize;
    m_webcamOffset = webcamOffset;
    m_webcamShape = webcamShape;
    m_webcamOrientation = webcamOrientation;
    updateWebcamControl();
    m_keyboardSize = keyboardSize;
    m_trailDurationMs = trailDurationMs;
    m_keyboardBackground = keyboardBackground;
    m_keyboardForeground = keyboardForeground;
    m_failed = false;
    if (trail.alpha() == 0 && click.alpha() == 0 && !keyboard && highlight.alpha() == 0 &&
        !recordMouseClicks && !recordMouseScrolling && playbackOverlay == 0 && !webcam) {
        stopAndClear();
    }
    // Status prompts remain available even when no native effect observer is needed.
    m_configurationTimer.start();
}

void RecordingEffectPreview::setStatusPrompts(bool enabled, const QString& format, bool microphone,
                                              bool speaker) {
    m_showStatusPrompts = enabled;
    m_statusPrompt->setStatus(format, microphone, speaker);
    updateReadout();
}

void RecordingEffectPreview::setEligible(bool eligible) {
    if (m_sessionEnding)
        return;
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
    if (m_sessionEnding)
        return;
    SNOW_SHOT_RECORDING_PERF_SCOPE("preview.synchronize");
    const bool enabled = m_eligible && m_area.isVisible() && !m_windowBlocked &&
                         QApplication::activeModalWidget() == nullptr && m_capture.isValid() &&
                         m_output.isValid();
    if (!enabled) {
        stopAndClear();
        return;
    }
    m_previewVisible = true;
    updateWebcamControl();
    updateReadout();
#ifdef Q_OS_MACOS
    // Native input uses desktop points; every preview layer uses physical display pixels.
    const double canvasScale = m_area.devicePixelRatioF();
#else
    const double canvasScale = 1.0;
#endif
    if (m_configurationDirty || m_playbackDirty) {
        clearPlaybackFrame();
        m_playbackDirty = false;
        if (m_playbackOverlay != 0) {
            const RecordingKeyboardFont font;
            m_playbackFrame = takeRecordingEffectsFrame(snow_recording_effects_playback_preview(
                static_cast<uint32_t>(m_capture.width()), static_cast<uint32_t>(m_capture.height()),
                canvasScale, m_playbackOverlay, rgba(m_progressBarColor), font.family.constData(),
                font.cjkFamily.constData(), font.weight));
            if (m_playbackFrame) {
                m_area.canvas()->update(frameRegion(*m_playbackFrame));
            } else {
                if (reportError) {
                    reportError(QString::fromUtf8(snow_recording_effects_last_error()));
                }
            }
        }
    }
    const bool motionEnabled = m_trail.alpha() != 0 || m_click.alpha() != 0 || m_keyboard ||
                               m_highlight.alpha() != 0 || m_recordMouseClicks ||
                               m_recordMouseScrolling || m_webcam != nullptr;
    if (!motionEnabled) {
        m_running = false;
        m_source->stop();
        clearFrame();
        m_readout->dismiss();
        m_configurationDirty = false;
        return;
    }
    if (m_failed || (m_running && !m_configurationDirty && !m_webcamLayoutDirty)) {
        updateReadout();
        return;
    }
    if (!m_running || m_configurationDirty) {
        ++m_generation;
        clearFrame();
    }
    const RecordingKeyboardLabels labels(m_keyboard || m_recordMouseClicks ||
                                         m_recordMouseScrolling);
    const RecordingKeyboardTheme theme(m_keyboardBackground, m_keyboardForeground);
    const RecordingKeyboardFont font;
    SnowRecordingEffectsConfig config{SNOW_RECORDING_EFFECTS_CONFIG_VERSION,
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
                                      static_cast<uint32_t>(m_keyboardSize),
                                      0u,
                                      rgba(m_highlight),
                                      static_cast<uint32_t>(m_recordMouseClicks),
                                      nullptr,
                                      nullptr,
                                      0u,
                                      canvasScale,
                                      static_cast<uint32_t>(m_highlightRadius),
                                      m_webcam,
                                      static_cast<uint32_t>(m_webcamSize),
                                      m_webcamShape,
                                      static_cast<uint32_t>(m_webcamOffset.x()),
                                      static_cast<uint32_t>(m_webcamOffset.y()),
                                      static_cast<uint32_t>(m_recordMouseScrolling),
                                      m_webcamOrientation,
                                      static_cast<uint32_t>(m_recordNumberKeys),
                                      static_cast<uint32_t>(m_recordLettersAndSymbols),
                                      static_cast<uint32_t>(m_recordControlKeys),
                                      static_cast<uint32_t>(m_preserveKeyboardShortcuts)};
    font.applyTo(config);
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
    m_webcamLayoutDirty = false;
    if (!success) {
        stopAndClear();
        m_failed = true;
        if (reportError) {
            reportError(error);
        }
    }
}

void RecordingEffectPreview::stopAndClear(bool present) {
    m_area.setWebcamOverlay({});
    m_configurationTimer.stop();
    ++m_generation;
    m_running = false;
    m_webcamLayoutDirty = false;
    m_source->stop();
    clearFrame();
    clearPlaybackFrame();
    m_readout->dismiss();
    m_previewVisible = false;
    m_statusPrompt->present(m_area.selectionRect(), false);
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
    if (m_sessionEnding || !m_running) {
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
    SNOW_SHOT_RECORDING_PERF_MILESTONE("preview.first_frame_received");
    if (!damage.isEmpty()) {
        m_area.canvas()->update(damage);
    }
    updateReadout();
}

void RecordingEffectPreview::prepareForSessionEnd() {
    m_sessionEnding = true;
    m_configurationTimer.stop();
    reportError = {};
    qApp->removeEventFilter(this);
    disconnect(&snow_shot::presentation::styles::ThemeManager::instance(), nullptr, this, nullptr);
    // Native source teardown joins its observer. The retained UI session keeps it alive until
    // process termination instead of invoking stopAndClear() on the session-ending thread.
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

void RecordingEffectPreview::clearPlaybackFrame() {
    m_playbackDirty = true;
    if (m_playbackFrame) {
        const QRegion damage = frameRegion(*m_playbackFrame);
        m_playbackFrame.reset();
        m_area.canvas()->update(damage);
    }
}

QTransform RecordingEffectPreview::outputToCanvas(const QSize& output) const {
    return recordingEffectsOutputTransform(m_capture, m_area.recordingRegion(),
                                           m_area.selectionRect(),
                                           m_area.canvasGeometry().topLeft(),
#ifdef Q_OS_MACOS
                                           1.0,
#else
                                           m_area.devicePixelRatioF(),
#endif
                                           output);
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

void RecordingEffectPreview::updateWebcamControl() {
    if (!m_previewVisible || !m_eligible || !m_webcam || m_capture.isEmpty()) {
        m_area.setWebcamOverlay({});
        return;
    }
#ifdef Q_OS_MACOS
    const qreal scale = m_area.devicePixelRatioF();
#else
    const qreal scale = 1;
#endif
    const QSize physical(qRound(m_capture.width() * scale), qRound(m_capture.height() * scale));
    const auto placement = snow_webcam_placement(
        static_cast<uint32_t>(physical.width()), static_cast<uint32_t>(physical.height()),
        static_cast<uint32_t>(m_webcamSize), static_cast<uint32_t>(m_webcamOffset.x()),
        static_cast<uint32_t>(m_webcamOffset.y()));
    const QRectF bounds =
        outputToCanvas(physical)
            .mapRect(QRectF(placement.x, placement.y, placement.size, placement.size))
            .translated(m_area.canvasGeometry().topLeft());
    m_area.setWebcamOverlay(
        bounds, physical, m_webcamSize, m_webcamOffset,
        QPoint(static_cast<int>(placement.x),
               physical.height() - static_cast<int>(placement.y + placement.size)),
        m_webcamShape == 1);
}

void RecordingEffectPreview::updateReadout() {
    m_statusPrompt->present(m_area.selectionRect(), m_previewVisible && m_showStatusPrompts);
    if (!m_running || !m_frame) {
        m_readout->dismiss();
        return;
    }
    m_readout->layoutIn(m_area.selectionRect());
    m_readout->showPersistent();
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
        case QEvent::ApplicationFontChange:
            m_configurationDirty = true;
            m_configurationTimer.start();
            break;
        case QEvent::Show:
            m_failed = false;
            m_configurationTimer.start();
            break;
        case QEvent::Resize:
            updateWebcamControl();
            updateReadout();
            break;
        case QEvent::DevicePixelRatioChange:
        case QEvent::ScreenChangeInternal:
            m_configurationDirty = true;
            m_failed = false;
            m_configurationTimer.start();
            updateReadout();
            break;
        case QEvent::LanguageChange:
            m_configurationDirty = true;
            m_failed = false;
            m_configurationTimer.start();
            break;
        default:
            break;
        }
    }
    // This filter is installed on the application, so it sees every event in the
    // process. Only Show and Hide are handled below; test the cheap event type
    // before the cast rather than casting on every delivery.
    if (event->type() == QEvent::Show || event->type() == QEvent::Hide) {
        if (auto* dialog = qobject_cast<QDialog*>(watched);
            dialog != nullptr && dialog->isModal()) {
            if (event->type() == QEvent::Show) {
                stopAndClear();
            }
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
    painter.setClipRect(m_area.selectionRect().translated(-m_area.canvasGeometry().topLeft()),
                        Qt::IntersectClip);
    const QTransform canvasTransform = painter.transform();
    QSize coordinateSize;
    for (const auto& frame : {m_frame, m_playbackFrame}) {
        if (!frame) {
            continue;
        }
        for (const auto& tile : frame->tiles) {
            const QSize next = tile.coordinateSize.isEmpty() ? frame->output : tile.coordinateSize;
            if (next != coordinateSize) {
                painter.setTransform(canvasTransform);
                painter.setTransform(outputToCanvas(next), true);
                coordinateSize = next;
            }
            painter.drawImage(tile.rect.topLeft(), tile.image);
        }
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
