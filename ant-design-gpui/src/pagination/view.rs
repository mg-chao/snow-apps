// Copyright (C) 2026 mg-chao
// SPDX-License-Identifier: Apache-2.0

use std::{
    rc::Rc,
    sync::{Arc, LazyLock},
};

use gpui::{
    AnyElement, App, Context, ElementId, Entity, FontWeight, IntoElement, MouseButton, Refineable,
    Render, RenderOnce, Role, SharedString, StyleRefinement, Styled, Subscription, WeakEntity,
    Window, accesskit, div, prelude::*, px, relative,
};

use super::{
    PaginationChange, PaginationChangeSource, PaginationEvent, PaginationItem, PaginationItemKind,
    PaginationMetrics, PaginationMode, PaginationSettings, PaginationSizeChange, PaginationState,
    PaginationTheme, PaginationTokens, QuickJumper, state::PaginationRenderConfig,
};
use crate::{
    ConfigScope, Direction, Input, Select, Size,
    behavior::{Accessibility, Activation, FocusRing},
    icons::{IconName, IconPalette, IconRenderOptions, icon},
    input::{InputInteraction, InputTheme},
    popup::PopupWidth,
    select::{SelectTheme, SelectValue},
};

#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
pub enum PaginationAlign {
    #[default]
    Start,
    Center,
    End,
}

/// Native equivalents of Pagination's public semantic styling slots.
#[derive(Clone, Default)]
pub struct PaginationStyles {
    pub root: StyleRefinement,
    pub item: StyleRefinement,
}

#[derive(Clone, Copy, Debug)]
pub struct PaginationStyleContext {
    pub total: u64,
    pub current: u64,
    pub page_size: u64,
    pub size: Size,
    pub mode: PaginationMode,
    pub disabled: bool,
    pub direction: Direction,
}

/// Item content can be replaced or suppressed without replacing its native
/// activation, focus and accessibility owner.
#[derive(Clone, Copy, Debug)]
pub struct PaginationItemRender {
    pub kind: PaginationItemKind,
    pub page: u64,
    pub selected: bool,
    pub disabled: bool,
}

/// Weak request connection for an application-owned replacement size changer.
#[derive(Clone)]
pub struct PaginationSizeChangerRender {
    pub value: u64,
    pub disabled: bool,
    pub size: Size,
    pub scope: ConfigScope,
    /// Resolved choices, including the current size when it was not configured.
    pub options: Arc<[u64]>,
    pub label: SharedString,
    state: WeakEntity<PaginationState>,
}

impl PaginationSizeChangerRender {
    pub fn change_size(&self, size: u64, window: &mut Window, cx: &mut App) {
        let _ = self.state.update(cx, |state, cx| {
            state.change_page_size(size, window, cx);
        });
    }
}

type Content = Rc<dyn Fn(&mut Window, &mut App) -> AnyElement>;
type ItemRenderer =
    Rc<dyn Fn(&PaginationItemRender, AnyElement, &mut Window, &mut App) -> Option<AnyElement>>;
type TotalRenderer = Rc<dyn Fn(u64, [u64; 2], Direction, &mut Window, &mut App) -> AnyElement>;
type SizeRenderer = Rc<dyn Fn(&PaginationSizeChangerRender, &mut Window, &mut App) -> AnyElement>;
type SelectRenderer = Rc<dyn Fn(Select<u64>, &mut Window, &mut App) -> Select<u64>>;
type StyleResolver = Rc<dyn Fn(&PaginationStyleContext, &mut Window, &mut App) -> PaginationStyles>;

/// Rebuild during render with a stable ID and a retained `PaginationState`.
#[derive(IntoElement)]
pub struct Pagination {
    id: ElementId,
    state: Entity<PaginationState>,
    scope: Option<ConfigScope>,
    total: u64,
    total_explicit: bool,
    current: Option<u64>,
    default_current: u64,
    page_size: Option<u64>,
    default_page_size: u64,
    size: Option<Size>,
    align: PaginationAlign,
    responsive: bool,
    disabled: Option<bool>,
    settings: PaginationSettings,
    show_size_changer: Option<bool>,
    size_changer_threshold: Option<u64>,
    page_size_options: Arc<[u64]>,
    show_title: bool,
    styles: PaginationStyles,
    styles_fn: Option<StyleResolver>,
    accessibility: Accessibility,
    item_render: Option<ItemRenderer>,
    total_render: Option<TotalRenderer>,
    size_render: Option<SizeRenderer>,
    select_render: Option<SelectRenderer>,
    go_content: Option<Content>,
    prev_icon: Option<Content>,
    next_icon: Option<Content>,
    jump_prev_icon: Option<Content>,
    jump_next_icon: Option<Content>,
    on_event: Option<super::state::EventHandler>,
    on_change: Option<super::state::ChangeHandler>,
    on_show_size_change: Option<super::state::SizeChangeHandler>,
}

