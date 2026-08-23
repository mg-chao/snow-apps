#include "snow_shot/presentation/components/shortcutkeyrow.h"

#include "snow_shot/presentation/components/infotooltipicon.h"
#include "snow_shot/presentation/styles/mainwindowcomponenttoken.h"
#include "snow_shot/presentation/styles/thememanager.h"

#include "widgets/button.h"
#include "widgets/modal.h"
#include "widgets/tooltip.h"

#include <QApplication>
#include <QCoreApplication>
#include <QEvent>
#include <QFontMetricsF>
#include <QKeyEvent>
#include <QLabel>
#include <QLayout>
#include <QString>
#include <QWheelEvent>

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <iostream>

namespace shortcuts = snow_shot::presentation;
namespace styles = snow_shot::presentation::styles;

namespace {
void require(bool condition, const char* message) {
    if (!condition) {
        std::cerr << message << '\n';
        std::exit(1);
    }
}

shortcuts::GlobalShortcutRegistrationState
stateFor(shortcuts::GlobalShortcutStatus status,
         const QVector<shortcuts::GlobalShortcutBindingResult>& bindings) {
    shortcuts::GlobalShortcutRegistrationState state;
    state.action = shortcuts::GlobalShortcutAction::Screenshot;
    state.status = status;
    state.bindings = bindings;
    for (const shortcuts::GlobalShortcutBindingResult& binding : bindings) {
        state.shortcuts.push_back(binding.shortcut);
    }
    return state;
}

void statusPresentationUsesSemanticTokens() {
    const styles::ThemeColorScheme scheme = styles::ThemeManager::instance().themeColorScheme();
    const auto mainWindowMetric = styles::buildMainWindowComponentMetricToken(scheme);

    const shortcuts::GlobalShortcutBindingResult firstSuccess{
        QStringLiteral("Ctrl+Shift+1"),
        true,
        shortcuts::GlobalShortcutFailureReason::None,
        0,
    };
    const shortcuts::GlobalShortcutBindingResult secondSuccess{
        QStringLiteral("Ctrl+Shift+2"),
        true,
        shortcuts::GlobalShortcutFailureReason::None,
        0,
    };
    const auto registeredState =
        stateFor(shortcuts::GlobalShortcutStatus::Registered, {firstSuccess, secondSuccess});
    const ShortcutKeyRowConfig config{
        QStringLiteral("Screenshot"),
        {},
        registeredState.shortcuts,
        registeredState,
        QStringLiteral("normal"),
        true,
        2,
    };

    ShortcutKeyRow row(config, scheme.metricAlias, mainWindowMetric);
    row.resize(720, row.height());
    row.show();
    QApplication::processEvents();

    auto* const statusTrigger =
        row.findChild<InfoTooltipIcon*>(QStringLiteral("shortcutRegistrationStatusTooltipTrigger"));
    auto* const shortcutButton =
        row.findChild<adqt::widgets::AdButton*>(QStringLiteral("shortcutKeyButton"));
    require(statusTrigger != nullptr,
            "shortcut row should expose an inline status tooltip trigger");
    require(shortcutButton != nullptr, "shortcut row should expose its shortcut control");
    auto* const statusTooltip = statusTrigger->tooltipHost();
    require(statusTooltip != nullptr && statusTooltip->targetWidget() == statusTrigger &&
                statusTooltip->placement() == adqt::widgets::AdTooltip::Placement::Top &&
                statusTooltip->hoverOpenDelayMs() == 0,
            "the shared info tooltip should open above its icon without delay");
    require(statusTrigger->parentWidget() == shortcutButton,
            "the status trigger should be placed inside the shortcut button");
    require(!statusTrigger->isVisible(),
            "registered shortcuts should not display a tooltip trigger");
    require(shortcutButton->height() == scheme.metricAlias.controlHeight &&
                shortcutButton->minimumWidth() == 0,
            "the shortcut button should use the reference 32px control height without a fixed "
            "minimum width");
    require(shortcutButton->accentRole() == adqt::widgets::AdButton::AccentRole::Green,
            "registered shortcuts should use the reference green dashed-button role");
    require(statusTrigger->tooltipText().isEmpty(),
            "registered shortcuts should not display a tooltip");
    const int stableHeight = row.height();

    const shortcuts::GlobalShortcutBindingResult failed{
        QStringLiteral("Ctrl+Shift+2"),
        false,
        shortcuts::GlobalShortcutFailureReason::AlreadyInUse,
        1409,
    };
    row.setRegistrationState(
        stateFor(shortcuts::GlobalShortcutStatus::PartiallyRegistered, {firstSuccess, failed}));
    QApplication::processEvents();
    require(statusTrigger->isVisible() &&
                statusTrigger->property("statusColor").value<QColor>() ==
                    scheme.map.presetColorHover.value(QStringLiteral("orange"),
                                                      scheme.map.colorWarning),
            "partial state should use the reference orange dashed-button color");
    require(shortcutButton->accentRole() == adqt::widgets::AdButton::AccentRole::Orange,
            "partial registrations should use the reference orange dashed-button role");
    require(statusTrigger->tooltipText().contains(QStringLiteral("Some shortcuts")) &&
                statusTrigger->tooltipText().contains(QStringLiteral("already used")),
            "partial state should display an explanatory tooltip");
    require(statusTrigger->geometry().left() > 0 &&
                statusTrigger->geometry().right() < shortcutButton->width(),
            "the tooltip trigger should sit inside the shortcut button after its text");
    require(statusTrigger->size() ==
                    QSize(scheme.metricAlias.fontSize, scheme.metricAlias.fontSize) &&
                statusTrigger->property("inlineGap").toInt() == scheme.metricAlias.marginXS,
            "the tooltip trigger should match the reference 14px icon and 8px Button gap");
    row.setRegistrationState(stateFor(shortcuts::GlobalShortcutStatus::Failed, {failed}));
    QApplication::processEvents();
    require(statusTrigger->property("statusColor").value<QColor>() == scheme.map.colorError,
            "failed state should use the antd danger color");
    require(shortcutButton->property("registrationStatus").toInt() ==
                static_cast<int>(shortcuts::GlobalShortcutStatus::Failed),
            "semantic state should also reach the shortcut control");
    require(shortcutButton->accentRole() == adqt::widgets::AdButton::AccentRole::Danger,
            "failed registrations should use the reference danger dashed-button role");
    require(row.height() == stableHeight, "status changes must not resize the shortcut row");

    const auto requireFullShortcutTextCapacity = [shortcutButton](const char* message) {
        const QString shortcutText = shortcutButton->text();
        const int requiredTextWidth = static_cast<int>(
            std::ceil(QFontMetricsF(shortcutButton->font()).horizontalAdvance(shortcutText)));

        shortcutButton->setText(QString(QChar(0x200B)));
        const int nonTextWidth = shortcutButton->sizeHint().width();
        shortcutButton->setText(QString(256, QLatin1Char('W')));
        const int maximumTextCapacity = shortcutButton->sizeHint().width() - nonTextWidth;
        shortcutButton->setText(shortcutText);

        const int expectedTextCapacity = std::min(requiredTextWidth, maximumTextCapacity);
        const int actualTextCapacity = shortcutButton->sizeHint().width() - nonTextWidth;

        if (actualTextCapacity < expectedTextCapacity) {
            std::cerr << "shortcut text capacity: required=" << requiredTextWidth
                      << ", expected=" << expectedTextCapacity << ", actual=" << actualTextCapacity
                      << '\n';
        }

        require(actualTextCapacity >= expectedTextCapacity, message);
    };

    const shortcuts::GlobalShortcutBindingResult ctrlF1{
        QStringLiteral("Ctrl+F1"),
        true,
        shortcuts::GlobalShortcutFailureReason::None,
        0,
    };
    const shortcuts::GlobalShortcutBindingResult shortSecond{
        QStringLiteral("Ctrl+4"),
        true,
        shortcuts::GlobalShortcutFailureReason::None,
        0,
    };
    row.setRegistrationState(
        stateFor(shortcuts::GlobalShortcutStatus::Registered, {ctrlF1, shortSecond}));
    QApplication::processEvents();
    requireFullShortcutTextCapacity(
        "Ctrl+F1 / Ctrl+4 should receive enough width to avoid rounding-induced elision");

    const shortcuts::GlobalShortcutBindingResult numLockSecond{
        QStringLiteral("Num+NumLock"),
        true,
        shortcuts::GlobalShortcutFailureReason::None,
        0,
    };
    row.setRegistrationState(
        stateFor(shortcuts::GlobalShortcutStatus::Registered, {ctrlF1, numLockSecond}));
    QApplication::processEvents();
    requireFullShortcutTextCapacity(
        "Ctrl+F1 / Num NumLock should use the same precise width calculation");

    const shortcuts::GlobalShortcutBindingResult longFirst{
        QStringLiteral("Ctrl+Alt+Shift+Print"),
        true,
        shortcuts::GlobalShortcutFailureReason::None,
        0,
    };
    const shortcuts::GlobalShortcutBindingResult longSecond{
        QStringLiteral("Ctrl+Alt+Shift+PageDown"),
        true,
        shortcuts::GlobalShortcutFailureReason::None,
        0,
    };
    row.setRegistrationState(
        stateFor(shortcuts::GlobalShortcutStatus::Registered, {longFirst, longSecond}));
    row.resize(440, stableHeight);
    QApplication::processEvents();
    require(shortcutButton->geometry().right() < row.width(),
            "long shortcut labels should shrink and elide inside the row");

    shortcuts::GlobalShortcutRegistrationState unsetState;
    unsetState.action = shortcuts::GlobalShortcutAction::Screenshot;
    row.setRegistrationState(unsetState);
    QApplication::processEvents();
    require(!statusTrigger->isVisible() && statusTrigger->tooltipText().isEmpty(),
            "unset shortcuts should not display a registration tooltip trigger");
    require(shortcutButton->accentRole() == adqt::widgets::AdButton::AccentRole::Neutral,
            "unset shortcuts should use the reference default dashed-button role");
}

void recorderAcceptsOnlyBackendSupportedShortcuts() {
    const styles::ThemeColorScheme scheme = styles::ThemeManager::instance().themeColorScheme();
    const auto mainWindowMetric = styles::buildMainWindowComponentMetricToken(scheme);

    QString lastValidatedShortcut;
    ShortcutKeyRowConfig config;
    config.title = QStringLiteral("Screenshot");
    config.maxShortcutCount = 2;
    config.shortcutValidator = [&lastValidatedShortcut](const QString& shortcut) {
        lastValidatedShortcut = shortcut;
        const bool supported = !shortcut.contains(QStringLiteral("F25"));
        return shortcuts::GlobalShortcutValidationResult{
            shortcut,
            supported,
            supported ? shortcuts::GlobalShortcutFailureReason::None
                      : shortcuts::GlobalShortcutFailureReason::InvalidShortcut,
        };
    };

    ShortcutKeyRow row(config, scheme.metricAlias, mainWindowMetric);
    QObject::connect(&row, &ShortcutKeyRow::shortcutsChanged, &row,
                     [&row](const QStringList& selectedShortcuts) {
                         shortcuts::GlobalShortcutRegistrationState state;
                         state.action = shortcuts::GlobalShortcutAction::Screenshot;
                         state.shortcuts = selectedShortcuts;
                         row.setRegistrationState(state);
                     });
    row.resize(720, row.height());
    row.show();
    QApplication::processEvents();

    auto* shortcutButton =
        row.findChild<adqt::widgets::AdButton*>(QStringLiteral("shortcutKeyButton"));
    require(shortcutButton != nullptr, "shortcut control should open the recorder");
    shortcutButton->click();
    QApplication::processEvents();

    auto* configContent = row.findChild<QWidget*>(QStringLiteral("shortcutConfigContent"));
    auto* modal = row.findChild<adqt::widgets::AdModal*>();
    require(configContent != nullptr, "the shortcut recorder should be created");
    require(modal != nullptr && modal->acceptButton() != nullptr,
            "the recorder modal should exist");

    QKeyEvent unsupportedEvent(QEvent::KeyPress, Qt::Key_F25, Qt::ControlModifier);
    QCoreApplication::sendEvent(configContent, &unsupportedEvent);
    QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
    QApplication::processEvents();

    auto* keyButton =
        row.findChild<adqt::widgets::AdButton*>(QStringLiteral("shortcutConfigKeyButton"));
    auto* actionButton =
        row.findChild<adqt::widgets::AdButton*>(QStringLiteral("shortcutConfigActionButton"));
    require(keyButton != nullptr && actionButton != nullptr, "recorder controls should exist");
    require(keyButton->property("shortcutValidationState").toString() == QStringLiteral("invalid"),
            "a backend-rejected key should use the invalid recording state");
    auto* validationInfo =
        row.findChild<InfoTooltipIcon*>(QStringLiteral("shortcutConfigValidationTooltipTrigger"));
    require(validationInfo != nullptr,
            "the recorder should use the shared validation info tooltip");
    require(validationInfo->parentWidget() == keyButton && validationInfo->geometry().left() > 0 &&
                validationInfo->geometry().right() < keyButton->width(),
            "the recorder info trigger should sit inside the key button after its text");
    auto* const validationTooltip = validationInfo->tooltipHost();
    require(keyButton->toolTip().isEmpty() &&
                validationInfo->tooltipText().contains(QStringLiteral("Windows global shortcut")) &&
                validationTooltip != nullptr &&
                validationTooltip->placement() == adqt::widgets::AdTooltip::Placement::Top &&
                validationTooltip->hoverOpenDelayMs() == 0,
            "the recorder info icon should own an immediate upward tooltip");
    require(keyButton->accessibleDescription().contains(QStringLiteral("Windows global shortcut")),
            "a rejected key should expose an understandable validation reason");
    require(!actionButton->isEnabled(), "a backend-rejected key must not be confirmable");
    require(!modal->acceptButton()->isEnabled(),
            "the modal must not commit while the active recording is invalid");

    QKeyEvent supportedNumpadEvent(QEvent::KeyPress, Qt::Key_1, Qt::KeypadModifier);
    QCoreApplication::sendEvent(configContent, &supportedNumpadEvent);
    QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
    QApplication::processEvents();

    keyButton = row.findChild<adqt::widgets::AdButton*>(QStringLiteral("shortcutConfigKeyButton"));
    actionButton =
        row.findChild<adqt::widgets::AdButton*>(QStringLiteral("shortcutConfigActionButton"));
    require(keyButton != nullptr && actionButton != nullptr, "recorder controls should rebuild");
    require(lastValidatedShortcut == QStringLiteral("Num+1"),
            "the recorder should preserve the keypad modifier sent to backend validation");
    require(keyButton->text().contains(QStringLiteral("Num 1")) &&
                !keyButton->text().contains(QStringLiteral("Num+1")),
            "a keypad digit should not be displayed as a modifier combination");
    require(keyButton->property("shortcutValidationState").toString() == QStringLiteral("valid") &&
                actionButton->isEnabled(),
            "a backend-supported key should recover from an earlier validation error");
    require(modal->acceptButton()->isEnabled(),
            "the modal should become confirmable after backend validation succeeds");

    actionButton->click();
    QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
    QApplication::processEvents();
    keyButton = row.findChild<adqt::widgets::AdButton*>(QStringLiteral("shortcutConfigKeyButton"));
    require(keyButton != nullptr && keyButton->text().contains(QStringLiteral("Num 1")) &&
                !keyButton->text().contains(QStringLiteral("Num+1")),
            "the committed shortcut should retain a clear numpad display");

    keyButton->click();
    QApplication::processEvents();

    QKeyEvent numpadPlusEvent(QEvent::KeyPress, Qt::Key_Plus, Qt::KeypadModifier);
    QCoreApplication::sendEvent(configContent, &numpadPlusEvent);
    QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
    QApplication::processEvents();

    keyButton = row.findChild<adqt::widgets::AdButton*>(QStringLiteral("shortcutConfigKeyButton"));
    actionButton =
        row.findChild<adqt::widgets::AdButton*>(QStringLiteral("shortcutConfigActionButton"));
    require(keyButton != nullptr && actionButton != nullptr,
            "numpad plus should rebuild recorder controls");
    require(lastValidatedShortcut == QStringLiteral("Num++"),
            "the recorder should preserve a keypad plus for backend validation");
    require(keyButton->text().contains(QStringLiteral("Num Plus")) &&
                !keyButton->text().contains(QStringLiteral("Num++")),
            "a keypad plus should not be displayed as two shortcut separators");

    QKeyEvent shiftEvent(QEvent::KeyPress, Qt::Key_Shift, Qt::ShiftModifier);
    QCoreApplication::sendEvent(configContent, &shiftEvent);
    QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
    QApplication::processEvents();

    keyButton = row.findChild<adqt::widgets::AdButton*>(QStringLiteral("shortcutConfigKeyButton"));
    actionButton =
        row.findChild<adqt::widgets::AdButton*>(QStringLiteral("shortcutConfigActionButton"));
    require(keyButton != nullptr && actionButton != nullptr &&
                lastValidatedShortcut == QStringLiteral("Shift") &&
                keyButton->text() == QStringLiteral("Shift") && actionButton->isEnabled() &&
                modal->acceptButton()->isEnabled(),
            "a bare Shift key must validate and display as Shift without a duplicated modifier");

    actionButton->click();
    QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
    QApplication::processEvents();
    keyButton = row.findChild<adqt::widgets::AdButton*>(QStringLiteral("shortcutConfigKeyButton"));
    require(keyButton != nullptr && keyButton->text() == QStringLiteral("Shift"),
            "a committed bare Shift key must retain its normalized display");

    modal->acceptButton()->click();
    QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
    QApplication::processEvents();
    require(shortcutButton->text() == QStringLiteral("Shift"),
            "the outer shortcut control must display a committed bare Shift key without duplication");
}

void drawingRecorderUsesLocalValidationLanguage() {
    const styles::ThemeColorScheme scheme = styles::ThemeManager::instance().themeColorScheme();
    const auto mainWindowMetric = styles::buildMainWindowComponentMetricToken(scheme);

    ShortcutKeyRowConfig config;
    config.title = QStringLiteral("Shape tool");
    config.maxShortcutCount = 2;
    config.showRegistrationStatus = false;
    config.validationScope = ShortcutKeyRowConfig::ValidationScope::DrawingShortcut;
    config.shortcutValidator = [](const QString& shortcut) {
        return shortcuts::GlobalShortcutValidationResult{
            shortcut,
            false,
            shortcuts::GlobalShortcutFailureReason::AlreadyInUse,
        };
    };

    ShortcutKeyRow row(config, scheme.metricAlias, mainWindowMetric);
    row.resize(720, row.height());
    row.show();
    QApplication::processEvents();

    auto* shortcutButton =
        row.findChild<adqt::widgets::AdButton*>(QStringLiteral("shortcutKeyButton"));
    require(shortcutButton != nullptr, "drawing shortcut control should open the recorder");
    shortcutButton->click();
    QApplication::processEvents();

    auto* configContent = row.findChild<QWidget*>(QStringLiteral("shortcutConfigContent"));
    require(configContent != nullptr, "drawing shortcut recorder should be created");
    QKeyEvent duplicateEvent(QEvent::KeyPress, Qt::Key_S, Qt::NoModifier);
    QCoreApplication::sendEvent(configContent, &duplicateEvent);
    QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
    QApplication::processEvents();

    auto* keyButton =
        row.findChild<adqt::widgets::AdButton*>(QStringLiteral("shortcutConfigKeyButton"));
    auto* validationInfo =
        row.findChild<InfoTooltipIcon*>(QStringLiteral("shortcutConfigValidationTooltipTrigger"));
    require(keyButton != nullptr && validationInfo != nullptr &&
                keyButton->property("shortcutValidationState").toString() ==
                    QStringLiteral("invalid") &&
                validationInfo->accessibleName() == QStringLiteral("Invalid drawing shortcut") &&
                validationInfo->tooltipText().contains(
                    QStringLiteral("already assigned to another drawing tool")) &&
                !validationInfo->tooltipText().contains(
                    QStringLiteral("Windows global shortcut")) &&
                keyButton->accessibleDescription() == validationInfo->tooltipText(),
            "drawing shortcut conflicts must use local validation and accessibility wording");
}

void compactTitleAndKeyButtonStylesMatchReference() {
    const styles::ThemeColorScheme scheme = styles::ThemeManager::instance().themeColorScheme();
    const auto mainWindowMetric = styles::buildMainWindowComponentMetricToken(scheme);

    ShortcutKeyRowConfig config;
    config.title = QStringLiteral("Shape tool");
    config.shortcuts = {QStringLiteral("Ctrl+Shift+S")};
    config.showRegistrationStatus = false;
    config.validationScope = ShortcutKeyRowConfig::ValidationScope::DrawingShortcut;
    config.presentation = ShortcutKeyRowConfig::Presentation::CompactFormField;

    ShortcutKeyRow row(config, scheme.metricAlias, mainWindowMetric);
    row.resize(360, row.height());
    row.show();
    QApplication::processEvents();

    auto* titleLabel = row.findChild<QLabel*>(QStringLiteral("shortcutTitleLabel"));
    auto* shortcutButton =
        row.findChild<adqt::widgets::AdButton*>(QStringLiteral("shortcutKeyButton"));
    require(titleLabel != nullptr && titleLabel->text() == QStringLiteral("Shape tool:") &&
                titleLabel->font().pixelSize() == scheme.metricAlias.fontSize &&
                titleLabel->font().weight() == QFont::Normal && shortcutButton != nullptr &&
                row.height() == scheme.metricAlias.controlHeight &&
                row.cursor().shape() == Qt::ArrowCursor && row.layout() != nullptr &&
                row.layout()->contentsMargins() == QMargins() &&
                row.layout()->spacing() == scheme.metricAlias.marginXS &&
                shortcutButton->buttonStyle() ==
                    adqt::widgets::AdButton::ButtonStyle::Outline &&
                shortcutButton->accentRole() ==
                    adqt::widgets::AdButton::AccentRole::Neutral &&
                shortcutButton->height() == scheme.metricAlias.controlHeight &&
                shortcutButton->text() == QStringLiteral("Ctrl+Shift+S"),
            "compact shortcut titles and key buttons must match the reference presentation");

    const QString originalText = shortcutButton->text();
    shortcutButton->setText(QString(256, QLatin1Char('W')));
    const int longWidth = shortcutButton->sizeHint().width();
    shortcutButton->setText(QString(100, QLatin1Char('W')));
    const int cappedWidth = shortcutButton->sizeHint().width();
    shortcutButton->setText(originalText);
    require(longWidth == cappedWidth,
            "compact shortcut values must use the reference 100px text cap");

    row.setRegistrationState({});
    QApplication::processEvents();
    require(shortcutButton->text().isEmpty() &&
                shortcutButton->accentRole() ==
                    adqt::widgets::AdButton::AccentRole::Danger &&
                shortcutButton->property("registrationStatus").toInt() ==
                    static_cast<int>(shortcuts::GlobalShortcutStatus::Unset),
            "unset compact shortcuts must use the reference icon-only danger button");
}

void adjustableDelayUsesWheelAndClampsRange() {
    const styles::ThemeColorScheme scheme = styles::ThemeManager::instance().themeColorScheme();
    const auto mainWindowMetric = styles::buildMainWindowComponentMetricToken(scheme);

    int persistedDelay = 3;
    int setterCalls = 0;
    int changeSignals = 0;
    bool acceptWrites = true;
    ShortcutKeyRowConfig config;
    config.title = QStringLiteral("Delay %1s to Execute");
    config.adjustableDelay = true;
    config.delaySeconds = 3;
    config.delaySetter = [&persistedDelay, &setterCalls, &acceptWrites](int seconds) {
        ++setterCalls;
        if (!acceptWrites) {
            return false;
        }
        persistedDelay = seconds;
        return true;
    };

    ShortcutKeyRow row(config, scheme.metricAlias, mainWindowMetric);
    row.resize(720, row.height());
    row.show();
    QObject::connect(&row, &ShortcutKeyRow::delaySecondsChanged, &row,
                     [&changeSignals](int) { ++changeSignals; });
    QApplication::processEvents();

    auto* shortcutButton =
        row.findChild<adqt::widgets::AdButton*>(QStringLiteral("shortcutKeyButton"));
    auto* delayTitleLabel = row.findChild<QLabel*>(QStringLiteral("delayTitleLabel"));
    auto* delayUnderline = row.findChild<QWidget*>(QStringLiteral("delaySecondsHoverUnderline"));
    auto* delayTitleWrap = delayTitleLabel != nullptr ? delayTitleLabel->parentWidget() : nullptr;
    bool titleShowsDefaultDelay = false;
    for (const QLabel* label : row.findChildren<QLabel*>()) {
        if (label->text() == QStringLiteral("Delay 3s to Execute")) {
            titleShowsDefaultDelay = true;
            break;
        }
    }
    require(shortcutButton != nullptr && delayTitleLabel != nullptr && delayTitleWrap != nullptr &&
                delayUnderline != nullptr &&
                row.delaySeconds() == 3 && titleShowsDefaultDelay &&
                row.toolTip() == QStringLiteral("Delay: 3 seconds") &&
                row.cursor().shape() == Qt::PointingHandCursor &&
                delayTitleLabel->cursor().shape() == Qt::SplitVCursor &&
                shortcutButton->cursor().shape() != Qt::SplitVCursor &&
                !delayUnderline->isVisible(),
            "only the delay title must advertise vertical scrolling before hover");

    const auto sendWheel = [](QWidget* target, int angleDelta) {
        const QPoint localPoint = target->rect().center();
        QWheelEvent event(QPointF(localPoint), QPointF(target->mapToGlobal(localPoint)), QPoint(),
                          QPoint(0, angleDelta), Qt::NoButton, Qt::NoModifier,
                          Qt::NoScrollPhase, false);
        QCoreApplication::sendEvent(target, &event);
        return event.isAccepted();
    };

    sendWheel(&row, 120);
    require(row.delaySeconds() == 3 && persistedDelay == 3 && setterCalls == 0 &&
                changeSignals == 0,
            "scrolling outside the delay title must not change the configured delay");

    sendWheel(delayTitleWrap, 120);
    require(row.delaySeconds() == 3 && persistedDelay == 3 && setterCalls == 0 &&
                changeSignals == 0,
            "scrolling over the title container outside its text must not change the configured delay");

    sendWheel(shortcutButton, 120);
    require(row.delaySeconds() == 3 && persistedDelay == 3 && setterCalls == 0 &&
                changeSignals == 0,
            "scrolling over the shortcut button must not change the configured delay");

    require(sendWheel(delayTitleLabel, 120),
            "the delay title must consume handled wheel events");
    require(row.delaySeconds() == 4 && persistedDelay == 4 && setterCalls == 1 &&
                changeSignals == 1,
            "scrolling over the delay title must persist and publish a one-second increment");

    QEvent enterEvent(QEvent::Enter);
    QCoreApplication::sendEvent(delayTitleLabel, &enterEvent);
    require(delayUnderline->isVisible() && delayUnderline->height() == 2 &&
                delayUnderline->property("highlightColor").value<QColor>() ==
                    scheme.map.presetColorHover.value(QStringLiteral("blue"),
                                                      scheme.map.colorPrimaryHover),
            "hovering the delay title must show a blue underline below the seconds value");
    QEvent leaveEvent(QEvent::Leave);
    QCoreApplication::sendEvent(delayTitleLabel, &leaveEvent);
    require(!delayUnderline->isVisible(),
            "leaving the delay title must hide the seconds underline");

    acceptWrites = false;
    sendWheel(delayTitleLabel, -120);
    require(row.delaySeconds() == 4 && persistedDelay == 4 && setterCalls == 2 &&
                changeSignals == 1,
            "a rejected delay write must leave the displayed and persisted values unchanged");
    acceptWrites = true;

    row.setDelaySeconds(100);
    const int callsAtUpperBound = setterCalls;
    sendWheel(delayTitleLabel, 120);
    require(row.delaySeconds() == 10 && setterCalls == callsAtUpperBound,
            "delay values and upward wheel input must clamp at ten seconds");

    row.setDelaySeconds(-100);
    const int callsAtLowerBound = setterCalls;
    sendWheel(delayTitleLabel, -120);
    require(row.delaySeconds() == 1 && setterCalls == callsAtLowerBound,
            "delay values and downward wheel input must clamp at one second");
}
} // namespace

int main(int argc, char** argv) {
    bool titleAndKeyButtonOnly = false;
    for (int argumentIndex = 1; argumentIndex < argc; ++argumentIndex) {
        if (QString::fromLocal8Bit(argv[argumentIndex]) ==
            QStringLiteral("--title-and-key-button-only")) {
            titleAndKeyButtonOnly = true;
        }
    }

    QApplication application(argc, argv);
    if (titleAndKeyButtonOnly) {
        compactTitleAndKeyButtonStylesMatchReference();
        return 0;
    }

    statusPresentationUsesSemanticTokens();
    recorderAcceptsOnlyBackendSupportedShortcuts();
    drawingRecorderUsesLocalValidationLanguage();
    compactTitleAndKeyButtonStylesMatchReference();
    adjustableDelayUsesWheelAndClampsRange();
    return 0;
}
