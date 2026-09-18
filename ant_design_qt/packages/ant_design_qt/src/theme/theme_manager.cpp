#include "theme_manager.h"

#include <QProxyStyle>
#include <QStyleFactory>

#include <algorithm>

namespace adqt::theme {

namespace {

class AdThemeProxyStyle final : public QProxyStyle {
 public:
  explicit AdThemeProxyStyle(QStyle* baseStyle) : QProxyStyle(baseStyle) {}

  void setTheme(const AdTheme& theme) { theme_ = theme; }

  int pixelMetric(PixelMetric metric, const QStyleOption* option = nullptr,
                  const QWidget* widget = nullptr) const override {
    switch (metric) {
      case PM_DefaultFrameWidth:
        return std::max(1, qRound(theme_.metrics.lineWidth));
      case PM_ButtonIconSize:
        return std::max(QProxyStyle::pixelMetric(metric, option, widget),
                        qRound(theme_.metrics.fontSizeLG));
      case PM_ScrollBarExtent:
        return std::max(QProxyStyle::pixelMetric(metric, option, widget),
                        qRound(theme_.metrics.controlHeightSM));
      case PM_FocusFrameHMargin:
      case PM_FocusFrameVMargin:
        return std::max(1, qRound(theme_.metrics.lineWidth * 2.0));
      case PM_MenuHMargin:
      case PM_MenuVMargin:
        return std::max(0, qRound(theme_.metrics.sizeXS));
      default:
        return QProxyStyle::pixelMetric(metric, option, widget);
    }
  }

  int styleHint(StyleHint hint, const QStyleOption* option = nullptr,
                const QWidget* widget = nullptr,
                QStyleHintReturn* returnData = nullptr) const override {
    switch (hint) {
      case SH_Menu_SubMenuPopupDelay:
      case SH_ToolButton_PopupDelay:
        return theme_.motion.timingMenuOpenDelayMs;
      default:
        return QProxyStyle::styleHint(hint, option, widget, returnData);
    }
  }