impl Pagination {
    pub fn new(id: impl Into<ElementId>, state: &Entity<PaginationState>) -> Self {
        Self {
            id: id.into(),
            state: state.clone(),
            scope: None,
            total: 0,
            total_explicit: false,
            current: None,
            default_current: 1,
            page_size: None,
            default_page_size: 10,
            size: None,
            align: PaginationAlign::Start,
            responsive: false,
            disabled: None,
            settings: PaginationSettings::default(),
            show_size_changer: None,
            size_changer_threshold: None,
            page_size_options: {
                static OPTIONS: LazyLock<Arc<[u64]>> =
                    LazyLock::new(|| Arc::from([10, 20, 50, 100]));
                OPTIONS.clone()
            },
            show_title: true,
            styles: PaginationStyles::default(),
            styles_fn: None,
            accessibility: Accessibility::new(Role::Navigation),
            item_render: None,
            total_render: None,
            size_render: None,
            select_render: None,
            go_content: None,
            prev_icon: None,
            next_icon: None,
            jump_prev_icon: None,
            jump_next_icon: None,
            on_event: None,
            on_change: None,
            on_show_size_change: None,
        }
    }
    pub fn scope(mut self, scope: ConfigScope) -> Self {
        self.scope = Some(scope);
        self
    }
    pub fn total(mut self, total: u64) -> Self {
        self.total = total;
        self.total_explicit = true;
        self
    }
    pub fn current(mut self, current: u64) -> Self {
        self.current = Some(current);
        self
    }
    pub fn default_current(mut self, current: u64) -> Self {
        self.default_current = current;
        self
    }
    pub fn page_size(mut self, size: u64) -> Self {
        self.page_size = Some(size);
        self
    }
    pub fn default_page_size(mut self, size: u64) -> Self {
        self.default_page_size = size;
        self
    }
    pub fn size(mut self, size: Size) -> Self {
        self.size = Some(size);
        self
    }
    pub fn align(mut self, align: PaginationAlign) -> Self {
        self.align = align;
        self
    }
    pub fn responsive(mut self, responsive: bool) -> Self {
        self.responsive = responsive;
        self
    }
    pub fn disabled(mut self, disabled: bool) -> Self {
        self.disabled = Some(disabled);
        self
    }
    pub fn mode(mut self, mode: PaginationMode) -> Self {
        self.settings.mode = mode;
        self
    }
    pub fn show_less_items(mut self, show: bool) -> Self {
        self.settings.show_less_items = show;
        self
    }
    pub fn show_prev_next_jumpers(mut self, show: bool) -> Self {
        self.settings.show_prev_next_jumpers = show;
        self
    }
    pub fn hide_on_single_page(mut self, hide: bool) -> Self {
        self.settings.hide_on_single_page = hide;
        self
    }
    pub fn quick_jumper(mut self, jumper: QuickJumper) -> Self {
        self.settings.quick_jumper = jumper;
        self
    }
    pub fn show_size_changer(mut self, show: bool) -> Self {
        self.show_size_changer = Some(show);
        self
    }
    pub fn total_boundary_show_size_changer(mut self, boundary: u64) -> Self {
        self.size_changer_threshold = Some(boundary);
        self
    }
    pub fn page_size_options(mut self, options: impl IntoIterator<Item = u64>) -> Self {
        self.page_size_options = options.into_iter().collect::<Vec<_>>().into();
        self
    }
    pub fn show_title(mut self, show: bool) -> Self {
        self.show_title = show;
        self
    }
    pub fn accessibility_label(mut self, label: impl Into<SharedString>) -> Self {
        self.accessibility.label = Some(label.into());
        self
    }
    pub fn accessibility_description(mut self, description: impl Into<SharedString>) -> Self {
        self.accessibility.description = Some(description.into());
        self
    }
    pub fn automation_id(mut self, id: impl Into<SharedString>) -> Self {
        self.accessibility.automation_id = Some(id.into());
        self
    }
    pub fn styles(mut self, styles: PaginationStyles) -> Self {
        self.styles = styles;
        self
    }
    pub fn styles_fn(
        mut self,
        resolver: impl Fn(&PaginationStyleContext, &mut Window, &mut App) -> PaginationStyles + 'static,
    ) -> Self {
        self.styles_fn = Some(Rc::new(resolver));
        self
    }
    pub fn on_event(
        mut self,
        handler: impl Fn(&PaginationEvent, &mut Window, &mut App) + 'static,
    ) -> Self {
        self.on_event = Some(Rc::new(handler));
        self
    }
    pub fn on_change(
        mut self,
        handler: impl Fn(&PaginationChange, &mut Window, &mut App) + 'static,
    ) -> Self {
        self.on_change = Some(Rc::new(handler));
        self
    }
    pub fn on_show_size_change(
        mut self,
        handler: impl Fn(&PaginationSizeChange, &mut Window, &mut App) + 'static,
    ) -> Self {
        self.on_show_size_change = Some(Rc::new(handler));
        self
    }
    pub fn item_render(
        mut self,
        renderer: impl Fn(
            &PaginationItemRender,
            AnyElement,
            &mut Window,
            &mut App,
        ) -> Option<AnyElement>
        + 'static,
    ) -> Self {
        self.item_render = Some(Rc::new(renderer));
        self
    }
    pub fn show_total<E: IntoElement>(
        mut self,
        renderer: impl Fn(u64, [u64; 2], &mut Window, &mut App) -> E + 'static,
    ) -> Self {
        self.total_render = Some(Rc::new(move |total, range, _, window, cx| {
            renderer(total, range, window, cx).into_any_element()
        }));
        self
    }
    /// Text totals use the inherited native font's exact advance, while still
    /// wrapping within a constrained container.
    pub fn show_total_text<T: Into<SharedString>>(
        mut self,
        renderer: impl Fn(u64, [u64; 2], &mut Window, &mut App) -> T + 'static,
    ) -> Self {
        self.total_render = Some(Rc::new(move |total, range, direction, window, cx| {
            super::text::IntrinsicText::new(renderer(total, range, window, cx).into(), direction)
                .into_any_element()
        }));
        self
    }
    pub fn size_changer_render<E: IntoElement>(
        mut self,
        renderer: impl Fn(&PaginationSizeChangerRender, &mut Window, &mut App) -> E + 'static,
    ) -> Self {
        self.size_render = Some(Rc::new(move |info, window, cx| {
            renderer(info, window, cx).into_any_element()
        }));
        self
    }
    /// Configure the built-in Select while keeping Pagination's value/event owner.
    pub fn size_changer_select(
        mut self,
        configure: impl Fn(Select<u64>, &mut Window, &mut App) -> Select<u64> + 'static,
    ) -> Self {
        self.select_render = Some(Rc::new(configure));
        self
    }
    pub fn go_button<E: IntoElement>(
        mut self,
        renderer: impl Fn(&mut Window, &mut App) -> E + 'static,
    ) -> Self {
        self.settings.quick_jumper = QuickJumper::Confirm;
        self.go_content = Some(Rc::new(move |window, cx| {
            renderer(window, cx).into_any_element()
        }));
        self
    }
    pub fn prev_icon<E: IntoElement>(
        mut self,
        renderer: impl Fn(&mut Window, &mut App) -> E + 'static,
    ) -> Self {
        self.prev_icon = Some(Rc::new(move |w, c| renderer(w, c).into_any_element()));
        self
    }
    pub fn next_icon<E: IntoElement>(
        mut self,
        renderer: impl Fn(&mut Window, &mut App) -> E + 'static,
    ) -> Self {
        self.next_icon = Some(Rc::new(move |w, c| renderer(w, c).into_any_element()));
        self
    }
    pub fn jump_prev_icon<E: IntoElement>(
        mut self,
        renderer: impl Fn(&mut Window, &mut App) -> E + 'static,
    ) -> Self {
        self.jump_prev_icon = Some(Rc::new(move |w, c| renderer(w, c).into_any_element()));
        self
    }
    pub fn jump_next_icon<E: IntoElement>(
        mut self,
        renderer: impl Fn(&mut Window, &mut App) -> E + 'static,
    ) -> Self {
        self.jump_next_icon = Some(Rc::new(move |w, c| renderer(w, c).into_any_element()));
        self
    }
}

impl Styled for Pagination {
    fn style(&mut self) -> &mut StyleRefinement {
        &mut self.styles.root
    }
}

struct Mount {
    state: WeakEntity<PaginationState>,
    _observer: Subscription,
    _release: Subscription,
}

