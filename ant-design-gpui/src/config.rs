// Copyright (C) 2026 mg-chao
// SPDX-License-Identifier: Apache-2.0

//! Retained configuration scopes with explicit ownership and immutable snapshots.

use crate::{
    alert::{AlertConfig, AlertConfigPatch},
    descriptions::{DescriptionsConfig, DescriptionsConfigPatch},
    icons::IconPalette,
    locale::{Locale, LocalePatch},
    message::{MessageConfig, MessageConfigPatch},
    popup::{PopupConfig, PopupConfigPatch, PopupTriggers, TooltipConfig, TooltipConfigPatch},
    progress::{ProgressConfig, ProgressConfigPatch},
    result::{ResultConfig, ResultConfigPatch},
    spin::{SpinConfig, SpinConfigPatch},
    theme::{ResolvedTheme, ThemeConfig},
};
use gpui::{App, AppContext, Context, Entity, Global, Subscription};
use std::{fmt, sync::Arc};

#[derive(Clone, Copy, Debug, Default, PartialEq, Eq, Hash)]
pub enum Size {
    Small,
    #[default]
    Medium,
    Large,
}

#[derive(Clone, Copy, Debug, Default, PartialEq, Eq, Hash)]
pub enum Direction {
    #[default]
    Ltr,
    Rtl,
}

impl Direction {
    /// Convert logical start/end values into physical left/right values.
    pub fn horizontal<T>(self, start: T, end: T) -> (T, T) {
        match self {
            Self::Ltr => (start, end),
            Self::Rtl => (end, start),
        }
    }

    /// The sign of a logical forward horizontal displacement.
    pub fn forward_sign(self) -> f32 {
        match self {
            Self::Ltr => 1.0,
            Self::Rtl => -1.0,
        }
    }
}

#[derive(Clone, Copy, Debug, Default, PartialEq, Eq, Hash)]
pub enum Variant {
    #[default]
    Outlined,
    Filled,
    Borderless,
    Underlined,
}

#[derive(Clone, Copy, Debug, Default, PartialEq, Eq, Hash)]
pub enum ValidationStatus {
    #[default]
    None,
    Success,
    Warning,
    Error,
    Validating,
}

/// Pagination defaults inherited by controls in a retained configuration scope.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct PaginationConfig {
    /// `None` keeps automatic visibility based on the total item count.
    pub show_size_changer: Option<bool>,
    pub total_boundary_show_size_changer: u64,
}

impl Default for PaginationConfig {
    fn default() -> Self {
        Self {
            show_size_changer: None,
            total_boundary_show_size_changer: 50,
        }
    }
}

#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
pub struct PaginationConfigPatch {
    pub show_size_changer: Option<bool>,
    pub total_boundary_show_size_changer: Option<u64>,
}

impl PaginationConfigPatch {
    pub fn resolve(self, parent: PaginationConfig) -> PaginationConfig {
        PaginationConfig {
            show_size_changer: self.show_size_changer.or(parent.show_size_changer),
            total_boundary_show_size_changer: self
                .total_boundary_show_size_changer
                .unwrap_or(parent.total_boundary_show_size_changer),
        }
    }
}

/// Unset fields inherit; an explicit `false` overrides an inherited `true`.
#[derive(Clone, Default, PartialEq)]
pub struct ConfigPatch {
    pub theme: Option<ThemeConfig>,
    pub locale: LocalePatch,
    pub size: Option<Size>,
    pub direction: Option<Direction>,
    pub disabled: Option<bool>,
    pub variant: Option<Variant>,
    pub motion: Option<bool>,
    pub tooltip: TooltipConfigPatch,
    pub popover: PopupConfigPatch,
    pub popconfirm: PopupConfigPatch,
    pub pagination: PaginationConfigPatch,
    pub alert: AlertConfigPatch,
    pub descriptions: DescriptionsConfigPatch,
    pub result: ResultConfigPatch,
    pub image: crate::image::ImageConfigPatch,
    pub spin: SpinConfigPatch,
    pub progress: ProgressConfigPatch,
    pub message: MessageConfigPatch,
    pub modal: crate::modal::ModalConfigPatch,
    pub notification: crate::notification::NotificationConfigPatch,
    pub carousel: crate::carousel::CarouselConfigPatch,
}

