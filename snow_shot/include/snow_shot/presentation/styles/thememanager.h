#ifndef SNOW_SHOT_PRESENTATION_STYLES_THEMEMANAGER_H
#define SNOW_SHOT_PRESENTATION_STYLES_THEMEMANAGER_H

#include <QObject>
#include <QMetaObject>

#include "snow_shot/presentation/styles/themecolorscheme.h"

class QApplication;

namespace snow_shot::presentation::styles {
class ThemeManager : public QObject {
    Q_OBJECT

  public:
    static ThemeManager& instance();

    void initialize(QApplication& application);

    [[nodiscard]] QString appFontFamily() const;
    [[nodiscard]] int appFontSizePercentage() const;
    [[nodiscard]] ThemeMode themeMode() const;
    [[nodiscard]] ThemeColorScheme themeColorScheme() const;

  public slots:
    void setThemeStyleConfig(const ThemeStyleConfig& config);
    bool setAppFontFamily(const QString& family);
    bool setAppFontSizePercentage(int percentage);
    bool setThemePrimaryColor(const QColor& color);
    void setThemeMode(ThemeMode mode);
    void setThemeAppearance(ThemeAppearance appearance);
    void setThemePreset(ThemePreset preset);

  signals:
    void appFontFamilyChanged(const QString& family);
    void appFontSizePercentageChanged(int percentage);
    void themeModeChanged(ThemeMode mode);
    void themeChanged(const ThemeColorScheme& scheme);

  private:
    explicit ThemeManager(QObject* parent = nullptr);

    void rebuildScheme();
    void applyThemeMode();
    void applyFontSize(ThemeStyleConfig& config) const;

    ThemeStyleConfig m_config;
    ThemeColorScheme m_scheme;
    ThemeMode m_mode = ThemeMode::FollowSystem;
    QFont m_baseAppFont;
    int m_baseFontSize = 14;
    int m_appFontSizePercentage = 100;
    QMetaObject::Connection m_systemColorSchemeConnection;
    bool m_initialized = false;
};
} // namespace snow_shot::presentation::styles

#endif // SNOW_SHOT_PRESENTATION_STYLES_THEMEMANAGER_H