impl RenderOnce for Pagination {
    fn render(mut self, window: &mut Window, cx: &mut App) -> impl IntoElement {
        if !self.total_explicit {
            self.total = self.state.read(cx).total();
        }
        let scope = self
            .scope
            .clone()
            .unwrap_or_else(|| ConfigScope::application(cx));
        let config = scope.snapshot(cx);
        let tokens = config.theme.component::<PaginationTheme>();
        let g = tokens.globals();
        let width = f32::from(window.viewport_size().width) as f64;
        // Ant 6.6.7 emits the legacy `mini` class for responsive sizing, while
        // its styles only target `small`. Preserve the measured visual result.
        let size = self
            .size
            .or(config.configured_size())
            .unwrap_or(Size::Medium);
        let _responsive = self.responsive;
        let metrics = tokens.metrics(size);
        let disabled = self.disabled.unwrap_or(config.disabled);
        let show_size_changer = self
            .show_size_changer
            .or(config.pagination.show_size_changer)
            .unwrap_or(
                self.total
                    > self
                        .size_changer_threshold
                        .unwrap_or(config.pagination.total_boundary_show_size_changer),
            );
        let state = self.state.clone();
        let observed = state.clone();
        let mount_id = (
            self.id.clone(),
            SharedString::from(format!("controller-{}", state.entity_id())),
        );
        let _mount = window.use_keyed_state(mount_id, cx, move |_, cx| {
            observed.update(cx, |state, _| state.mounted = true);
            Mount {
                state: observed.downgrade(),
                _observer: cx.observe(&observed, |_, _, cx| cx.notify()),
                _release: cx.on_release(|mount: &mut Mount, cx| {
                    let _ = mount.state.update(cx, |state, cx| state.unmount(cx));
                }),
            }
        });
        state.update(cx, |state, cx| {
            state.on_event = self.on_event.clone();
            state.on_change = self.on_change.clone();
            state.on_show_size_change = self.on_show_size_change.clone();
            state.sync_render(
                PaginationRenderConfig {
                    total: self.total,
                    current: self.current,
                    default_current: self.default_current,
                    page_size: self.page_size,
                    default_page_size: self.default_page_size,
                    settings: self.settings,
                    disabled: self.disabled,
                    show_size_changer: show_size_changer && width > g.screen_sm,
                    options_visible: width > g.screen_sm,
                    narrow: width <= g.screen_lg,
                },
                scope.clone(),
                window,
                cx,
            );
            state.sync_size_options(
                &self.page_size_options,
                &config.locale.pagination.items_per_page,
                cx,
            );
        });
        let s = state.read(cx);
        let current = s.model.current;
        let page_size = s.model.page_size;
        let count = s.model.page_count();
        let range = s.model.range();
        let items: Vec<_> = s
            .model
            .items()
            .into_iter()
            .filter(|item| !(width <= g.screen_lg && item.hidden_at_narrow))
            .collect();
        if self.settings.hide_on_single_page && self.total <= page_size {
            return div().id(self.id).into_any_element();
        }
        if let Some(resolve) = &self.styles_fn {
            let styles = resolve(
                &PaginationStyleContext {
                    total: self.total,
                    current,
                    page_size,
                    size,
                    mode: self.settings.mode,
                    disabled,
                    direction: config.direction,
                },
                window,
                cx,
            );
            self.styles.root.refine(&styles.root);
            self.styles.item.refine(&styles.item);
        }
        let rtl = config.direction == Direction::Rtl;
        let mut root = div()
            .id(self.id.clone())
            .relative()
            .flex()
            .items_center()
            .w_full()
            .font(tokens.font().clone())
            .text_size(px(metrics.font_size as f32))
            .line_height(relative(g.line_height as f32))
            .text_color(g.color_text)
            .when(rtl, |root| root.flex_row_reverse());
        root = match (self.align, rtl) {
            (PaginationAlign::Start, false) | (PaginationAlign::End, true) => root.justify_start(),
            (PaginationAlign::Center, _) => root.justify_center(),
            _ => root.justify_end(),
        };
        root.style().refine(&self.styles.root);
        if let Some(total) = &self.total_render {
            root = root.child(
                logical_end(
                    div()
                        .min_w_0()
                        .h(px(metrics.item_size as f32))
                        .line_height(px(metrics.line_height as f32)),
                    metrics.item_spacing,
                    rtl,
                )
                .child(total(
                    self.total,
                    [range.0, range.1],
                    config.direction,
                    window,
                    cx,
                )),
            );
        }
        let mut item_cursor = super::geometry::PixelCursor::default();
        for item in items {
            if self.settings.mode != PaginationMode::Full && item.kind == PaginationItemKind::Next {
                let mut pager = div()
                    .flex()
                    .items_center()
                    .flex_shrink_0()
                    .h(px(metrics.item_size as f32))
                    .text_color(if disabled {
                        g.color_text_disabled
                    } else {
                        g.color_text
                    })
                    .when(rtl, |d| d.flex_row_reverse());
                pager.style().refine(&self.styles.item);
                pager = if self.settings.mode == PaginationMode::SimpleReadOnly {
                    pager.child(sized_text(
                        current.to_string().into(),
                        tokens.font(),
                        metrics.font_size,
                        g.color_text.to_hsla(),
                        window,
                    ))
                } else {
                    pager.child(
                        self.editor(true, size, &scope, &tokens, &metrics, disabled, window, cx),
                    )
                };
                pager = pager
                    .child(div().mx(px(metrics.slash_margin as f32)).child(sized_text(
                        "/".into(),
                        tokens.font(),
                        metrics.font_size,
                        g.color_text.to_hsla(),
                        window,
                    )))
                    .child(sized_text(
                        count.to_string().into(),
                        tokens.font(),
                        metrics.font_size,
                        g.color_text.to_hsla(),
                        window,
                    ));
                root = root.child(logical_end(pager, metrics.item_spacing, rtl));
            }
            if let Some(element) = self.render_item(
                item,
                current,
                size,
                &scope,
                &tokens,
                &metrics,
                disabled,
                rtl,
                &mut item_cursor,
                window,
                cx,
            ) {
                root = root.child(element);
            }
        }
        state.update(cx, |state, cx| state.recover_rendered_focus(window, cx));
        if width > g.screen_sm {
            let show_quick =
                self.settings.quick_jumper != QuickJumper::Hidden && self.total > page_size;
            if show_size_changer || show_quick {
                let mut options = logical_start(
                    div()
                        .flex()
                        .items_start()
                        .flex_wrap()
                        .min_w_0()
                        .min_h(px(metrics.item_size as f32))
                        .when(rtl, |d| d.flex_row_reverse()),
                    metrics.options_margin,
                    rtl,
                );
                if show_size_changer {
                    let info = PaginationSizeChangerRender {
                        value: page_size,
                        disabled,
                        size,
                        scope: scope.clone(),
                        options: state.read(cx).size_options.clone(),
                        label: config.locale.pagination.page_size.clone(),
                        state: state.downgrade(),
                    };
                    let child = if let Some(render) = &self.size_render {
                        render(&info, window, cx)
                    } else {
                        let label = SharedString::from(format!(
                            "{} {}",
                            page_size, config.locale.pagination.items_per_page
                        ));
                        let select_tokens = config.theme.component::<SelectTheme>();
                        let input = select_tokens
                            .surface(
                                size,
                                config.variant,
                                crate::ValidationStatus::None,
                                false,
                                InputInteraction {
                                    disabled,
                                    ..Default::default()
                                },
                            )
                            .at_scale(window.scale_factor());
                        let run = gpui::TextRun {
                            len: label.len(),
                            font: config.font.clone(),
                            color: g.color_text.to_hsla(),
                            background_color: None,
                            underline: None,
                            strikethrough: None,
                        };
                        let label_width = window
                            .text_system()
                            .shape_line(label.clone(), px(input.font_size as f32), &[run], None)
                            .width;
                        let selector = state.read(cx).size_selector.clone();
                        let select_id = (
                            self.id.clone(),
                            SharedString::from(format!("size-changer-{}", selector.entity_id())),
                        );
                        let mut select = Select::new(select_id, &selector)
                            .scope(scope.clone())
                            .size(size)
                            .disabled(disabled)
                            .value([SelectValue::new(page_size, label.clone())])
                            .popup_width(PopupWidth::AtLeastTrigger)
                            .virtualized(false)
                            .accessibility_label(config.locale.pagination.page_size.clone())
                            .automation_id("pagination-size-changer")
                            .w(label_width
                                + px((input.padding_inline * 2.
                                    + input.border_width * 2.
                                    + select_tokens.show_arrow_padding_inline_end)
                                    as f32));
                        if rtl {
                            // GPUI has no inherited paragraph-direction style.
                            // Keep values and native semantics in logical order;
                            // apply the direction only to the displayed labels.
                            select = select
                                .label_render(|value, _, _| {
                                    div().flex().justify_end().text_right().truncate().child(
                                        super::text::IntrinsicText::decorative(
                                            value.label.clone(),
                                            Direction::Rtl,
                                        ),
                                    )
                                })
                                .option_render(|option, _, _| {
                                    div().w_full().flex().justify_end().text_right().child(
                                        super::text::IntrinsicText::decorative(
                                            option.option.label.clone(),
                                            Direction::Rtl,
                                        ),
                                    )
                                });
                        }
                        if let Some(configure) = &self.select_render {
                            select = configure(select, window, cx);
                        }
                        select = select
                            .value([SelectValue::new(page_size, label)])
                            .disabled(disabled);
                        select.into_any_element()
                    };
                    options = options.child(
                        div()
                            .relative()
                            .self_center()
                            .flex_shrink_0()
                            .top(px(tokens.mini_options_size_changer_top as f32))
                            .when(size != Size::Small, |d| d.top(px(0.)))
                            .child(child),
                    );
                }
                if show_quick {
                    let mut quick_cursor = super::geometry::PixelCursor::default();
                    let prefix = f64::from(f32::from(text_width(
                        &config.locale.pagination.jump_to,
                        tokens.font(),
                        metrics.font_size,
                        g.color_text.to_hsla(),
                        window,
                    )));
                    let suffix = f64::from(f32::from(text_width(
                        &config.locale.pagination.page,
                        tokens.font(),
                        metrics.font_size,
                        g.color_text.to_hsla(),
                        window,
                    )));
                    // The inner text keeps its reading order, but the body is
                    // anchored at the RTL trailing edge. Carry widths from that
                    // edge so fractional text advances do not displace the input.
                    let leading_width = quick_cursor
                        .span(if rtl { suffix } else { prefix }, |value| {
                            window.pixel_snap_f64(value)
                        });
                    let leading_margin = quick_cursor
                        .span(metrics.item_spacing, |value| window.pixel_snap_f64(value));
                    let input_width = quick_cursor
                        .span(metrics.jumper_width, |value| window.pixel_snap_f64(value));
                    let trailing_margin = quick_cursor
                        .span(metrics.item_spacing, |value| window.pixel_snap_f64(value));
                    let trailing_width = quick_cursor
                        .span(if rtl { prefix } else { suffix }, |value| {
                            window.pixel_snap_f64(value)
                        });
                    let (prefix_width, margin_left, margin_right, suffix_width) = if rtl {
                        (
                            trailing_width,
                            trailing_margin,
                            leading_margin,
                            leading_width,
                        )
                    } else {
                        (
                            leading_width,
                            leading_margin,
                            trailing_margin,
                            trailing_width,
                        )
                    };
                    let mut editor_metrics = metrics;
                    editor_metrics.jumper_width = input_width;
                    let font_id = window.text_system().resolve_font(tokens.font());
                    let descent = window
                        .text_system()
                        .descent(font_id, px(metrics.font_size as f32));
                    let descent = px(f32::from(descent).abs());
                    let mut body = div()
                        .flex()
                        .flex_wrap()
                        .items_center()
                        .min_w_0()
                        .when(rtl, |body| body.justify_end())
                        .line_height(px(metrics.item_size as f32))
                        .mb(-descent)
                        .child(
                            sized_text(
                                config.locale.pagination.jump_to.clone(),
                                tokens.font(),
                                metrics.font_size,
                                g.color_text.to_hsla(),
                                window,
                            )
                            .w(px(prefix_width as f32)),
                        )
                        .child(
                            div()
                                .relative()
                                .flex_shrink_0()
                                .top(px(4. / 3.))
                                .ml(px(margin_left as f32))
                                .mr(px(margin_right as f32))
                                .child(self.editor(
                                    false,
                                    size,
                                    &scope,
                                    &tokens,
                                    &editor_metrics,
                                    disabled,
                                    window,
                                    cx,
                                )),
                        )
                        .child(
                            sized_text(
                                config.locale.pagination.page.clone(),
                                tokens.font(),
                                metrics.font_size,
                                g.color_text.to_hsla(),
                                window,
                            )
                            .w(px(suffix_width as f32)),
                        );
                    body = body.self_start();
                    let mut quick = logical_start(
                        div()
                            .flex()
                            .items_start()
                            .min_w_0()
                            .min_h(px(metrics.item_size as f32))
                            .when(rtl, |d| d.flex_row_reverse()),
                        metrics.quick_jumper_margin,
                        rtl,
                    )
                    .child(body);
                    if self.settings.quick_jumper == QuickJumper::Confirm {
                        quick = quick.child(
                            div()
                                .self_center()
                                .child(self.confirm(false, size, &scope, disabled, window, cx)),
                        );
                    }
                    options = options.child(quick);
                }
                root = root.child(options);
            }
        }
        self.accessibility.disabled = disabled;
        if self.accessibility.label.is_none() {
            self.accessibility.label = Some(config.locale.pagination.root_label.clone());
        }
        self.accessibility.apply(root).into_any_element()
    }
}