 private:
  AdTheme theme_ = makeTheme();
};

QStyle* makeBaseStyle(const QApplication* app) {
  if (QStyle* fusion = QStyleFactory::create(QStringLiteral("Fusion"))) {
    return fusion;
  }

  if (app && QApplication::style()) {
    const QString objectName = QApplication::style()->objectName();
    if (!objectName.isEmpty()) {
      if (QStyle* style = QStyleFactory::create(objectName)) {
        return style;
      }
    }
  }

  return nullptr;
}

QPalette standardPaletteFor(const QApplication* app, const QStyle* style) {
  if (const auto* themedStyle = dynamic_cast<const AdThemeProxyStyle*>(style)) {
    if (QStyle* baseStyle = themedStyle->baseStyle()) {
      return baseStyle->standardPalette();
    }
  }

  if (style) {
    return style->standardPalette();
  }
  if (app && QApplication::style()) {
    return QApplication::style()->standardPalette();
  }
  if (QApplication::style()) {
    return QApplication::style()->standardPalette();
  }
  return QPalette();
}

ThemeConfig transitionedDefaults(const ThemeConfig& current, ThemeScheme nextScheme,
                                 ThemeDensity nextDensity) {
  const ThemeConfig currentDefaults = defaultThemeConfig(current.scheme, current.density);
  const ThemeConfig nextDefaults = defaultThemeConfig(nextScheme, nextDensity);

  ThemeConfig next = current;
  next.scheme = nextScheme;
  next.density = nextDensity;

  if (current.primary == currentDefaults.primary) {
    next.primary = nextDefaults.primary;
  }
  if (current.success == currentDefaults.success) {
    next.success = nextDefaults.success;
  }
  if (current.warning == currentDefaults.warning) {
    next.warning = nextDefaults.warning;
  }
  if (current.error == currentDefaults.error) {
    next.error = nextDefaults.error;
  }
  if (current.info == currentDefaults.info) {
    next.info = nextDefaults.info;
  }
  if (current.link == currentDefaults.link) {
    next.link = nextDefaults.link;
  }
  if (current.sizeStep == currentDefaults.sizeStep) {
    next.sizeStep = nextDefaults.sizeStep;
  }
  if (current.controlHeight == currentDefaults.controlHeight) {
    next.controlHeight = nextDefaults.controlHeight;
  }

  return next;
}

// Popup classes resolve their font from application class fonts instead of their owner's
// font chain, so an unhinted application font alone cannot reach them.
const char* const kSmoothOutlineFontClasses[] = {"QTipLabel", "QMessageBox", "QMenu"};

}  // namespace

ThemeManager& ThemeManager::instance() {
  static ThemeManager manager;
  return manager;
}

ThemeManager::ThemeManager(QObject* parent)
    : QObject(parent),
      config_(defaultThemeConfig()),
      resolved_(makeResolvedTheme(config_)),
      palette_(resolved_.palette) {}

void ThemeManager::refreshGlobalState(bool updateApplicationPalette) {
  resolved_ = makeResolvedTheme(config_);
  palette_ = buildPalette(resolved_.theme, standardPaletteFor(app_.data(), style_.data()));
  resolved_.palette = palette_;

  if (auto* themedStyle = dynamic_cast<AdThemeProxyStyle*>(style_.data())) {
    themedStyle->setTheme(resolved_.theme);
  }

  if (updateApplicationPalette && app_) {
    ensureApplicationStyle();
    QApplication::setPalette(palette_);

    if (!originalAppFontCaptured_) {
      originalAppFont_ = QApplication::font();
      originalAppFontCaptured_ = true;
    }

    applyApplicationTypography();
  }

  refreshScopeStates();
}

void ThemeManager::applyApplicationTypography() {
  // Every themed surface renders unhinted outlines: DirectWrite's default hinting keeps
  // grid fitting at fractional device scale factors, which looks rough next to the smooth
  // geometry this design system draws everywhere else. Unresolved font properties merge
  // with the application font (QFont::resolve), so parentless top-level windows, popup
  // surfaces, and partially-configured widget fonts all inherit this preference.
  QFont applicationFont =
      resolved_.config.appFont != QFont() ? resolved_.theme.appFont : originalAppFont_;
  applicationFont.setHintingPreference(QFont::PreferNoHinting);
  QApplication::setFont(applicationFont);

  for (const char* className : kSmoothOutlineFontClasses) {
    const QString classNameText = QString::fromLatin1(className);
    QFont popupFont = originalPopupClassFonts_.value(classNameText, QApplication::font());
    popupFont.setHintingPreference(QFont::PreferNoHinting);
    QApplication::setFont(popupFont, className);
  }
}

void ThemeManager::ensureApplicationStyle() {
  if (!app_) {
    return;
  }

  if (auto* themedStyle = dynamic_cast<AdThemeProxyStyle*>(style_.data())) {
    themedStyle->setTheme(resolved_.theme);
    if (QApplication::style() != themedStyle) {
      QApplication::setStyle(themedStyle);
    }
    return;
  }

  auto* themedStyle = new AdThemeProxyStyle(makeBaseStyle(app_.data()));
  themedStyle->setTheme(resolved_.theme);
  QApplication::setStyle(themedStyle);
  style_ = themedStyle;
}

void ThemeManager::setConfig(const ThemeConfig& config) {
  if (config_ == config) {
    return;
  }

  config_ = config;
  ++revision_;
  refreshGlobalState(true);
  emit themeChanged();
}

const ThemeConfig& ThemeManager::config() const { return config_; }

void ThemeManager::setSeed(const ThemeSeedToken& seed) { setConfig(seed); }

const ThemeSeedToken& ThemeManager::seed() const { return config_; }

void ThemeManager::setTheme(const AdTheme& theme) { setConfig(makeResolvedTheme(theme).config); }

void ThemeManager::setColorScheme(ThemeScheme scheme) {
  if (config_.scheme == scheme) {
    return;
  }
  setConfig(transitionedDefaults(config_, scheme, config_.density));
}

void ThemeManager::setDensity(ThemeDensity density) {
  if (config_.density == density) {
    return;
  }
  setConfig(transitionedDefaults(config_, config_.scheme, density));
}

void ThemeManager::setPreset(ThemeScheme scheme, ThemeDensity density) {
  setConfig(defaultThemeConfig(scheme, density));
}

void ThemeManager::setScopeOverride(QObject* scope, const ThemeOverride& overrideValue) {
  if (!scope) {
    return;
  }

  if (isEmptyThemeOverride(overrideValue)) {
    clearScopeOverride(scope);
    return;
  }

  auto it = scopeStates_.find(scope);
  if (it != scopeStates_.end()) {
    if (it->overrideValue == overrideValue) {
      return;
    }
    it->overrideValue = overrideValue;
  } else {
    ScopeState state;
    state.overrideValue = overrideValue;
    if (auto* widget = qobject_cast<QWidget*>(scope)) {
      state.hadExplicitPalette = widget->testAttribute(Qt::WA_SetPalette);
      state.originalPalette = widget->palette();
      state.hadExplicitFont = widget->testAttribute(Qt::WA_SetFont);
      state.originalFont = widget->font();
    }
    it = scopeStates_.insert(scope, state);
    connect(scope, &QObject::destroyed, this,
            [this](QObject* destroyedScope) { scopeStates_.remove(destroyedScope); });
  }

  applyScopeState(scope);
  ++revision_;
  emit themeChanged();
}

ThemeOverride ThemeManager::scopeOverride(const QObject* scope) const {
  if (!scope) {
    return {};
  }

  const auto it = scopeStates_.constFind(const_cast<QObject*>(scope));
  return it != scopeStates_.cend() ? it->overrideValue : ThemeOverride{};
}

void ThemeManager::clearScopeOverride(QObject* scope) {
  if (!scope) {
    return;
  }
  cleanupScope(scope);
}

const AdTheme& ThemeManager::theme() const { return resolved_.theme; }

const ResolvedTheme& ThemeManager::globalResolvedTheme() const { return resolved_; }

const ResolvedTheme& ThemeManager::resolvedTheme() const { return resolved_; }

ThemeConfig ThemeManager::resolvedConfigFor(const QWidget* widget,
                                            const QWidget* logicalOwner) const {
  ThemeConfig merged = config_;
  const QObject* cursor = logicalOwner ? static_cast<const QObject*>(logicalOwner)
                                       : static_cast<const QObject*>(widget);
  QVector<const QObject*> orderedScopes;
  while (cursor) {
    if (scopeStates_.contains(const_cast<QObject*>(cursor))) {
      orderedScopes.prepend(cursor);
    }
    cursor = cursor->parent();
  }

  for (const QObject* scope : orderedScopes) {
    const auto it = scopeStates_.constFind(const_cast<QObject*>(scope));
    if (it != scopeStates_.cend()) {
      merged = mergeThemeConfig(merged, it->overrideValue);
    }
  }

  return merged;
}

void ThemeManager::applyScopeState(QObject* scope) {
  auto it = scopeStates_.find(scope);
  if (it == scopeStates_.end()) {
    return;
  }

  auto* widget = qobject_cast<QWidget*>(scope);
  if (!widget) {
    return;
  }

  const ThemeConfig mergedConfig = resolvedConfigFor(widget);
  ResolvedTheme localResolved = makeResolvedTheme(mergedConfig);
  const QPalette basePalette = it->hadExplicitPalette ? it->originalPalette : palette_;
  localResolved.palette = buildPalette(localResolved.theme, basePalette);

  widget->setPalette(localResolved.palette);
  if (localResolved.config.appFont != QFont()) {
    widget->setFont(localResolved.theme.appFont);
  } else if (it->hadExplicitFont) {
    widget->setFont(it->originalFont);
  } else {
    widget->setFont(QFont());
    widget->setAttribute(Qt::WA_SetFont, false);
  }
}

void ThemeManager::refreshScopeStates() {
  if (scopeStates_.isEmpty()) {
    return;
  }

  QList<QObject*> scopes = scopeStates_.keys();
  std::sort(scopes.begin(), scopes.end(), [](const QObject* lhs, const QObject* rhs) {
    auto depthFor = [](const QObject* object) {
      int depth = 0;
      for (const QObject* cursor = object; cursor; cursor = cursor->parent()) {
        ++depth;
      }
      return depth;
    };
    return depthFor(lhs) < depthFor(rhs);
  });

  for (QObject* scope : scopes) {
    applyScopeState(scope);
  }
}

void ThemeManager::restoreScopeState(QObject* scope, const ScopeState& state) {
  auto* widget = qobject_cast<QWidget*>(scope);
  if (!widget) {
    return;
  }

  if (state.hadExplicitPalette) {
    widget->setPalette(state.originalPalette);
  } else {
    widget->setPalette(QPalette());
    widget->setAttribute(Qt::WA_SetPalette, false);
  }

  if (state.hadExplicitFont) {
    widget->setFont(state.originalFont);
  } else {
    widget->setFont(QFont());
    widget->setAttribute(Qt::WA_SetFont, false);
  }
}

void ThemeManager::cleanupScope(QObject* scope) {
  auto it = scopeStates_.find(scope);
  if (it == scopeStates_.end()) {
    return;
  }

  const ScopeState& state = it.value();
  restoreScopeState(scope, state);
  scopeStates_.erase(it);
  ++revision_;
  emit themeChanged();
}

ResolvedTheme ThemeManager::resolve(const QWidget* widget, const QWidget* logicalOwner) const {
  if (!widget && !logicalOwner) {
    return resolved_;
  }

  const ThemeConfig mergedConfig = resolvedConfigFor(widget, logicalOwner);
  ResolvedTheme localResolved = makeResolvedTheme(mergedConfig);

  const QWidget* paletteSource = logicalOwner ? logicalOwner : widget;
  if (paletteSource) {
    localResolved.palette = buildPalette(localResolved.theme, paletteSource->palette());
  } else {
    localResolved.palette = buildPalette(localResolved.theme, palette_);
  }
  return localResolved;
}

ThemeMapToken ThemeManager::resolveTheme(const QWidget* widget, const QWidget* logicalOwner) const {
  return resolve(widget, logicalOwner).values;
}

const QPalette& ThemeManager::globalPalette() const { return palette_; }

const QPalette& ThemeManager::palette() const { return palette_; }

const QStyle* ThemeManager::style() const {
  return style_ ? style_.data() : (app_ ? QApplication::style() : nullptr);
}

quint64 ThemeManager::themeRevision() const { return revision_; }

void ThemeManager::applyTo(QApplication& app) {
  app_ = &app;
  if (!originalAppFontCaptured_) {
    originalAppFont_ = QApplication::font();
    originalAppFontCaptured_ = true;
  }
  for (const char* className : kSmoothOutlineFontClasses) {
    const QString classNameText = QString::fromLatin1(className);
    if (originalPopupClassFonts_.contains(classNameText)) {
      continue;
    }
    // Only remember class fonts the platform actually customized; unset ones keep
    // following the application font on every refresh instead of pinning a stale copy.
    const QFont classFont = QApplication::font(className);
    if (classFont != QApplication::font()) {
      originalPopupClassFonts_.insert(classNameText, classFont);
    }
  }
  ensureApplicationStyle();
  refreshGlobalState(true);
}

}  // namespace adqt::theme