/// A read-only snapshot. Cloning it shares all expensive theme/catalog data.
#[derive(Clone)]
pub struct ResolvedConfig {
    pub theme: Arc<ResolvedTheme>,
    pub icon_palette: IconPalette,
    pub font: gpui::Font,
    pub locale: Arc<Locale>,
    pub size: Size,
    pub direction: Direction,
    pub disabled: bool,
    pub variant: Variant,
    /// Scoped permission for decorative motion. GPUI reduced motion still wins.
    pub motion: bool,
    pub tooltip: TooltipConfig,
    pub popover: PopupConfig,
    pub popconfirm: PopupConfig,
    pub pagination: PaginationConfig,
    pub alert: AlertConfig,
    pub descriptions: DescriptionsConfig,
    pub result: ResultConfig,
    pub image: crate::image::ImageConfig,
    pub spin: SpinConfig,
    pub progress: ProgressConfig,
    pub message: MessageConfig,
    pub modal: crate::modal::ModalConfig,
    pub notification: crate::notification::NotificationConfig,
    pub carousel: crate::carousel::CarouselConfig,
    configured_size: Option<Size>,
    theme_config: Arc<ThemeConfig>,
}

fn shared_eq<T: PartialEq>(left: &Arc<T>, right: &Arc<T>) -> bool {
    Arc::ptr_eq(left, right) || left == right
}

impl PartialEq for ResolvedConfig {
    fn eq(&self, other: &Self) -> bool {
        self.size == other.size
            && self.configured_size == other.configured_size
            && self.direction == other.direction
            && self.disabled == other.disabled
            && self.variant == other.variant
            && self.motion == other.motion
            && self.tooltip == other.tooltip
            && self.popover == other.popover
            && self.popconfirm == other.popconfirm
            && self.pagination == other.pagination
            && self.alert == other.alert
            && self.descriptions == other.descriptions
            && self.result == other.result
            && self.image == other.image
            && self.spin == other.spin
            && self.progress == other.progress
            && self.message == other.message
            && self.modal == other.modal
            && self.notification == other.notification
            && self.carousel == other.carousel
            && shared_eq(&self.locale, &other.locale)
            && shared_eq(&self.theme_config, &other.theme_config)
            && shared_eq(&self.theme, &other.theme)
            && self.icon_palette == other.icon_palette
            && self.font == other.font
    }
}

impl Default for ResolvedConfig {
    fn default() -> Self {
        let theme_config = Arc::new(ThemeConfig::default());
        let theme = Arc::new(theme_config.resolve());
        Self {
            icon_palette: IconPalette::from_theme(&theme.tokens),
            font: theme.tokens.font_family.to_gpui_font(),
            theme,
            theme_config,
            locale: Locale::en_us(),
            size: Size::Medium,
            configured_size: None,
            direction: Direction::Ltr,
            disabled: false,
            variant: Variant::Outlined,
            motion: true,
            tooltip: TooltipConfig::default(),
            popover: PopupConfig::default(),
            popconfirm: PopupConfig {
                triggers: PopupTriggers::CLICK,
                ..Default::default()
            },
            pagination: PaginationConfig::default(),
            alert: AlertConfig::default(),
            descriptions: DescriptionsConfig::default(),
            result: ResultConfig::default(),
            image: Default::default(),
            spin: SpinConfig::default(),
            progress: ProgressConfig::default(),
            message: MessageConfig::default(),
            modal: Default::default(),
            notification: Default::default(),
            carousel: Default::default(),
        }
    }
}

impl ResolvedConfig {
    /// The nearest explicitly configured size, preserving an unset provider.
    /// Some controls, such as Divider, distinguish their default spacing from medium.
    pub fn configured_size(&self) -> Option<Size> {
        self.configured_size
    }

    pub fn theme_config(&self) -> &ThemeConfig {
        &self.theme_config
    }

    /// Resolve a control's local availability using provider-style override precedence.
    pub fn availability(
        &self,
        disabled: Option<bool>,
        busy: bool,
    ) -> crate::behavior::Availability {
        crate::behavior::Availability {
            disabled: disabled.unwrap_or(self.disabled),
            busy,
        }
    }

    pub fn motion_policy(&self, cx: &App) -> crate::behavior::MotionPolicy {
        crate::behavior::MotionPolicy::new(
            self.motion && self.theme.tokens.motion,
            cx.reduce_motion(),
        )
    }

    pub fn focus_ring(&self) -> crate::behavior::FocusRing {
        crate::behavior::FocusRing::new(
            self.theme.tokens.color_primary_border,
            gpui::px(self.theme.tokens.line_width_focus as f32),
        )
        .radius(gpui::px(self.theme.tokens.border_radius as f32))
    }