impl Pagination {
    #[allow(clippy::too_many_arguments)]
    fn editor(
        &self,
        simple: bool,
        size: Size,
        scope: &ConfigScope,
        tokens: &Arc<PaginationTokens>,
        metrics: &PaginationMetrics,
        disabled: bool,
        window: &mut Window,
        cx: &mut App,
    ) -> AnyElement {
        let config = scope.snapshot(cx);
        let state = self.state.read(cx);
        let editor = if simple {
            state.simple_editor.clone()
        } else {
            state.quick_editor.clone()
        };
        let base = config.theme.component::<InputTheme>();
        let token_owner = tokens.clone();
        let base_owner = base.clone();
        let cache = window.use_keyed_state(
            (
                self.id.clone(),
                if simple {
                    "simple-tokens"
                } else {
                    "quick-tokens"
                },
            ),
            cx,
            move |_, _| EditorTokens {
                resolved: Arc::new(token_owner.input_tokens(&base_owner, size, simple)),
                pagination: token_owner,
                base: base_owner,
                size,
            },
        );
        let input_tokens = cache.update(cx, |cache, _| {
            if !Arc::ptr_eq(&cache.pagination, tokens)
                || !Arc::ptr_eq(&cache.base, &base)
                || cache.size != size
            {
                cache.resolved = Arc::new(tokens.input_tokens(&base, size, simple));
                cache.pagination = tokens.clone();
                cache.base = base;
                cache.size = size;
            }
            cache.resolved.clone()
        });
        let mut input = Input::new(
            (
                self.id.clone(),
                SharedString::from(format!(
                    "{}-input-{}",
                    if simple { "simple" } else { "quick" },
                    editor.entity_id()
                )),
            ),
            &editor,
        )
        .scope(scope.clone())
        .size(size)
        .disabled(disabled)
        .accessibility_label(if simple {
            config.locale.pagination.jump_to.clone()
        } else {
            config.locale.pagination.page.clone()
        })
        .automation_id(if simple {
            "pagination-simple-input"
        } else {
            "pagination-quick-input"
        })
        .w(px(metrics.jumper_width as f32))
        .h(px(metrics.item_size as f32));
        input.tokens = Some(input_tokens);
        input.centered = simple;
        if simple {
            let weak = self.state.downgrade();
            input.frame_decorator = Some(Box::new(move |frame, _, _, _| {
                frame.capture_key_down(move |event, window, cx| {
                    if !event.keystroke.modifiers.modified()
                        && matches!(event.keystroke.key.as_str(), "up" | "down")
                    {
                        let _ = weak.update(cx, |state, cx| {
                            state.step_simple(event.keystroke.key == "down", window, cx);
                        });
                        window.prevent_default();
                        cx.stop_propagation();
                    }
                })
            }));
        }
        input.into_any_element()
    }

