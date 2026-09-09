#ifndef SNOW_SHOT_PRESENTATION_COMPONENTS_SHORTCUTKEYROW_H
#define SNOW_SHOT_PRESENTATION_COMPONENTS_SHORTCUTKEYROW_H

#include "icon_core.h"
#include "snow_shot/presentation/globalshortcuttypes.h"
#include "snow_shot/presentation/components/actionrow.h"

#include <QString>
#include <QStringList>

#include <functional>

#include "snow_shot/presentation/styles/themecolorscheme.h"

class QWidget;
class QLabel;
class QColor;
class QEvent;
class QPaintEvent;
class QObject;
namespace snow_shot::presentation::styles {
struct MainWindowComponentMetricToken;
struct ThemeAliasMetricToken;
} // namespace snow_shot::presentation::styles
struct ShortcutKeyRowConfig {
    enum class Presentation {
        ActionCard,
        CompactFormField,
    };

    enum class ValidationScope {
        GlobalShortcut,
        ScreenshotShortcut,
        DrawingShortcut,
        PinnedWindowShortcut,
        RecordingShortcut,
    };

    QString title;
    adqt::icons::IconRef iconRef;
    QStringList shortcuts;
    snow_shot::presentation::GlobalShortcutRegistrationState registrationState;
    QString rowState;
    bool useStableBorder = false;
    int maxShortcutCount = 2;
    std::function<snow_shot::presentation::GlobalShortcutValidationResult(const QString&)>
        shortcutValidator;
    bool adjustableDelay = false;
    int delaySeconds = 3;
    std::function<bool(int)> delaySetter;
    bool showRegistrationStatus = true;
    ValidationScope validationScope = ValidationScope::GlobalShortcut;
    Presentation presentation = Presentation::ActionCard;
};

class ShortcutKeyRow : public ActionRow {
    Q_OBJECT

  public:
    explicit ShortcutKeyRow(
        const ShortcutKeyRowConfig& config,
        const snow_shot::presentation::styles::ThemeAliasMetricToken& metric,
        const snow_shot::presentation::styles::MainWindowComponentMetricToken& mainWindowMetric,
        QWidget* parent = nullptr);
    void applyTheme(const snow_shot::presentation::styles::ThemeColorScheme& scheme) override;
    void setTitle(const QString& title);
    void retranslateUi();
    void
    setRegistrationState(const snow_shot::presentation::GlobalShortcutRegistrationState& state);
    void setDelaySeconds(int seconds);
    [[nodiscard]] int delaySeconds() const;

  signals:
    void shortcutsChanged(const QStringList& shortcuts);
    void delaySecondsChanged(int seconds);

  protected:
    bool event(QEvent* event) override;
    bool eventFilter(QObject* watched, QEvent* event) override;

  private:
    void openShortcutConfigDialog();
    [[nodiscard]] QString delayDisplayTitle() const;
    [[nodiscard]] QString titleLabelText() const;
    bool adjustDelayFromWheel(QEvent* event);
    void syncDelayUnderline();
    void syncRegistrationStatus();
    [[nodiscard]] QString registrationTooltipText() const;

    QWidget* m_delayUnderline = nullptr;
    adqt::widgets::AdButton* m_shortcutButton = nullptr;
    QString m_baseTitle;
    snow_shot::presentation::GlobalShortcutRegistrationState m_registrationState;
    bool m_showRegistrationStatus = true;
    ShortcutKeyRowConfig::ValidationScope m_validationScope =
        ShortcutKeyRowConfig::ValidationScope::GlobalShortcut;
    int m_maxShortcutCount = 2;
    bool m_adjustableDelay = false;
    bool m_delayTitleHovered = false;
    int m_delaySeconds = 3;
    std::function<bool(int)> m_delaySetter;
    std::function<snow_shot::presentation::GlobalShortcutValidationResult(const QString&)>
        m_shortcutValidator;
};

#endif // SNOW_SHOT_PRESENTATION_COMPONENTS_SHORTCUTKEYROW_H