    fn resolve(
        parent: &Self,
        patch: &ConfigPatch,
        previous: Option<&Self>,
    ) -> Result<Self, ConfigError> {
        let theme_config = patch.theme.as_ref().map_or_else(
            || parent.theme_config.clone(),
            |local| Arc::new(ThemeConfig::merged(&parent.theme_config, local)),
        );
        let theme = if shared_eq(&theme_config, &parent.theme_config) {
            parent.theme.clone()
        } else if let Some(previous) =
            previous.filter(|old| shared_eq(&old.theme_config, &theme_config))
        {
            previous.theme.clone()
        } else {
            Arc::new(
                theme_config
                    .try_resolve()
                    .map_err(|error| ConfigError(error.to_string()))?,
            )
        };
        let icon_palette = if Arc::ptr_eq(&theme, &parent.theme) {
            parent.icon_palette
        } else if let Some(previous) = previous.filter(|old| Arc::ptr_eq(&theme, &old.theme)) {
            previous.icon_palette
        } else {
            IconPalette::from_theme(&theme.tokens)
        };
        let font = if Arc::ptr_eq(&theme, &parent.theme) {
            parent.font.clone()
        } else if let Some(previous) = previous.filter(|old| Arc::ptr_eq(&theme, &old.theme)) {
            previous.font.clone()
        } else {
            theme.tokens.font_family.to_gpui_font()
        };
        Ok(Self {
            theme,
            icon_palette,
            font,
            theme_config,
            locale: patch
                .locale
                .resolve_with_previous(&parent.locale, previous.map(|old| &old.locale)),
            size: patch.size.unwrap_or(parent.size),
            configured_size: patch.size.or(parent.configured_size),
            direction: patch.direction.unwrap_or(parent.direction),
            disabled: patch.disabled.unwrap_or(parent.disabled),
            variant: patch.variant.unwrap_or(parent.variant),
            motion: patch.motion.unwrap_or(parent.motion),
            tooltip: patch.tooltip.resolve(parent.tooltip),
            popover: patch.popover.resolve(parent.popover),
            popconfirm: patch.popconfirm.resolve(parent.popconfirm),
            pagination: patch.pagination.resolve(parent.pagination),
            alert: patch.alert.resolve(&parent.alert),
            descriptions: patch.descriptions.resolve(&parent.descriptions),
            result: patch.result.resolve(&parent.result),
            image: patch.image.resolve(&parent.image),
            spin: patch.spin.resolve(&parent.spin),
            progress: patch.progress.resolve(&parent.progress),
            message: patch.message.resolve(&parent.message),
            modal: patch.modal.resolve(&parent.modal),
            notification: patch.notification.resolve(&parent.notification),
            carousel: patch.carousel.resolve(&parent.carousel),
        })
    }
}

#[derive(Clone, Debug, PartialEq, Eq)]
pub struct ConfigError(String);

impl fmt::Display for ConfigError {
    fn fmt(&self, formatter: &mut fmt::Formatter<'_>) -> fmt::Result {
        formatter.write_str(&self.0)
    }
}

impl std::error::Error for ConfigError {}

struct ConfigState {
    parent: Option<ConfigScope>,
    base: Option<Arc<ResolvedConfig>>,
    // Semantic style patches grow with the component catalog. Keep GPUI entity
    // moves and the closures that create/update them independent of that size.
    patch: Box<ConfigPatch>,
    snapshot: Arc<ResolvedConfig>,
    last_error: Option<ConfigError>,
    _parent_subscription: Option<Subscription>,
}

impl ConfigState {
    fn parent_snapshot(&self, cx: &App) -> Arc<ResolvedConfig> {
        self.parent.as_ref().map_or_else(
            || {
                self.base
                    .as_ref()
                    .expect("application defaults own their base")
                    .clone()
            },
            |parent| parent.snapshot(cx),
        )
    }

    fn recompute(&mut self, cx: &mut Context<Self>) {
        let parent = self.parent_snapshot(cx);
        match ResolvedConfig::resolve(&parent, &self.patch, Some(&self.snapshot)) {
            Ok(next) => {
                let changed = *self.snapshot != next;
                let recovered = self.last_error.take().is_some();
                if changed {
                    self.snapshot = Arc::new(next);
                }
                if changed || recovered {
                    cx.notify();
                }
            }
            Err(error) => {
                // A custom algorithm can become invalid after a parent update.
                // Retain a coherent last-good snapshot and expose the diagnostic.
                if self.last_error.as_ref() != Some(&error) {
                    self.last_error = Some(error);
                    cx.notify();
                }
            }
        }
    }
}

struct ApplicationDefaults(ConfigScope);
impl Global for ApplicationDefaults {}

