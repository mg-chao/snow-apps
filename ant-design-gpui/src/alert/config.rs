// Copyright (C) 2026 mg-chao
// SPDX-License-Identifier: Apache-2.0

//! Scoped Alert defaults. Clones retain icon and resolver identity for no-op updates.

use std::{fmt, sync::Arc};

use gpui::{AnyElement, App, IntoElement, SharedString, Window};

use super::{AlertStyleContext, AlertStyles, AlertVariant};
use crate::icons::{IconName, IconRef};

type ContentFn = dyn Fn(&AlertStyleContext, &mut Window, &mut App) -> AnyElement + Send + Sync;

enum IconSource {
    Reference(IconRef),
    Content(Box<ContentFn>),
}

/// A shared configured icon reference or native content factory.
///
/// Retain or clone this wrapper when editing configuration; rebuilding a wrapper explicitly
/// replaces the configured icon even when its source happens to have the same key.
#[derive(Clone)]
pub struct AlertIcon(Arc<IconSource>);

impl AlertIcon {
    pub fn new(icon: impl Into<IconRef>) -> Self {
        Self(Arc::new(IconSource::Reference(icon.into())))
    }

    /// Rebuild native content for each rendered Alert, without executing during scope resolution.
    pub fn from_content<E: IntoElement>(
        factory: impl Fn(&AlertStyleContext, &mut Window, &mut App) -> E + Send + Sync + 'static,
    ) -> Self {
        Self(Arc::new(IconSource::Content(Box::new(
            move |context, window, cx| factory(context, window, cx).into_any_element(),
        ))))
    }

    pub fn reference(&self) -> Option<&IconRef> {
        match self.0.as_ref() {
            IconSource::Reference(icon) => Some(icon),
            IconSource::Content(_) => None,
        }
    }

    pub fn render_content(
        &self,
        context: &AlertStyleContext,
        window: &mut Window,
        cx: &mut App,
    ) -> Option<AnyElement> {
        match self.0.as_ref() {
            IconSource::Reference(_) => None,
            IconSource::Content(factory) => Some(factory(context, window, cx)),
        }
    }
}

impl fmt::Debug for AlertIcon {
    fn fmt(&self, formatter: &mut fmt::Formatter<'_>) -> fmt::Result {
        formatter
            .debug_tuple("AlertIcon")
            .field(&Arc::as_ptr(&self.0))
            .finish()
    }
}

impl PartialEq for AlertIcon {
    fn eq(&self, other: &Self) -> bool {
        Arc::ptr_eq(&self.0, &other.0)
    }
}

impl Eq for AlertIcon {}

impl From<IconRef> for AlertIcon {
    fn from(icon: IconRef) -> Self {
        Self::new(icon)
    }
}

impl From<IconName> for AlertIcon {
    fn from(icon: IconName) -> Self {
        Self::new(icon)
    }
}

type StylesFn = dyn Fn(&AlertStyleContext, &mut Window, &mut App) -> AlertStyles + Send + Sync;

/// An identity-comparable scoped semantic-style resolver.
#[derive(Clone)]
pub struct AlertStyleResolver(Arc<StylesFn>);

impl AlertStyleResolver {
    pub fn new(
        resolver: impl Fn(&AlertStyleContext, &mut Window, &mut App) -> AlertStyles
        + Send
        + Sync
        + 'static,
    ) -> Self {
        Self(Arc::new(resolver))
    }

    pub fn resolve(
        &self,
        context: &AlertStyleContext,
        window: &mut Window,
        cx: &mut App,
    ) -> AlertStyles {
        (self.0)(context, window, cx)
    }
}

impl PartialEq for AlertStyleResolver {
    fn eq(&self, other: &Self) -> bool {
        Arc::ptr_eq(&self.0, &other.0)
    }
}

impl Eq for AlertStyleResolver {}

impl fmt::Debug for AlertStyleResolver {
    fn fmt(&self, formatter: &mut fmt::Formatter<'_>) -> fmt::Result {
        formatter
            .debug_tuple("AlertStyleResolver")
            .field(&Arc::as_ptr(&self.0))
            .finish()
    }
}

