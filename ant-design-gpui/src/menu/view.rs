// Copyright (C) 2026 mg-chao
// SPDX-License-Identifier: Apache-2.0

use super::{
    MenuColorTheme, MenuContentRenderer, MenuEntry, MenuItems, MenuKey, MenuMode, MenuOptions,
    MenuPopupContext, MenuPopupRenderer, MenuRenderContext, MenuSource, MenuState, MenuTheme,
    MenuTokens, MenuTrigger,
    overflow::{MenuStack, ResponsiveMenu},
    state::{self, EventHandler},
    style::{MenuStyle, MenuVisualState},
};
use crate::{
    ConfigScope, Direction, ResolvedConfig, Tooltip,
    behavior::{Accessibility, FocusRing, ScalarTransition},
    icons::{IconName, IconRenderOptions, icon},
    popup::{Popup, PopupPlacement, PopupTriggers},
};
use gpui::{
    AnyElement, App, Context, ElementId, Entity, IntoElement, MouseButton, Pixels, Refineable,
    Render, RenderOnce, Role, ScrollHandle, SharedString, StyleRefinement, Subscription,
    WeakEntity, Window, accesskit, canvas, div, prelude::*, px, relative, size,
};
use std::{
    collections::{HashMap, HashSet},
    rc::Rc,
    sync::Arc,
    time::Duration,
};

/// Native styling slots. Item identities, focus and activation remain owned by Menu.
#[derive(Clone, Default)]
pub struct MenuStyles {
    pub root: StyleRefinement,
    pub item: StyleRefinement,
    pub item_content: StyleRefinement,
    pub submenu_title: StyleRefinement,
    pub group_title: StyleRefinement,
    pub divider: StyleRefinement,
    pub popup: StyleRefinement,
    pub submenu_list: StyleRefinement,
    pub icon: StyleRefinement,
    pub extra: StyleRefinement,
    pub expand_icon: StyleRefinement,
}

impl MenuStyles {
    fn refine(&mut self, other: &Self) {
        macro_rules! slots { ($($slot:ident),+) => { $(self.$slot.refine(&other.$slot);)+ }; }
        slots!(
            root,
            item,
            item_content,
            submenu_title,
            group_title,
            divider,
            popup,
            submenu_list,
            icon,
            extra,
            expand_icon
        );
    }
}

type StyleResolver = Rc<dyn Fn(&MenuStyleContext, &mut Window, &mut App) -> MenuStyles>;

#[derive(Clone, Copy, Debug, PartialEq)]
pub struct MenuStyleContext {
    pub mode: MenuMode,
    pub color_theme: MenuColorTheme,
    pub inline_collapsed: bool,
    pub multiple: bool,
    pub selectable: bool,
    pub disabled: bool,
}

/// Build a fresh element around a retained controller on every render.
#[derive(IntoElement)]
pub struct Menu {
    id: ElementId,
    state: Entity<MenuState>,
    scope: Option<ConfigScope>,
    options: Option<MenuOptions>,
    mode: Option<MenuMode>,
    color_theme: Option<MenuColorTheme>,
    inline_collapsed: Option<bool>,
    inline_indent: Option<f64>,
    multiple: Option<bool>,
    selectable: Option<bool>,
    trigger: Option<MenuTrigger>,
    open_delay: Option<Duration>,
    close_delay: Option<Duration>,
    force_submenu_render: Option<bool>,
    tooltip: Option<bool>,
    tooltip_placement: Option<PopupPlacement>,
    disabled: Option<bool>,
    selected: Option<Vec<MenuKey>>,
    default_selected: Option<Vec<MenuKey>>,
    open: Option<Vec<MenuKey>>,
    default_open: Option<Vec<MenuKey>>,
    label: Option<SharedString>,
    automation_id: Option<SharedString>,
    styles: MenuStyles,
    styles_fn: Option<StyleResolver>,
    expand_icon: Option<MenuContentRenderer>,
    overflowed_indicator: Option<MenuContentRenderer>,
    max_height: Option<Pixels>,
    popup_render: Option<MenuPopupRenderer>,
    on_event: Option<EventHandler>,
}

impl Menu {
    pub fn new(id: impl Into<ElementId>, state: &Entity<MenuState>) -> Self {
        Self {
            id: id.into(),
            state: state.clone(),
            scope: None,
            options: None,
            mode: None,
            color_theme: None,
            inline_collapsed: None,
            inline_indent: None,
            multiple: None,
            selectable: None,
            trigger: None,
            open_delay: None,
            close_delay: None,
            force_submenu_render: None,
            tooltip: None,
            tooltip_placement: None,
            disabled: None,
            selected: None,
            default_selected: None,
            open: None,
            default_open: None,
            label: None,
            automation_id: None,
            styles: MenuStyles::default(),
            styles_fn: None,
            expand_icon: None,
            overflowed_indicator: None,
            max_height: None,
            popup_render: None,
            on_event: None,
        }
    }

    pub fn scope(mut self, scope: ConfigScope) -> Self {
        self.scope = Some(scope);
        self
    }

    pub fn options(mut self, options: MenuOptions) -> Self {
        self.options = Some(options);
        self
    }

    pub fn mode(mut self, mode: MenuMode) -> Self {
        self.mode = Some(mode);
        self
    }

    pub fn color_theme(mut self, theme: MenuColorTheme) -> Self {
        self.color_theme = Some(theme);
        self
    }

    pub fn inline_collapsed(mut self, collapsed: bool) -> Self {
        self.inline_collapsed = Some(collapsed);
        self
    }

    pub fn inline_indent(mut self, indent: f64) -> Self {
        self.inline_indent = Some(indent);
        self
    }

    pub fn multiple(mut self, multiple: bool) -> Self {
        self.multiple = Some(multiple);
        self
    }

    pub fn selectable(mut self, selectable: bool) -> Self {
        self.selectable = Some(selectable);
        self
    }

    pub fn trigger_submenu_action(mut self, trigger: MenuTrigger) -> Self {
        self.trigger = Some(trigger);
        self
    }

    pub fn open_delay(mut self, delay: Duration) -> Self {
        self.open_delay = Some(delay);
        self
    }

    pub fn close_delay(mut self, delay: Duration) -> Self {
        self.close_delay = Some(delay);
        self
    }

    pub fn force_submenu_render(mut self, render: bool) -> Self {
        self.force_submenu_render = Some(render);
        self
    }

    pub fn tooltip(mut self, enabled: bool) -> Self {
        self.tooltip = Some(enabled);
        self
    }

    pub fn tooltip_placement(mut self, placement: PopupPlacement) -> Self {
        self.tooltip_placement = Some(placement);
        self
    }

    pub fn disabled(mut self, disabled: bool) -> Self {
        self.disabled = Some(disabled);
        self
    }

    pub fn selected_keys(mut self, keys: impl IntoIterator<Item = impl Into<MenuKey>>) -> Self {
        self.selected = Some(keys.into_iter().map(Into::into).collect());
        self
    }

    pub fn default_selected_keys(
        mut self,
        keys: impl IntoIterator<Item = impl Into<MenuKey>>,
    ) -> Self {
        self.default_selected = Some(keys.into_iter().map(Into::into).collect());
        self
    }

    pub fn open_keys(mut self, keys: impl IntoIterator<Item = impl Into<MenuKey>>) -> Self {
        self.open = Some(keys.into_iter().map(Into::into).collect());
        self
    }

    pub fn default_open_keys(mut self, keys: impl IntoIterator<Item = impl Into<MenuKey>>) -> Self {
        self.default_open = Some(keys.into_iter().map(Into::into).collect());
        self
    }

    pub fn accessibility_label(mut self, label: impl Into<SharedString>) -> Self {
        self.label = Some(label.into());
        self
    }

    pub fn automation_id(mut self, id: impl Into<SharedString>) -> Self {
        self.automation_id = Some(id.into());
        self
    }

    pub fn styles(mut self, styles: MenuStyles) -> Self {
        self.styles = styles;
        self
    }

    pub fn popup_render<E: IntoElement>(
        mut self,
        render: impl Fn(AnyElement, &MenuPopupContext, &mut Window, &mut App) -> E + 'static,
    ) -> Self {
        self.popup_render = Some(Rc::new(move |content, context, window, cx| {
            render(content, context, window, cx).into_any_element()
        }));
        self
    }

    pub fn styles_fn(
        mut self,
        resolve: impl Fn(&MenuStyleContext, &mut Window, &mut App) -> MenuStyles + 'static,
    ) -> Self {
        self.styles_fn = Some(Rc::new(resolve));
        self
    }

    pub fn expand_icon<E: IntoElement>(
        mut self,
        render: impl Fn(&MenuRenderContext, &mut Window, &mut App) -> E + 'static,
    ) -> Self {
        self.expand_icon = Some(Rc::new(move |context, window, cx| {
            render(context, window, cx).into_any_element()
        }));
        self
    }

    /// Bound the persistent root viewport. Long bounded menus use native lazy rows.
    pub fn max_height(mut self, height: Pixels) -> Self {
        self.max_height = Some(height.max(px(0.)));
        self
    }

    pub fn overflowed_indicator<E: IntoElement>(
        mut self,
        render: impl Fn(&MenuRenderContext, &mut Window, &mut App) -> E + 'static,
    ) -> Self {
        self.overflowed_indicator = Some(Rc::new(move |context, window, cx| {
            render(context, window, cx).into_any_element()
        }));
        self
    }

    pub fn on_event(
        mut self,
        callback: impl Fn(&super::MenuEvent, &mut Window, &mut App) + 'static,
    ) -> Self {
        self.on_event = Some(Rc::new(callback));
        self
    }
}

impl Styled for Menu {
    fn style(&mut self) -> &mut StyleRefinement {
        &mut self.styles.root
    }
}

struct BranchMotion {
    transition: ScalarTransition,
    open: bool,
}

struct CollapseMotion {
    width: ScalarTransition,
    icon: ScalarTransition,
    padding: ScalarTransition,
    content: ScalarTransition,
    collapsed: bool,
}

#[derive(Default)]
struct RowMotion {
    foreground: crate::input::motion::ColorMotion,
    background: crate::input::motion::ColorMotion,
}

#[derive(Clone, Copy)]
struct CollapseSample {
    width: Pixels,
    icon: f64,
    padding: f64,
    content: f64,
    moving: bool,
}

struct Mount {
    state: WeakEntity<MenuState>,
    epoch: std::time::Instant,
    motion: HashMap<MenuKey, BranchMotion>,
    scroll: HashMap<MenuKey, ScrollHandle>,
    lists: HashMap<MenuKey, gpui::ListState>,
    revealed: HashMap<MenuKey, MenuKey>,
    list_sources: HashMap<MenuKey, Rc<Vec<FlatRow>>>,
    flat_cache: Option<FlatCache>,
    widths: HashMap<MenuKey, WidthCache>,
    frame_pending: bool,
    collapse: Option<CollapseMotion>,
    last_root_width: Pixels,
    expanded_width: Pixels,
    hovered: HashSet<MenuKey>,
    pressed: HashSet<MenuKey>,
    row_motion: HashMap<MenuKey, RowMotion>,
    arrows: HashMap<MenuKey, ScalarTransition>,
    branch_model: Option<MenuItems>,
    branch_keys: Vec<MenuKey>,
    popup_rows: HashMap<MenuKey, FlatCache>,
    overflow_items: Option<(MenuItems, usize, MenuItems)>,
    model_revision: u64,
    row_bounds: HashMap<MenuKey, gpui::Bounds<Pixels>>,
    row_geometry_revision: u64,
    _observer: Subscription,
    _release: Subscription,
}

struct FlatCache {
    items: MenuItems,
    open: Vec<MenuKey>,
    inline: bool,
    color_theme: MenuColorTheme,
    rows: Rc<Vec<FlatRow>>,
    tokens: Arc<MenuTokens>,
    row_geometry_revision: u64,
}

struct WidthCache {
    items: MenuItems,
    tokens: Arc<MenuTokens>,
    font: gpui::Font,
    width: Pixels,
}

impl Mount {
    fn sync_model(&mut self, items: &MenuItems, revision: u64) {
        if self.model_revision == revision {
            return;
        }
        self.model_revision = revision;
        self.widths.clear();
        self.popup_rows.clear();
        self.flat_cache = None;
        self.overflow_items = None;
        self.list_sources.clear();
        self.branch_model = None;
        self.branch_keys.clear();
        self.pressed.clear();
        self.row_bounds.clear();
        self.row_geometry_revision = self.row_geometry_revision.wrapping_add(1);
        self.revealed.clear();
        self.hovered.retain(|key| items.enabled(key));
        self.motion.retain(|key, _| items.is_submenu(key));
        self.arrows.retain(|key, _| items.is_submenu(key));
        self.row_motion.retain(|key, _| items.item(key).is_some());
        let current = |key: &MenuKey| {
            key.strip_prefix("submenu:")
                .is_none_or(|key| items.is_submenu(key))
        };
        self.lists.retain(|key, _| current(key));
        self.scroll.retain(|key, _| current(key));
    }