/// A cheap, retained handle. Child scopes own parents; observers hold weak children.
///
/// Views retain the subscription returned by [`Self::observe`]. Parent propagation
/// is coalesced by GPUI's notification queue at the end of the current update.
#[derive(Clone, PartialEq, Eq)]
pub struct ConfigScope(Entity<ConfigState>);

impl ConfigScope {
    /// The nearest explicit unique-tooltip boundary owns its window-local group.
    pub(crate) fn tooltip_unique_group(&self, cx: &App) -> Option<gpui::EntityId> {
        let mut scope = self.clone();
        loop {
            let state = scope.0.read(cx);
            if let Some(unique) = state.patch.tooltip.unique {
                return unique.then_some(scope.0.entity_id());
            }
            let parent = state.parent.clone()?;
            scope = parent;
        }
    }
    pub(crate) fn identity(&self) -> gpui::EntityId {
        self.0.entity_id()
    }

    /// Application-wide defaults shared by new and existing windows.
    pub fn application(cx: &mut App) -> Self {
        crate::init(cx);
        cx.global::<ApplicationDefaults>().0.clone()
    }

    /// Create an independently configurable window root inheriting application defaults.
    pub fn new_window(cx: &mut App) -> Self {
        Self::application(cx)
            .child(ConfigPatch::default(), cx)
            .expect("default scope is valid")
    }

    pub fn child(&self, patch: ConfigPatch, cx: &mut App) -> Result<Self, ConfigError> {
        let patch = Box::new(patch);
        let parent = self.clone();
        let base = parent.snapshot(cx);
        let snapshot = Arc::new(ResolvedConfig::resolve(&base, &patch, None)?);
        let entity = cx.new(|cx| {
            let subscription = cx.observe(&parent.0, |this: &mut ConfigState, _, cx| {
                this.recompute(cx)
            });
            ConfigState {
                parent: Some(parent),
                base: None,
                patch,
                snapshot,
                last_error: None,
                _parent_subscription: Some(subscription),
            }
        });
        Ok(Self(entity))
    }

    /// Read without resolving tokens or allocating a new snapshot.
    pub fn snapshot(&self, cx: &App) -> Arc<ResolvedConfig> {
        self.0.read(cx).snapshot.clone()
    }

    pub fn patch(&self, cx: &App) -> ConfigPatch {
        (*self.0.read(cx).patch).clone()
    }

    /// Failure leaves both the patch and effective configuration unchanged.
    /// Returns whether the effective snapshot changed.
    pub fn set_patch(&self, patch: ConfigPatch, cx: &mut App) -> Result<bool, ConfigError> {
        let patch = Box::new(patch);
        self.0.update(cx, |this, cx| {
            if this.patch == patch {
                return Ok(false);
            }
            let parent = this.parent_snapshot(cx);
            let next = ResolvedConfig::resolve(&parent, &patch, Some(&this.snapshot))?;
            this.patch = patch;
            let changed = *this.snapshot != next;
            let recovered = this.last_error.take().is_some();
            if changed {
                this.snapshot = Arc::new(next);
            }
            if changed || recovered {
                cx.notify();
            }
            Ok(changed)
        })
    }

    /// Replace selected local overrides, resolving once after the edit.
    pub fn update_patch(
        &self,
        cx: &mut App,
        edit: impl FnOnce(&mut ConfigPatch),
    ) -> Result<bool, ConfigError> {
        let mut patch = self.patch(cx);
        edit(&mut patch);
        self.set_patch(patch, cx)
    }

    pub fn last_error(&self, cx: &App) -> Option<ConfigError> {
        self.0.read(cx).last_error.clone()
    }

    pub fn observe<T: 'static>(&self, cx: &mut Context<T>) -> Subscription {
        cx.observe(&self.0, |_, _, cx| cx.notify())
    }

    pub(crate) fn observe_in<T: 'static>(
        &self,
        window: &mut gpui::Window,
        cx: &mut Context<T>,
        mut on_notify: impl FnMut(&mut T, &mut gpui::Window, &mut Context<T>) + 'static,
    ) -> Subscription {
        cx.observe_in(&self.0, window, move |this, _, window, cx| {
            on_notify(this, window, cx)
        })
    }
}