    fn confirm(
        &self,
        simple: bool,
        _size: Size,
        scope: &ConfigScope,
        disabled: bool,
        window: &mut Window,
        cx: &mut App,
    ) -> AnyElement {
        let label = scope.snapshot(cx).locale.pagination.jump_to_confirm.clone();
        let confirm_id: ElementId = (
            self.id.clone(),
            SharedString::from(format!("confirm-{}", self.state.entity_id())),
        )
            .into();
        let controller = self.state.downgrade();
        let retained = window.use_keyed_state(confirm_id.clone(), cx, move |window, cx| {
            let focus = crate::behavior::focus_handle(cx);
            let guard = crate::behavior::ActivationGuard::default();
            let cancellation = guard.clone();
            let blur = window.on_focus_out(&focus, cx, move |_, _, cx| {
                cancellation.cancel();
                let _ = controller.update(cx, |state, cx| {
                    if state.confirm_pointer_pressed || state.confirm_space_pressed {
                        state.confirm_pointer_pressed = false;
                        state.confirm_space_pressed = false;
                        cx.notify();
                    }
                });
            });
            ConfirmState {
                focus,
                guard,
                _blur: blur,
            }
        });
        let (focus, guard) = retained.update(cx, |retained, cx| {
            retained.guard.update(
                crate::behavior::Availability {
                    disabled,
                    busy: false,
                },
                &retained.focus,
                window,
                cx,
            );
            (retained.focus.clone(), retained.guard.clone())
        });
        let (hovered, pressed) = self.state.update(cx, |state, _| {
            state.confirm_focus = (!disabled).then(|| focus.clone());
            state.confirm_guard = Some(guard.clone());
            (
                state.confirm_hovered,
                state.confirm_space_pressed
                    || state.confirm_pointer_pressed && state.confirm_hovered,
            )
        });
        let mut element = div()
            .id((confirm_id, "custom-go-action"))
            .flex()
            .items_center()
            .justify_center()
            .flex_shrink_0()
            .debug_selector(|| "pagination-go".into())
            .when(!disabled, |d| d.track_focus(&focus))
            .when(self.go_content.is_none(), |d| d.cursor_default())
            .when(!disabled && self.go_content.is_some(), |d| {
                d.cursor_pointer()
            });
        if let Some(content) = &self.go_content {
            element = element.child(content(window, cx));
        } else {
            // Ant renders a plain HTML confirmation button, whose form
            // typography and neutral surface differ from an Ant Button.
            element = element
                .relative()
                .top(px(4. / 3.))
                .font(gpui::font("Arial"))
                .text_size(px(40. / 3.))
                .line_height(px(46. / 3.))
                .px(px(6.))
                .py(px(1.))
                .border(px(2.))
                .border_color(gpui::Hsla::transparent_black())
                .bg(if disabled {
                    gpui::Hsla::transparent_black()
                } else {
                    gpui::rgb(if pressed {
                        0xf5f5f5
                    } else if hovered {
                        0xe5e5e5
                    } else {
                        0xefefef
                    })
                    .into()
                })
                .rounded(px(2.))
                .text_color(if disabled {
                    gpui::rgba(0x1010104d)
                } else {
                    gpui::rgba(0x000000ff)
                })
                .child(
                    div()
                        .absolute()
                        .top(px(-2.))
                        .left(px(-2.))
                        .right(px(-2.))
                        .bottom(px(-2.))
                        .child(
                            gpui::canvas(|_, _, _| (), {
                                let focus = focus.clone();
                                move |bounds, (), window, _| {
                                    let scale = window.scale_factor();
                                    let focused = !disabled
                                        && focus.is_focused(window)
                                        && window.last_input_was_keyboard();
                                    let stroke =
                                        px(if focused { 2. } else { scale.floor() / scale });
                                    let resolved_border = scale.floor() >= 2.;
                                    if disabled {
                                        // The native border straddles the fill edge.
                                        // At two physical pixels its outer band is
                                        // parent-backed and inner band is fill-backed.
                                        // A one-pixel border uses the measured
                                        // collapsed color instead of double blending.
                                        let inset = if resolved_border {
                                            stroke * 0.5
                                        } else {
                                            stroke
                                        };
                                        window.paint_quad(
                                            gpui::fill(bounds.dilate(-inset), disabled_go_fill())
                                                .corner_radii(gpui::Corners::all(
                                                    (px(2.) - inset).max(px(0.)),
                                                )),
                                        );
                                    }
                                    window.paint_quad(
                                        gpui::outline(
                                            bounds,
                                            if disabled {
                                                disabled_go_outline(resolved_border)
                                            } else {
                                                gpui::rgb(if focused {
                                                    0x101010
                                                } else if pressed {
                                                    0x8d8d8d
                                                } else if hovered {
                                                    0x4f4f4f
                                                } else {
                                                    0x767676
                                                })
                                                .into()
                                            },
                                            Default::default(),
                                        )
                                        .border_widths(stroke)
                                        .corner_radii(gpui::Corners::all(px(2.))),
                                    );
                                }
                            })
                            .size_full(),
                        ),
                )
                .child(sized_text(
                    label.clone(),
                    &gpui::font("Arial"),
                    40. / 3.,
                    gpui::black(),
                    window,
                ));
        }
        let weak = self.state.downgrade();
        element = element.on_hover(move |hovered, _, cx| {
            let _ = weak.update(cx, |state, cx| {
                if state.confirm_pointer_pressed {
                    return;
                }
                let hovered = *hovered
                    && state
                        .confirm_guard
                        .as_ref()
                        .is_some_and(|guard| guard.availability().can_activate());
                if state.confirm_hovered != hovered {
                    state.confirm_hovered = hovered;
                    cx.notify();
                }
            });
        });
        let weak = self.state.downgrade();
        element = element.on_mouse_down(MouseButton::Left, move |_, _, cx| {
            set_confirm_pressed(&weak, true, false, cx);
        });
        let weak = self.state.downgrade();
        element = element.on_mouse_up(MouseButton::Left, move |_, _, cx| {
            set_confirm_pressed(&weak, false, false, cx);
        });
        let weak = self.state.downgrade();
        element = element.on_mouse_up_out(MouseButton::Left, move |_, _, cx| {
            set_confirm_pressed(&weak, false, false, cx);
        });
        let weak = self.state.downgrade();
        element = element.capture_key_down(move |event, _, cx| {
            if !event.is_held
                && !event.keystroke.modifiers.modified()
                && event.keystroke.key == "space"
            {
                set_confirm_pressed(&weak, true, true, cx);
            }
        });
        let weak = self.state.downgrade();
        element = element.on_key_up(move |_, _, cx| set_confirm_pressed(&weak, false, true, cx));
        let weak = self.state.downgrade();
        element = element.on_mouse_exit(move |_, _, cx| {
            let _ = weak.update(cx, |state, cx| {
                if state.confirm_hovered {
                    state.confirm_hovered = false;
                    cx.notify();
                }
            });
        });
        if self.go_content.is_none() && self.state.read(cx).confirm_pointer_pressed {
            let weak = self.state.downgrade();
            element = element.child(
                gpui::canvas(
                    |_, _, _| (),
                    move |bounds, (), window, cx| {
                        let visible = bounds.intersect(&window.content_mask().bounds);
                        let _ = weak.update(cx, |state, cx| {
                            let hovered = visible.contains(&window.mouse_position());
                            if state.confirm_pointer_pressed && state.confirm_hovered != hovered {
                                state.confirm_hovered = hovered;
                                cx.notify();
                            }
                        });
                        let weak = weak.clone();
                        window.on_mouse_event(move |event: &gpui::MouseMoveEvent, phase, _, cx| {
                            if phase == gpui::DispatchPhase::Capture {
                                let _ = weak.update(cx, |state, cx| {
                                    let hovered = visible.contains(&event.position);
                                    if state.confirm_pointer_pressed
                                        && state.confirm_hovered != hovered
                                    {
                                        state.confirm_hovered = hovered;
                                        cx.notify();
                                    }
                                });
                            }
                        });
                    },
                )
                .absolute()
                .inset_0(),
            );
        }
        let weak = self.state.downgrade();
        let element = guard.install(element, move |_, window, cx| {
            let _ = weak.update(cx, |state, cx| {
                if simple {
                    state.commit_simple(window, cx);
                } else {
                    state.commit_quick(window, cx);
                }
            });
        });
        let element = if self.go_content.is_some() {
            FocusRing::new(gpui::black(), px(1.))
                .apply(element, &focus)
                .into_any_element()
        } else {
            element.into_any_element()
        };
        Accessibility {
            disabled,
            ..Accessibility::new(Role::Button).label(label)
        }
        .apply(element)
        .into_any_element()
    }