    fn sample_collapse(
        &mut self,
        options: &MenuOptions,
        styles: &MenuStyles,
        tokens: &MenuTokens,
        policy: crate::behavior::MotionPolicy,
        now: std::time::Instant,
        rem: Pixels,
    ) -> CollapseSample {
        let elapsed = now.saturating_duration_since(self.epoch);
        let collapsed = options.collapsed();
        let explicit = styles.root.size.width.and_then(|length| match length {
            gpui::Length::Definite(gpui::DefiniteLength::Absolute(value)) => {
                Some(value.to_pixels(rem))
            }
            _ => None,
        });
        if !collapsed && let Some(width) = explicit {
            self.expanded_width = width;
        }
        let target = if collapsed {
            explicit.unwrap_or(metric(tokens.collapsed_width))
        } else {
            explicit.unwrap_or(self.expanded_width)
        };
        let curve = tokens.globals().motion_ease_in_out;
        let ratio = if collapsed { 1. } else { 0. };
        let motion = self.collapse.get_or_insert_with(|| CollapseMotion {
            width: ScalarTransition::new(
                f64::from(f32::from(target)),
                crate::theme::CubicBezier::new(0.2, 0., 0., 1.),
            ),
            icon: ScalarTransition::new(ratio, tokens.globals().motion_ease_out),
            padding: ScalarTransition::new(ratio, curve),
            content: ScalarTransition::new(1. - ratio, curve),
            collapsed,
        });
        if motion.collapsed != collapsed
            || !policy.allows_motion()
            || (motion.width.target() - f64::from(f32::from(target))).abs() > 0.01
        {
            motion.collapsed = collapsed;
            motion.width.retarget(
                f64::from(f32::from(target)),
                elapsed,
                tokens.globals().motion_duration_slow,
                policy,
            );
            motion
                .icon
                .retarget(ratio, elapsed, tokens.globals().motion_duration_mid, policy);
            motion.padding.retarget(
                ratio,
                elapsed,
                tokens.globals().motion_duration_slow + Duration::from_millis(100),
                policy,
            );
            motion.content.retarget(
                1. - ratio,
                elapsed,
                tokens.globals().motion_duration_slow,
                policy,
            );
        }
        let width = motion.width.sample(elapsed);
        let icon = motion.icon.sample(elapsed);
        let padding = motion.padding.sample(elapsed);
        let content = motion.content.sample(elapsed);
        CollapseSample {
            width: px(width.value as f32),
            icon: icon.value.clamp(0., 1.),
            padding: padding.value.clamp(0., 1.),
            content: content.value.clamp(0., 1.),
            moving: !width.finished || !icon.finished || !padding.finished || !content.finished,
        }
    }
}

#[derive(Clone)]
struct MenuView {
    state: Entity<MenuState>,
    mount: Entity<Mount>,
    scope: ConfigScope,
    config: Arc<ResolvedConfig>,
    tokens: Arc<MenuTokens>,
    options: MenuOptions,
    styles: MenuStyles,
    popup_render: Option<MenuPopupRenderer>,
    tooltip_placement: Option<PopupPlacement>,
    automation_id: Option<SharedString>,
    expand_icon: Option<MenuContentRenderer>,
    flattened: bool,
    collapse: CollapseSample,
    overflowed_indicator: Option<MenuContentRenderer>,
}

struct FlatRow {
    key: MenuKey,
    entry: Option<MenuEntry>,
    depth: usize,
    parent: Option<MenuKey>,
    color_theme: MenuColorTheme,
    clips: Vec<VirtualClip>,
}

struct VirtualClip {
    branch: MenuKey,
    offset: Pixels,
    total: Pixels,
}

struct MenuMount {
    renderer: Entity<MenuRenderer>,
}

struct MenuRenderer {
    props: Option<MenuProps>,
    cache: Entity<Mount>,
    _subscription: Subscription,
}

#[derive(Clone)]
struct MenuProps {
    id: ElementId,
    state: Entity<MenuState>,
    styles: MenuStyles,
    styles_fn: Option<StyleResolver>,
    popup_render: Option<MenuPopupRenderer>,
    tooltip_placement: Option<PopupPlacement>,
    automation_id: Option<SharedString>,
    expand_icon: Option<MenuContentRenderer>,
    label: Option<SharedString>,
    max_height: Option<Pixels>,
    disabled: Option<bool>,
    overflowed_indicator: Option<MenuContentRenderer>,
}

impl RenderOnce for Menu {
    fn render(self, window: &mut Window, cx: &mut App) -> impl IntoElement {
        let scope = self.scope.unwrap_or_else(|| ConfigScope::application(cx));
        let config = scope.snapshot(cx);
        let state = self.state;
        let instance: ElementId = (
            self.id,
            SharedString::from(format!("menu-controller-{}", state.entity_id())),
        )
            .into();
        let observed = state.clone();
        let holder = window.use_keyed_state(instance.clone(), cx, move |_, cx| {
            observed.update(cx, |s, _| s.mounted = true);
            let cache = cx.new(|cx| Mount {
                state: observed.downgrade(),
                epoch: cx.background_executor().now(),
                motion: HashMap::new(),
                scroll: HashMap::new(),
                lists: HashMap::new(),
                revealed: HashMap::new(),
                list_sources: HashMap::new(),
                flat_cache: None,
                widths: HashMap::new(),
                frame_pending: false,
                collapse: None,
                last_root_width: px(0.),
                expanded_width: px(256.),
                hovered: HashSet::new(),
                pressed: HashSet::new(),
                row_motion: HashMap::new(),
                arrows: HashMap::new(),
                branch_model: None,
                branch_keys: Vec::new(),
                popup_rows: HashMap::new(),
                overflow_items: None,
                model_revision: u64::MAX,
                row_bounds: HashMap::new(),
                row_geometry_revision: 0,
                _observer: cx.observe(&observed, |_, _, cx| cx.notify()),
                _release: cx.on_release(|mount: &mut Mount, cx| {
                    let _ = mount.state.update(cx, |s, _| s.unmount());
                }),
            });
            let renderer = cx.new(|cx| MenuRenderer {
                props: None,
                _subscription: cx.observe(&cache, |_, _, cx| cx.notify()),
                cache,
            });
            MenuMount { renderer }
        });
        let renderer = holder.read(cx).renderer.clone();
        let mut options = self
            .options
            .unwrap_or_else(|| state.read(cx).options.clone());
        macro_rules! option { ($($name:ident),+) => { $(if let Some(value) = self.$name { options.$name = value; })+ }; }
        option!(
            mode,
            color_theme,
            inline_collapsed,
            inline_indent,
            multiple,
            selectable,
            trigger,
            open_delay,
            close_delay,
            force_submenu_render,
            tooltip
        );
        state.update(cx, |s, cx| {
            s.bind_scope(scope.clone(), cx);
            s.disabled = self.disabled.unwrap_or(config.disabled);
            s.on_event = self.on_event;
            s.configure(
                options.clone(),
                self.selected,
                self.default_selected,
                self.open,
                self.default_open,
                cx,
            );
        });
        renderer.update(cx, |renderer, _| {
            renderer.props = Some(MenuProps {
                id: instance,
                state,
                styles: self.styles,
                styles_fn: self.styles_fn,
                popup_render: self.popup_render,
                tooltip_placement: self.tooltip_placement,
                automation_id: self.automation_id,
                expand_icon: self.expand_icon,
                label: self.label,
                max_height: self.max_height,
                disabled: self.disabled,
                overflowed_indicator: self.overflowed_indicator,
            })
        });
        renderer
    }
}

impl Render for MenuRenderer {
    fn render(&mut self, window: &mut Window, cx: &mut Context<Self>) -> impl IntoElement {
        let props = self
            .props
            .as_ref()
            .expect("configured menu renderer")
            .clone();
        let state = props.state;
        let scope = state.read(cx).scope.clone().expect("mounted menu scope");
        let config = scope.snapshot(cx);
        let model = state.read(cx).items.clone();
        let revision = state.read(cx).revision;
        self.cache
            .update(cx, |cache, _| cache.sync_model(&model, revision));
        state.update(cx, |s, _| {
            s.disabled = props.disabled.unwrap_or(config.disabled)
        });
        state.update(cx, |s, cx| s.repair_rendered_focus(window, cx));
        let options = state.read(cx).options.clone();
        let tokens = config.theme.component::<MenuTheme>();
        let palette = MenuStyle::from_tokens(&tokens, options.color_theme);
        let mut styles = props.styles;
        if let Some(resolve) = props.styles_fn {
            styles.refine(&resolve(
                &MenuStyleContext {
                    mode: options.mode,
                    color_theme: options.color_theme,
                    inline_collapsed: options.inline_collapsed,
                    multiple: options.multiple,
                    selectable: options.selectable,
                    disabled: state.read(cx).disabled,
                },
                window,
                cx,
            ));
        }
        let policy = crate::behavior::MotionPolicy::new(
            config.motion && tokens.globals().motion,
            cx.reduce_motion(),
        );
        let now = cx.background_executor().now();
        let collapse = self.cache.update(cx, |cache, _| {
            cache.sample_collapse(&options, &styles, &tokens, policy, now, window.rem_size())
        });
        let view = MenuView {
            state: state.clone(),
            mount: self.cache.clone(),
            scope,
            config: config.clone(),
            tokens: tokens.clone(),
            options: options.clone(),
            styles: styles.clone(),
            popup_render: props.popup_render,
            tooltip_placement: props.tooltip_placement,
            automation_id: props.automation_id.clone(),
            expand_icon: props.expand_icon,
            flattened: false,
            collapse,
            overflowed_indicator: props.overflowed_indicator,
        };
        let items = state.read(cx).items.clone();
        let disabled = state.read(cx).disabled;
        let focus = state.update(cx, |s, _| {
            s.focus = s.focus.clone().tab_stop(!disabled);
            s.focus.clone()
        });
        let keys = state.clone();
        let next = state.clone();
        let previous = state.clone();
        let mut root = div()
            .id(props.id)
            .relative()
            .min_w(px(0.))
            .font(tokens.font().clone())
            .text_size(px(tokens.globals().font_size as f32))
            .line_height(relative(tokens.globals().line_height as f32))
            .text_color(palette.text)
            .bg(palette.bg)
            .track_focus(&focus)
            .tab_index(if state.read(cx).disabled { -1 } else { 0 })
            .on_key_down(move |event, window, cx| state::key(&keys, None, event, window, cx))
            .on_action(move |_: &crate::behavior::FocusNext, window, cx| {
                state::traverse(&next, false, window, cx)
            })
            .on_action(move |_: &crate::behavior::FocusPrevious, window, cx| {
                state::traverse(&previous, true, window, cx)
            });
        if options.mode == MenuMode::Horizontal && items.is_empty() {
            let border = if options.color_theme == MenuColorTheme::Dark {
                px(0.)
            } else {
                metric(tokens.globals().line_width)
            };
            root = root
                .w_full()
                .h(border)
                .border_b(border)
                .border_color(palette.split);
            state.update(cx, |s, _| {
                s.overflow_start = 0;
                s.overflow_open = false;
            });
        } else if options.mode == MenuMode::Horizontal {
            let overflow_width = view.measure_overflow(window, cx);
            let item_view = view.clone();
            let width_view = view.clone();
            let overflow_view = view.clone();
            let item_items = items.clone();
            let width_items = items.clone();
            let overflow_items = items.clone();
            let range_state = state.clone();
            root = root
                .w_full()
                .h(metric(tokens.horizontal_line_height))
                .border_b(if options.color_theme == MenuColorTheme::Dark {
                    px(0.)
                } else {
                    px(tokens.globals().line_width as f32)
                })
                .border_color(palette.split)
                .child(ResponsiveMenu {
                    count: items.entries().len(),
                    height: metric(tokens.horizontal_line_height),
                    overflow_width,
                    width: Rc::new(move |index, window, _| {
                        width_view.horizontal_width(&width_items.entries()[index], window)
                    }),
                    direction: config.direction,
                    item: Rc::new(move |index, window, cx| {
                        item_view.entry(
                            &item_items.entries()[index],
                            0,
                            false,
                            None,
                            item_view.options.color_theme,
                            window,
                            cx,
                        )
                    }),
                    overflow: Rc::new(move |first, window, cx| {
                        overflow_view.overflow(&overflow_items, first, window, cx)
                    }),
                    on_range: Rc::new(move |first, window, cx| {
                        let repair = range_state.update(cx, |s, _| {
                            let previous = s.overflow_start;
                            if previous == first {
                                return None;
                            }
                            s.overflow_start = first;
                            let mut repair = None;
                            if !s.overflow_open
                                && let Some((key, focus)) = s
                                    .row_focus
                                    .iter()
                                    .find(|(_, focus)| focus.is_focused(window))
                                && s.items
                                    .root_position(key)
                                    .is_some_and(|index| index >= first)
                            {
                                repair = Some((focus.clone(), s.overflow_focus.clone()));
                            }
                            if first >= s.items.entries().len() {
                                s.overflow_open = false;
                                if s.overflow_focus.is_focused(window) {
                                    let key = s
                                        .active
                                        .clone()
                                        .filter(|key| s.items.enabled(key))
                                        .or_else(|| {
                                            s.items
                                                .visible_keys(&s.open, false, None)
                                                .into_iter()
                                                .find(|key| {
                                                    s.items
                                                        .root_position(key)
                                                        .is_some_and(|index| index >= previous)
                                                })
                                        })
                                        .or_else(|| {
                                            s.items
                                                .visible_keys(&s.open, false, None)
                                                .into_iter()
                                                .next()
                                        });
                                    if let Some(key) = key {
                                        s.active = Some(key.clone());
                                        let target = s
                                            .row_focus
                                            .get(&key)
                                            .cloned()
                                            .unwrap_or_else(|| s.focus.clone());
                                        repair = Some((s.overflow_focus.clone(), target));
                                    }
                                }
                            }
                            repair.map(|(from, to)| (from, to, s.revision))
                        });
                        if let Some((from, to, revision)) = repair
                            && range_state.read(cx).mounted
                            && range_state.read(cx).revision == revision
                            && range_state.read(cx).overflow_start == first
                            && from.is_focused(window)
                        {
                            window.focus(&to, cx);
                        }
                    }),
                });
        } else {
            root = root
                .flex()
                .flex_col()
                .pb(view.trailing_margin(&items, false, cx))
                .border_color(palette.split)
                .when(config.direction == Direction::Ltr, |root| {
                    root.border_r(metric(palette.active_bar_border_width))
                })
                .when(config.direction == Direction::Rtl, |root| {
                    root.border_l(metric(palette.active_bar_border_width))
                });
            if options.inline_collapsed {
                root = root.w(metric(tokens.collapsed_width));
            }
            if let Some(max_height) = props.max_height {
                root = root
                    .max_h(max_height)
                    .overflow_hidden()
                    .child(view.bounded_root(&items, max_height, window, cx));
            } else if view.can_stack(&items, cx) {
                root =
                    root.child(view.stack(&items, 0, false, None, options.color_theme, window, cx));
            } else {
                root = root.children(items.entries().iter().map(|entry| {
                    view.entry(entry, 0, false, None, options.color_theme, window, cx)
                }));
            }
        }
        root.style().refine(&styles.root);
        if collapse.moving && options.mode != MenuMode::Horizontal {
            root = root.w(collapse.width).overflow_hidden();
            view.request_frame(window, cx);
        }
        let measured = self.cache.clone();
        let expanded = !options.collapsed() && !collapse.moving;
        root = root.child(
            canvas(
                |_, _, _| (),
                move |bounds, _, _, cx| {
                    measured.update(cx, |cache, _| {
                        cache.last_root_width = bounds.size.width;
                        if expanded && bounds.size.width > px(0.) {
                            cache.expanded_width = bounds.size.width;
                        }
                    });
                },
            )
            .absolute()
            .inset_0(),
        );
        let mut semantics = Accessibility::new(if options.mode == MenuMode::Horizontal {
            Role::MenuBar
        } else {
            Role::Menu
        });
        semantics.label = Some(
            props
                .label
                .unwrap_or_else(|| config.locale.common.navigation_menu.clone()),
        );
        semantics.automation_id = props.automation_id;
        semantics.disabled = state.read(cx).disabled;
        semantics.apply(root)
    }
}

