#include "recordingaudiogainpopover.h"

#include "theme/theme_manager.h"
#include "widgets/button.h"
#include "widgets/control_scale.h"
#include "widgets/popover.h"
#include "widgets/slider.h"

#include <QApplication>
#include <QEvent>
#include <QKeyEvent>
#include <QScreen>
#include <QSignalBlocker>
#include <QTimer>
#include <QVBoxLayout>

#include <algorithm>
#include <cmath>

RecordingAudioGainPopover::RecordingAudioGainPopover(adqt::widgets::AdButton* trigger,
                                                     Source source, QObject* parent)
    : QObject(parent != nullptr ? parent : trigger), m_trigger(trigger), m_source(source) {
    m_popover = new adqt::widgets::AdPopover(this);
    const QString prefix = source == Source::Microphone
                               ? QStringLiteral("screenRecordingMicrophone")
                               : QStringLiteral("screenRecordingSystemAudio");
    setObjectName(prefix + QStringLiteral("GainController"));
    m_popover->setObjectName(prefix + QStringLiteral("GainPopover"));
    m_popover->setSourceWidget(trigger);
    m_popover->setTriggers(adqt::widgets::AdPopover::Trigger::Hover |
                           adqt::widgets::AdPopover::Trigger::Focus);
    m_popover->setPlacement(adqt::widgets::AdPopover::Placement::Top);
    m_popover->setPopupLayerMode(adqt::widgets::AdPopover::PopupLayerMode::QtTool);
    m_popover->setHoverOpenDelayMs(150);
    m_popover->setHoverCloseDelayMs(250);
    m_popover->setContentFactory([this]() { return createContent(); });
    if (trigger != nullptr) {
        trigger->setFocusPolicy(Qt::StrongFocus);
        trigger->installEventFilter(this);
    }
    connect(m_popover, &adqt::widgets::AdPopover::visibleChanged, this, [this](bool visible) {
        if (!visible)
            m_focusWhenVisible = false;
        emit visibleChanged(visible);
        if (visible)
            focusSliderWhenVisible();
    });
    connect(&adqt::theme::ThemeManager::instance(), &adqt::theme::ThemeManager::themeChanged, this,
            [this]() { refreshMeterStyle(); });
    if (qApp != nullptr) {
        qApp->installEventFilter(this);
        // Qt emits screenRemoved before it reroutes and re-shows surviving
        // native windows. Close first so its saved visibility is already false.
        connect(qApp, &QGuiApplication::screenRemoved, this, [this](QScreen*) { close(); });
        connect(qApp, &QGuiApplication::screenAdded, this, [this](QScreen*) { close(); });
    }
}

RecordingAudioGainPopover::~RecordingAudioGainPopover() {
    if (qApp != nullptr)
        qApp->removeEventFilter(this);
    m_popover->hide();
}

adqt::widgets::AdButton* RecordingAudioGainPopover::trigger() const {
    return m_trigger;
}
adqt::widgets::AdPopover* RecordingAudioGainPopover::popover() const {
    return m_popover;
}
int RecordingAudioGainPopover::gainDb() const {
    return m_gainDb;
}

void RecordingAudioGainPopover::setGainDb(int gainDb) {
    const int normalized = std::clamp(gainDb, -24, 24);
    if (m_gainDb == normalized)
        return;
    m_gainDb = normalized;
    if (m_slider) {
        const QSignalBlocker blocker(m_slider);
        m_slider->setValue(m_gainDb);
    }
}

void RecordingAudioGainPopover::setLevel(double peak, bool clipping, LevelStatus status) {
    const double normalized = std::isfinite(peak) ? std::clamp(peak, 0.0, 1.0) : 0.0;
    const bool styleChanged = m_clipping != clipping;
    m_peak = normalized;
    m_clipping = clipping;
    m_status = status;
    refreshLevel();
    if (styleChanged)
        refreshMeterStyle();
}

void RecordingAudioGainPopover::setAvailable(bool available) {
    if (m_available == available)
        return;
    m_available = available;
    m_popover->setEnabled(available);
    if (!available)
        close();
}

void RecordingAudioGainPopover::setAudioEnabled(bool enabled) {
    if (m_audioEnabled == enabled)
        return;
    m_audioEnabled = enabled;
    m_peak = 0.0;
    m_clipping = false;
    refreshLevel();
    refreshMeterStyle();
}

void RecordingAudioGainPopover::openAndFocus() {
    if (!m_available || !m_trigger || !m_trigger->isEnabled())
        return;
    m_focusWhenVisible = true;
    m_popover->show();
    focusSliderWhenVisible();
}

void RecordingAudioGainPopover::close() {
    if (m_closing)
        return;
    m_closing = true;
    m_focusWhenVisible = false;
    if (m_content && m_trigger && m_trigger->isEnabled()) {
        QWidget* focus = QApplication::focusWidget();
        if (focus && (focus == m_content || m_content->isAncestorOf(focus)))
            m_trigger->setFocus(Qt::OtherFocusReason);
    }
    // Restoring focus may open a Focus trigger; the final hide clears all reasons.
    m_popover->hide();
    m_closing = false;
}

QWidget* RecordingAudioGainPopover::prepareSurface() {
    m_popover->preparePopup();
    return m_popover->surfaceWidget();
}

void RecordingAudioGainPopover::setRetainNativeSurfaceOnHide(bool retain) {
    m_popover->setRetainNativeSurfaceOnHide(retain);
}