    #[allow(clippy::too_many_arguments)]
    fn render_item(
        &self,
        item: PaginationItem,
        current: u64,
        _size: Size,
        scope: &ConfigScope,
        tokens: &PaginationTokens,
        metrics: &PaginationMetrics,
        disabled: bool,
        rtl: bool,
        cursor: &mut super::geometry::PixelCursor,
        window: &mut Window,
        cx: &mut App,
    ) -> Option<AnyElement> {
        let config = scope.snapshot(cx);
        let selected = self.total != 0 && item.kind == PaginationItemKind::Page(current);
        let navigation = !matches!(item.kind, PaginationItemKind::Page(_));
        let jump = matches!(
            item.kind,
            PaginationItemKind::JumpPrevious | PaginationItemKind::JumpNext
        );
        let unavailable = disabled || item.disabled;
        self.state
            .update(cx, |state, cx| state.restore_item(item.kind, window, cx));
        let (focus, guard, hovered, pressed) = {
            let s = self.state.read(cx);
            let retained = s.items.get(&item.kind)?;
            (
                retained.focus.clone(),
                retained.guard.clone(),
                retained.hovered,
                retained.pressed,
            )
        };
        let focused = focus.is_focused(window) && window.last_input_was_keyboard();
        let mut surface = tokens.surface(selected, navigation, unavailable, hovered, pressed);
        if jump {
            surface.background = crate::theme::Color::TRANSPARENT;
            surface.border = crate::theme::Color::TRANSPARENT;
        } else if !navigation && item.disabled && !disabled {
            surface.background = tokens.item_bg;
        } else if navigation && item.disabled && !disabled && tokens.globals().wireframe {
            surface.background = tokens.item_link_bg;
        }
        let mut policy = config.motion_policy(cx);
        policy.enabled &= window.is_visible() && window.is_window_active();
        let (background, border, text, jump_opacity) = self.state.update(cx, |state, cx| {
            let now = cx
                .background_executor()
                .now()
                .saturating_duration_since(state.epoch);
            let retained = state
                .items
                .get_mut(&item.kind)
                .expect("visible pagination item");
            let (background, a) = retained.colors.0.resolve(
                surface.background.to_hsla(),
                now,
                tokens.motion_duration(),
                crate::input::motion::CSS_EASE,
                policy,
            );
            let (border, b) = retained.colors.1.resolve(
                surface.border.to_hsla(),
                now,
                tokens.motion_duration(),
                crate::input::motion::CSS_EASE,
                policy,
            );
            let (text, c) = retained.colors.2.resolve(
                surface.text.to_hsla(),
                now,
                tokens.motion_duration(),
                crate::input::motion::CSS_EASE,
                policy,
            );
            let (opacity, d) = retained.jump_opacity.resolve(
                if !unavailable && (hovered || focused) {
                    1.
                } else {
                    0.
                },
                now,
                tokens.motion_duration(),
                crate::input::motion::CSS_EASE,
                policy,
                crate::input::motion::scalar,
            );
            if a || b || c || d {
                state.request_frame(window, cx);
            }
            (background, border, text, opacity)
        });
        if jump {
            surface.font_weight = 400.;
        }
        let scale = f64::from(window.scale_factor());
        let border_width = (metrics.border_width * scale).floor() / scale;
        let mut label_style = window.text_style();
        label_style.font_family = tokens.font().family.clone();
        label_style.font_features = tokens.font().features.clone();
        label_style.font_fallbacks = tokens.font().fallbacks.clone();
        label_style.font_style = tokens.font().style;
        label_style.font_weight = tokens.font().weight;
        label_style.font_size = px(metrics.font_size as f32).into();
        label_style.refine(&self.styles.root.text);
        if selected {
            label_style.font_weight = FontWeight(surface.font_weight);
        }
        if !jump {
            label_style.refine(&self.styles.item.text);
        }
        let label_font = label_style.font();
        let label_size = label_style.font_size.to_pixels(window.rem_size());
        let label = if let PaginationItemKind::Page(page) = item.kind {
            Some(SharedString::from(page.to_string()))
        } else {
            None
        };
        let intrinsic = label
            .as_ref()
            .map_or(metrics.item_size, |label| {
                let run = gpui::TextRun {
                    len: label.len(),
                    font: label_font.clone(),
                    color: text,
                    background_color: None,
                    underline: None,
                    strikethrough: None,
                };
                f64::from(f32::from(
                    window
                        .text_system()
                        .shape_line(label.clone(), label_size, &[run], None)
                        .width,
                )) + 2. * metrics.padding_inline
                    + 2. * border_width
            })
            .max(metrics.item_size);
        let previous_cursor = *cursor;
        let item_width = cursor.span(intrinsic, |value| window.pixel_snap_f64(value));
        let item_gap = if item.kind == PaginationItemKind::Next {
            0.
        } else {
            cursor.span(metrics.item_spacing, |value| window.pixel_snap_f64(value))
        };
        let original = match item.kind {
            PaginationItemKind::Page(page) => {
                let left_border = self
                    .styles
                    .item
                    .border_widths
                    .left
                    .map_or(px(border_width as f32), |width| {
                        width.to_pixels(window.rem_size())
                    });
                let right_border = self
                    .styles
                    .item
                    .border_widths
                    .right
                    .map_or(px(border_width as f32), |width| {
                        width.to_pixels(window.rem_size())
                    });
                div()
                    .flex()
                    .items_center()
                    .justify_center()
                    .min_w_0()
                    .w(px(
                        (item_width - f64::from(f32::from(left_border + right_border))).max(0.)
                            as f32,
                    ))
                    .px(px(metrics.padding_inline as f32))
                    .child(sized_text(
                        page.to_string().into(),
                        &label_font,
                        f64::from(f32::from(label_size)),
                        text,
                        window,
                    ))
                    .into_any_element()
            }
            PaginationItemKind::Previous | PaginationItemKind::Next => {
                let custom = if item.kind == PaginationItemKind::Previous {
                    &self.prev_icon
                } else {
                    &self.next_icon
                };
                let content = if let Some(render) = custom {
                    render(window, cx)
                } else {
                    let left = (item.kind == PaginationItemKind::Previous) != rtl;
                    themed_icon(
                        if left {
                            IconName::LeftOutlined
                        } else {
                            IconName::RightOutlined
                        },
                        metrics.icon_size,
                        text,
                        tokens,
                        window,
                    )
                    .into_any_element()
                };
                div()
                    .flex()
                    .items_center()
                    .justify_center()
                    .w_full()
                    .h_full()
                    .min_w(px(item_width as f32))
                    .h(px(metrics.item_size as f32))
                    .border(px(if self.settings.mode == PaginationMode::Full {
                        border_width as f32
                    } else {
                        0.
                    }))
                    .border_color(border)
                    .rounded(px(metrics.radius as f32))
                    .bg(background)
                    .child(content)
                    .into_any_element()
            }
            PaginationItemKind::JumpPrevious | PaginationItemKind::JumpNext => {
                let custom = if item.kind == PaginationItemKind::JumpPrevious {
                    &self.jump_prev_icon
                } else {
                    &self.jump_next_icon
                };
                if let Some(render) = custom {
                    render(window, cx)
                } else {
                    let left = (item.kind == PaginationItemKind::JumpPrevious) != rtl;
                    div()
                        .relative()
                        .flex()
                        .items_center()
                        .justify_center()
                        .w(px(item_width as f32))
                        .h(px(metrics.item_size as f32))
                        .when(disabled && tokens.globals().wireframe, |d| {
                            d.bg(tokens.globals().color_bg_container_disabled)
                        })
                        .child(
                            div()
                                .opacity(jump_opacity as f32)
                                .text_color(tokens.globals().color_primary)
                                .child(themed_icon(
                                    if left {
                                        IconName::DoubleLeftOutlined
                                    } else {
                                        IconName::DoubleRightOutlined
                                    },
                                    metrics.icon_size,
                                    tokens.globals().color_primary.to_hsla(),
                                    tokens,
                                    window,
                                )),
                        )
                        .child(
                            div()
                                .absolute()
                                .inset_0()
                                .flex()
                                .items_center()
                                .justify_center()
                                .opacity((1. - jump_opacity) as f32)
                                .text_color(tokens.globals().color_text_disabled)
                                .child(themed_icon(
                                    IconName::EllipsisOutlined,
                                    metrics.ellipsis_size,
                                    tokens.globals().color_text_disabled.to_hsla(),
                                    tokens,
                                    window,
                                )),
                        )
                        .into_any_element()
                }
            }
        };
        let content = if let Some(render) = &self.item_render {
            let rendered = render(
                &PaginationItemRender {
                    kind: item.kind,
                    page: item.target,
                    selected,
                    disabled: unavailable,
                },
                original,
                window,
                cx,
            );
            match rendered {
                Some(content) => content,
                None => {
                    *cursor = previous_cursor;
                    self.state
                        .update(cx, |state, cx| state.suppress_item(item.kind, window, cx));
                    return None;
                }
            }
        } else {
            original
        };
        let id = item_id(item.kind);
        let mut element = div()
            .id(id.clone())
            .relative()
            .flex()
            .items_center()
            .justify_center()
            .flex_shrink_0()
            .min_w(px(item_width as f32))
            .h(px(metrics.item_size as f32))
            .line_height(px(metrics.line_height as f32))
            .border(px(if navigation { 0. } else { border_width as f32 }))
            .border_color(border)
            .rounded(px(metrics.radius as f32))
            .bg(if navigation {
                gpui::Hsla::transparent_black()
            } else {
                background
            })
            .text_color(text)
            .when(selected, |element| {
                element.font_weight(FontWeight(surface.font_weight))
            })
            .child(content)
            .when(!unavailable, |d| d.track_focus(&focus).cursor_pointer())
            .when(unavailable, |d| d.cursor_not_allowed());
        if item.kind != PaginationItemKind::Next {
            element = logical_end(element, item_gap, rtl);
        }
        if !jump {
            element.style().refine(&self.styles.item);
        }
        let corners = gpui::Corners {
            top_left: element.style().corner_radii.top_left.unwrap_or_default(),
            top_right: element.style().corner_radii.top_right.unwrap_or_default(),
            bottom_left: element.style().corner_radii.bottom_left.unwrap_or_default(),
            bottom_right: element
                .style()
                .corner_radii
                .bottom_right
                .unwrap_or_default(),
        }
        .to_pixels(window.rem_size());
        let weak = self.state.downgrade();
        let kind = item.kind;
        element = element.on_hover(move |hovered, _, cx| {
            let _ = weak.update(cx, |state, cx| {
                if let Some(item) = state.items.get_mut(&kind)
                    && item.hovered != *hovered
                {
                    item.hovered = *hovered;
                    cx.notify();
                }
            });
        });
        let weak = self.state.downgrade();
        element = element.on_mouse_down(MouseButton::Left, move |_, window, cx| {
            let _ = weak.update(cx, |state, cx| {
                if let Some(item) = state.items.get_mut(&kind)
                    && item.guard.availability().can_activate()
                {
                    item.pressed = true;
                    item.focus.focus(window, cx);
                    cx.notify();
                }
            });
        });
        for outside in [false, true] {
            let weak = self.state.downgrade();
            let reset = move |_: &gpui::MouseUpEvent, _: &mut Window, cx: &mut App| {
                let _ = weak.update(cx, |state, cx| {
                    if let Some(item) = state.items.get_mut(&kind)
                        && item.pressed
                    {
                        item.pressed = false;
                        cx.notify();
                    }
                });
            };
            element = if outside {
                element.on_mouse_up_out(MouseButton::Left, reset)
            } else {
                element.on_mouse_up(MouseButton::Left, reset)
            };
        }
        if self.show_title {
            let title = item_title(
                item.kind,
                &config.locale.pagination,
                self.settings.show_less_items,
            );
            let scope = scope.clone();
            element = element.tooltip(move |_, cx| {
                cx.new(|_| PaginationTooltip {
                    text: title.clone(),
                    scope: scope.clone(),
                })
                .into()
            });
        }
        let weak = self.state.downgrade();
        element = guard.install(element, move |activation, window, cx| {
            let source = if activation == Activation::Accessibility {
                PaginationChangeSource::Accessibility
            } else {
                match kind {
                    PaginationItemKind::Page(_) => PaginationChangeSource::Page,
                    PaginationItemKind::Previous => PaginationChangeSource::Previous,
                    PaginationItemKind::Next => PaginationChangeSource::Next,
                    PaginationItemKind::JumpPrevious => PaginationChangeSource::JumpPrevious,
                    PaginationItemKind::JumpNext => PaginationChangeSource::JumpNext,
                }
            };
            let _ = weak.update(cx, |state, cx| state.activate(kind, source, window, cx));
        });
        let label = match item.kind {
            PaginationItemKind::Page(page) => config
                .locale
                .pagination
                .page_label
                .replace("{page}", &page.to_string())
                .into(),
            _ => item_title(
                item.kind,
                &config.locale.pagination,
                self.settings.show_less_items,
            ),
        };
        let accessibility = Accessibility {
            disabled: unavailable,
            current: selected.then_some(accesskit::AriaCurrent::Page),
            ..Accessibility::new(Role::Button)
                .label(label)
                .automation_id(id)
        };
        Some(
            accessibility
                .apply(
                    FocusRing::new(
                        tokens.focus_color().to_hsla(),
                        px(tokens.focus_width() as f32),
                    )
                    .with_offset(px(tokens.focus_offset() as f32))
                    .radius(px(metrics.radius as f32))
                    .apply(element, &focus)
                    .corners(corners),
                )
                .into_any_element(),
        )
    }
}