fn metric(value: f64) -> Pixels {
    px(if value.is_finite() {
        value.clamp(0., 100_000.) as f32
    } else {
        0.
    })
}

impl MenuView {
    fn motion_policy(&self, cx: &App) -> crate::behavior::MotionPolicy {
        crate::behavior::MotionPolicy::new(
            self.config.motion && self.tokens.globals().motion,
            cx.reduce_motion(),
        )
    }

    fn can_stack(&self, items: &MenuItems, cx: &App) -> bool {
        if self.styles.item != StyleRefinement::default()
            || self.styles.item_content != StyleRefinement::default()
            || self.styles.submenu_title != StyleRefinement::default()
            || self.styles.group_title != StyleRefinement::default()
            || self.styles.divider != StyleRefinement::default()
            || self.styles.submenu_list != StyleRefinement::default()
        {
            return false;
        }
        items.entries().iter().all(|entry| {
            if let Some(item) = entry.item()
                && (item.render.is_some()
                    || item.extra_render.is_some()
                    || item.style != StyleRefinement::default())
            {
                return false;
            }
            match entry {
                MenuEntry::Group(group) => self.can_stack(&group.children, cx),
                MenuEntry::SubMenu(submenu)
                    if self.inline(false)
                        && (self.state.read(cx).open.contains(&submenu.item.key)
                            || self
                                .mount
                                .read(cx)
                                .motion
                                .get(&submenu.item.key)
                                .is_some_and(|motion| {
                                    motion
                                        .transition
                                        .sample(
                                            cx.background_executor()
                                                .now()
                                                .saturating_duration_since(
                                                    self.mount.read(cx).epoch,
                                                ),
                                        )
                                        .value
                                        > 0.
                                })) =>
                {
                    self.can_stack(&submenu.children, cx)
                }
                _ => true,
            }
        })
    }

    fn row_height(&self, item: &super::MenuItem, submenu: bool, cx: &App) -> Pixels {
        let height = item
            .style
            .size
            .height
            .or(if submenu {
                self.styles.submenu_title.size.height
            } else {
                None
            })
            .or(self.styles.item.size.height);
        match height {
            Some(gpui::Length::Definite(gpui::DefiniteLength::Absolute(value))) => {
                value.to_pixels(px(16.))
            }
            Some(gpui::Length::Auto) => self
                .mount
                .read(cx)
                .row_bounds
                .get(&item.key)
                .map_or(metric(self.tokens.item_height), |bounds| bounds.size.height),
            _ => metric(self.tokens.item_height),
        }
    }

    fn leading_margin(&self, entry: &MenuEntry) -> Pixels {
        match entry {
            MenuEntry::Group(_) => px(0.),
            MenuEntry::Divider(_) => metric(self.tokens.globals().line_width),
            _ => metric(self.tokens.item_margin_block),
        }
    }

    fn trailing_entry(&self, entry: &MenuEntry, popup: bool, cx: &App) -> Pixels {
        match entry {
            MenuEntry::Group(group) => self.trailing_margin(&group.children, popup, cx),
            MenuEntry::Divider(_) => metric(self.tokens.globals().line_width),
            MenuEntry::SubMenu(submenu) if self.inline(popup) => {
                let elapsed = cx
                    .background_executor()
                    .now()
                    .saturating_duration_since(self.mount.read(cx).epoch);
                let progress = self.mount.read(cx).motion.get(&submenu.item.key).map_or(
                    if self.state.read(cx).open.contains(&submenu.item.key) {
                        1.
                    } else {
                        0.
                    },
                    |motion| motion.transition.sample(elapsed).value,
                );
                if progress > 0. {
                    px(0.)
                } else {
                    metric(self.tokens.item_margin_block)
                }
            }
            _ => metric(self.tokens.item_margin_block),
        }
    }

    fn trailing_margin(&self, items: &MenuItems, popup: bool, cx: &App) -> Pixels {
        items
            .entries()
            .last()
            .map_or(px(0.), |entry| self.trailing_entry(entry, popup, cx))
    }

    fn sequence_height(&self, items: &MenuItems, popup: bool, cx: &App) -> Pixels {
        let mut previous = px(0.);
        let mut height = px(0.);
        for entry in items.entries() {
            height += self.entry_height(entry, popup, cx)
                + (previous - self.leading_margin(entry)).max(px(0.));
            previous = self.trailing_entry(entry, popup, cx);
        }
        height
    }

    fn entry_height(&self, entry: &MenuEntry, popup: bool, cx: &App) -> Pixels {
        match entry {
            MenuEntry::Group(group) => {
                metric(
                    self.tokens.group_title_font_size * self.tokens.group_title_line_height
                        + self.tokens.globals().padding_xs * 2.,
                ) + self.sequence_height(&group.children, popup, cx)
            }
            MenuEntry::Divider(_) => metric(self.tokens.globals().line_width * 2.),
            MenuEntry::SubMenu(submenu) if self.inline(popup) => {
                let now = cx
                    .background_executor()
                    .now()
                    .saturating_duration_since(self.mount.read(cx).epoch);
                let progress = self.mount.read(cx).motion.get(&submenu.item.key).map_or(
                    if self.state.read(cx).open.contains(&submenu.item.key) {
                        1.
                    } else {
                        0.
                    },
                    |motion| motion.transition.sample(now).value.clamp(0., 1.),
                );
                self.row_height(&submenu.item, true, cx)
                    + metric(self.tokens.item_margin_block)
                    + if progress > 0. {
                        metric(self.tokens.item_margin_block)
                            + self.inline_height(&submenu.children, cx) * progress as f32
                    } else {
                        px(0.)
                    }
            }
            MenuEntry::SubMenu(submenu) => {
                self.row_height(&submenu.item, true, cx) + metric(self.tokens.item_margin_block)
            }
            MenuEntry::Item(item) => {
                self.row_height(item, false, cx) + metric(self.tokens.item_margin_block)
            }
        }
    }

    #[allow(clippy::too_many_arguments)]
    fn stack(
        &self,
        items: &MenuItems,
        depth: usize,
        popup: bool,
        parent: Option<MenuKey>,
        color_theme: MenuColorTheme,
        window: &mut Window,
        cx: &mut App,
    ) -> AnyElement {
        let mut previous = px(0.);
        let children = items
            .entries()
            .iter()
            .map(|entry| {
                let element =
                    self.entry(entry, depth, popup, parent.clone(), color_theme, window, cx);
                let before = (previous - self.leading_margin(entry)).max(px(0.));
                previous = self.trailing_entry(entry, popup, cx);
                let inset = match entry {
                    MenuEntry::Group(_) => px(0.),
                    MenuEntry::SubMenu(_) if self.inline(popup) => px(0.),
                    _ => self.leading_margin(entry),
                };
                (
                    element,
                    self.entry_height(entry, popup, cx) + before,
                    inset,
                    before,
                )
            })
            .collect();
        MenuStack::with_spacing(children).into_any_element()
    }

    fn overflow_content(&self, selected: bool, window: &mut Window, cx: &mut App) -> AnyElement {
        if let Some(render) = &self.overflowed_indicator {
            let context = MenuRenderContext {
                key: SharedString::default(),
                label: self.config.locale.common.more_navigation_items.clone(),
                depth: 0,
                selected,
                active: false,
                expanded: self.state.read(cx).overflow_open,
                collapsed: false,
                disabled: self.state.read(cx).disabled,
                color_theme: self.options.color_theme,
                mode: MenuMode::Horizontal,
            };
            render(&context, window, cx)
        } else {
            let dimension = metric(self.tokens.icon_size);
            icon(IconName::EllipsisOutlined)
                .inherit_color()
                .render_options(IconRenderOptions {
                    size: size(dimension, dimension),
                    scale_factor: window.scale_factor(),
                    ..Default::default()
                })
                .into_any_element()
        }
    }

    fn measure_overflow(&self, window: &mut Window, cx: &mut App) -> Pixels {
        if self.overflowed_indicator.is_none() {
            return metric(self.tokens.icon_size + self.tokens.item_padding_inline * 2.);
        }
        let content = self.overflow_content(!self.state.read(cx).selected.is_empty(), window, cx);
        let mut element = div()
            .flex()
            .items_center()
            .px(metric(self.tokens.item_padding_inline))
            .h(metric(self.tokens.horizontal_line_height))
            .child(content)
            .into_any_element();
        element
            .layout_as_root(
                size(
                    gpui::AvailableSpace::MaxContent,
                    gpui::AvailableSpace::Definite(metric(self.tokens.horizontal_line_height)),
                ),
                window,
                cx,
            )
            .width
    }

    fn horizontal_width(&self, entry: &MenuEntry, window: &Window) -> Option<Pixels> {
        let item = entry.item()?;
        if item.render.is_some()
            || item.extra_render.is_some()
            || item.style != StyleRefinement::default()
            || self.styles.item != StyleRefinement::default()
            || self.styles.item_content != StyleRefinement::default()
            || self.styles.icon != StyleRefinement::default()
            || self.styles.extra != StyleRefinement::default()
        {
            return None;
        }
        let measure = |text: &SharedString, size: f64| {
            let run = gpui::TextRun {
                len: text.len(),
                font: self.tokens.font().clone(),
                color: self.tokens.globals().color_text.into(),
                background_color: None,
                underline: None,
                strikethrough: None,
            };
            window
                .text_system()
                .shape_line(text.clone(), metric(size), &[run], None)
                .width
        };
        let mut width = measure(&item.label, self.tokens.globals().font_size)
            + metric(self.tokens.item_padding_inline * 2.);
        if item.icon.is_some() {
            width += metric(self.tokens.icon_size + self.tokens.icon_margin_inline_end);
        }
        if let Some(extra) = &item.extra {
            width += measure(extra, self.tokens.globals().font_size)
                + metric(self.tokens.globals().padding);
        }
        Some(width)
    }