/// Defaults inherited through the Alert field of a configuration scope.
#[derive(Clone, Default, PartialEq)]
pub struct AlertConfig {
    pub variant: AlertVariant,
    pub closable: bool,
    /// `None` uses the effective locale's common Close label.
    pub close_label: Option<SharedString>,
    pub close_icon: Option<AlertIcon>,
    pub success_icon: Option<AlertIcon>,
    pub info_icon: Option<AlertIcon>,
    pub warning_icon: Option<AlertIcon>,
    pub error_icon: Option<AlertIcon>,
    pub styles: AlertStyles,
    pub styles_fn: Option<AlertStyleResolver>,
}

/// Missing values inherit. Reset an override to `None` to restore its parent's default.
#[derive(Clone, Default, PartialEq)]
pub struct AlertConfigPatch {
    pub variant: Option<AlertVariant>,
    pub closable: Option<bool>,
    pub close_label: Option<SharedString>,
    pub close_icon: Option<AlertIcon>,
    pub success_icon: Option<AlertIcon>,
    pub info_icon: Option<AlertIcon>,
    pub warning_icon: Option<AlertIcon>,
    pub error_icon: Option<AlertIcon>,
    pub styles: AlertStyles,
    pub styles_fn: Option<AlertStyleResolver>,
}

