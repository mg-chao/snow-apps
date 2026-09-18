#pragma once

#include "theme_palette.h"
#include "theme_types.h"

#include <QApplication>
#include <QHash>
#include <QObject>
#include <QPointer>
#include <QWidget>

class QStyle;

namespace adqt::theme {

class ThemeManager final : public QObject {
  Q_OBJECT

 public:
  static ThemeManager& instance();

  void setConfig(const ThemeConfig& config);
  const ThemeConfig& config() const;

  void setSeed(const ThemeSeedToken& seed);
  const ThemeSeedToken& seed() const;

  void setTheme(const AdTheme& theme);
  void setColorScheme(ThemeScheme scheme);
  void setDensity(ThemeDensity density);
  void setPreset(ThemeScheme scheme, ThemeDensity density = ThemeDensity::Comfortable);

  void setScopeOverride(QObject* scope, const ThemeOverride& overrideValue);
  ThemeOverride scopeOverride(const QObject* scope) const;
  void clearScopeOverride(QObject* scope);

  const AdTheme& theme() const;
  const ResolvedTheme& globalResolvedTheme() const;
  const ResolvedTheme& resolvedTheme() const;
  ResolvedTheme resolve(const QWidget* widget = nullptr,
                        const QWidget* logicalOwner = nullptr) const;
  ThemeMapToken resolveTheme(const QWidget* widget = nullptr,
                             const QWidget* logicalOwner = nullptr) const;

  const QPalette& globalPalette() const;
  const QPalette& palette() const;
  const QStyle* style() const;
  quint64 themeRevision() const;

  void applyTo(QApplication& app);

 signals:
  void themeChanged();

 private:
  struct ScopeState {
    ThemeOverride overrideValue;
    bool hadExplicitPalette = false;
    QPalette originalPalette;
    bool hadExplicitFont = false;
    QFont originalFont;
  };

  explicit ThemeManager(QObject* parent = nullptr);

  void refreshGlobalState(bool updateApplicationPalette);
  void applyApplicationTypography();
  void ensureApplicationStyle();
  ThemeConfig resolvedConfigFor(const QWidget* widget, const QWidget* logicalOwner = nullptr) const;
  void applyScopeState(QObject* scope);
  void refreshScopeStates();
  void restoreScopeState(QObject* scope, const ScopeState& state);
  void cleanupScope(QObject* scope);

  ThemeConfig config_;
  ResolvedTheme resolved_;
  QPalette palette_;
  QPointer<QApplication> app_;
  QPointer<QStyle> style_;
  QHash<QObject*, ScopeState> scopeStates_;
  QFont originalAppFont_;
  bool originalAppFontCaptured_ = false;
  QHash<QString, QFont> originalPopupClassFonts_;
  quint64 revision_ = 1;
};

}  // namespace adqt::theme