    fn bounded_root(
        &self,
        items: &MenuItems,
        max_height: Pixels,
        window: &mut Window,
        cx: &mut App,
    ) -> AnyElement {
        let identity: MenuKey = "__menu_bounded_root".into();
        let rows = self.flatten_cached(items, cx);
        let active = self.state.read(cx).active.clone();
        let (list, reveal) = self.mount.update(cx, |mount, _| {
            let list = mount
                .lists
                .entry(identity.clone())
                .or_insert_with(|| gpui::ListState::new(0, gpui::ListAlignment::Top, px(64.)))
                .clone();
            if !mount
                .list_sources
                .get(&identity)
                .is_some_and(|old| Rc::ptr_eq(old, &rows))
            {
                list.reset_with_uniform_height(
                    rows.len(),
                    metric(self.tokens.item_height + self.tokens.item_margin_block),
                );
                mount.list_sources.insert(identity.clone(), rows.clone());
                mount.revealed.remove(&identity);
            }
            let reveal = active
                .as_ref()
                .filter(|active| mount.revealed.get(&identity) != Some(*active))
                .and_then(|active| rows.iter().position(|row| &row.key == active));
            if let Some(active) = active {
                mount.revealed.insert(identity.clone(), active);
            }
            (list, reveal)
        });
        if let Some(index) = reveal {
            if list.bounds_for_item(index).is_some() {
                list.scroll_to_reveal_item(index);
            } else {
                list.scroll_to(gpui::ListOffset {
                    item_ix: index,
                    offset_in_item: px(0.),
                });
            }
        }
        let mut view = self.clone();
        view.flattened = true;
        let now = cx
            .background_executor()
            .now()
            .saturating_duration_since(self.mount.read(cx).epoch);
        let moving = self
            .mount
            .read(cx)
            .motion
            .values()
            .any(|motion| !motion.transition.sample(now).finished);
        if moving {
            list.remeasure();
            self.request_frame(window, cx);
        }
        gpui::list(list, move |index, window, cx| {
            let row = &rows[index];
            let palette = MenuStyle::from_tokens(&view.tokens, row.color_theme);
            let mut height = view.flat_height(row, cx);
            let mut opacity = 1f32;
            let mut clipping = false;
            let elapsed = cx
                .background_executor()
                .now()
                .saturating_duration_since(view.mount.read(cx).epoch);
            for clip in &row.clips {
                let progress = view
                    .mount
                    .read(cx)
                    .motion
                    .get(&clip.branch)
                    .map_or(1., |motion| {
                        motion.transition.sample(elapsed).value.clamp(0., 1.)
                    }) as f32;
                height = height.min((clip.total * progress - clip.offset).max(px(0.)));
                opacity = opacity.min(progress);
                clipping |= progress < 1.;
            }
            if height <= px(0.) {
                return div().h(px(0.)).into_any_element();
            }
            let element = if let Some(entry) = &row.entry {
                div()
                    .flex()
                    .flex_col()
                    .w_full()
                    .bg(if row.depth > 0 {
                        palette.submenu_bg
                    } else {
                        palette.bg
                    })
                    .child(view.entry(
                        entry,
                        row.depth,
                        false,
                        row.parent.clone(),
                        row.color_theme,
                        window,
                        cx,
                    ))
                    .into_any_element()
            } else {
                div()
                    .h(metric(view.tokens.item_margin_block))
                    .bg(palette.submenu_bg)
                    .into_any_element()
            };
            if row.clips.is_empty() {
                element
            } else {
                let mut semantics = Accessibility::new(Role::GenericContainer);
                semantics.hidden = row
                    .clips
                    .iter()
                    .any(|clip| !view.state.read(cx).open.contains(&clip.branch));
                let wrapper = div().child(element);
                let wrapper = if clipping {
                    wrapper.h(height).overflow_hidden().opacity(opacity)
                } else {
                    wrapper
                };
                semantics.apply(wrapper).into_any_element()
            }
        })
        .with_sizing_behavior(gpui::ListSizingBehavior::Infer)
        .w_full()
        .max_h(max_height)
        .into_any_element()
    }

    fn flatten(&self, items: &MenuItems, open: &[MenuKey], cx: &App) -> Vec<FlatRow> {
        #[allow(clippy::too_many_arguments)]
        fn append(
            view: &MenuView,
            items: &MenuItems,
            depth: usize,
            parent: Option<MenuKey>,
            theme: MenuColorTheme,
            rows: &mut Vec<FlatRow>,
            open: &[MenuKey],
        ) {
            for entry in items.entries() {
                rows.push(FlatRow {
                    key: entry.key().clone(),
                    entry: Some(entry.clone()),
                    depth,
                    parent: parent.clone(),
                    color_theme: theme,
                    clips: Vec::new(),
                });
                match entry {
                    MenuEntry::Group(group) => append(
                        view,
                        &group.children,
                        depth,
                        parent.clone(),
                        theme,
                        rows,
                        open,
                    ),
                    MenuEntry::SubMenu(submenu)
                        if view.inline(false) && open.contains(&submenu.item.key) =>
                    {
                        rows.push(FlatRow {
                            key: format!("{}.inline-start", submenu.item.key).into(),
                            entry: None,
                            depth: depth + 1,
                            parent: Some(submenu.item.key.clone()),
                            color_theme: theme,
                            clips: Vec::new(),
                        });
                        append(
                            view,
                            &submenu.children,
                            depth + 1,
                            Some(submenu.item.key.clone()),
                            theme,
                            rows,
                            open,
                        );
                        rows.push(FlatRow {
                            key: format!("{}.inline-end", submenu.item.key).into(),
                            entry: None,
                            depth: depth + 1,
                            parent: Some(submenu.item.key.clone()),
                            color_theme: theme,
                            clips: Vec::new(),
                        });
                    }
                    _ => {}
                }
            }
        }
        let mut rows = Vec::new();
        append(
            self,
            items,
            0,
            None,
            self.options.color_theme,
            &mut rows,
            open,
        );
        let mut starts = HashMap::new();
        let mut totals = HashMap::new();
        let mut offset = px(0.);
        for row in &mut rows {
            let height = self.flat_height(row, cx);
            if row.entry.is_none()
                && row.key.ends_with(".inline-start")
                && let Some(parent) = &row.parent
            {
                starts.insert(parent.clone(), offset);
            }
            let mut ancestor = row.parent.clone();
            while let Some(parent) = ancestor {
                if let Some(start) = starts.get(&parent) {
                    row.clips.push(VirtualClip {
                        branch: parent.clone(),
                        offset: offset - *start,
                        total: px(0.),
                    });
                }
                ancestor = self.state.read(cx).items.parent(&parent).cloned();
            }
            if row.entry.is_none()
                && row.key.ends_with(".inline-end")
                && let Some(parent) = &row.parent
                && let Some(start) = starts.get(parent)
            {
                totals.insert(parent.clone(), offset + height - *start);
            }
            offset += height;
        }
        for row in &mut rows {
            for clip in &mut row.clips {
                clip.total = totals.get(&clip.branch).copied().unwrap_or(px(0.));
            }
        }
        rows
    }

    fn flatten_cached(&self, items: &MenuItems, cx: &mut App) -> Rc<Vec<FlatRow>> {
        let open = self.virtual_open(items, cx);
        if let Some(cache) = &self.mount.read(cx).flat_cache
            && cache.items.same_collection(items)
            && cache.open == open
            && cache.inline == self.inline(false)
            && Arc::ptr_eq(&cache.tokens, &self.tokens)
            && cache.row_geometry_revision == self.mount.read(cx).row_geometry_revision
            && cache.color_theme == self.options.color_theme
        {
            return cache.rows.clone();
        }
        let rows = Rc::new(self.flatten(items, &open, cx));
        self.mount.update(cx, |mount, _| {
            mount.flat_cache = Some(FlatCache {
                items: items.clone(),
                open,
                inline: self.inline(false),
                tokens: self.tokens.clone(),
                row_geometry_revision: mount.row_geometry_revision,
                color_theme: self.options.color_theme,
                rows: rows.clone(),
            })
        });
        rows
    }

    fn virtual_open(&self, items: &MenuItems, cx: &mut App) -> Vec<MenuKey> {
        fn keys(items: &MenuItems, result: &mut Vec<MenuKey>) {
            for entry in items.entries() {
                if let MenuEntry::SubMenu(submenu) = entry {
                    result.push(submenu.item.key.clone());
                }
                if let Some(children) = entry.children() {
                    keys(children, result);
                }
            }
        }
        let open = self.state.read(cx).open.clone();
        let policy = self.motion_policy(cx);
        let now = cx.background_executor().now();
        self.mount.update(cx, |cache, _| {
            if !cache
                .branch_model
                .as_ref()
                .is_some_and(|old| old.same_collection(items))
            {
                cache.branch_keys.clear();
                keys(items, &mut cache.branch_keys);
                cache.branch_model = Some(items.clone());
            }
            let elapsed = now.saturating_duration_since(cache.epoch);
            let mut visible = Vec::new();
            for key in &cache.branch_keys {
                let expanded = open.contains(key);
                let motion = cache
                    .motion
                    .entry(key.clone())
                    .or_insert_with(|| BranchMotion {
                        transition: ScalarTransition::new(
                            if expanded { 1. } else { 0. },
                            self.tokens.globals().motion_ease_in_out,
                        ),
                        open: expanded,
                    });
                motion.open = expanded;
                motion.transition.retarget(
                    if expanded { 1. } else { 0. },
                    elapsed,
                    self.tokens.globals().motion_duration_mid,
                    policy,
                );
                if expanded || motion.transition.sample(elapsed).value > 0. {
                    visible.push(key.clone());
                }
            }
            visible
        })
    }

    fn flat_height(&self, row: &FlatRow, cx: &App) -> Pixels {
        match &row.entry {
            Some(MenuEntry::Group(_)) => metric(
                self.tokens.group_title_font_size * self.tokens.group_title_line_height
                    + self.tokens.globals().padding_xs * 2.,
            ),
            Some(MenuEntry::Divider(_)) => metric(
                self.tokens
                    .item_margin_block
                    .max(self.tokens.globals().line_width)
                    + self.tokens.globals().line_width * 2.,
            ),
            None => metric(self.tokens.item_margin_block),
            Some(MenuEntry::Item(item)) => {
                self.row_height(item, false, cx) + metric(self.tokens.item_margin_block)
            }
            Some(MenuEntry::SubMenu(submenu)) => {
                self.row_height(&submenu.item, true, cx) + metric(self.tokens.item_margin_block)
            }
        }
    }

    fn collapsed(&self, depth: usize, popup: bool) -> bool {
        !popup
            && depth == 0
            && self.options.inline_collapsed
            && self.options.mode != MenuMode::Horizontal
    }

    fn inline(&self, popup: bool) -> bool {
        !popup
            && self.options.mode == MenuMode::Inline
            && (!self.options.inline_collapsed
                || self.collapse.moving && self.collapse.content > 0.)
    }

    #[allow(clippy::too_many_arguments)]
    fn entry(
        &self,
        entry: &MenuEntry,
        depth: usize,
        popup: bool,
        parent: Option<MenuKey>,
        color_theme: MenuColorTheme,
        window: &mut Window,
        cx: &mut App,
    ) -> AnyElement {
        match entry {
            MenuEntry::Divider(divider) => {
                let palette = MenuStyle::from_tokens(&self.tokens, color_theme);
                let mut line = div()
                    .id(divider.key.clone())
                    .h(px(0.))
                    .mt(metric(self.tokens.globals().line_width))
                    .border_t(px(self.tokens.globals().line_width as f32))
                    .border_color(palette.split)
                    .role(Role::Splitter);
                if divider.dashed {
                    line = line.border_dashed();
                }
                line.style().refine(&self.styles.divider);
                line.into_any_element()
            }
            MenuEntry::Group(group) => {
                let palette = MenuStyle::from_tokens(&self.tokens, color_theme);
                let mut title = div()
                    .px(metric(self.tokens.globals().padding))
                    .py(metric(self.tokens.globals().padding_xs))
                    .text_size(metric(self.tokens.group_title_font_size))
                    .line_height(relative(self.tokens.group_title_line_height as f32))
                    .text_color(palette.group_text)
                    .child(group.label.clone());
                if self.inline(popup) && depth > 0 {
                    title = if self.config.direction == Direction::Rtl {
                        title.pr(metric(self.tokens.globals().padding_xl))
                    } else {
                        title.pl(metric(self.tokens.globals().padding_xl))
                    };
                }
                if self.collapsed(depth, popup) {
                    title = title
                        .px(metric(self.tokens.globals().padding_xs))
                        .truncate();
                }
                title.style().refine(&self.styles.group_title);
                let mut group_element = div()
                    .id(group.key.clone())
                    .flex()
                    .flex_col()
                    .role(Role::Group)
                    .child(title);
                if !self.flattened && self.can_stack(&group.children, cx) {
                    group_element = group_element.child(self.stack(
                        &group.children,
                        depth,
                        popup,
                        parent.clone(),
                        color_theme,
                        window,
                        cx,
                    ));
                } else if !self.flattened {
                    group_element =
                        group_element.children(group.children.entries().iter().map(|child| {
                            self.entry(child, depth, popup, parent.clone(), color_theme, window, cx)
                        }));
                }
                Accessibility::new(Role::Group)
                    .label(group.label.clone())
                    .apply(group_element)
                    .into_any_element()
            }
            MenuEntry::Item(item) => {
                self.item(entry, item, depth, popup, parent, color_theme, window, cx)
            }
            MenuEntry::SubMenu(submenu) => self.item(
                entry,
                &submenu.item,
                depth,
                popup,
                parent,
                color_theme,
                window,
                cx,
            ),
        }
    }