fn logical_start<E: Styled>(element: E, margin: f64, rtl: bool) -> E {
    if rtl {
        element.mr(px(margin as f32))
    } else {
        element.ml(px(margin as f32))
    }
}

fn sized_text(
    text: SharedString,
    font: &gpui::Font,
    font_size: f64,
    color: gpui::Hsla,
    window: &Window,
) -> gpui::Div {
    let width = text_width(&text, font, font_size, color, window);
    div()
        .flex_shrink_0()
        .min_w_0()
        .w(width)
        .whitespace_nowrap()
        .child(text)
}

fn text_width(
    text: &SharedString,
    font: &gpui::Font,
    font_size: f64,
    color: gpui::Hsla,
    window: &Window,
) -> gpui::Pixels {
    let run = gpui::TextRun {
        len: text.len(),
        font: font.clone(),
        color,
        background_color: None,
        underline: None,
        strikethrough: None,
    };
    window
        .text_system()
        .shape_line(text.clone(), px(font_size as f32), &[run], None)
        .width
}
fn logical_end<E: Styled>(element: E, margin: f64, rtl: bool) -> E {
    if rtl {
        element.ml(px(margin as f32))
    } else {
        element.mr(px(margin as f32))
    }
}

pub(super) fn item_id(kind: PaginationItemKind) -> SharedString {
    match kind {
        PaginationItemKind::Page(page) => format!("pagination-page-{page}").into(),
        PaginationItemKind::Previous => "pagination-previous".into(),
        PaginationItemKind::Next => "pagination-next".into(),
        PaginationItemKind::JumpPrevious => "pagination-jump-previous".into(),
        PaginationItemKind::JumpNext => "pagination-jump-next".into(),
    }
}