pub(crate) fn initialize(cx: &mut App) {
    if cx.has_global::<ApplicationDefaults>() {
        return;
    }
    let base = Arc::new(ResolvedConfig::default());
    let patch = Box::new(ConfigPatch::default());
    let scope = ConfigScope(cx.new(|_| ConfigState {
        parent: None,
        base: Some(base.clone()),
        patch,
        snapshot: base,
        last_error: None,
        _parent_subscription: None,
    }));
    cx.set_global(ApplicationDefaults(scope));
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::locale::{CommonMessagePatch, LocaleId};
    use gpui::TestAppContext;

    #[test]
    fn retained_config_keeps_entity_creation_and_update_frames_small() {
        // Native Windows UI threads have a much smaller stack than Rust test
        // threads. A by-value patch made opening a nested Modal exhaust it as
        // GPUI moved ConfigState through entity-creation closures.
        assert!(std::mem::size_of::<ConfigState>() <= 256);
    }

    #[gpui::test]
    fn carousel_styles_inherit_and_keep_unrelated_snapshots_shared(cx: &mut TestAppContext) {
        use gpui::{StyleRefinement, Styled, px};

        let (parent, child) = cx.update(|cx| {
            let parent = ConfigScope::new_window(cx);
            let child = parent
                .child(
                    ConfigPatch {
                        carousel: crate::carousel::CarouselConfigPatch {
                            styles: crate::carousel::CarouselStyles {
                                dot: StyleRefinement::default().h(px(5.)),
                                ..Default::default()
                            },
                            ..Default::default()
                        },
                        ..Default::default()
                    },
                    cx,
                )
                .unwrap();
            parent
                .update_patch(cx, |patch| {
                    patch.carousel.styles.root = StyleRefinement::default().pt(px(12.));
                })
                .unwrap();
            (parent, child)
        });
        cx.update(|cx| {
            let snapshot = child.snapshot(cx);
            assert_eq!(
                snapshot.carousel.styles.root.padding.top,
                Some(px(12.).into())
            );
            assert_eq!(
                snapshot.carousel.styles.dot.size.height,
                Some(px(5.).into())
            );
            assert!(!child.update_patch(cx, |_| {}).unwrap());
            assert!(Arc::ptr_eq(&snapshot, &child.snapshot(cx)));
            parent
                .update_patch(cx, |patch| {
                    patch.carousel.styles.root = StyleRefinement::default().pt(px(24.));
                })
                .unwrap();
        });
        cx.update(|cx| {
            assert_eq!(
                child.snapshot(cx).carousel.styles.root.padding.top,
                Some(px(24.).into())
            );
            assert_eq!(
                child.snapshot(cx).carousel.styles.dot.size.height,
                Some(px(5.).into())
            );
        });
    }

    #[gpui::test]
    fn message_and_modal_settings_propagate_independently(cx: &mut TestAppContext) {
        let (parent, child) = cx.update(|cx| {
            let parent = ConfigScope::new_window(cx);
            let child = parent.child(ConfigPatch::default(), cx).unwrap();
            parent
                .update_patch(cx, |patch| {
                    patch.message.top = Some(32.);
                    patch.modal.centered = Some(true);
                })
                .unwrap();
            (parent, child)
        });
        cx.update(|cx| {
            assert_eq!(child.snapshot(cx).message.top, 32.);
            assert!(child.snapshot(cx).modal.centered);
            assert!(
                parent
                    .update_patch(cx, |patch| patch.message.top = Some(48.))
                    .unwrap()
            );
        });
        cx.update(|cx| {
            assert_eq!(child.snapshot(cx).message.top, 48.);
            assert!(child.snapshot(cx).modal.centered);
            assert!(
                parent
                    .update_patch(cx, |patch| patch.modal.centered = Some(false))
                    .unwrap()
            );
        });
        cx.update(|cx| {
            assert_eq!(child.snapshot(cx).message.top, 48.);
            assert!(!child.snapshot(cx).modal.centered);
        });
    }

    #[gpui::test]
    fn result_scopes_inherit_styles_without_running_resolvers(cx: &mut TestAppContext) {
        use crate::result::{ResultStyleResolver, ResultStyles};
        use gpui::{StyleRefinement, Styled, px};
        use std::sync::atomic::{AtomicUsize, Ordering};

        let calls = Arc::new(AtomicUsize::new(0));
        let count = calls.clone();
        let resolver = ResultStyleResolver::new(move |_, _, _| {
            count.fetch_add(1, Ordering::Relaxed);
            ResultStyles::default()
        });
        let (parent, child) = cx.update(|cx| {
            let parent = ConfigScope::new_window(cx);
            parent
                .update_patch(cx, |patch| {
                    patch.result.styles.root = StyleRefinement::default().pt(px(19.));
                    patch.result.styles_fn = Some(resolver.clone());
                })
                .unwrap();
            let child = parent
                .child(
                    ConfigPatch {
                        result: crate::result::ResultConfigPatch {
                            styles: ResultStyles {
                                title: StyleRefinement::default().text_size(px(27.)),
                                ..Default::default()
                            },
                            ..Default::default()
                        },
                        ..Default::default()
                    },
                    cx,
                )
                .unwrap();
            (parent, child)
        });
        cx.update(|cx| {
            let snapshot = child.snapshot(cx);
            assert_eq!(
                snapshot.result.styles.root.padding.top,
                Some(px(19.).into())
            );
            assert_eq!(
                snapshot.result.styles.title.text.font_size,
                Some(px(27.).into())
            );
            assert!(snapshot.result.styles_fn.as_ref() == Some(&resolver));
            let original = parent.snapshot(cx);
            assert!(!parent.update_patch(cx, |_| {}).unwrap());
            assert!(Arc::ptr_eq(&original, &parent.snapshot(cx)));
            parent
                .update_patch(cx, |patch| {
                    patch.result.styles.root = StyleRefinement::default().pt(px(23.));
                })
                .unwrap();
        });
        cx.update(|cx| {
            let snapshot = child.snapshot(cx);
            assert_eq!(
                snapshot.result.styles.root.padding.top,
                Some(px(23.).into())
            );
            assert_eq!(
                snapshot.result.styles.title.text.font_size,
                Some(px(27.).into())
            );
        });
        assert_eq!(calls.load(Ordering::Relaxed), 0, "configuration stays lazy");
    }

    #[gpui::test]
    fn pagination_defaults_inherit_threshold_and_explicit_false(cx: &mut TestAppContext) {
        let (root, inherited, hidden) = cx.update(|cx| {
            let root = ConfigScope::new_window(cx);
            assert_eq!(root.snapshot(cx).pagination, PaginationConfig::default());
            let inherited = root.child(ConfigPatch::default(), cx).unwrap();
            let hidden = root
                .child(
                    ConfigPatch {
                        pagination: PaginationConfigPatch {
                            show_size_changer: Some(false),
                            ..Default::default()
                        },
                        ..Default::default()
                    },
                    cx,
                )
                .unwrap();
            (root, inherited, hidden)
        });
        cx.update(|cx| {
            root.update_patch(cx, |patch| {
                patch.pagination.show_size_changer = Some(true);
                patch.pagination.total_boundary_show_size_changer = Some(100);
            })
            .unwrap();
        });
        cx.update(|cx| {
            let inherited_config = inherited.snapshot(cx);
            assert_eq!(inherited_config.pagination.show_size_changer, Some(true));
            assert_eq!(
                inherited_config.pagination.total_boundary_show_size_changer,
                100
            );
            let hidden_config = hidden.snapshot(cx);
            assert_eq!(hidden_config.pagination.show_size_changer, Some(false));
            assert_eq!(
                hidden_config.pagination.total_boundary_show_size_changer,
                100
            );
            let previous = root.snapshot(cx);
            assert!(!root.update_patch(cx, |_| {}).unwrap());
            assert!(Arc::ptr_eq(&previous, &root.snapshot(cx)));
            root.update_patch(cx, |patch| {
                patch.pagination = PaginationConfigPatch::default();
            })
            .unwrap();
        });
        cx.update(|cx| {
            assert_eq!(
                inherited.snapshot(cx).pagination,
                PaginationConfig::default()
            );
            assert_eq!(
                hidden.snapshot(cx).pagination.show_size_changer,
                Some(false)
            );
            assert_eq!(
                hidden
                    .snapshot(cx)
                    .pagination
                    .total_boundary_show_size_changer,
                50
            );
        });
    }

    #[gpui::test]
    fn scopes_inherit_explicit_false_and_remain_isolated(cx: &mut TestAppContext) {
        let (application, left, right, child) = cx.update(|cx| {
            let application = ConfigScope::application(cx);
            let left = ConfigScope::new_window(cx);
            let right = ConfigScope::new_window(cx);
            let child = left
                .child(
                    ConfigPatch {
                        disabled: Some(false),
                        ..Default::default()
                    },
                    cx,
                )
                .unwrap();
            (application, left, right, child)
        });
        cx.update(|cx| {
            application
                .update_patch(cx, |patch| patch.disabled = Some(true))
                .unwrap()
        });
        cx.update(|cx| {
            assert!(left.snapshot(cx).disabled);
            assert!(right.snapshot(cx).disabled);
            assert!(!child.snapshot(cx).disabled);
            left.update_patch(cx, |patch| patch.size = Some(Size::Small))
                .unwrap();
        });
        cx.update(|cx| {
            assert_eq!(child.snapshot(cx).size, Size::Small);
            assert_eq!(right.snapshot(cx).size, Size::Medium);
        });
    }

    #[gpui::test]
    fn no_op_updates_and_unrelated_config_preserve_theme_identity(cx: &mut TestAppContext) {
        let scope = cx.update(ConfigScope::new_window);
        let original = cx.update(|cx| scope.snapshot(cx));
        cx.update(|cx| {
            assert!(!scope.set_patch(ConfigPatch::default(), cx).unwrap());
            assert!(Arc::ptr_eq(&original, &scope.snapshot(cx)));
            assert!(
                scope
                    .update_patch(cx, |patch| patch.size = Some(Size::Medium))
                    .unwrap()
            );
            let explicit = scope.snapshot(cx);
            assert_eq!(explicit.size, original.size);
            assert_eq!(explicit.configured_size(), Some(Size::Medium));
            assert_eq!(original.configured_size(), None);
            assert!(Arc::ptr_eq(&original.theme, &explicit.theme));
            assert!(
                !scope
                    .update_patch(cx, |patch| patch.size = Some(Size::Medium))
                    .unwrap()
            );
            assert!(Arc::ptr_eq(&explicit, &scope.snapshot(cx)));
            scope
                .update_patch(cx, |patch| patch.direction = Some(Direction::Rtl))
                .unwrap();
            assert!(Arc::ptr_eq(&original.theme, &scope.snapshot(cx).theme));
        });
    }

    #[gpui::test]
    fn configured_size_preserves_inherited_presence_and_removal(cx: &mut TestAppContext) {
        let (root, child) = cx.update(|cx| {
            let root = ConfigScope::new_window(cx);
            let child = root.child(ConfigPatch::default(), cx).unwrap();
            assert_eq!(child.snapshot(cx).configured_size(), None);
            root.update_patch(cx, |patch| patch.size = Some(Size::Medium))
                .unwrap();
            (root, child)
        });
        cx.update(|cx| {
            assert_eq!(child.snapshot(cx).configured_size(), Some(Size::Medium));
            child
                .update_patch(cx, |patch| patch.size = Some(Size::Small))
                .unwrap();
            root.update_patch(cx, |patch| patch.size = None).unwrap();
        });
        cx.update(|cx| {
            assert_eq!(root.snapshot(cx).configured_size(), None);
            assert_eq!(child.snapshot(cx).configured_size(), Some(Size::Small));
            child.update_patch(cx, |patch| patch.size = None).unwrap();
            assert_eq!(child.snapshot(cx).configured_size(), None);
            assert_eq!(child.snapshot(cx).size, Size::Medium);
        });
    }

    #[gpui::test]
    fn runtime_locale_changes_preserve_explicit_direction(cx: &mut TestAppContext) {
        let (root, child) = cx.update(|cx| {
            let root = ConfigScope::new_window(cx);
            let child = root
                .child(
                    ConfigPatch {
                        direction: Some(Direction::Rtl),
                        locale: LocalePatch {
                            common: CommonMessagePatch {
                                close: Some("Dismiss".into()),
                                ..Default::default()
                            },
                            ..Default::default()
                        },
                        ..Default::default()
                    },
                    cx,
                )
                .unwrap();
            (root, child)
        });
        cx.update(|cx| {
            root.update_patch(cx, |patch| patch.locale.locale = Some(Locale::zh_tw()))
                .unwrap()
        });
        cx.update(|cx| {
            let snapshot = child.snapshot(cx);
            assert_eq!(snapshot.locale.id, LocaleId::ZhTw);
            assert_eq!(snapshot.locale.common.close.as_ref(), "Dismiss");
            assert_eq!(snapshot.locale.common.show.as_ref(), "顯示");
            assert_eq!(snapshot.direction, Direction::Rtl);
        });
    }

    #[test]
    fn logical_edges_follow_direction() {
        assert_eq!(Direction::Ltr.horizontal(4, 8), (4, 8));
        assert_eq!(Direction::Rtl.horizontal(4, 8), (8, 4));
    }

    #[gpui::test]
    fn theme_inheritance_reset_and_invalid_updates_are_transactional(cx: &mut TestAppContext) {
        let (root, inherited, reset) = cx.update(|cx| {
            let root = ConfigScope::new_window(cx);
            root.update_patch(cx, |patch| {
                let mut theme = ThemeConfig::default();
                theme.seed.font_size = Some(18.);
                patch.theme = Some(theme);
            })
            .unwrap();
            let inherited = root.child(ConfigPatch::default(), cx).unwrap();
            let reset = root
                .child(
                    ConfigPatch {
                        theme: Some(ThemeConfig {
                            inherit: false,
                            ..Default::default()
                        }),
                        ..Default::default()
                    },
                    cx,
                )
                .unwrap();
            (root, inherited, reset)
        });
        cx.update(|cx| {
            assert_eq!(inherited.snapshot(cx).theme.tokens.font_size, 18.);
            assert_eq!(reset.snapshot(cx).theme.tokens.font_size, 14.);
            let original = root.snapshot(cx);
            let result = root.update_patch(cx, |patch| {
                patch.theme.as_mut().unwrap().seed.font_size = Some(f64::NAN);
            });
            assert!(result.is_err());
            assert!(Arc::ptr_eq(&original, &root.snapshot(cx)));
            assert_eq!(root.patch(cx).theme.unwrap().seed.font_size, Some(18.));
        });
    }

    #[gpui::test]
    fn child_scope_subscriptions_do_not_retain_released_children(cx: &mut TestAppContext) {
        let parent = cx.update(ConfigScope::new_window);
        let child = cx.update(|cx| parent.child(ConfigPatch::default(), cx).unwrap());
        let weak_child = child.0.downgrade();
        drop(child);
        cx.update(|cx| {
            parent
                .update_patch(cx, |patch| patch.size = Some(Size::Large))
                .unwrap();
        });
        assert!(weak_child.upgrade().is_none());
    }

    #[gpui::test]
    fn child_scopes_release_obsolete_parent_themes_and_reuse_local_catalogs(
        cx: &mut TestAppContext,
    ) {
        let (root, child, old_theme, old_locale) = cx.update(|cx| {
            let root = ConfigScope::new_window(cx);
            root.update_patch(cx, |patch| {
                patch.theme = Some(ThemeConfig {
                    seed: crate::theme::SeedTokenPatch {
                        font_size: Some(18.),
                        ..Default::default()
                    },
                    ..Default::default()
                })
            })
            .unwrap();
            let child = root
                .child(
                    ConfigPatch {
                        locale: LocalePatch {
                            common: CommonMessagePatch {
                                close: Some("Dismiss".into()),
                                ..Default::default()
                            },
                            ..Default::default()
                        },
                        ..Default::default()
                    },
                    cx,
                )
                .unwrap();
            let old_theme = Arc::downgrade(&root.snapshot(cx).theme);
            let old_locale = child.snapshot(cx).locale.clone();
            (root, child, old_theme, old_locale)
        });
        cx.update(|cx| {
            root.update_patch(cx, |patch| {
                patch.theme.as_mut().unwrap().seed.font_size = Some(20.);
            })
            .unwrap()
        });
        cx.update(|cx| {
            assert!(
                old_theme.upgrade().is_none(),
                "child retained an obsolete parent theme"
            );
            assert!(Arc::ptr_eq(&old_locale, &child.snapshot(cx).locale));
        });
    }

    #[gpui::test]
    fn inherited_custom_algorithm_failures_keep_last_good_snapshot_and_recover(
        cx: &mut TestAppContext,
    ) {
        use crate::theme::{
            AlgorithmChain, BuiltinAlgorithm, MapTokens, MappingAlgorithm, SeedTokens,
        };
        let (root, child, original) = cx.update(|cx| {
            let root = ConfigScope::new_window(cx);
            let algorithms = AlgorithmChain::default().then_custom(Arc::new(
                |seed: &SeedTokens, previous: Option<&MapTokens>| {
                    let mut map = previous
                        .cloned()
                        .unwrap_or_else(|| BuiltinAlgorithm::Default.derive(seed, None));
                    if seed.font_size > 16. {
                        map.line_height = f64::NAN;
                    }
                    map
                },
            ));
            let child = root
                .child(
                    ConfigPatch {
                        theme: Some(ThemeConfig {
                            algorithms: Some(algorithms),
                            ..Default::default()
                        }),
                        ..Default::default()
                    },
                    cx,
                )
                .unwrap();
            let original = child.snapshot(cx);
            (root, child, original)
        });
        cx.update(|cx| {
            root.update_patch(cx, |patch| {
                patch.theme = Some(ThemeConfig {
                    seed: crate::theme::SeedTokenPatch {
                        font_size: Some(18.),
                        ..Default::default()
                    },
                    ..Default::default()
                });
            })
            .unwrap()
        });
        cx.update(|cx| {
            assert!(child.last_error(cx).is_some());
            assert!(Arc::ptr_eq(&original, &child.snapshot(cx)));
            root.update_patch(cx, |patch| patch.theme = None).unwrap();
        });
        cx.update(|cx| {
            assert!(child.last_error(cx).is_none());
            assert_eq!(child.snapshot(cx).theme.tokens.font_size, 14.);
        });
    }
}