    #[allow(clippy::too_many_arguments)]
    fn item(
        &self,
        entry: &MenuEntry,
        item: &super::MenuItem,
        depth: usize,
        popup: bool,
        parent: Option<MenuKey>,
        color_theme: MenuColorTheme,
        window: &mut Window,
        cx: &mut App,
    ) -> AnyElement {
        // Finish the row's by-value style and element builders before descending
        // into children, so their large debug stack frame is not held per level.
        let (row, expanded) =
            self.item_row(entry, item, depth, popup, parent, color_theme, window, cx);
        if let MenuEntry::SubMenu(submenu) = entry {
            if self.inline(popup) {
                if self.flattened {
                    return row;
                }
                return self.inline_branch(row, submenu, depth, expanded, color_theme, window, cx);
            }
            let horizontal = !popup && self.options.mode == MenuMode::Horizontal;
            return self.submenu_popup(row, submenu, depth, horizontal, color_theme, window, cx);
        }
        row
    }

    #[allow(clippy::too_many_arguments)]
    fn item_row(
        &self,
        entry: &MenuEntry,
        item: &super::MenuItem,
        depth: usize,
        popup: bool,
        parent: Option<MenuKey>,
        color_theme: MenuColorTheme,
        window: &mut Window,
        cx: &mut App,
    ) -> (AnyElement, bool) {
        let palette = MenuStyle::from_tokens(&self.tokens, color_theme);
        let horizontal = !popup && self.options.mode == MenuMode::Horizontal;
        let collapsed = self.collapsed(depth, popup);
        let collapsing_root = !popup && depth == 0 && self.options.mode != MenuMode::Horizontal;
        let icon_progress = if collapsing_root {
            self.collapse.icon
        } else {
            0.
        };
        let content_opacity = if collapsing_root {
            self.collapse.content as f32
        } else {
            1.
        };
        let show_content =
            !collapsed || collapsing_root && self.collapse.moving && content_opacity > 0.;
        let inline = self.inline(popup);
        let rtl = self.config.direction == Direction::Rtl;
        let content_rtl = rtl && !popup;
        let submenu = matches!(entry, MenuEntry::SubMenu(_));
        let selected = {
            let s = self.state.read(cx);
            s.selected.contains(&item.key)
                || submenu
                    && s.selected
                        .iter()
                        .any(|key| s.items.path(key).contains(&item.key))
        };
        let (active, expanded, disabled) = {
            let s = self.state.read(cx);
            (
                s.active.as_ref() == Some(&item.key),
                s.open.contains(&item.key),
                s.disabled || !s.items.enabled(&item.key),
            )
        };
        let context = MenuRenderContext {
            key: item.key.clone(),
            label: item.label.clone(),
            depth,
            selected,
            active,
            expanded,
            collapsed,
            disabled,
            color_theme,
            mode: self.options.mode,
        };
        let row_focus = self.state.update(cx, |s, cx| {
            s.row_focus
                .entry(item.key.clone())
                .or_insert_with(|| cx.focus_handle().tab_stop(false))
                .clone()
        });
        let visual = MenuVisualState {
            horizontal,
            submenu,
            selected: selected && !submenu,
            descendant_selected: selected && submenu,
            danger: item.danger,
            disabled,
            hovered: false,
            active: false,
            open: expanded,
        };
        let now = cx.background_executor().now();
        let policy = self.motion_policy(cx);
        let (foreground, background, moving) = self.mount.update(cx, |cache, _| {
            let visual = MenuVisualState {
                hovered: cache.hovered.contains(&item.key),
                active: cache.pressed.contains(&item.key),
                ..visual
            };
            let base = palette.row(visual);
            let background = if horizontal && submenu && selected && !visual.active {
                palette.horizontal_selected_bg
            } else {
                base.background
            };
            let elapsed = now.saturating_duration_since(cache.epoch);
            let row = cache.row_motion.entry(item.key.clone()).or_default();
            let (foreground, fg_moving) = row.foreground.resolve(
                base.text.into(),
                elapsed,
                self.tokens.globals().motion_duration_slow,
                crate::input::motion::CSS_EASE,
                policy,
            );
            let (background, bg_moving) = row.background.resolve(
                background.into(),
                elapsed,
                self.tokens.globals().motion_duration_slow,
                crate::input::motion::CSS_EASE,
                policy,
            );
            (foreground, background, fg_moving || bg_moving)
        });
        if moving {
            self.request_frame(window, cx);
        }
        let base = palette.row(visual);
        let revision = self.state.read(cx).revision;
        let key = item.key.clone();
        let debug_key = key.clone();
        let clicked = self.state.clone();
        let click_key = key.clone();
        let hovered = self.state.clone();
        let hover_key = key.clone();
        let hover_cache = self.mount.clone();
        let press_cache = self.mount.clone();
        let press_key = key.clone();
        let release_cache = self.mount.clone();
        let release_key = key.clone();
        let outside_cache = self.mount.clone();
        let outside_key = key.clone();
        let keyboard = self.state.clone();
        let keyboard_key = key.clone();
        let key_parent = parent.clone();
        let focus_state = self.state.clone();
        let focus_key = key.clone();
        let a11y = self.state.clone();
        let a11y_key = key.clone();
        let mut row = div()
            .id(key.clone())
            .debug_selector(move || debug_key.to_string())
            .group("ant-menu-item")
            .relative()
            .flex()
            .items_center()
            .flex_none()
            .min_w(px(0.))
            .track_focus(&row_focus)
            .tab_index(-1)
            .h(metric(if horizontal {
                self.tokens.horizontal_line_height
            } else {
                self.tokens.item_height
            }))
            .rounded(metric(if horizontal {
                self.tokens.horizontal_item_border_radius
            } else if popup {
                self.tokens.sub_menu_item_border_radius
            } else {
                self.tokens.item_border_radius
            }))
            .text_color(foreground)
            .bg(background)
            .when(content_rtl, |row| row.flex_row_reverse())
            .when(disabled, |row| row.cursor_not_allowed())
            .when(!disabled, |row| row.cursor_pointer());
        if horizontal {
            row = row.px(metric(self.tokens.item_padding_inline));
        } else {
            row = row
                .mx(metric(self.tokens.item_margin_inline))
                .mt(metric(self.tokens.item_margin_block));
            let normal_padding = if inline {
                self.options.inline_indent * (depth as f64 + 1.)
            } else {
                self.tokens.item_padding_inline
            };
            let centered_padding =
                ((self.tokens.collapsed_width - self.tokens.collapsed_icon_size) / 2.
                    - self.tokens.item_margin_inline)
                    .max(0.);
            let start_padding = if collapsing_root {
                normal_padding + (centered_padding - normal_padding) * self.collapse.padding
            } else {
                normal_padding
            };
            row = if rtl {
                row.pr(metric(start_padding))
                    .pl(metric(self.tokens.item_padding_inline))
            } else {
                row.pl(metric(start_padding))
                    .pr(metric(self.tokens.item_padding_inline))
            };
        }
        if collapsed && !self.collapse.moving {
            row = row.px(px(0.)).justify_center();
        }
        if !disabled {
            row = row
                .on_hover(move |is_hovered, window, cx| {
                    hover_cache.update(cx, |cache, cx| {
                        let changed = if *is_hovered {
                            cache.hovered.insert(hover_key.clone())
                        } else {
                            cache.hovered.remove(&hover_key)
                        };
                        if changed {
                            cx.notify();
                        }
                    });
                    if row_current(&hovered, &hover_key, revision, cx) {
                        state::hover(&hovered, &hover_key, *is_hovered, window, cx);
                    }
                })
                .on_click(move |event, window, cx| {
                    if matches!(event, gpui::ClickEvent::Keyboard(_)) {
                        return;
                    }
                    if row_current(&clicked, &click_key, revision, cx) {
                        state::activate(&clicked, &click_key, MenuSource::Pointer, window, cx);
                    }
                })
                .on_mouse_down(MouseButton::Left, move |_, _, cx| {
                    press_cache.update(cx, |cache, cx| {
                        if cache.pressed.insert(press_key.clone()) {
                            cx.notify();
                        }
                    });
                    cx.stop_propagation();
                })
                .on_mouse_up(MouseButton::Left, move |_, _, cx| {
                    release_cache.update(cx, |cache, cx| {
                        if cache.pressed.remove(&release_key) {
                            cx.notify();
                        }
                    });
                })
                .on_mouse_up_out(MouseButton::Left, move |_, _, cx| {
                    outside_cache.update(cx, |cache, cx| {
                        if cache.pressed.remove(&outside_key) {
                            cx.notify();
                        }
                    });
                })
                .on_a11y_action(accesskit::Action::Click, move |_, window, cx| {
                    if row_current(&a11y, &a11y_key, revision, cx) {
                        state::activate(&a11y, &a11y_key, MenuSource::Accessibility, window, cx);
                    }
                })
                .on_a11y_action(accesskit::Action::Focus, move |_, window, cx| {
                    if !row_current(&focus_state, &focus_key, revision, cx) {
                        return;
                    }
                    focus_state.update(cx, |s, cx| {
                        if s.revision == revision && s.mounted && s.items.enabled(&focus_key) {
                            s.active = Some(focus_key.clone());
                            if let Some(focus) = s.row_focus.get(&focus_key) {
                                window.focus(focus, cx);
                            }
                            cx.notify();
                        }
                    });
                });
        }
        row = row.on_key_down(move |event, window, cx| {
            if !row_current(&keyboard, &keyboard_key, revision, cx) {
                window.prevent_default();
                cx.stop_propagation();
                return;
            }
            state::key(&keyboard, key_parent.as_ref(), event, window, cx)
        });
        if let Some(icon_ref) = &item.icon {
            let dimension = metric(
                self.tokens.icon_size
                    + (self.tokens.collapsed_icon_size - self.tokens.icon_size) * icon_progress,
            );
            let mut icon_slot =
                div()
                    .flex()
                    .items_center()
                    .justify_center()
                    .flex_none()
                    .size(dimension)
                    .when(show_content, |slot| {
                        slot.mr(if content_rtl {
                            px(0.)
                        } else {
                            metric(self.tokens.icon_margin_inline_end * f64::from(content_opacity))
                        })
                        .ml(if content_rtl {
                            metric(self.tokens.icon_margin_inline_end * f64::from(content_opacity))
                        } else {
                            px(0.)
                        })
                    })
                    .child(icon(icon_ref.clone()).inherit_color().render_options(
                        IconRenderOptions {
                            size: size(dimension, dimension),
                            scale_factor: window.scale_factor(),
                            ..Default::default()
                        },
                    ));
            icon_slot.style().refine(&self.styles.icon);
            row = row.child(icon_slot);
        }
        if show_content {
            let content = item
                .render
                .as_ref()
                .map(|render| render(&context, window, cx))
                .unwrap_or_else(|| {
                    div()
                        .truncate()
                        .child(item.label.clone())
                        .into_any_element()
                });
            let mut content_slot = div()
                .min_w(px(0.))
                .flex_1()
                .opacity(content_opacity)
                .child(content);
            content_slot.style().refine(&self.styles.item_content);
            row = row.child(content_slot);
            let extra = item
                .extra_render
                .as_ref()
                .map(|render| render(&context, window, cx))
                .or_else(|| {
                    item.extra
                        .as_ref()
                        .map(|extra| div().child(extra.clone()).into_any_element())
                });
            if let Some(extra) = extra {
                let mut extra_slot = div()
                    .flex_none()
                    .opacity(content_opacity)
                    .when(content_rtl, |slot| {
                        slot.pr(metric(self.tokens.globals().padding))
                    })
                    .when(!content_rtl, |slot| {
                        slot.pl(metric(self.tokens.globals().padding))
                    })
                    .text_color(palette.group_text)
                    .child(extra);
                extra_slot.style().refine(&self.styles.extra);
                row = row.child(extra_slot);
            }
            if submenu && !horizontal {
                let arrow_size = metric(self.tokens.arrow_size());
                let content = if let Some(render) = &self.expand_icon {
                    render(&context, window, cx)
                } else {
                    let progress = if inline {
                        let policy = self.motion_policy(cx);
                        let now = cx.background_executor().now();
                        let (progress, moving) = self.mount.update(cx, |cache, _| {
                            let elapsed = now.saturating_duration_since(cache.epoch);
                            let arrow = cache.arrows.entry(key.clone()).or_insert_with(|| {
                                ScalarTransition::new(
                                    if expanded { 1. } else { 0. },
                                    self.tokens.globals().motion_ease_in_out,
                                )
                            });
                            arrow.retarget(
                                if expanded { 1. } else { 0. },
                                elapsed,
                                self.tokens.globals().motion_duration_slow,
                                policy,
                            );
                            let sample = arrow.sample(elapsed);
                            (sample.value.clamp(0., 1.), !sample.finished)
                        });
                        if moving {
                            self.request_frame(window, cx);
                        }
                        progress
                    } else {
                        0.
                    };
                    chevron(
                        arrow_size,
                        metric(self.tokens.globals().border_radius),
                        inline,
                        rtl,
                        content_rtl,
                        progress,
                    )
                };
                let mut arrow = div()
                    .absolute()
                    .top(relative(0.5))
                    .mt(-arrow_size / 2.)
                    .flex()
                    .items_center()
                    .flex_none()
                    .size(arrow_size)
                    .opacity(content_opacity)
                    .child(content);
                arrow = if content_rtl {
                    arrow.left(metric(self.tokens.globals().margin))
                } else {
                    arrow.right(metric(self.tokens.globals().margin))
                };
                let reserve = self.tokens.arrow_size()
                    + self.tokens.globals().padding
                    + self.tokens.globals().margin_xs;
                row = if content_rtl {
                    row.pl(metric(reserve))
                } else {
                    row.pr(metric(reserve))
                };
                arrow.style().refine(&self.styles.expand_icon);
                row = row.child(arrow);
            }
        } else if item.icon.is_none() {
            row = row.text_size(metric(self.tokens.collapsed_icon_size));
            row = row.child(
                item.label
                    .chars()
                    .next()
                    .map(|c| c.to_string())
                    .unwrap_or_default(),
            );
        }
        if horizontal && !disabled && palette.active_bar_height > 0. {
            let color = base.indicator;
            row = row.child(
                div()
                    .absolute()
                    .bottom(px(0.))
                    .left(metric(self.tokens.item_padding_inline))
                    .right(metric(self.tokens.item_padding_inline))
                    .h(metric(palette.active_bar_height))
                    .bg(color)
                    .group_hover("ant-menu-item", move |style| {
                        style.bg(palette.horizontal_selected_text)
                    }),
            );
        } else if inline && selected && !submenu && !disabled && self.tokens.active_bar_width > 0. {
            let mut bar = div()
                .absolute()
                .top(px(0.))
                .bottom(px(0.))
                .w(metric(self.tokens.active_bar_width))
                .bg(foreground);
            bar = if rtl {
                bar.left(px(0.))
            } else {
                bar.right(px(0.))
            };
            row = row.child(bar);
        }
        row.style().refine(&self.styles.item);
        if submenu {
            row.style().refine(&self.styles.submenu_title);
        }
        row.style().refine(&item.style);
        let measured = self.mount.clone();
        let measured_key = key.clone();
        let automatic_height = item
            .style
            .size
            .height
            .or(if submenu {
                self.styles.submenu_title.size.height
            } else {
                None
            })
            .or(self.styles.item.size.height)
            == Some(gpui::Length::Auto);
        let default_height = metric(self.tokens.item_height);
        row = row.child(
            canvas(
                move |bounds, _, cx| {
                    measured.update(cx, |cache, cx| {
                        let previous = cache.row_bounds.insert(measured_key.clone(), bounds);
                        if automatic_height
                            && (previous.map_or(default_height, |old| old.size.height)
                                - bounds.size.height)
                                .abs()
                                > px(0.1)
                        {
                            cache.row_geometry_revision =
                                cache.row_geometry_revision.wrapping_add(1);
                            cx.notify();
                        }
                    });
                },
                |_, _, _, _| (),
            )
            .absolute()
            .inset_0(),
        );
        if submenu {
            if !disabled {
                let expand_state = self.state.clone();
                let collapse_state = self.state.clone();
                let expand_key = key.clone();
                let collapse_key = key.clone();
                row = row
                    .on_a11y_action(accesskit::Action::Expand, move |_, window, cx| {
                        if row_current(&expand_state, &expand_key, revision, cx) {
                            state::toggle(
                                &expand_state,
                                &expand_key,
                                true,
                                MenuSource::Accessibility,
                                window,
                                cx,
                            );
                        }
                    })
                    .on_a11y_action(accesskit::Action::Collapse, move |_, window, cx| {
                        if row_current(&collapse_state, &collapse_key, revision, cx) {
                            state::toggle(
                                &collapse_state,
                                &collapse_key,
                                false,
                                MenuSource::Accessibility,
                                window,
                                cx,
                            );
                        }
                    });
            }
            row = row
                .aria_expanded(expanded)
                .a11y_synthetic_children(move |builder| {
                    if !inline {
                        builder
                            .parent_node()
                            .set_has_popup(accesskit::HasPopup::Menu);
                    }
                });
        }
        let checkable = self.options.multiple && self.options.selectable && !submenu;
        if checkable {
            row = row.aria_toggled(if selected {
                gpui::Toggled::True
            } else {
                gpui::Toggled::False
            });
        }
        let mut semantics = Accessibility::new(if checkable {
            Role::MenuItemCheckBox
        } else {
            Role::MenuItem
        })
        .label(item.label.clone());
        if selected && !submenu && !self.options.multiple {
            semantics.current = Some(accesskit::AriaCurrent::Page);
        }
        semantics.disabled = disabled;
        semantics.automation_id = self
            .automation_id
            .as_ref()
            .map(|id| format!("{id}.item.{}", item.key).into());
        let focused = FocusRing::new(
            palette.focus_color,
            window.pixel_snap(metric(palette.focus_width)),
        )
        .with_offset(window.pixel_snap(metric(palette.focus_offset)))
        .radius(metric(if horizontal {
            self.tokens.horizontal_item_border_radius
        } else if popup {
            self.tokens.sub_menu_item_border_radius
        } else {
            self.tokens.item_border_radius
        }))
        .apply(row, &row_focus);
        let mut row = semantics.apply(focused).into_any_element();
        if collapsed && !submenu && self.options.tooltip {
            row = Tooltip::new((ElementId::from(key.clone()), "collapsed-tooltip"))
                .scope(self.scope.clone())
                .placement(self.tooltip_placement.unwrap_or(if rtl {
                    PopupPlacement::Left
                } else {
                    PopupPlacement::Right
                }))
                .title(item.title.clone().unwrap_or_else(|| item.label.clone()))
                .child(row)
                .into_any_element();
        }
        (row, expanded)
    }