fn item_title(
    kind: PaginationItemKind,
    messages: &crate::locale::PaginationMessages,
    less: bool,
) -> SharedString {
    match kind {
        PaginationItemKind::Page(page) => page.to_string().into(),
        PaginationItemKind::Previous => messages.prev_page.clone(),
        PaginationItemKind::Next => messages.next_page.clone(),
        PaginationItemKind::JumpPrevious => {
            if less {
                messages.prev_3.clone()
            } else {
                messages.prev_5.clone()
            }
        }
        PaginationItemKind::JumpNext => {
            if less {
                messages.next_3.clone()
            } else {
                messages.next_5.clone()
            }
        }
    }
}

fn themed_icon(
    name: IconName,
    dimension: f64,
    color: gpui::Hsla,
    tokens: &PaginationTokens,
    window: &Window,
) -> crate::icons::Icon {
    icon(name)
        .inherit_color()
        .render_options(IconRenderOptions {
            size: gpui::size(px(dimension as f32), px(dimension as f32)),
            scale_factor: window.scale_factor(),
            colors: IconPalette {
                text: color.into(),
                ..tokens.icon_palette()
            },
            ..Default::default()
        })
}

struct PaginationTooltip {
    text: SharedString,
    scope: ConfigScope,
}
struct ConfirmState {
    focus: gpui::FocusHandle,
    guard: crate::behavior::ActivationGuard,
    _blur: Subscription,
}

// Effective native UA colors recovered from the original painted control over
// five independently verified parent backgrounds, in both light and dark themes.
// Computed CSS alone omits the platform form-control compositing adjustment.
fn disabled_go_fill() -> gpui::Hsla {
    gpui::Rgba {
        r: 216.213_6 / 255.,
        g: 216.213_6 / 255.,
        b: 216.213_6 / 255.,
        a: 103. / 235.,
    }
    .into()
}
fn disabled_go_outline(resolved: bool) -> gpui::Hsla {
    if resolved {
        return gpui::Rgba {
            r: 118. / 255.,
            g: 118. / 255.,
            b: 118. / 255.,
            a: 0.3,
        }
        .into();
    }
    gpui::Rgba {
        r: 152.731_48 / 255.,
        g: 152.731_48 / 255.,
        b: 152.731_48 / 255.,
        a: 108. / 235.,
    }
    .into()
}

fn set_confirm_pressed(
    state: &WeakEntity<PaginationState>,
    pressed: bool,
    space: bool,
    cx: &mut App,
) {
    let _ = state.update(cx, |state, cx| {
        let pressed = pressed
            && state
                .confirm_guard
                .as_ref()
                .is_some_and(|guard| guard.availability().can_activate());
        if pressed && !space {
            state.confirm_hovered = true;
        }
        let current = if space {
            &mut state.confirm_space_pressed
        } else {
            &mut state.confirm_pointer_pressed
        };
        if *current != pressed {
            *current = pressed;
            cx.notify();
        }
    });
}
struct EditorTokens {
    pagination: Arc<PaginationTokens>,
    base: Arc<crate::input::InputTokens>,
    resolved: Arc<crate::input::InputTokens>,
    size: Size,
}
impl Render for PaginationTooltip {
    fn render(&mut self, _: &mut Window, cx: &mut Context<Self>) -> impl IntoElement {
        let config = self.scope.snapshot(cx);
        let g = &config.theme.tokens;
        div()
            .px(px(g.padding_xs as f32))
            .py(px(g.padding_xxs as f32))
            .rounded(px(g.border_radius as f32))
            .bg(g.color_bg_spotlight)
            .text_color(g.color_text_light_solid)
            .font(config.font.clone())
            .text_size(px(g.font_size as f32))
            .line_height(relative(g.line_height as f32))
            .child(self.text.clone())
    }
}