impl AlertConfigPatch {
    pub fn resolve(&self, parent: &AlertConfig) -> AlertConfig {
        let mut styles = parent.styles.clone();
        styles.refine(&self.styles);
        AlertConfig {
            variant: self.variant.unwrap_or(parent.variant),
            closable: self.closable.unwrap_or(parent.closable),
            close_label: self
                .close_label
                .clone()
                .or_else(|| parent.close_label.clone()),
            close_icon: self
                .close_icon
                .clone()
                .or_else(|| parent.close_icon.clone()),
            success_icon: self
                .success_icon
                .clone()
                .or_else(|| parent.success_icon.clone()),
            info_icon: self.info_icon.clone().or_else(|| parent.info_icon.clone()),
            warning_icon: self
                .warning_icon
                .clone()
                .or_else(|| parent.warning_icon.clone()),
            error_icon: self
                .error_icon
                .clone()
                .or_else(|| parent.error_icon.clone()),
            styles,
            styles_fn: self.styles_fn.clone().or_else(|| parent.styles_fn.clone()),
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::{ConfigPatch, ConfigScope, theme::Color};
    use gpui::{Styled, TestAppContext, px};
    use std::sync::atomic::{AtomicUsize, Ordering};

    #[test]
    fn alert_scoped_defaults_preserve_send_sync_configuration_snapshots() {
        fn assert_send_sync<T: Send + Sync>() {}
        assert_send_sync::<AlertIcon>();
        assert_send_sync::<AlertStyleResolver>();
        assert_send_sync::<AlertConfig>();
        assert_send_sync::<AlertConfigPatch>();
        assert_send_sync::<crate::config::ResolvedConfig>();
    }

    #[test]
    fn alert_icon_and_style_resolver_clones_preserve_identity() {
        let icon = AlertIcon::new(IconName::SmileOutlined);
        assert_eq!(icon, icon.clone());
        assert_ne!(icon, AlertIcon::new(IconName::SmileOutlined));
        assert_eq!(
            icon.reference().unwrap().key(),
            IconName::SmileOutlined.icon().key()
        );
        let factory = AlertIcon::from_content(|_, _, _| gpui::div());
        assert_eq!(factory, factory.clone());
        assert_ne!(factory, AlertIcon::from_content(|_, _, _| gpui::div()));
        assert!(factory.reference().is_none());
        let resolver = AlertStyleResolver::new(|_, _, _| AlertStyles::default());
        assert_eq!(resolver, resolver.clone());
        assert_ne!(
            resolver,
            AlertStyleResolver::new(|_, _, _| AlertStyles::default())
        );
    }

    #[gpui::test]
    fn alert_config_resolves_native_icon_factories_without_executing_them(cx: &mut TestAppContext) {
        let calls = Arc::new(AtomicUsize::new(0));
        let count = calls.clone();
        let icon = AlertIcon::from_content(move |_, _, _| {
            count.fetch_add(1, Ordering::Relaxed);
            gpui::div()
        });
        cx.update(|cx| {
            let parent = ConfigScope::new_window(cx);
            parent
                .update_patch(cx, |patch| {
                    patch.alert.success_icon = Some(icon.clone());
                    patch.alert.close_icon = Some(icon.clone());
                })
                .unwrap();
            let child = parent.child(ConfigPatch::default(), cx).unwrap();
            assert_eq!(child.snapshot(cx).alert.success_icon.as_ref(), Some(&icon));
            assert_eq!(child.snapshot(cx).alert.close_icon.as_ref(), Some(&icon));
            assert!(!child.set_patch(child.patch(cx), cx).unwrap());
            child
                .update_patch(cx, |patch| patch.alert.closable = Some(false))
                .unwrap();
            child
                .update_patch(cx, |patch| patch.alert = AlertConfigPatch::default())
                .unwrap();
            assert_eq!(child.snapshot(cx).alert.success_icon.as_ref(), Some(&icon));
            assert_eq!(
                calls.load(Ordering::Relaxed),
                0,
                "configuration changes must not build native elements"
            );
        });
        assert_eq!(calls.load(Ordering::Relaxed), 0);
    }

    #[gpui::test]
    fn alert_config_inherits_resets_and_preserves_theme_and_locale(cx: &mut TestAppContext) {
        let (parent, child, icon, resolver) = cx.update(|cx| {
            let parent = ConfigScope::new_window(cx);
            let icon = AlertIcon::new(IconName::SmileOutlined);
            let resolver = AlertStyleResolver::new(|_, _, _| AlertStyles::default());
            parent
                .update_patch(cx, |p| {
                    p.alert = AlertConfigPatch {
                        variant: Some(AlertVariant::Filled),
                        closable: Some(true),
                        close_label: Some("Dismiss notice".into()),
                        success_icon: Some(icon.clone()),
                        styles: AlertStyles {
                            root: gpui::StyleRefinement::default().bg(Color::rgb(0xeeeeee)),
                            title: gpui::StyleRefinement::default().text_size(px(18.)),
                            ..Default::default()
                        },
                        styles_fn: Some(resolver.clone()),
                        ..Default::default()
                    };
                })
                .unwrap();
            let child = parent
                .child(
                    ConfigPatch {
                        alert: AlertConfigPatch {
                            closable: Some(false),
                            styles: AlertStyles {
                                root: gpui::StyleRefinement::default().rounded(px(3.)),
                                ..Default::default()
                            },
                            ..Default::default()
                        },
                        ..Default::default()
                    },
                    cx,
                )
                .unwrap();
            (parent, child, icon, resolver)
        });
        cx.update(|cx| {
            let original = child.snapshot(cx);
            assert_eq!(original.alert.variant, AlertVariant::Filled);
            assert!(!original.alert.closable);
            assert_eq!(
                original.alert.close_label.as_deref(),
                Some("Dismiss notice")
            );
            assert_eq!(original.alert.success_icon.as_ref(), Some(&icon));
            assert_eq!(original.alert.styles_fn.as_ref(), Some(&resolver));
            assert!(original.alert.styles.root.background.is_some());
            assert!(original.alert.styles.root.corner_radii.top_left.is_some());
            assert!(original.alert.styles.title.text.font_size.is_some());
            assert!(!child.set_patch(child.patch(cx), cx).unwrap());
            assert!(std::sync::Arc::ptr_eq(&original, &child.snapshot(cx)));
            child
                .update_patch(cx, |p| p.alert = AlertConfigPatch::default())
                .unwrap();
            let reset = child.snapshot(cx);
            assert!(reset.alert.closable);
            assert_eq!(reset.alert.success_icon.as_ref(), Some(&icon));
            assert_eq!(reset.alert.styles_fn.as_ref(), Some(&resolver));
            assert!(reset.alert.styles.root.corner_radii.top_left.is_none());
            assert!(std::sync::Arc::ptr_eq(&original.theme, &reset.theme));
            assert!(std::sync::Arc::ptr_eq(&original.locale, &reset.locale));
            parent
                .update_patch(cx, |p| p.alert = AlertConfigPatch::default())
                .unwrap();
        });
        cx.update(|cx| {
            let config = child.snapshot(cx);
            assert_eq!(config.alert.variant, AlertVariant::Outlined);
            assert!(!config.alert.closable);
            assert!(config.alert.success_icon.is_none());
            assert!(config.alert.styles_fn.is_none());
            assert!(config.alert.close_label.is_none());
            assert!(config.alert.styles == AlertStyles::default());
        });
    }
}