    #[allow(clippy::too_many_arguments)]
    fn inline_branch(
        &self,
        title: AnyElement,
        submenu: &super::SubMenu,
        depth: usize,
        expanded: bool,
        color_theme: MenuColorTheme,
        window: &mut Window,
        cx: &mut App,
    ) -> AnyElement {
        let key = submenu.item.key.clone();
        let policy = self.motion_policy(cx);
        let curve = self.tokens.globals().motion_ease_in_out;
        let duration = self.tokens.globals().motion_duration_mid;
        let now = cx.background_executor().now();
        let (progress, moving) = self.mount.update(cx, |mount, _| {
            let elapsed = now.saturating_duration_since(mount.epoch);
            let branch = mount
                .motion
                .entry(key.clone())
                .or_insert_with(|| BranchMotion {
                    transition: ScalarTransition::new(if expanded { 1. } else { 0. }, curve),
                    open: expanded,
                });
            if branch.open != expanded || !policy.allows_motion() {
                branch.open = expanded;
                branch.transition.retarget(
                    if expanded { 1. } else { 0. },
                    elapsed,
                    duration,
                    policy,
                );
            }
            let sample = branch.transition.sample(elapsed);
            (sample.value.clamp(0., 1.), !sample.finished)
        });
        if moving {
            self.request_frame(window, cx);
        }
        let use_stack = self.can_stack(&submenu.children, cx);
        let mut branch_rows = vec![(
            title,
            self.row_height(&submenu.item, true, cx) + metric(self.tokens.item_margin_block),
        )];
        let child_gap = if progress > 0. {
            metric(self.tokens.item_margin_block)
        } else {
            px(0.)
        };
        if expanded || progress > 0. || self.options.force_submenu_render {
            let palette = MenuStyle::from_tokens(&self.tokens, color_theme);
            let mut children = div()
                .id("inline-submenu-list")
                .mt(child_gap)
                .flex()
                .flex_col()
                .pb(if progress > 0. {
                    self.trailing_margin(&submenu.children, false, cx)
                } else {
                    px(0.)
                })
                .bg(palette.submenu_bg)
                .role(Role::Menu)
                .aria_label(submenu.item.label.clone());
            if self.can_stack(&submenu.children, cx) {
                children = children.child(self.stack(
                    &submenu.children,
                    depth + 1,
                    false,
                    Some(key.clone()),
                    color_theme,
                    window,
                    cx,
                ));
            } else {
                children = children.children(submenu.children.entries().iter().map(|entry| {
                    self.entry(
                        entry,
                        depth + 1,
                        false,
                        Some(key.clone()),
                        color_theme,
                        window,
                        cx,
                    )
                }));
            }
            if moving || !expanded {
                let height = self.inline_height(&submenu.children, cx) * progress as f32;
                children = children.h(height).overflow_hidden();
            }
            children = children.opacity(progress as f32);
            let mut semantics = Accessibility::new(Role::Menu).label(submenu.item.label.clone());
            semantics.hidden = !expanded;
            let height = self.inline_height(&submenu.children, cx) * progress as f32 + child_gap;
            branch_rows.push((semantics.apply(children).into_any_element(), height));
        }
        let branch = div()
            .id((ElementId::from(key), "inline-branch"))
            .flex()
            .flex_col();
        if use_stack {
            branch
                .child(MenuStack::with_insets(
                    branch_rows
                        .into_iter()
                        .enumerate()
                        .map(|(index, (child, height))| {
                            (
                                child,
                                height,
                                if index == 0 {
                                    metric(self.tokens.item_margin_block)
                                } else {
                                    child_gap
                                },
                            )
                        })
                        .collect(),
                ))
                .into_any_element()
        } else {
            branch
                .children(branch_rows.into_iter().map(|(element, _)| element))
                .into_any_element()
        }
    }

    fn inline_height(&self, items: &MenuItems, cx: &App) -> Pixels {
        self.sequence_height(items, false, cx) + self.trailing_margin(items, false, cx)
    }

    fn request_frame(&self, window: &Window, cx: &mut App) {
        let weak = self.mount.downgrade();
        let pending = self.mount.update(cx, |mount, _| {
            std::mem::replace(&mut mount.frame_pending, true)
        });
        if pending {
            return;
        }
        window.on_next_frame(move |_, cx| {
            let _ = weak.update(cx, |mount, cx| {
                mount.frame_pending = false;
                if mount.state.upgrade().is_some_and(|s| s.read(cx).mounted) {
                    cx.notify();
                }
            });
        });
    }

    #[allow(clippy::too_many_arguments)]
    fn submenu_popup(
        &self,
        title: AnyElement,
        submenu: &super::SubMenu,
        depth: usize,
        horizontal: bool,
        inherited_theme: MenuColorTheme,
        window: &mut Window,
        cx: &mut App,
    ) -> AnyElement {
        let key = submenu.item.key.clone();
        let color_theme = submenu.color_theme.unwrap_or(inherited_theme);
        let expanded = self.state.read(cx).open.contains(&key);
        let handle = self
            .state
            .update(cx, |s, _| s.popups.entry(key.clone()).or_default().clone());
        let view = self.clone();
        let children = submenu.children.clone();
        let content_key = key.clone();
        let label = submenu.item.label.clone();
        let custom = submenu
            .popup_render
            .clone()
            .or_else(|| self.popup_render.clone());
        let eager_custom = custom.clone();
        let changed = self.state.clone();
        let changed_key = key.clone();
        let rtl = self.config.direction == Direction::Rtl;
        let placement = if horizontal {
            PopupPlacement::BottomLeft
        } else if rtl {
            PopupPlacement::LeftTop
        } else {
            PopupPlacement::RightTop
        };
        let offset = submenu.popup_offset
            + if horizontal {
                gpui::point(metric(self.tokens.item_padding_inline), px(0.))
            } else {
                gpui::point(px(0.), px(0.))
            };
        let popup = self
            .surface(
                Popup::new((ElementId::from(key), "submenu-popup")),
                color_theme,
            )
            .handle(&handle)
            .child(title)
            .placement(placement)
            .offset(offset)
            .triggers(PopupTriggers::NONE)
            .disabled(self.state.read(cx).disabled || submenu.item.disabled)
            .open(expanded)
            .fresh(true)
            .destroy_on_hidden(!self.options.force_submenu_render)
            .on_open_change(move |change, window, cx| {
                state::popup_changed(&changed, &changed_key, change, window, cx)
            })
            .content(move |_, window, cx| {
                let content = view.popup_list(
                    &children,
                    Some(content_key.clone()),
                    depth + 1,
                    &label,
                    color_theme,
                    window,
                    cx,
                );
                if let Some(custom) = &custom {
                    let context = MenuPopupContext {
                        key: content_key.clone(),
                        key_path: view.state.read(cx).items.path(&content_key),
                        color_theme,
                    };
                    custom(content, &context, window, cx)
                } else {
                    content
                }
            });
        let popup = if horizontal {
            popup
        } else {
            popup.flipped_gap(px(0.)).gap(if rtl {
                px(0.)
            } else {
                metric(self.tokens.popup_gap())
            })
        };
        if self.options.force_submenu_render && !expanded {
            let mut content = self.popup_list(
                &submenu.children,
                Some(submenu.item.key.clone()),
                depth + 1,
                &submenu.item.label,
                color_theme,
                window,
                cx,
            );
            if let Some(render) = eager_custom {
                let context = MenuPopupContext {
                    key: submenu.item.key.clone(),
                    key_path: self.state.read(cx).items.path(&submenu.item.key),
                    color_theme,
                };
                content = render(content, &context, window, cx);
            }
            let mut semantics = Accessibility::new(Role::Menu);
            semantics.hidden = true;
            div()
                .relative()
                .flex()
                .flex_col()
                .child(popup)
                .child(
                    semantics.apply(
                        div()
                            .absolute()
                            .invisible()
                            .size(px(0.))
                            .overflow_hidden()
                            .child(content),
                    ),
                )
                .into_any_element()
        } else {
            popup.into_any_element()
        }
    }

