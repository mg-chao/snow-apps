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

    [[nodiscard]] ThemeMode themeMode() const;
    [[nodiscard]] ThemeColorScheme themeColorScheme() const;

  public slots:
    void setThemeStyleConfig(const ThemeStyleConfig& config);
    void setThemeMode(ThemeMode mode);
    void setThemeAppearance(ThemeAppearance appearance);
    void setThemePreset(ThemePreset preset);

  signals:
    void themeModeChanged(ThemeMode mode);
    void themeChanged(const ThemeColorScheme& scheme);

  private:
    explicit ThemeManager(QObject* parent = nullptr);

    void rebuildScheme();
    void applyThemeMode();

    ThemeStyleConfig m_config;
    ThemeColorScheme m_scheme;
    ThemeMode m_mode = ThemeMode::FollowSystem;
    QMetaObject::Connection m_systemColorSchemeConnection;
    bool m_initialized = false;
};
} // namespace snow_shot::presentation::styles

#endif // SNOW_SHOT_PRESENTATION_STYLES_THEMEMANAGER_H