void RecordingAudioGainPopover::setSurfaceShowGuard(SurfaceShowGuard guard) {
    m_popover->setSurfaceShowGuard(std::move(guard));
}

QWidget* RecordingAudioGainPopover::createContent() {
    auto* content = new QWidget;
    m_content = content;
    content->setObjectName(objectName() + QStringLiteral("Content"));
    content->setFixedWidth(180);
    auto* layout = new QVBoxLayout(content);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);
    m_slider = new adqt::widgets::AdSlider(content);
    m_slider->setObjectName(QStringLiteral("recordingAudioGainSlider"));
    m_slider->setRange(-24, 24);
    m_slider->setSingleStep(1);
    m_slider->setPageStep(6);
    adqt::widgets::AdSlider::Mark unityMark;
    unityMark.labelVisible = false;
    m_slider->setMarks({{0.0, unityMark}});
    m_slider->setValue(m_gainDb);
    m_slider->setMinimumHeight(32);
    m_slider->setTrackFillRatio(0.0);
    layout->addWidget(m_slider);
    connect(m_slider, &adqt::widgets::AdSlider::valueChanged, this, [this](double value) {
        const int gain = qRound(value);
        if (m_gainDb == gain)
            return;
        m_gainDb = gain;
        emit gainChanged(gain);
    });
    auto* scope = new adqt::widgets::AdControlScaleScope(content, content);
    scope->applyCurrentScaleToSubtree(content);
    refreshText();
    refreshLevel();
    refreshMeterStyle();
    return content;
}

void RecordingAudioGainPopover::refreshText() {
    if (m_slider) {
        m_slider->setAccessibleName(m_source == Source::Microphone ? tr("Microphone gain")
                                                                   : tr("System audio gain"));
        m_slider->setAccessibleDescription(
            tr("Adjust gain from -24 to +24 dB. The track shows the processed audio level."));
        m_slider->setTooltipFormatter([this](double value) {
            const int gain = qRound(value);
            const QString amount =
                gain > 0 ? QStringLiteral("+%1").arg(gain) : QString::number(gain);
            return tr("%1 dB").arg(amount);
        });
    }
}

void RecordingAudioGainPopover::refreshLevel() {
    if (!m_slider)
        return;
    const double level = m_audioEnabled && m_status == LevelStatus::Live ? m_peak : 0.0;
    m_slider->setTrackFillRatio(level);
}

void RecordingAudioGainPopover::refreshMeterStyle() {
    if (!m_slider)
        return;
    const auto colors = adqt::theme::ThemeManager::instance().resolveTheme(m_slider);
    auto styles = m_slider->semanticStyles();
    const QColor color = m_clipping && m_audioEnabled ? colors.colorError : colors.colorSuccess;
    styles.track.backgroundColor = color;
    m_slider->setSemanticStyles(styles);
}

void RecordingAudioGainPopover::focusSliderWhenVisible() {
    if (!m_focusWhenVisible || !m_slider || !m_popover->surfaceWidget() ||
        !m_popover->surfaceWidget()->isVisible())
        return;
    m_focusWhenVisible = false;
    m_popover->surfaceWidget()->activateWindow();
    m_slider->setFocus(Qt::OtherFocusReason);
}

bool RecordingAudioGainPopover::eventFilter(QObject* watched, QEvent* event) {
    if (!event)
        return false;
    if (event->type() == QEvent::LanguageChange && (watched == m_trigger || watched == qApp)) {
        refreshText();
        refreshLevel();
    }
    if ((event->type() == QEvent::PaletteChange ||
         event->type() == QEvent::ApplicationPaletteChange) &&
        (watched == m_trigger || watched == qApp))
        refreshMeterStyle();
    if ((event->type() == QEvent::Show || event->type() == QEvent::Hide) &&
        watched == m_popover->surfaceWidget()) {
        const bool visible = event->type() == QEvent::Show;
        emit surfaceVisibilityChanged(visible);
        if (visible)
            QTimer::singleShot(0, this, [this]() { focusSliderWhenVisible(); });
    }
    if (event->type() != QEvent::KeyPress && event->type() != QEvent::ShortcutOverride)
        return false;
    auto* key = static_cast<QKeyEvent*>(event);
    const auto* widget = qobject_cast<QWidget*>(watched);
    const bool inContent =
        m_content && widget && (widget == m_content || m_content->isAncestorOf(widget));
    const bool sourceDown =
        watched == m_trigger && key->key() == Qt::Key_Down &&
        (key->modifiers() == Qt::NoModifier || key->modifiers() == Qt::AltModifier);
    const bool escape = key->key() == Qt::Key_Escape && (m_popover->isVisible() || inContent);
    if (event->type() == QEvent::ShortcutOverride) {
        const bool editing =
            inContent && (key->key() == Qt::Key_Left || key->key() == Qt::Key_Right ||
                          key->key() == Qt::Key_Up || key->key() == Qt::Key_Down ||
                          key->key() == Qt::Key_PageUp || key->key() == Qt::Key_PageDown ||
                          key->key() == Qt::Key_Home || key->key() == Qt::Key_End ||
                          key->key() == Qt::Key_Space || key->key() == Qt::Key_Return ||
                          key->key() == Qt::Key_Enter || key->key() == Qt::Key_Tab ||
                          key->key() == Qt::Key_Backtab);
        if (escape || sourceDown || editing) {
            key->accept();
            return true;
        }
        return false;
    }
    if (escape) {
        close();
        key->accept();
        return true;
    }
    if (sourceDown) {
        openAndFocus();
        key->accept();
        return true;
    }
    return false;
}