    fn surface(&self, popup: Popup, color_theme: MenuColorTheme) -> Popup {
        let palette = MenuStyle::from_tokens(&self.tokens, color_theme);
        let mut surface = div()
            .p(px(0.))
            .overflow_hidden()
            .shadow(self.tokens.popup_shadows().to_vec());
        surface.style().refine(&self.styles.popup);
        popup
            .scope(self.scope.clone())
            .motion_duration(self.tokens.globals().motion_duration_mid)
            .exit_duration(self.tokens.globals().motion_duration_mid)
            .arrow(false)
            .gap(metric(self.tokens.popup_gap()))
            .constrain_to_viewport(px(8.))
            .z_index(self.tokens.z_index_popup)
            .accessibility_role(Role::GenericContainer)
            .appearance(
                surface.style().clone(),
                palette.popup_bg.into(),
                metric(self.tokens.popup_radius()),
                self.tokens.globals(),
                self.tokens.globals().border_radius_outer,
            )
            .shadows(crate::theme::ShadowList::default())
    }

    #[allow(clippy::too_many_arguments)]
    fn popup_rows(
        &self,
        items: &MenuItems,
        parent: Option<MenuKey>,
        identity: &MenuKey,
        depth: usize,
        color_theme: MenuColorTheme,
        cx: &mut App,
    ) -> Rc<Vec<FlatRow>> {
        if let Some(cache) = self.mount.read(cx).popup_rows.get(identity)
            && cache.items.same_collection(items)
            && cache.color_theme == color_theme
            && Arc::ptr_eq(&cache.tokens, &self.tokens)
        {
            return cache.rows.clone();
        }
        fn append(
            items: &MenuItems,
            parent: &Option<MenuKey>,
            depth: usize,
            theme: MenuColorTheme,
            rows: &mut Vec<FlatRow>,
        ) {
            for entry in items.entries() {
                rows.push(FlatRow {
                    key: entry.key().clone(),
                    entry: Some(entry.clone()),
                    depth,
                    parent: parent.clone(),
                    color_theme: theme,
                    clips: Vec::new(),
                });
                if let MenuEntry::Group(group) = entry {
                    append(&group.children, parent, depth, theme, rows);
                }
            }
        }
        let mut rows = Vec::new();
        append(items, &parent, depth, color_theme, &mut rows);
        let rows = Rc::new(rows);
        self.mount.update(cx, |cache, _| {
            cache.popup_rows.insert(
                identity.clone(),
                FlatCache {
                    items: items.clone(),
                    open: Vec::new(),
                    inline: false,
                    color_theme,
                    rows: rows.clone(),
                    tokens: self.tokens.clone(),
                    row_geometry_revision: cache.row_geometry_revision,
                },
            );
        });
        rows
    }

    #[allow(clippy::too_many_arguments)]
    fn popup_list(
        &self,
        items: &MenuItems,
        parent: Option<MenuKey>,
        depth: usize,
        label: &SharedString,
        color_theme: MenuColorTheme,
        window: &mut Window,
        cx: &mut App,
    ) -> AnyElement {
        let palette = MenuStyle::from_tokens(&self.tokens, color_theme);
        let identity: MenuKey = parent.as_ref().map_or_else(
            || "overflow-list".into(),
            |key| format!("submenu:{key}").into(),
        );
        let popup_rows = self.popup_rows(items, parent.clone(), &identity, depth, color_theme, cx);
        let width = self.popup_width(items, &identity, depth, color_theme, window, cx);
        let max_height = (window.viewport_size().height
            - metric(self.tokens.popup_viewport_inset()))
        .max(px(40.));
        let scroll = self.mount.update(cx, |mount, _| {
            mount.scroll.entry(identity.clone()).or_default().clone()
        });
        let hover_state = self.state.clone();
        let hover_parent = parent.clone();
        let key_state = self.state.clone();
        let key_parent = parent.clone();
        let previous = self.state.clone();
        let next = self.state.clone();
        let mut root = div()
            .id((ElementId::from(identity.clone()), "popup-list"))
            .flex()
            .flex_col()
            .pb(self.trailing_margin(items, true, cx))
            .w(width)
            .min_w(metric(self.tokens.dropdown_width))
            .max_w((window.viewport_size().width - px(16.)).max(px(0.)))
            .max_h(max_height)
            .overflow_y_scroll()
            .track_scroll(&scroll)
            .font(self.tokens.font().clone())
            .text_size(px(self.tokens.globals().font_size as f32))
            .line_height(relative(self.tokens.globals().line_height as f32))
            .text_color(palette.text)
            .text_left()
            .role(Role::Menu)
            .aria_label(label.clone())
            .on_hover(move |hovered, window, cx| {
                state::hover_level(&hover_state, hover_parent.as_ref(), *hovered, window, cx)
            })
            .on_key_down(move |event, window, cx| {
                state::key(&key_state, key_parent.as_ref(), event, window, cx)
            })
            .on_action(move |_: &crate::behavior::FocusNext, window, cx| {
                state::traverse(&next, false, window, cx)
            })
            .on_action(move |_: &crate::behavior::FocusPrevious, window, cx| {
                state::traverse(&previous, true, window, cx)
            });
        let active = self.state.read(cx).active.clone();
        let reveal = self.mount.update(cx, |mount, _| {
            if let Some(active) = active.as_ref()
                && mount.revealed.get(&identity) != Some(active)
            {
                mount.revealed.insert(identity.clone(), active.clone());
                return popup_rows.iter().position(|row| &row.key == active);
            }
            None
        });
        if popup_rows.len() > 64 && self.styles.submenu_list == StyleRefinement::default() {
            let list = self.mount.update(cx, |mount, _| {
                let list = mount
                    .lists
                    .entry(identity)
                    .or_insert_with(|| gpui::ListState::new(0, gpui::ListAlignment::Top, px(64.)));
                if list.item_count() != popup_rows.len() {
                    list.reset_with_uniform_height(
                        popup_rows.len(),
                        metric(self.tokens.item_height + self.tokens.item_margin_block),
                    );
                }
                list.clone()
            });
            if let Some(index) = reveal {
                if list.bounds_for_item(index).is_some() {
                    list.scroll_to_reveal_item(index);
                } else {
                    list.scroll_to(gpui::ListOffset {
                        item_ix: index,
                        offset_in_item: px(0.),
                    });
                }
            }
            let mut view = self.clone();
            view.flattened = true;
            let rows = popup_rows;
            let parent = parent.clone();
            root = root.child(
                gpui::list(list, move |index, window, cx| {
                    view.entry(
                        rows[index].entry.as_ref().expect("popup row"),
                        rows[index].depth,
                        true,
                        parent.clone(),
                        color_theme,
                        window,
                        cx,
                    )
                })
                .with_sizing_behavior(gpui::ListSizingBehavior::Infer)
                .w_full()
                .max_h(max_height),
            );
        } else {
            if let Some(index) = reveal {
                let viewport = scroll.bounds();
                if viewport.size.height > px(0.) {
                    let measured = active
                        .as_ref()
                        .and_then(|key| self.mount.read(cx).row_bounds.get(key).copied());
                    if let Some(bounds) = measured {
                        let delta = if bounds.top() < viewport.top() {
                            viewport.top() - bounds.top()
                        } else if bounds.bottom() > viewport.bottom() {
                            viewport.bottom() - bounds.bottom()
                        } else {
                            px(0.)
                        };
                        let offset = scroll.offset();
                        scroll.set_offset(gpui::point(offset.x, offset.y + delta));
                    } else {
                        let top: Pixels = popup_rows[..index]
                            .iter()
                            .map(|row| self.flat_height(row, cx))
                            .sum();
                        let bottom = top + self.flat_height(&popup_rows[index], cx);
                        let offset = -scroll.offset().y;
                        let height = viewport.size.height.min(max_height);
                        let target = if top < offset {
                            top
                        } else if bottom > offset + height {
                            bottom - height
                        } else {
                            offset
                        };
                        scroll.set_offset(gpui::point(px(0.), -target));
                    }
                }
            }
            if self.can_stack(items, cx) {
                root = root.child(self.stack(
                    items,
                    depth,
                    true,
                    parent.clone(),
                    color_theme,
                    window,
                    cx,
                ));
            } else {
                root = root.children(items.entries().iter().map(|entry| {
                    self.entry(entry, depth, true, parent.clone(), color_theme, window, cx)
                }));
            }
        }
        root.style().refine(&self.styles.submenu_list);
        root.into_any_element()
    }

    #[allow(clippy::too_many_arguments)]
    fn popup_width(
        &self,
        items: &MenuItems,
        key: &MenuKey,
        depth: usize,
        color_theme: MenuColorTheme,
        window: &mut Window,
        cx: &mut App,
    ) -> Pixels {
        fn custom_count(items: &MenuItems, count: &mut usize) -> bool {
            let mut custom = false;
            for entry in items.entries() {
                *count += 1;
                if *count > 64 {
                    return true;
                }
                if let Some(item) = entry.item() {
                    custom |= item.render.is_some()
                        || item.extra_render.is_some()
                        || item.style != StyleRefinement::default();
                }
                if let MenuEntry::Group(group) = entry {
                    custom |= custom_count(&group.children, count);
                }
            }
            custom
        }
        let mut count = 0;
        let custom = custom_count(items, &mut count)
            || self.styles.item != StyleRefinement::default()
            || self.styles.item_content != StyleRefinement::default();
        if custom && count <= 64 {
            let mut width = metric(self.tokens.dropdown_width);
            for entry in items.entries() {
                width = width.max(self.content_width(entry, depth, color_theme, window, cx));
            }
            return width;
        }
        if let Some(cache) = self.mount.read(cx).widths.get(key)
            && cache.items.same_collection(items)
            && Arc::ptr_eq(&cache.tokens, &self.tokens)
            && cache.font == *self.tokens.font()
        {
            let width = cache.width;
            return if custom {
                self.sample_content_width(items, depth, color_theme, width, window, cx)
            } else {
                width
            };
        }
        fn labels(items: &MenuItems, view: &MenuView, window: &Window, widest: &mut Pixels) {
            for entry in items.entries() {
                if let Some(item) = entry.item() {
                    let run = gpui::TextRun {
                        len: item.label.len(),
                        font: view.tokens.font().clone(),
                        color: view.tokens.globals().color_text.into(),
                        background_color: None,
                        underline: None,
                        strikethrough: None,
                    };
                    let mut width = window
                        .text_system()
                        .shape_line(
                            item.label.clone(),
                            px(view.tokens.globals().font_size as f32),
                            &[run],
                            None,
                        )
                        .width;
                    width += metric(
                        view.tokens.item_padding_inline * 2. + view.tokens.item_margin_inline * 2.,
                    );
                    if item.icon.is_some() {
                        width += metric(view.tokens.icon_size + view.tokens.icon_margin_inline_end);
                    }
                    if matches!(entry, MenuEntry::SubMenu(_)) {
                        width += px(24.);
                    }
                    if let Some(extra) = &item.extra {
                        let run = gpui::TextRun {
                            len: extra.len(),
                            font: view.tokens.font().clone(),
                            color: view.tokens.globals().color_text.into(),
                            background_color: None,
                            underline: None,
                            strikethrough: None,
                        };
                        width += window
                            .text_system()
                            .shape_line(
                                extra.clone(),
                                px(view.tokens.globals().font_size as f32),
                                &[run],
                                None,
                            )
                            .width
                            + metric(view.tokens.globals().padding);
                    }
                    *widest = (*widest).max(width);
                } else if let MenuEntry::Group(group) = entry {
                    labels(&group.children, view, window, widest);
                }
            }
        }
        let mut width = metric(self.tokens.dropdown_width);
        labels(items, self, window, &mut width);
        self.mount.update(cx, |mount, _| {
            mount.widths.insert(
                key.clone(),
                WidthCache {
                    items: items.clone(),
                    tokens: self.tokens.clone(),
                    font: self.tokens.font().clone(),
                    width,
                },
            );
        });
        if custom {
            self.sample_content_width(items, depth, color_theme, width, window, cx)
        } else {
            width
        }
    }

    #[allow(clippy::too_many_arguments)]
    fn sample_content_width(
        &self,
        items: &MenuItems,
        depth: usize,
        color_theme: MenuColorTheme,
        mut width: Pixels,
        window: &mut Window,
        cx: &mut App,
    ) -> Pixels {
        fn collect(items: &MenuItems, rows: &mut Vec<MenuEntry>) {
            for entry in items.entries() {
                if rows.len() >= 16 {
                    return;
                }
                if let MenuEntry::Group(group) = entry {
                    collect(&group.children, rows);
                } else if entry.item().is_some() {
                    rows.push(entry.clone());
                }
            }
        }
        let mut rows = Vec::new();
        collect(items, &mut rows);
        if let Some(active) = self.state.read(cx).active.as_ref()
            && let Some(entry) = items.find(active)
            && !rows.iter().any(|row| row.key() == active)
        {
            rows.push(entry.clone());
        }
        for entry in &rows {
            width = width.max(self.content_width(entry, depth, color_theme, window, cx));
        }
        width
    }

    #[allow(clippy::too_many_arguments)]
    fn content_width(
        &self,
        entry: &MenuEntry,
        depth: usize,
        color_theme: MenuColorTheme,
        window: &mut Window,
        cx: &mut App,
    ) -> Pixels {
        if let MenuEntry::Group(group) = entry {
            return group
                .children
                .entries()
                .iter()
                .map(|entry| self.content_width(entry, depth, color_theme, window, cx))
                .fold(px(0.), Pixels::max);
        }
        let Some(item) = entry.item() else {
            return px(0.);
        };
        let selected = self.state.read(cx).selected_descendant(&item.key);
        let context = MenuRenderContext {
            key: item.key.clone(),
            label: item.label.clone(),
            depth,
            selected,
            active: self.state.read(cx).active.as_ref() == Some(&item.key),
            expanded: self.state.read(cx).open.contains(&item.key),
            collapsed: false,
            disabled: item.disabled || self.state.read(cx).disabled,
            color_theme,
            mode: self.options.mode,
        };
        let measure_element = |mut element: AnyElement, window: &mut Window, cx: &mut App| {
            element
                .layout_as_root(
                    size(
                        gpui::AvailableSpace::MaxContent,
                        gpui::AvailableSpace::MaxContent,
                    ),
                    window,
                    cx,
                )
                .width
        };
        let measure_text = |text: &SharedString, window: &Window| {
            let run = gpui::TextRun {
                len: text.len(),
                font: self.tokens.font().clone(),
                color: self.tokens.globals().color_text.into(),
                background_color: None,
                underline: None,
                strikethrough: None,
            };
            window
                .text_system()
                .shape_line(
                    text.clone(),
                    metric(self.tokens.globals().font_size),
                    &[run],
                    None,
                )
                .width
        };
        let content = if let Some(render) = &item.render {
            measure_element(render(&context, window, cx), window, cx)
        } else {
            measure_text(&item.label, window)
        };
        let mut width = content + metric(self.tokens.item_padding_inline * 2.);
        if item.icon.is_some() {
            width += metric(self.tokens.icon_size + self.tokens.icon_margin_inline_end);
        }
        if let Some(render) = &item.extra_render {
            width += measure_element(render(&context, window, cx), window, cx)
                + metric(self.tokens.globals().padding);
        } else if let Some(extra) = &item.extra {
            width += measure_text(extra, window) + metric(self.tokens.globals().padding);
        }
        if matches!(entry, MenuEntry::SubMenu(_)) {
            width += metric(self.tokens.arrow_size() + self.tokens.globals().margin_xs);
        }
        if let Some(gpui::Length::Definite(gpui::DefiniteLength::Absolute(explicit))) =
            item.style.size.width
        {
            width = explicit.to_pixels(window.rem_size());
        }
        width + metric(self.tokens.item_margin_inline * 2.)
    }

    fn overflow(
        &self,
        items: &MenuItems,
        first: usize,
        window: &mut Window,
        cx: &mut App,
    ) -> AnyElement {
        let label = self.config.locale.common.more_navigation_items.clone();
        let palette = MenuStyle::from_tokens(&self.tokens, self.options.color_theme);
        let state = self.state.clone();
        let keyboard = self.state.clone();
        let changed = self.state.clone();
        let expanded = state.read(cx).overflow_open;
        let disabled = state.read(cx).disabled;
        let revision = state.read(cx).revision;
        let expand_state = self.state.clone();
        let collapse_state = self.state.clone();
        let handle = state.read(cx).overflow_handle.clone();
        let overflow_focus = state.read(cx).overflow_focus.clone();
        let selected = items.entries()[first.min(items.entries().len())..]
            .iter()
            .any(|entry| self.state.read(cx).selected_descendant(entry.key()));
        let content = self.overflow_content(selected, window, cx);
        let row = div()
            .id("overflow-title")
            .debug_selector(|| "menu-overflow".to_owned())
            .track_focus(&overflow_focus)
            .tab_index(-1)
            .relative()
            .flex()
            .items_center()
            .justify_center()
            .px(metric(self.tokens.item_padding_inline))
            .h(metric(self.tokens.horizontal_line_height))
            .text_color(if selected {
                palette.horizontal_selected_text
            } else {
                palette.text
            })
            .cursor_pointer()
            .role(Role::MenuItem)
            .aria_label(label.clone())
            .aria_expanded(expanded)
            .a11y_synthetic_children(|builder| {
                builder
                    .parent_node()
                    .set_has_popup(accesskit::HasPopup::Menu)
            })
            .child(content)
            .on_click(move |event, _, cx| {
                if matches!(event, gpui::ClickEvent::Keyboard(_)) {
                    return;
                }
                state.update(cx, |s, cx| {
                    if !s.mounted || s.disabled || s.revision != revision {
                        return;
                    }
                    s.overflow_open = !s.overflow_open;
                    cx.notify();
                })
            })
            .on_key_down(move |event, window, cx| {
                if keyboard.read(cx).disabled {
                    return;
                }
                if matches!(event.keystroke.key.as_str(), "down" | "enter" | "space") {
                    let first = keyboard.update(cx, |s, cx| {
                        s.overflow_open = true;
                        cx.notify();
                        s.items
                            .entries()
                            .iter()
                            .skip(s.overflow_start)
                            .filter_map(MenuEntry::item)
                            .find(|item| !item.disabled)
                            .map(|item| item.key.clone())
                    });
                    if let Some(first) = first {
                        state::focus_key(&keyboard, first, window, cx);
                    }
                    window.prevent_default();
                    cx.stop_propagation();
                }
            })
            .on_a11y_action(accesskit::Action::Expand, move |_, _, cx| {
                expand_state.update(cx, |s, cx| {
                    if s.mounted && !s.disabled && s.revision == revision && !s.overflow_open {
                        s.overflow_open = true;
                        cx.notify();
                    }
                });
            })
            .on_a11y_action(accesskit::Action::Collapse, move |_, _, cx| {
                collapse_state.update(cx, |s, cx| {
                    if s.mounted && !s.disabled && s.revision == revision && s.overflow_open {
                        s.overflow_open = false;
                        cx.notify();
                    }
                });
            });
        let mut semantics = Accessibility::new(Role::MenuItem);
        semantics.disabled = disabled;
        let row = semantics.apply(row);
        let view = self.clone();
        let hidden = if let Some((source, old_first, hidden)) = &self.mount.read(cx).overflow_items
            && source.same_collection(items)
            && *old_first == first
        {
            hidden.clone()
        } else {
            let hidden = MenuItems::new(items.entries().iter().skip(first).cloned())
                .expect("validated overflow subset");
            self.mount.update(cx, |cache, _| {
                cache.overflow_items = Some((items.clone(), first, hidden.clone()))
            });
            hidden
        };
        let popup_label = label;
        self.surface(
            Popup::new("horizontal-overflow-popup"),
            self.options.color_theme,
        )
        .handle(&handle)
        .disabled(disabled)
        .child(row)
        .open(expanded)
        .triggers(if self.options.trigger == MenuTrigger::Hover {
            PopupTriggers::HOVER
        } else {
            PopupTriggers::NONE
        })
        .mouse_enter_delay(self.options.open_delay)
        .mouse_leave_delay(self.options.close_delay)
        .placement(if self.config.direction == Direction::Rtl {
            PopupPlacement::BottomRight
        } else {
            PopupPlacement::BottomLeft
        })
        .on_open_change(move |change, _, cx| {
            changed.update(cx, |s, cx| {
                if s.overflow_open != change.open {
                    s.overflow_open = change.open;
                    cx.notify();
                }
            });
        })
        .content(move |_, window, cx| {
            view.popup_list(
                &hidden,
                None,
                0,
                &popup_label,
                view.options.color_theme,
                window,
                cx,
            )
        })
        .into_any_element()
    }
}

fn row_current(state: &Entity<MenuState>, key: &MenuKey, revision: u64, cx: &App) -> bool {
    let s = state.read(cx);
    s.mounted
        && !s.disabled
        && s.revision == revision
        && s.items.enabled(key)
        && (s.options.mode != MenuMode::Horizontal
            || s.overflow_open
            || s.items
                .root_position(key)
                .is_some_and(|index| index < s.overflow_start))
        && s.items
            .path(key)
            .iter()
            .skip(1)
            .all(|parent| !s.items.is_submenu(parent) || s.open.contains(parent))
}

fn chevron(
    dimension: Pixels,
    border_radius: Pixels,
    inline: bool,
    rtl: bool,
    content_rtl: bool,
    progress: f64,
) -> AnyElement {
    canvas(
        |_, _, _| (),
        move |bounds, _, window, _| {
            let side = f32::from(dimension);
            if !side.is_finite() || side <= 0. {
                return;
            }
            let p = progress as f32;
            // Ant's zero-height arrow anchors two rounded .6S × .15S bars at
            // the row center. Their default transform origin is (.3S, .075S);
            // RTL content places each bar at S - .6S before rotation. Keeping
            // those anchors also distinguishes RTL roots from LTR popup content.
            let origin = bounds.origin
                + gpui::point(
                    px((if content_rtl { 0.4 } else { 0. }) * side + 0.3 * side),
                    window.pixel_snap(dimension / 2.)
                        + px((0.075 - if inline { 0.2 * p } else { 0. }) * side),
                );
            let quarter = std::f32::consts::FRAC_PI_4;
            let bars = if inline {
                [
                    ((2. * p - 1.) * quarter, 0.25 * side, 0.),
                    ((1. - 2. * p) * quarter, -0.25 * side, 0.),
                ]
            } else {
                let angle = if rtl { -quarter } else { quarter };
                [(angle, 0., -0.25 * side), (-angle, 0., 0.25 * side)]
            };
            let half_width = 0.3 * side;
            let half_height = 0.075 * side;
            let radius = border_radius.max(px(0.)).min(px(half_height));
            let color = window.text_style().color;
            for (angle, translate_x, translate_y) in bars {
                let (sin, cos) = angle.sin_cos();
                let transform = |x: f32, y: f32| {
                    let x = x + translate_x;
                    let y = y + translate_y;
                    origin + gpui::point(px(x * cos - y * sin), px(x * sin + y * cos))
                };
                let r = f32::from(radius);
                let mut path = gpui::PathBuilder::fill();
                path.move_to(transform(-half_width + r, -half_height));
                path.line_to(transform(half_width - r, -half_height));
                if r > 0. {
                    path.arc_to(
                        gpui::point(radius, radius),
                        px(0.),
                        false,
                        true,
                        transform(half_width, -half_height + r),
                    );
                }
                path.line_to(transform(half_width, half_height - r));
                if r > 0. {
                    path.arc_to(
                        gpui::point(radius, radius),
                        px(0.),
                        false,
                        true,
                        transform(half_width - r, half_height),
                    );
                }
                path.line_to(transform(-half_width + r, half_height));
                if r > 0. {
                    path.arc_to(
                        gpui::point(radius, radius),
                        px(0.),
                        false,
                        true,
                        transform(-half_width, half_height - r),
                    );
                }
                path.line_to(transform(-half_width, -half_height + r));
                if r > 0. {
                    path.arc_to(
                        gpui::point(radius, radius),
                        px(0.),
                        false,
                        true,
                        transform(-half_width + r, -half_height),
                    );
                }
                path.close();
                if let Ok(path) = path.build() {
                    window.paint_path(path, color);
                }
            }
        },
    )
    .size(dimension)
    .into_any_element()
}
