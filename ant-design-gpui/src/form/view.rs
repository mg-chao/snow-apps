// Copyright (C) 2026 mg-chao
// SPDX-License-Identifier: Apache-2.0

//! Explicit native form composition. Fields and controls retain their own entities;
//! fresh builders describe presentation and never register or write store values.

use std::{
    cell::Cell,
    rc::Rc,
    sync::{Arc, LazyLock},
    time::Duration,
};

use gpui::{
    Animation, AnimationExt, AnyElement, App, AppContext, Bounds, Context, ElementId, Entity,
    IntoElement, Pixels, Refineable, Render, RenderOnce, Role, SharedString, StyleRefinement,
    Styled, Subscription, Window, div, prelude::*, px, relative,
};

use super::{
    FieldHandle, FormState,
    element::{
        ExtraAfterHelp, FormElement, FormScrollTarget, FractionalText, IntrinsicLabel,
        IntrinsicLabelMark, LayoutCallback,
    },
    motion::HelpMotion,
    style::{FormStyle, FormTheme},
};
use crate::{
    ConfigScope, Direction, Size, ValidationStatus, Variant,
    behavior::{Accessibility, MotionPolicy, text::normal_paragraph_text},
    icons::{IconName, icon},
    theme::AliasTokens,
    tooltip::Tooltip,
};

#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
pub enum FormLayout {
    #[default]
    Horizontal,
    Vertical,
    Inline,
}

#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
pub enum FormLabelAlign {
    Left,
    #[default]
    Right,
}

#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
pub enum FormRequiredMark {
    #[default]
    Required,
    Optional,
    Hidden,
}

/// A native 24-column specification, or a fixed/flexible label track.
#[derive(Clone, Copy, Debug, Default, PartialEq)]
pub struct FormColumnSize {
    pub span: Option<u8>,
    /// `None` inherits the previous breakpoint; explicit zero clears an offset.
    pub offset: Option<u8>,
    pub width: Option<f32>,
    pub flex: Option<f32>,
}
impl From<u8> for FormColumnSize {
    fn from(span: u8) -> Self {
        Self {
            span: Some(span.min(24)),
            ..Self::default()
        }
    }
}

/// Responsive tracks use the effective theme's viewport breakpoints, matching
/// Ant Design's Grid rather than a form's own available width.
#[derive(Clone, Debug, Default, PartialEq)]
pub struct FormColumn {
    pub base: FormColumnSize,
    pub xs: Option<FormColumnSize>,
    pub sm: Option<FormColumnSize>,
    pub md: Option<FormColumnSize>,
    pub lg: Option<FormColumnSize>,
    pub xl: Option<FormColumnSize>,
    pub xxl: Option<FormColumnSize>,
}
impl FormColumn {
    pub fn span(span: u8) -> Self {
        Self {
            base: span.into(),
            ..Self::default()
        }
    }
    pub fn width(width: f32) -> Self {
        Self {
            base: FormColumnSize {
                width: valid_dimension(width),
                ..Default::default()
            },
            ..Default::default()
        }
    }
    pub fn flex(grow: f32) -> Self {
        Self {
            base: FormColumnSize {
                flex: valid_dimension(grow),
                ..Default::default()
            },
            ..Default::default()
        }
    }
    pub fn offset(mut self, offset: u8) -> Self {
        self.base.offset = Some(offset.min(24));
        self
    }
    pub fn xs(mut self, size: impl Into<FormColumnSize>) -> Self {
        self.xs = Some(size.into());
        self
    }
    pub fn sm(mut self, size: impl Into<FormColumnSize>) -> Self {
        self.sm = Some(size.into());
        self
    }
    pub fn md(mut self, size: impl Into<FormColumnSize>) -> Self {
        self.md = Some(size.into());
        self
    }
    pub fn lg(mut self, size: impl Into<FormColumnSize>) -> Self {
        self.lg = Some(size.into());
        self
    }
    pub fn xl(mut self, size: impl Into<FormColumnSize>) -> Self {
        self.xl = Some(size.into());
        self
    }
    pub fn xxl(mut self, size: impl Into<FormColumnSize>) -> Self {
        self.xxl = Some(size.into());
        self
    }
    pub fn resolve(&self, viewport: f64, g: &AliasTokens) -> FormColumnSize {
        let mut result = self.base;
        for (active, candidate) in [
            (true, self.xs),
            (viewport >= g.screen_sm, self.sm),
            (viewport >= g.screen_md, self.md),
            (viewport >= g.screen_lg, self.lg),
            (viewport >= g.screen_xl, self.xl),
            (viewport >= g.screen_xxl, self.xxl),
        ] {
            if active && let Some(candidate) = candidate {
                result.span = candidate.span.or(result.span);
                result.offset = candidate.offset.or(result.offset);
                result.width = candidate.width.or(result.width);
                result.flex = candidate.flex.or(result.flex);
            }
        }
        result.span = result.span.map(|v| v.min(24));
        result.offset = result.offset.map(|offset| offset.min(24));
        result.width = result.width.and_then(valid_dimension);
        result.flex = result.flex.and_then(valid_dimension);
        result
    }
    fn infer_label_offset(&mut self, label: &Self) {
        fn inherit(target: &mut FormColumnSize, label: FormColumnSize) {
            if target.offset.is_none()
                && let Some(span) = label.span.filter(|span| *span < 24)
            {
                target.offset = Some(span);
            }
        }
        inherit(&mut self.base, label.base);
        for (target, label) in [
            (&mut self.xs, label.xs),
            (&mut self.sm, label.sm),
            (&mut self.md, label.md),
            (&mut self.lg, label.lg),
            (&mut self.xl, label.xl),
            (&mut self.xxl, label.xxl),
        ] {
            if let Some(label) = label.filter(|label| label.span.is_some_and(|span| span < 24)) {
                inherit(target.get_or_insert_default(), label);
            }
        }
    }
}
impl From<u8> for FormColumn {
    fn from(span: u8) -> Self {
        Self::span(span)
    }
}
fn valid_dimension(value: f32) -> Option<f32> {
    (value.is_finite() && value >= 0.).then_some(value)
}
fn bottom_margin(value: Option<gpui::Length>, fallback: f64, window: &Window) -> f32 {
    let margin = match value {
        Some(gpui::Length::Definite(value)) => {
            f32::from(value.to_pixels(window.viewport_size().width.into(), window.rem_size()))
        }
        Some(gpui::Length::Auto) => 0.,
        None => fallback as f32,
    };
    if margin.is_finite() {
        margin
    } else {
        fallback as f32
    }
}

#[derive(Clone, Debug, Default, PartialEq)]
pub struct FormStyles {
    pub root: StyleRefinement,
    pub item: StyleRefinement,
    pub label: StyleRefinement,
    /// Native label track layout; `label` styles the actual label content.
    pub label_column: StyleRefinement,
    /// Native control column layout; `content` styles its control content.
    pub control: StyleRefinement,
    pub content: StyleRefinement,
    pub input: StyleRefinement,
    pub extra: StyleRefinement,
    pub explain: StyleRefinement,
    pub feedback: StyleRefinement,
    pub required_mark: StyleRefinement,
}
impl FormStyles {
    pub(crate) fn refine(&mut self, other: &Self) {
        self.root.refine(&other.root);
        self.item.refine(&other.item);
        self.label.refine(&other.label);
        self.label_column.refine(&other.label_column);
        self.control.refine(&other.control);
        self.content.refine(&other.content);
        self.input.refine(&other.input);
        self.extra.refine(&other.extra);
        self.explain.refine(&other.explain);
        self.feedback.refine(&other.feedback);
        self.required_mark.refine(&other.required_mark);
    }
}
fn empty_styles() -> Arc<FormStyles> {
    static EMPTY: LazyLock<Arc<FormStyles>> = LazyLock::new(|| Arc::new(FormStyles::default()));
    EMPTY.clone()
}
fn only_text_style(style: &StyleRefinement) -> bool {
    let text = StyleRefinement {
        text: style.text.clone(),
        ..Default::default()
    };
    *style == text
}
pub type FormItemStyles = FormStyles;

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct FormStyleContext {
    pub layout: FormLayout,
    pub size: Size,
    pub direction: Direction,
    pub disabled: bool,
    pub variant: Variant,
    pub status: ValidationStatus,
    pub required: bool,
    pub has_feedback: bool,
    pub has_help: bool,
}

/// Passed explicitly to adapters, so controls inherit presentation without an
/// ambient render context or downcasts into arbitrary child elements.
#[derive(Clone)]
pub struct FormItemContext {
    pub id: ElementId,
    pub field: FieldHandle,
    pub scope: ConfigScope,
    pub size: Size,
    pub disabled: bool,
    pub variant: Variant,
    pub status: ValidationStatus,
    /// Actual field validity, independent of an overridden visual status.
    pub invalid: bool,
    /// Validation in progress remains editable while its result is pending.
    pub busy: bool,
    pub label: Option<SharedString>,
    pub description: Option<SharedString>,
    pub required: bool,
    pub has_feedback: bool,
    pub feedback_style: StyleRefinement,
    pub feedback: Option<FormFeedback>,
}
/// Repeatable custom native feedback without retaining frame elements.
#[derive(Clone)]
pub struct FormFeedback(FeedbackFn);
type FeedbackFn = Rc<dyn Fn(ValidationStatus, &mut Window, &mut App) -> AnyElement>;
#[cfg(any(test, feature = "test-support"))]
type LabelLayoutInspection = Rc<dyn Fn(Bounds<Pixels>)>;
impl FormFeedback {
    pub fn new(
        factory: impl Fn(ValidationStatus, &mut Window, &mut App) -> AnyElement + 'static,
    ) -> Self {
        Self(Rc::new(factory))
    }
}
impl FormItemContext {
    /// Feedback belongs inside the native control's suffix slot. Custom
    /// controls can call this helper to participate in the same presentation.
    pub fn feedback_element(&self, window: &mut Window, cx: &mut App) -> Option<AnyElement> {
        if !self.has_feedback || self.status == ValidationStatus::None {
            return None;
        }
        if let Some(feedback) = &self.feedback {
            let mut slot = div().id("custom-feedback").flex().items_center();
            slot.style().refine(&self.feedback_style);
            return Some(
                Accessibility {
                    hidden: true,
                    ..Default::default()
                }
                .apply(slot.child((feedback.0)(self.status, window, cx)))
                .into_any_element(),
            );
        }
        let config = self.scope.snapshot(cx);
        let tokens = config.theme.component::<FormTheme>();
        let style = FormStyle::from_tokens(&tokens, self.size);
        let (name, color) = match self.status {
            ValidationStatus::Success => (IconName::CheckCircleFilled, style.success_color),
            ValidationStatus::Warning => (IconName::ExclamationCircleFilled, style.warning_color),
            ValidationStatus::Error => (IconName::CloseCircleFilled, style.error_color),
            ValidationStatus::Validating => (IconName::LoadingOutlined, style.primary_color),
            ValidationStatus::None => return None,
        };
        let mut palette = config.icon_palette;
        palette.text = color.to_rgb();
        let mut glyph = icon(name)
            .palette(palette)
            .w(px(style.font_size as f32))
            .h(px(style.font_size as f32));
        glyph.style().refine(&self.feedback_style);
        let policy = MotionPolicy::new(
            config.motion && tokens.globals().motion && window.is_visible(),
            cx.reduce_motion(),
        );
        let glyph = if let Some(animation) = policy.animation(
            tokens.globals().motion_duration_mid,
            tokens.globals().motion_ease_out_back,
        ) {
            let font_size = style.font_size as f32;
            let glyph = glyph.with_animation(
                ("form-feedback-enter", self.status as usize),
                animation,
                move |glyph, progress| {
                    let glyph = glyph.opacity(progress.clamp(0., 1.));
                    if (progress - 1.).abs() < 0.001 {
                        glyph
                    } else {
                        glyph.paint_size(gpui::size(
                            px(font_size * progress.max(0.)),
                            px(font_size * progress.max(0.)),
                        ))
                    }
                },
            );
            if self.status == ValidationStatus::Validating {
                glyph
                    .with_animation(
                        "form-validating",
                        Animation::new(Duration::from_secs(1)).repeat(),
                        |glyph, progress| {
                            glyph.map_element(|glyph| {
                                glyph.rotation(gpui::radians(progress * std::f32::consts::TAU))
                            })
                        },
                    )
                    .into_any_element()
            } else {
                glyph.into_any_element()
            }
        } else {
            glyph.into_any_element()
        };
        Some(
            Accessibility {
                hidden: true,
                ..Default::default()
            }
            .apply(glyph)
            .into_any_element(),
        )
    }
}

type ContentFn = Rc<dyn Fn(&mut Window, &mut App) -> AnyElement>;
enum Content {
    Text(SharedString),
    Element(AnyElement),
    Factory(ContentFn),
}
struct HelpSnapshot {
    help: Option<Content>,
    errors: Vec<SharedString>,
    warnings: Vec<SharedString>,
    status: ValidationStatus,
}
impl Clone for HelpSnapshot {
    fn clone(&self) -> Self {
        Self {
            help: self.help.as_ref().and_then(Content::repeat),
            errors: self.errors.clone(),
            warnings: self.warnings.clone(),
            status: self.status,
        }
    }
}
impl PartialEq for HelpSnapshot {
    fn eq(&self, other: &Self) -> bool {
        let help = match (&self.help, &other.help) {
            (None, None) => true,
            (Some(Content::Text(a)), Some(Content::Text(b))) => a == b,
            (Some(Content::Factory(_)), Some(Content::Factory(_))) => true,
            _ => false,
        };
        help && self.errors == other.errors
            && self.warnings == other.warnings
            && self.status == other.status
    }
}
impl Content {
    fn text(text: impl Into<SharedString>) -> Self {
        Self::Text(text.into())
    }
    fn into_element(self, direction: Direction, window: &mut Window, cx: &mut App) -> AnyElement {
        match self {
            Self::Text(text) => normal_paragraph_text(text, direction),
            Self::Element(element) => element,
            Self::Factory(factory) => factory(window, cx),
        }
    }
    fn plain(&self) -> Option<SharedString> {
        match self {
            Self::Text(text) => Some(text.clone()),
            _ => None,
        }
    }
    fn is_empty(&self) -> bool {
        matches!(self, Self::Text(text) if text.is_empty())
    }
    fn repeat(&self) -> Option<Self> {
        match self {
            Self::Text(text) => Some(Self::Text(text.clone())),
            Self::Factory(factory) => Some(Self::Factory(factory.clone())),
            Self::Element(_) => None,
        }
    }
}
type StylesFn = Rc<dyn Fn(&FormStyleContext, &mut Window, &mut App) -> FormStyles>;
type ControlFn = Rc<dyn Fn(FormItemContext) -> AnyElement>;
type MarkFn = Rc<dyn Fn(bool, &mut Window, &mut App) -> AnyElement>;
enum FormChild {
    Item(Box<FormItem>),
    Element(AnyElement),
}

/// A native form surface with explicit retained store ownership.
#[derive(IntoElement)]
pub struct Form {
    id: ElementId,
    state: Entity<FormState>,
    scope: Option<ConfigScope>,
    children: Vec<FormChild>,
    layout: Option<FormLayout>,
    label_col: Option<FormColumn>,
    wrapper_col: Option<FormColumn>,
    label_align: Option<FormLabelAlign>,
    label_wrap: Option<bool>,
    colon: Option<bool>,
    required_mark: Option<FormRequiredMark>,
    mark: Option<MarkFn>,
    feedback: Option<FormFeedback>,
    size: Option<Size>,
    disabled: Option<bool>,
    variant: Option<Variant>,
    styles: Arc<FormStyles>,
    styles_fn: Option<StylesFn>,
    style: StyleRefinement,
    accessibility: Accessibility,
    scroll_handle: Option<gpui::ScrollHandle>,
}
impl Form {
    pub fn new(id: impl Into<ElementId>, state: &Entity<FormState>) -> Self {
        Self {
            id: id.into(),
            state: state.clone(),
            scope: None,
            children: Vec::new(),
            layout: None,
            label_col: None,
            wrapper_col: None,
            label_align: None,
            label_wrap: None,
            colon: None,
            required_mark: None,
            mark: None,
            feedback: None,
            size: None,
            disabled: None,
            variant: None,
            styles: empty_styles(),
            styles_fn: None,
            style: StyleRefinement::default(),
            accessibility: Accessibility::new(Role::Form),
            scroll_handle: None,
        }
    }
    pub fn state(&self) -> &Entity<FormState> {
        &self.state
    }
    /// Reveal fields inside an existing native scroll container. GPUI Lists
    /// consume automatic requests; a tracked Div needs its explicit owner.
    pub fn scroll_handle(mut self, handle: &gpui::ScrollHandle) -> Self {
        self.scroll_handle = Some(handle.clone());
        self
    }
    pub fn scope(mut self, scope: ConfigScope) -> Self {
        self.scope = Some(scope);
        self
    }
    pub fn item(mut self, item: FormItem) -> Self {
        self.children.push(FormChild::Item(Box::new(item)));
        self
    }
    pub fn items(mut self, items: impl IntoIterator<Item = FormItem>) -> Self {
        self.children.extend(
            items
                .into_iter()
                .map(|item| FormChild::Item(Box::new(item))),
        );
        self
    }
    pub fn child(mut self, child: impl IntoElement) -> Self {
        self.children
            .push(FormChild::Element(child.into_any_element()));
        self
    }
    pub fn layout(mut self, value: FormLayout) -> Self {
        self.layout = Some(value);
        self
    }
    pub fn label_col(mut self, value: impl Into<FormColumn>) -> Self {
        self.label_col = Some(value.into());
        self
    }
    pub fn wrapper_col(mut self, value: impl Into<FormColumn>) -> Self {
        self.wrapper_col = Some(value.into());
        self
    }
    pub fn label_align(mut self, value: FormLabelAlign) -> Self {
        self.label_align = Some(value);
        self
    }
    pub fn label_wrap(mut self, value: bool) -> Self {
        self.label_wrap = Some(value);
        self
    }
    pub fn colon(mut self, value: bool) -> Self {
        self.colon = Some(value);
        self
    }
    pub fn required_mark(mut self, value: FormRequiredMark) -> Self {
        self.required_mark = Some(value);
        self
    }
    pub fn required_mark_content(
        mut self,
        factory: impl Fn(bool, &mut Window, &mut App) -> AnyElement + 'static,
    ) -> Self {
        self.mark = Some(Rc::new(factory));
        self
    }
    pub fn feedback_content(mut self, feedback: FormFeedback) -> Self {
        self.feedback = Some(feedback);
        self
    }
    pub fn size(mut self, value: Size) -> Self {
        self.size = Some(value);
        self
    }
    pub fn disabled(mut self, value: bool) -> Self {
        self.disabled = Some(value);
        self
    }
    pub fn variant(mut self, value: Variant) -> Self {
        self.variant = Some(value);
        self
    }
    pub fn styles(mut self, value: FormStyles) -> Self {
        self.styles = Arc::new(value);
        self
    }
    pub fn styles_fn(
        mut self,
        resolver: impl Fn(&FormStyleContext, &mut Window, &mut App) -> FormStyles + 'static,
    ) -> Self {
        self.styles_fn = Some(Rc::new(resolver));
        self
    }
    pub fn accessibility_label(mut self, value: impl Into<SharedString>) -> Self {
        self.accessibility.label = Some(value.into());
        self
    }
    pub fn accessibility_description(mut self, value: impl Into<SharedString>) -> Self {
        self.accessibility.description = Some(value.into());
        self
    }
    pub fn automation_id(mut self, value: impl Into<SharedString>) -> Self {
        self.accessibility.automation_id = Some(value.into());
        self
    }
}
impl Styled for Form {
    fn style(&mut self) -> &mut StyleRefinement {
        &mut self.style
    }
}

#[derive(Clone)]
struct ItemParent {
    scope: ConfigScope,
    layout: FormLayout,
    label_col: Option<FormColumn>,
    wrapper_col: Option<FormColumn>,
    label_align: FormLabelAlign,
    label_wrap: bool,
    colon: bool,
    required_mark: FormRequiredMark,
    mark: Option<MarkFn>,
    feedback: Option<FormFeedback>,
    size: Size,
    disabled: bool,
    variant: Variant,
    styles: Arc<FormStyles>,
    last_item: bool,
    item_status: ValidationStatus,
    feedback_enabled: bool,
    item_description: Option<SharedString>,
    scroll: FormScrollTarget,
}

/// One label/control group. An unnamed item is useful for action rows and
/// compound layouts; a named item observes only its own retained field metadata.
#[derive(IntoElement)]
pub struct FormItem {
    id: ElementId,
    field: Option<FieldHandle>,
    scope: Option<ConfigScope>,
    parent: Option<ItemParent>,
    label: Option<Content>,
    label_placeholder: bool,
    content: Option<AnyElement>,
    content_factory: Option<ContentFn>,
    control: Option<ControlFn>,
    children: Vec<FormItem>,
    group_gap: f32,
    help: Option<Content>,
    extra: Option<Content>,
    tooltip: Option<SharedString>,
    mark: Option<MarkFn>,
    errors: Vec<SharedString>,
    warnings: Vec<SharedString>,
    required: Option<bool>,
    status: Option<ValidationStatus>,
    has_feedback: bool,
    feedback: Option<FormFeedback>,
    hidden: bool,
    no_style: bool,
    layout: Option<FormLayout>,
    label_col: Option<FormColumn>,
    wrapper_col: Option<FormColumn>,
    label_align: Option<FormLabelAlign>,
    label_wrap: Option<bool>,
    colon: Option<bool>,
    required_mark: Option<FormRequiredMark>,
    size: Option<Size>,
    disabled: Option<bool>,
    variant: Option<Variant>,
    styles: Arc<FormStyles>,
    styles_fn: Option<StylesFn>,
    style: StyleRefinement,
    accessibility: Accessibility,
    render_revision: u64,
    on_layout: Option<LayoutCallback>,
    #[cfg(any(test, feature = "test-support"))]
    label_layout_inspection: Option<LabelLayoutInspection>,
    cache_root: bool,
}
impl FormItem {
    fn repeatable(&self) -> bool {
        self.content.is_none()
            && [&self.label, &self.help, &self.extra]
                .into_iter()
                .all(|content| !matches!(content, Some(Content::Element(_))))
            && self.children.iter().all(FormItem::repeatable)
    }
    pub fn new(id: impl Into<ElementId>) -> Self {
        Self {
            id: id.into(),
            field: None,
            scope: None,
            parent: None,
            label: None,
            label_placeholder: false,
            content: None,
            content_factory: None,
            control: None,
            children: Vec::new(),
            group_gap: 8.,
            help: None,
            extra: None,
            tooltip: None,
            mark: None,
            errors: Vec::new(),
            warnings: Vec::new(),
            required: None,
            status: None,
            has_feedback: false,
            feedback: None,
            hidden: false,
            no_style: false,
            layout: None,
            label_col: None,
            wrapper_col: None,
            label_align: None,
            label_wrap: None,
            colon: None,
            required_mark: None,
            size: None,
            disabled: None,
            variant: None,
            styles: empty_styles(),
            styles_fn: None,
            style: StyleRefinement::default(),
            accessibility: Accessibility::new(Role::Group),
            render_revision: 0,
            on_layout: None,
            #[cfg(any(test, feature = "test-support"))]
            label_layout_inspection: None,
            cache_root: false,
        }
    }
    pub fn scope(mut self, value: ConfigScope) -> Self {
        self.scope = Some(value);
        self
    }
    /// Passively inspect the existing label element's native logical bounds.
    /// This diagnostic does not create a layout node or change rendering.
    #[doc(hidden)]
    #[cfg(any(test, feature = "test-support"))]
    pub fn inspect_label_layout(mut self, callback: impl Fn(Bounds<Pixels>) + 'static) -> Self {
        self.label_layout_inspection = Some(Rc::new(callback));
        self
    }
    pub fn field(mut self, field: FieldHandle) -> Self {
        self.field = Some(field);
        self
    }
    pub fn label(mut self, value: impl Into<SharedString>) -> Self {
        self.label = Some(Content::text(value));
        self.label_placeholder = false;
        self
    }
    pub fn label_content(mut self, value: impl IntoElement) -> Self {
        self.label = Some(Content::Element(value.into_any_element()));
        self.label_placeholder = false;
        self
    }
    /// Reserve the inherited label track for an action row without rendering
    /// a label. This corresponds to Ant Design's explicit `label={null}`.
    pub fn label_placeholder(mut self) -> Self {
        self.label = None;
        self.label_placeholder = true;
        self
    }
    pub fn label_content_with(
        mut self,
        factory: impl Fn(&mut Window, &mut App) -> AnyElement + 'static,
    ) -> Self {
        self.label = Some(Content::Factory(Rc::new(factory)));
        self
    }
    pub fn content(mut self, value: impl IntoElement) -> Self {
        self.children.clear();
        self.content = Some(value.into_any_element());
        self.content_factory = None;
        self.control = None;
        self
    }
    /// Rebuild custom content within this field's retained view. This keeps a
    /// field update from reconstructing neighboring items or their editors.
    pub fn content_with(
        mut self,
        factory: impl Fn(&mut Window, &mut App) -> AnyElement + 'static,
    ) -> Self {
        self.children.clear();
        self.content_factory = Some(Rc::new(factory));
        self.content = None;
        self.control = None;
        self
    }
    pub fn control_with(
        mut self,
        field: FieldHandle,
        factory: impl Fn(FormItemContext) -> AnyElement + 'static,
    ) -> Self {
        self.children.clear();
        self.field = Some(field);
        self.control = Some(Rc::new(factory));
        self.content = None;
        self.content_factory = None;
        self
    }
    /// Compose a compound field with explicit metadata ownership. No-style
    /// children contribute errors to this nearest styled parent.
    pub fn item(mut self, item: FormItem) -> Self {
        self.content = None;
        self.content_factory = None;
        self.control = None;
        self.children.push(item);
        self
    }
    pub fn items(mut self, items: impl IntoIterator<Item = FormItem>) -> Self {
        self.content = None;
        self.content_factory = None;
        self.control = None;
        self.children.extend(items);
        self
    }
    pub fn group_gap(mut self, gap: f32) -> Self {
        self.group_gap = valid_dimension(gap).unwrap_or(0.);
        self
    }
    pub fn help(mut self, value: impl Into<SharedString>) -> Self {
        self.help = Some(Content::text(value));
        self
    }
    pub fn help_content(mut self, value: impl IntoElement) -> Self {
        self.help = Some(Content::Element(value.into_any_element()));
        self
    }
    pub fn help_content_with(
        mut self,
        factory: impl Fn(&mut Window, &mut App) -> AnyElement + 'static,
    ) -> Self {
        self.help = Some(Content::Factory(Rc::new(factory)));
        self
    }
    pub fn extra(mut self, value: impl Into<SharedString>) -> Self {
        self.extra = Some(Content::text(value));
        self
    }
    pub fn extra_content(mut self, value: impl IntoElement) -> Self {
        self.extra = Some(Content::Element(value.into_any_element()));
        self
    }
    pub fn extra_content_with(
        mut self,
        factory: impl Fn(&mut Window, &mut App) -> AnyElement + 'static,
    ) -> Self {
        self.extra = Some(Content::Factory(Rc::new(factory)));
        self
    }
    pub fn tooltip(mut self, value: impl Into<SharedString>) -> Self {
        self.tooltip = Some(value.into());
        self
    }
    pub fn required_mark_content(
        mut self,
        factory: impl Fn(bool, &mut Window, &mut App) -> AnyElement + 'static,
    ) -> Self {
        self.mark = Some(Rc::new(factory));
        self
    }
    pub fn errors(mut self, value: impl IntoIterator<Item = impl Into<SharedString>>) -> Self {
        self.errors = value.into_iter().map(Into::into).collect();
        self
    }
    pub fn warnings(mut self, value: impl IntoIterator<Item = impl Into<SharedString>>) -> Self {
        self.warnings = value.into_iter().map(Into::into).collect();
        self
    }
    pub fn required(mut self, value: bool) -> Self {
        self.required = Some(value);
        self
    }
    pub fn validate_status(mut self, value: ValidationStatus) -> Self {
        self.status = Some(value);
        self
    }
    pub fn has_feedback(mut self, value: bool) -> Self {
        self.has_feedback = value;
        self
    }
    pub fn feedback_content(mut self, feedback: FormFeedback) -> Self {
        self.feedback = Some(feedback);
        self
    }
    pub fn hidden(mut self, value: bool) -> Self {
        self.hidden = value;
        self
    }
    pub fn no_style(mut self, value: bool) -> Self {
        self.no_style = value;
        self
    }
    pub fn layout(mut self, value: FormLayout) -> Self {
        self.layout = Some(value);
        self
    }
    pub fn label_col(mut self, value: impl Into<FormColumn>) -> Self {
        self.label_col = Some(value.into());
        self
    }
    pub fn wrapper_col(mut self, value: impl Into<FormColumn>) -> Self {
        self.wrapper_col = Some(value.into());
        self
    }
    pub fn label_align(mut self, value: FormLabelAlign) -> Self {
        self.label_align = Some(value);
        self
    }
    pub fn label_wrap(mut self, value: bool) -> Self {
        self.label_wrap = Some(value);
        self
    }
    pub fn colon(mut self, value: bool) -> Self {
        self.colon = Some(value);
        self
    }
    pub fn required_mark(mut self, value: FormRequiredMark) -> Self {
        self.required_mark = Some(value);
        self
    }
    pub fn size(mut self, value: Size) -> Self {
        self.size = Some(value);
        self
    }
    pub fn disabled(mut self, value: bool) -> Self {
        self.disabled = Some(value);
        self
    }
    pub fn variant(mut self, value: Variant) -> Self {
        self.variant = Some(value);
        self
    }
    pub fn styles(mut self, value: FormStyles) -> Self {
        self.styles = Arc::new(value);
        self
    }
    pub fn styles_fn(
        mut self,
        resolver: impl Fn(&FormStyleContext, &mut Window, &mut App) -> FormStyles + 'static,
    ) -> Self {
        self.styles_fn = Some(Rc::new(resolver));
        self
    }
    pub fn accessibility_label(mut self, value: impl Into<SharedString>) -> Self {
        self.accessibility.label = Some(value.into());
        self
    }
    pub fn accessibility_description(mut self, value: impl Into<SharedString>) -> Self {
        self.accessibility.description = Some(value.into());
        self
    }
    pub fn automation_id(mut self, value: impl Into<SharedString>) -> Self {
        self.accessibility.automation_id = Some(value.into());
        self
    }
    /// Invalidate captured scalar presentation when rebuilding a host view.
    /// Retained entities read inside the control factory invalidate natively.
    pub fn render_revision(mut self, revision: u64) -> Self {
        self.render_revision = revision;
        self
    }
}
impl Styled for FormItem {
    fn style(&mut self) -> &mut StyleRefinement {
        &mut self.style
    }
}

impl FormItem {
    fn no_style_fields(&self) -> Vec<FieldHandle> {
        fn collect(item: &FormItem, fields: &mut Vec<FieldHandle>) {
            if item.no_style {
                if let Some(field) = &item.field {
                    fields.push(field.clone());
                }
                for child in &item.children {
                    collect(child, fields);
                }
            }
        }
        let mut fields = Vec::new();
        for child in &self.children {
            collect(child, &mut fields);
        }
        let mut seen = std::collections::HashSet::new();
        fields.retain(|field| seen.insert(field.state().entity_id()));
        fields
    }
    fn is_hidden(&self, cx: &App) -> bool {
        self.hidden
            || self
                .field
                .as_ref()
                .is_some_and(|field| field.state().read(cx).options().hidden)
    }
    fn has_help(&self, cx: &App) -> bool {
        self.help.is_some()
            || (self.help.is_none()
                && (!self.errors.is_empty()
                    || !self.warnings.is_empty()
                    || self.field.as_ref().is_some_and(|field| {
                        let field = field.state().read(cx);
                        !field.meta().errors.is_empty() || !field.meta().warnings.is_empty()
                    })
                    || self.no_style_fields().iter().any(|field| {
                        let meta = field.meta(cx);
                        !meta.errors.is_empty() || !meta.warnings.is_empty()
                    })))
    }
    fn repeat(&self) -> Option<Self> {
        fn content(value: &Option<Content>) -> Option<Option<Content>> {
            match value {
                None => Some(None),
                Some(value) => value.repeat().map(Some),
            }
        }
        if self.content.is_some() {
            return None;
        }
        Some(Self {
            id: self.id.clone(),
            field: self.field.clone(),
            scope: self.scope.clone(),
            parent: self.parent.clone(),
            label: content(&self.label)?,
            label_placeholder: self.label_placeholder,
            content: None,
            content_factory: self.content_factory.clone(),
            control: self.control.clone(),
            children: self
                .children
                .iter()
                .map(FormItem::repeat)
                .collect::<Option<Vec<_>>>()?,
            group_gap: self.group_gap,
            help: content(&self.help)?,
            extra: content(&self.extra)?,
            tooltip: self.tooltip.clone(),
            mark: self.mark.clone(),
            errors: self.errors.clone(),
            warnings: self.warnings.clone(),
            required: self.required,
            status: self.status,
            has_feedback: self.has_feedback,
            feedback: self.feedback.clone(),
            hidden: self.hidden,
            no_style: self.no_style,
            layout: self.layout,
            label_col: self.label_col.clone(),
            wrapper_col: self.wrapper_col.clone(),
            label_align: self.label_align,
            label_wrap: self.label_wrap,
            colon: self.colon,
            required_mark: self.required_mark,
            size: self.size,
            disabled: self.disabled,
            variant: self.variant,
            styles: self.styles.clone(),
            styles_fn: self.styles_fn.clone(),
            style: self.style.clone(),
            accessibility: self.accessibility.clone(),
            render_revision: self.render_revision,
            on_layout: self.on_layout.clone(),
            #[cfg(any(test, feature = "test-support"))]
            label_layout_inspection: self.label_layout_inspection.clone(),
            cache_root: self.cache_root,
        })
    }
    fn presentation_eq(&self, other: &Self) -> bool {
        fn content(a: &Option<Content>, b: &Option<Content>) -> bool {
            match (a, b) {
                (None, None) => true,
                (Some(Content::Text(a)), Some(Content::Text(b))) => a == b,
                (Some(Content::Factory(_)), Some(Content::Factory(_))) => true,
                _ => false,
            }
        }
        fn parent(a: &Option<ItemParent>, b: &Option<ItemParent>) -> bool {
            match (a, b) {
                (None, None) => true,
                (Some(a), Some(b)) => {
                    a.scope == b.scope
                        && a.layout == b.layout
                        && a.label_col == b.label_col
                        && a.wrapper_col == b.wrapper_col
                        && a.label_align == b.label_align
                        && a.label_wrap == b.label_wrap
                        && a.colon == b.colon
                        && a.required_mark == b.required_mark
                        && a.size == b.size
                        && a.disabled == b.disabled
                        && a.variant == b.variant
                        && a.styles == b.styles
                        && a.last_item == b.last_item
                        && a.item_status == b.item_status
                        && a.feedback_enabled == b.feedback_enabled
                        && a.item_description == b.item_description
                        && a.mark.is_some() == b.mark.is_some()
                        && a.feedback.is_some() == b.feedback.is_some()
                        && a.scroll.same_source(&b.scroll)
                }
                _ => false,
            }
        }
        let a = &self.accessibility;
        let b = &other.accessibility;
        self.id == other.id
            && self.field.as_ref().map(|f| f.state().entity_id())
                == other.field.as_ref().map(|f| f.state().entity_id())
            && self.scope == other.scope
            && parent(&self.parent, &other.parent)
            && content(&self.label, &other.label)
            && content(&self.help, &other.help)
            && content(&self.extra, &other.extra)
            && self.label_placeholder == other.label_placeholder
            && self.content_factory.is_some() == other.content_factory.is_some()
            && self.control.is_some() == other.control.is_some()
            && self.group_gap == other.group_gap
            && self.children.len() == other.children.len()
            && self
                .children
                .iter()
                .zip(&other.children)
                .all(|(a, b)| a.presentation_eq(b))
            && self.tooltip == other.tooltip
            && self.mark.is_some() == other.mark.is_some()
            && self.feedback.is_some() == other.feedback.is_some()
            && self.errors == other.errors
            && self.warnings == other.warnings
            && self.required == other.required
            && self.status == other.status
            && self.has_feedback == other.has_feedback
            && self.hidden == other.hidden
            && self.no_style == other.no_style
            && self.layout == other.layout
            && self.label_col == other.label_col
            && self.wrapper_col == other.wrapper_col
            && self.label_align == other.label_align
            && self.label_wrap == other.label_wrap
            && self.colon == other.colon
            && self.required_mark == other.required_mark
            && self.size == other.size
            && self.disabled == other.disabled
            && self.variant == other.variant
            && self.styles == other.styles
            && self.style == other.style
            && self.render_revision == other.render_revision
            && self.styles_fn.is_some() == other.styles_fn.is_some()
            && a.role == b.role
            && a.label == b.label
            && a.description == b.description
            && a.automation_id == b.automation_id
            && a.disabled == b.disabled
            && a.busy == b.busy
            && a.modal == b.modal
            && a.invalid == b.invalid
            && a.required == b.required
            && a.hidden == b.hidden
            && a.read_only == b.read_only
            && a.current == b.current
    }
}

#[derive(Default)]
struct ItemMeasurement {
    bounds: Cell<Option<Bounds<Pixels>>>,
    frame_pending: Cell<bool>,
}
struct RetainedItem {
    item: FormItem,
    measured: Rc<ItemMeasurement>,
    repair: gpui::WeakEntity<ItemSlot>,
    viewport: Pixels,
    cached: bool,
}
impl Render for RetainedItem {
    fn render(&mut self, window: &mut Window, cx: &mut Context<Self>) -> impl IntoElement {
        let mut item = self
            .item
            .repeat()
            .expect("retained form item uses repeatable content");
        item.cache_root = self.cached;
        let measured = self.measured.clone();
        let repair = self.repair.clone();
        item.on_layout = Some(Rc::new(move |bounds, window, _| {
            let previous = measured.bounds.replace(Some(bounds));
            if previous
                .is_none_or(|old| (f32::from(old.size.height - bounds.size.height)).abs() > 0.01)
                && !measured.frame_pending.replace(true)
            {
                let measured = measured.clone();
                let repair = repair.clone();
                window.on_next_frame(move |_, cx| {
                    measured.frame_pending.set(false);
                    let _ = repair.update(cx, |_, cx| cx.notify());
                });
            }
        }));
        item.render_inline(window, cx)
    }
}
// `use_keyed_state` observes its state into the current view. Store the child
// view in a separate, non-notifying holder: field notifications then invalidate
// the child view only, rather than bubbling through that implicit observation.
struct ItemSlot {
    view: Entity<RetainedItem>,
    measured: Rc<ItemMeasurement>,
}

struct FormMount {
    form: Entity<FormState>,
    scroll: FormScrollTarget,
    _release: Subscription,
}

struct SubfieldObserver {
    fields: Vec<gpui::EntityId>,
    subscriptions: Vec<Subscription>,
}
fn observe_subfields(id: &ElementId, fields: &[FieldHandle], window: &mut Window, cx: &mut App) {
    let identities = fields
        .iter()
        .map(|field| field.state().entity_id())
        .collect::<Vec<_>>();
    let observer = window.use_keyed_state(
        (id.clone(), "form-subfields"),
        cx,
        |_, cx: &mut Context<SubfieldObserver>| SubfieldObserver {
            fields: identities.clone(),
            subscriptions: fields
                .iter()
                .map(|field| cx.observe(field.state(), |_, _, cx| cx.notify()))
                .collect(),
        },
    );
    observer.update(cx, |observer, cx| {
        if observer.fields != identities {
            observer.subscriptions = fields
                .iter()
                .map(|field| cx.observe(field.state(), |_, _, cx| cx.notify()))
                .collect();
            observer.fields = identities;
        }
    });
}

struct Observer {
    scope: ConfigScope,
    field: Option<FieldHandle>,
    mounted_field: bool,
    _scope: Subscription,
    _field: Option<Subscription>,
    _release: Subscription,
}
fn observe(
    id: &ElementId,
    scope: &ConfigScope,
    field: Option<&FieldHandle>,
    mount: bool,
    window: &mut Window,
    cx: &mut App,
) {
    let mounted = window.use_keyed_state(
        (id.clone(), "form-observer"),
        cx,
        |_, cx: &mut Context<Observer>| {
            if mount && let Some(field) = field {
                field.mount(cx);
            }
            Observer {
                scope: scope.clone(),
                field: field.cloned(),
                mounted_field: mount,
                _scope: scope.observe(cx),
                _field: field.map(|field| cx.observe(field.state(), |_, _, cx| cx.notify())),
                _release: cx.on_release(|state: &mut Observer, cx| {
                    if state.mounted_field
                        && let Some(field) = &state.field
                    {
                        field.unmount(cx);
                    }
                }),
            }
        },
    );
    mounted.update(cx, |state, cx| {
        if state.scope != *scope {
            state._scope = scope.observe(cx);
            state.scope = scope.clone();
        }
        if state.field.as_ref().map(|field| field.state().entity_id())
            != field.map(|field| field.state().entity_id())
        {
            state._field = None;
            if state.mounted_field
                && let Some(previous) = &state.field
            {
                previous.unmount(cx);
            }
            if mount && let Some(field) = field {
                field.mount(cx);
            }
            state._field = field.map(|field| cx.observe(field.state(), |_, _, cx| cx.notify()));
            state.field = field.cloned();
            state.mounted_field = mount;
        }
    });
}

impl RenderOnce for Form {
    fn render(mut self, window: &mut Window, cx: &mut App) -> impl IntoElement {
        let scope = self
            .scope
            .take()
            .unwrap_or_else(|| ConfigScope::application(cx));
        self.state
            .update(cx, |form, cx| form.set_scope(scope.clone(), cx));
        let mount = window.use_keyed_state(
            (self.id.clone(), "form-mount"),
            cx,
            |_, cx: &mut Context<FormMount>| {
                self.state.update(cx, |form, _| form.connect());
                FormMount {
                    form: self.state.clone(),
                    scroll: FormScrollTarget::new(),
                    _release: cx.on_release(|state: &mut FormMount, cx| {
                        state.form.update(cx, |form, cx| form.disconnect(cx))
                    }),
                }
            },
        );
        mount.update(cx, |state, cx| {
            state.scroll.set(self.scroll_handle.clone());
            if state.form != self.state {
                state.form.update(cx, |form, cx| form.disconnect(cx));
                self.state.update(cx, |form, _| form.connect());
                state.form = self.state.clone();
            }
        });
        observe(&self.id, &scope, None, false, window, cx);
        let config = scope.snapshot(cx);
        let defaults = &config.form;
        let layout = self.layout.unwrap_or(defaults.layout);
        let size = self.size.unwrap_or(config.size);
        let disabled = self.disabled.unwrap_or(config.disabled);
        let variant = self.variant.unwrap_or(config.variant);
        let context = FormStyleContext {
            layout,
            size,
            direction: config.direction,
            disabled,
            variant,
            status: ValidationStatus::None,
            required: false,
            has_feedback: false,
            has_help: false,
        };
        let mut styles = Box::new(defaults.styles.clone());
        if let Some(resolver) = &defaults.styles_fn {
            styles.refine(&resolver.resolve(&context, window, cx));
        }
        styles.refine(&self.styles);
        if let Some(resolver) = &self.styles_fn {
            styles.refine(&resolver(&context, window, cx));
        }
        styles.root.refine(&self.style);
        let parent = ItemParent {
            scope: scope.clone(),
            layout,
            label_col: self.label_col.or_else(|| defaults.label_col.clone()),
            wrapper_col: self.wrapper_col.or_else(|| defaults.wrapper_col.clone()),
            label_align: self.label_align.unwrap_or(defaults.label_align),
            label_wrap: self.label_wrap.unwrap_or(defaults.label_wrap),
            colon: self.colon.unwrap_or(defaults.colon),
            required_mark: self.required_mark.unwrap_or(defaults.required_mark),
            mark: self.mark.clone(),
            feedback: self.feedback.clone(),
            size,
            disabled,
            variant,
            styles: Arc::from(styles.clone()),
            last_item: false,
            item_status: ValidationStatus::None,
            feedback_enabled: false,
            item_description: None,
            scroll: mount.read(cx).scroll.clone(),
        };
        let tokens = config.theme.component::<FormTheme>();
        let last = self
            .children
            .iter()
            .enumerate()
            .rfind(|(_, child)| !matches!(child, FormChild::Item(item) if item.is_hidden(cx)))
            .map(|(index, _)| index);
        let external_margin = if layout != FormLayout::Inline {
            last.and_then(|index| match &self.children[index] {
                FormChild::Item(item) if !item.no_style && !item.has_help(cx) => {
                    Some(f64::from(bottom_margin(
                        item.style
                            .margin
                            .bottom
                            .or(item.styles.item.margin.bottom)
                            .or(styles.item.margin.bottom),
                        FormStyle::from_tokens(&tokens, item.size.unwrap_or(size))
                            .item_margin_bottom,
                        window,
                    )))
                }
                _ => None,
            })
            .unwrap_or(0.)
        } else {
            0.
        };
        let mut root = div()
            .id(self.id)
            .flex()
            .min_w_0()
            .font(tokens.font().clone())
            .text_size(px(tokens.globals().font_size as f32))
            .line_height(relative(tokens.globals().line_height as f32))
            .text_color(tokens.globals().color_text.to_hsla())
            .mb(px(external_margin as f32))
            .when(layout != FormLayout::Inline, |root| root.flex_col())
            .when(layout == FormLayout::Inline, |root| root.flex_wrap())
            .when(
                layout == FormLayout::Inline && config.direction == Direction::Rtl,
                |root| root.flex_row_reverse(),
            );
        root.style().refine(&styles.root);
        for (index, child) in self.children.into_iter().enumerate() {
            root = match child {
                FormChild::Element(child) => root.child(child),
                FormChild::Item(mut item) => {
                    let mut item_parent = parent.clone();
                    item_parent.last_item = layout != FormLayout::Inline && last == Some(index);
                    item.parent = Some(item_parent);
                    root.child(*item)
                }
            };
        }
        self.accessibility.apply(root)
    }
}

fn apply_column(
    mut element: gpui::Stateful<gpui::Div>,
    column: FormColumnSize,
    direction: Direction,
) -> gpui::Stateful<gpui::Div> {
    if let Some(width) = column.width {
        element = element
            .w(px(width))
            .flex_basis(px(width))
            .flex_grow(0.)
            .flex_shrink_0();
    } else if let Some(span) = column.span {
        element = element
            .w(relative(f32::from(span) / 24.))
            .flex_basis(relative(f32::from(span) / 24.))
            .flex_shrink_0()
            .flex_grow(0.);
    } else if let Some(grow) = column.flex {
        element = element
            .flex_grow(grow)
            .flex_shrink(1.)
            .flex_basis(gpui::auto());
    }
    if let Some(offset) = column.offset.filter(|offset| *offset > 0) {
        let offset = relative(f32::from(offset) / 24.);
        element = match direction {
            Direction::Ltr => element.ml(offset),
            Direction::Rtl => element.mr(offset),
        };
    }
    if let Some(span) = column.span {
        element = element.max_w(relative(f32::from(span) / 24.));
    }
    element
}

impl RenderOnce for FormItem {
    fn render(self, window: &mut Window, cx: &mut App) -> impl IntoElement {
        if (self.field.is_some() || !self.no_style_fields().is_empty()) && self.repeatable() {
            let mut created = false;
            let viewport = window.viewport_size().width;
            // The pinned GPUI scene replay omits accessibility subtrees. Active
            // assistive technology needs each native field emitted this frame.
            let cacheable = !window.is_a11y_active()
                && !self.no_style
                && !self.hidden
                && self.styles_fn.is_none()
                && !self
                    .field
                    .as_ref()
                    .is_some_and(|field| field.state().read(cx).options().hidden)
                && self.layout.or(self.parent.as_ref().map(|p| p.layout))
                    != Some(FormLayout::Inline);
            let mut cache_style = StyleRefinement::default()
                .w_full()
                .min_w_0()
                .flex_shrink_0();
            for style in self
                .parent
                .as_ref()
                .map(|parent| &parent.styles.item)
                .into_iter()
                .chain([&self.styles.item, &self.style])
            {
                if style.size.width.is_some() {
                    cache_style.size.width = style.size.width;
                }
                if style.min_size.width.is_some() {
                    cache_style.min_size.width = style.min_size.width;
                }
                if style.max_size.width.is_some() {
                    cache_style.max_size.width = style.max_size.width;
                }
                if style.flex_basis.is_some() {
                    cache_style.flex_basis = style.flex_basis;
                }
                if style.flex_grow.is_some() {
                    cache_style.flex_grow = style.flex_grow;
                }
                if style.flex_shrink.is_some() {
                    cache_style.flex_shrink = style.flex_shrink;
                }
                if style.align_self.is_some() {
                    cache_style.align_self = style.align_self;
                }
            }
            let holder =
                window.use_keyed_state((self.id.clone(), "form-item-view"), cx, |_, cx| {
                    created = true;
                    let measured = Rc::new(ItemMeasurement::default());
                    let repair = cx.weak_entity();
                    let view = cx.new(|_| RetainedItem {
                        item: self
                            .repeat()
                            .expect("retained form item uses repeatable content"),
                        measured: measured.clone(),
                        repair,
                        viewport,
                        cached: false,
                    });
                    ItemSlot { view, measured }
                });
            let view = holder.read(cx).view.clone();
            let measured = holder.read(cx).measured.bounds.get();
            let cached = cacheable && measured.is_some_and(|bounds| bounds.size.width > px(0.));
            if !created {
                view.update(cx, |state, cx| {
                    let changed = !state.item.presentation_eq(&self)
                        || state.viewport != viewport
                        || state.cached != cached;
                    state.item = self;
                    state.viewport = viewport;
                    state.cached = cached;
                    if changed {
                        cx.notify();
                    }
                });
            }
            if cacheable && let Some(bounds) = measured.filter(|bounds| bounds.size.width > px(0.))
            {
                view.cached(cache_style.h(bounds.size.height))
                    .into_any_element()
            } else {
                view.into_any_element()
            }
        } else {
            self.render_inline(window, cx)
        }
    }
}

impl FormItem {
    fn render_inline(mut self, window: &mut Window, cx: &mut App) -> AnyElement {
        let scope = self
            .scope
            .take()
            .or_else(|| self.parent.as_ref().map(|p| p.scope.clone()))
            .unwrap_or_else(|| ConfigScope::application(cx));
        observe(&self.id, &scope, self.field.as_ref(), true, window, cx);
        let subfields = self.no_style_fields();
        observe_subfields(&self.id, &subfields, window, cx);
        let config = scope.snapshot(cx);
        let defaults = &config.form;
        let parent = self.parent.take().unwrap_or_else(|| ItemParent {
            scope: scope.clone(),
            layout: defaults.layout,
            label_col: defaults.label_col.clone(),
            wrapper_col: defaults.wrapper_col.clone(),
            label_align: defaults.label_align,
            label_wrap: defaults.label_wrap,
            colon: defaults.colon,
            required_mark: defaults.required_mark,
            mark: None,
            feedback: None,
            size: config.size,
            disabled: config.disabled,
            variant: config.variant,
            styles: Arc::new(defaults.styles.clone()),
            last_item: false,
            item_status: ValidationStatus::None,
            feedback_enabled: false,
            item_description: None,
            scroll: FormScrollTarget::default(),
        });
        let layout = self.layout.unwrap_or(parent.layout);
        let collapsed_last = parent.last_item;
        let size = self.size.unwrap_or(parent.size);
        let disabled = self.disabled.unwrap_or(parent.disabled);
        let variant = self.variant.unwrap_or(parent.variant);
        let has_feedback = self.has_feedback || (self.no_style && parent.feedback_enabled);
        let direction = config.direction;
        let tokens = config.theme.component::<FormTheme>();
        let metrics = FormStyle::from_tokens(&tokens, size);
        if let Some(field) = &self.field {
            field.set_presentation_label(
                self.label
                    .as_ref()
                    .and_then(Content::plain)
                    .or_else(|| self.accessibility.label.clone()),
                cx,
            );
        }
        let meta = self.field.as_ref().map(|field| field.meta(cx));
        let required = self
            .required
            .unwrap_or_else(|| meta.as_ref().is_some_and(|meta| meta.required));
        let mut errors = meta
            .as_ref()
            .map_or_else(Vec::new, |meta| meta.errors.clone());
        let mut warnings = meta
            .as_ref()
            .map_or_else(Vec::new, |meta| meta.warnings.clone());
        errors.extend(self.errors);
        warnings.extend(self.warnings);
        for field in &subfields {
            let meta = field.meta(cx);
            errors.extend(meta.errors);
            warnings.extend(meta.warnings);
        }
        let status = self.status.unwrap_or_else(|| {
            if meta.as_ref().is_some_and(|m| m.validating) {
                ValidationStatus::Validating
            } else if !errors.is_empty() {
                ValidationStatus::Error
            } else if !warnings.is_empty() {
                ValidationStatus::Warning
            } else if meta
                .as_ref()
                .is_some_and(|m| m.touched || (has_feedback && m.validated))
            {
                ValidationStatus::Success
            } else {
                ValidationStatus::None
            }
        });
        let status = if self.no_style && status == ValidationStatus::None {
            parent.item_status
        } else {
            status
        };
        let invalid = !errors.is_empty() || status == ValidationStatus::Error;
        let busy = meta.as_ref().is_some_and(|meta| meta.validating)
            || status == ValidationStatus::Validating;
        let has_help = self.help.is_some()
            || (self.help.is_none() && (!errors.is_empty() || !warnings.is_empty()));
        let repeatable_help = !self.no_style
            && self
                .help
                .as_ref()
                .is_none_or(|help| !matches!(help, Content::Element(_)));
        let snapshot = (has_help && repeatable_help).then(|| HelpSnapshot {
            help: self.help.as_ref().and_then(Content::repeat),
            errors: errors.clone(),
            warnings: warnings.clone(),
            status,
        });
        let help_motion =
            window.use_keyed_state((self.id.clone(), "form-help-motion"), cx, |_, cx| {
                HelpMotion::new(snapshot.clone(), tokens.globals().motion_ease_in_out, cx)
            });
        let mut help_sample = help_motion.update(cx, |state, cx| {
            state.sample(
                snapshot,
                MotionPolicy::new(
                    repeatable_help
                        && config.motion
                        && tokens.globals().motion
                        && window.is_visible(),
                    cx.reduce_motion(),
                ),
                tokens.globals().motion_duration_fast,
                window,
                cx,
            )
        });
        if !repeatable_help {
            help_sample.opacity = 1.;
            help_sample.height = None;
            help_sample.leaving = false;
        }
        let help_geometry = help_motion.downgrade();
        let visible_help = if repeatable_help {
            help_sample.content.is_some()
        } else {
            has_help
        };
        let context = FormStyleContext {
            layout,
            size,
            direction,
            disabled,
            variant,
            status,
            required,
            has_feedback,
            has_help,
        };
        let styles: Arc<FormStyles> = if defaults.styles_fn.is_none()
            && self.styles_fn.is_none()
            && Arc::ptr_eq(&self.styles, &empty_styles())
            && self.style == StyleRefinement::default()
        {
            parent.styles.clone()
        } else {
            let mut styles = Box::new(parent.styles.as_ref().clone());
            if let Some(resolver) = &defaults.styles_fn {
                styles.refine(&resolver.resolve(&context, window, cx));
            }
            styles.refine(&self.styles);
            if let Some(resolver) = &self.styles_fn {
                styles.refine(&resolver(&context, window, cx));
            }
            styles.item.refine(&self.style);
            Arc::from(styles)
        };
        let item_margin = bottom_margin(
            styles.item.margin.bottom,
            if layout == FormLayout::Inline {
                metrics.inline_item_margin_bottom
            } else {
                metrics.item_margin_bottom
            },
            window,
        );
        let label = self
            .accessibility
            .label
            .clone()
            .or_else(|| self.label.as_ref().and_then(Content::plain))
            .or_else(|| {
                self.field.as_ref().and_then(|field| {
                    let label = &field.state().read(cx).options().label;
                    (!label.is_empty()).then(|| label.clone())
                })
            });
        let help_description = self.help.as_ref().and_then(Content::plain);
        let extra_description = self.extra.as_ref().and_then(Content::plain);
        let description = self
            .accessibility
            .description
            .clone()
            .or_else(|| {
                let values = help_description
                    .into_iter()
                    .chain(if self.help.is_none() {
                        errors
                            .iter()
                            .cloned()
                            .chain(warnings.iter().cloned())
                            .collect::<Vec<_>>()
                    } else {
                        Vec::new()
                    })
                    .chain(extra_description)
                    .filter(|v| !v.is_empty())
                    .collect::<Vec<_>>();
                (!values.is_empty()).then(|| {
                    SharedString::from(
                        values
                            .iter()
                            .map(SharedString::as_ref)
                            .collect::<Vec<_>>()
                            .join(" "),
                    )
                })
            })
            .or_else(|| {
                self.no_style
                    .then(|| parent.item_description.clone())
                    .flatten()
            });
        let child = if let (Some(control), Some(field)) = (self.control.take(), self.field.as_ref())
        {
            control(FormItemContext {
                id: self.id.clone(),
                field: field.clone(),
                scope: scope.clone(),
                size,
                disabled,
                variant,
                status,
                invalid,
                busy,
                label: label.clone(),
                description: description.clone(),
                required,
                has_feedback,
                feedback_style: styles.feedback.clone(),
                feedback: self.feedback.clone().or_else(|| parent.feedback.clone()),
            })
        } else if !self.children.is_empty() {
            let mut group = div()
                .id("compound")
                .flex()
                .items_start()
                .gap(px(self.group_gap))
                .when(direction == Direction::Rtl, |group| {
                    group.flex_row_reverse()
                });
            for mut child in std::mem::take(&mut self.children) {
                child.parent = Some(ItemParent {
                    label_col: None,
                    wrapper_col: None,
                    last_item: false,
                    item_status: status,
                    feedback_enabled: has_feedback,
                    item_description: description.clone(),
                    size,
                    disabled,
                    variant,
                    ..parent.clone()
                });
                group = group.child(child);
            }
            group.into_any_element()
        } else if let Some(factory) = self.content_factory.take() {
            factory(window, cx)
        } else {
            self.content
                .take()
                .unwrap_or_else(|| div().into_any_element())
        };
        let hidden = self.hidden
            || self
                .field
                .as_ref()
                .is_some_and(|field| field.state().read(cx).options().hidden);
        if hidden {
            return div().id(self.id).hidden().into_any_element();
        }
        if self.no_style {
            return FormElement {
                element: child,
                field: self.field,
                required: false,
                live: false,
                on_layout: None,
                scroll: parent.scroll.clone(),
            }
            .into_any_element();
        }

        let viewport = f64::from(f32::from(window.viewport_size().width));
        let label_col_spec = self.label_col.as_ref().or(parent.label_col.as_ref());
        let mut wrapper_spec = self
            .wrapper_col
            .clone()
            .or_else(|| parent.wrapper_col.clone());
        if self.label_placeholder
            && self.label_col.is_none()
            && self.wrapper_col.is_none()
            && let Some(label) = label_col_spec
        {
            wrapper_spec
                .get_or_insert_default()
                .infer_label_offset(label);
        }
        let wrapper_col_spec = wrapper_spec.as_ref();
        let mut label_col = label_col_spec.map_or_else(FormColumnSize::default, |c| {
            c.resolve(viewport, tokens.globals())
        });
        let mut wrapper_col = wrapper_col_spec.map_or_else(FormColumnSize::default, |c| {
            c.resolve(viewport, tokens.globals())
        });
        let narrow = viewport <= tokens.globals().screen_xs_max && layout == FormLayout::Horizontal;
        if narrow {
            if label_col_spec.is_none_or(|c| c.xs.is_none()) {
                label_col.span = Some(24);
            }
            if wrapper_col_spec.is_none_or(|c| c.xs.is_none()) {
                wrapper_col.span = Some(24);
            }
        }
        let has_label = self.label.as_ref().is_some_and(|label| !label.is_empty());
        let explicit_vertical = layout == FormLayout::Vertical;
        let vertical =
            explicit_vertical || (layout == FormLayout::Horizontal && label_col.span == Some(24));
        let label_wrap = self.label_wrap.unwrap_or(parent.label_wrap);
        let label_align = self.label_align.unwrap_or(parent.label_align);
        let align_right = (label_align == FormLabelAlign::Right) != (direction == Direction::Rtl);
        let colon = self.colon.unwrap_or(parent.colon) && !explicit_vertical;
        let required_mark = self.required_mark.unwrap_or(parent.required_mark);
        let has_required_mark = required && required_mark == FormRequiredMark::Required;
        let literal_label = (!label_wrap
            && self.mark.is_none()
            && parent.mark.is_none()
            && (!has_required_mark || only_text_style(&styles.required_mark))
            && self.tooltip.as_ref().is_none_or(|text| text.is_empty()))
        .then(|| self.label.as_ref().and_then(Content::plain))
        .flatten()
        .map(|text| {
            if colon {
                let trimmed = text.trim_end();
                trimmed
                    .strip_suffix(':')
                    .or_else(|| trimmed.strip_suffix('：'))
                    .map_or(text.clone(), |text| text.to_owned().into())
            } else {
                text
            }
        });
        let mut root = div()
            .id(self.id.clone())
            .flex()
            .flex_col()
            .when(layout != FormLayout::Inline, |root| root.w_full())
            .min_w_0()
            .font(tokens.font().clone())
            .text_size(px(metrics.font_size as f32))
            .line_height(relative(metrics.line_height as f32))
            .text_color(metrics.text_color)
            .mb(px(if help_sample.leaving && collapsed_last {
                -metrics.item_margin_bottom
            } else if visible_help || collapsed_last {
                0.
            } else if layout == FormLayout::Inline {
                metrics.inline_item_margin_bottom
            } else {
                metrics.item_margin_bottom
            } as f32));
        if layout == FormLayout::Inline {
            root = root
                .flex_shrink_0()
                .when(direction == Direction::Ltr, |e| {
                    e.mr(px(metrics.inline_item_margin_end as f32))
                })
                .when(direction == Direction::Rtl, |e| {
                    e.ml(px(metrics.inline_item_margin_end as f32))
                });
        }
        root.style().refine(&styles.item);
        root = root.mb(px(if help_sample.leaving && collapsed_last {
            -item_margin
        } else if visible_help || collapsed_last {
            0.
        } else {
            item_margin
        }));
        if self.cache_root {
            // The cache wrapper already resolved these constraints against its
            // parent. Resolve percentages once, then fill that allocated width.
            root.style().size.width = Some(relative(1.).into());
            root.style().min_size.width = Some(px(0.).into());
            root.style().max_size.width = None;
        }
        let mut row = div()
            .id("row")
            .flex()
            .min_w_0()
            .items_stretch()
            .when(explicit_vertical, |e| e.flex_col())
            .when(!explicit_vertical && layout != FormLayout::Inline, |e| {
                e.flex_wrap()
            })
            .when(!explicit_vertical && direction == Direction::Rtl, |e| {
                e.flex_row_reverse()
            });
        if has_label {
            let mut label_slot = div()
                .id("label")
                .flex()
                .items_start()
                .flex_shrink_0()
                .min_w_0()
                .when(
                    (!vertical && align_right) || (vertical && direction == Direction::Rtl),
                    |e| e.justify_end(),
                )
                .when(
                    (!vertical && !align_right) || (vertical && direction == Direction::Ltr),
                    |e| e.justify_start(),
                )
                .when(!label_wrap, |e| e.overflow_hidden())
                .when(vertical, |e| {
                    e.w_full()
                        .pb(px(metrics.vertical_label_padding as f32))
                        .mb(px(metrics.vertical_label_margin as f32))
                });
            if !explicit_vertical {
                label_slot = apply_column(label_slot, label_col, direction);
            } else {
                if let Some(width) = label_col.width {
                    label_slot = label_slot
                        .flex_basis(px(width))
                        .flex_grow(0.)
                        .flex_shrink_0();
                }
                if let Some(span) = label_col.span {
                    label_slot = label_slot.max_w(relative(f32::from(span) / 24.));
                }
            }
            label_slot.style().refine(&styles.label_column);
            let mut label_content = div()
                .id("label-content")
                .flex()
                .items_center()
                .min_w_0()
                .max_w_full()
                .text_size(px(metrics.label_font_size as f32))
                .text_color(metrics.label_color)
                .when(!explicit_vertical, |e| e.h(px(metrics.label_height as f32)))
                .when(direction == Direction::Rtl, |e| e.flex_row_reverse());
            if explicit_vertical && let Some(height) = metrics.vertical_label_height {
                label_content = label_content.h(px(height as f32));
            }
            label_content.style().refine(&styles.label);
            if let Some(field) = self.field.clone() {
                label_content = label_content
                    .cursor_default()
                    .on_click(move |_, window, cx| {
                        field.activate_label(window, cx);
                    });
            }
            if let Some(mark) = self.mark.or(parent.mark) {
                label_content = label_content.child(mark(required, window, cx));
            } else if required && required_mark == FormRequiredMark::Required {
                let mut mark = div()
                    .id("required-mark")
                    .font_family(tokens.required_mark_font().family.clone())
                    .text_size(px(metrics.font_size as f32))
                    .text_color(metrics.required_color)
                    .line_height(relative(1.))
                    .when(direction == Direction::Ltr, |e| {
                        e.mr(px(metrics.mark_margin as f32))
                    })
                    .when(direction == Direction::Rtl, |e| {
                        e.ml(px(metrics.mark_margin as f32))
                    });
                mark.style().refine(&styles.required_mark);
                label_content = label_content.child(
                    Accessibility {
                        hidden: true,
                        ..Default::default()
                    }
                    .apply(mark.child(FractionalText {
                        text: SharedString::new_static("*"),
                        direction,
                    })),
                );
            }
            if let Some(mut label) = self.label {
                if colon && let Content::Text(text) = &mut label {
                    let trimmed = text.trim_end();
                    if let Some(value) = trimmed
                        .strip_suffix(':')
                        .or_else(|| trimmed.strip_suffix('：'))
                    {
                        *text = value.to_owned().into();
                    }
                }
                let mut text = div().id("label-text").min_w_0();
                if label_wrap {
                    text = text.flex_shrink(1.);
                } else {
                    text = text.whitespace_nowrap();
                }
                let label = match label {
                    Content::Text(label) if !label_wrap => FractionalText {
                        text: label,
                        direction,
                    }
                    .into_any_element(),
                    label => label.into_element(direction, window, cx),
                };
                label_content = label_content.child(text.child(label));
            }
            if let Some(tooltip) = self.tooltip.filter(|text| !text.is_empty()) {
                let hint = div()
                    .id("tooltip-anchor")
                    .flex()
                    .items_center()
                    .cursor_default()
                    .on_click(|_, window, cx| {
                        window.prevent_default();
                        cx.stop_propagation();
                    })
                    .text_color(metrics.description_color)
                    .when(direction == Direction::Ltr, |e| {
                        e.ml(px(metrics.mark_margin as f32))
                    })
                    .when(direction == Direction::Rtl, |e| {
                        e.mr(px(metrics.mark_margin as f32))
                    })
                    .child(
                        icon(IconName::QuestionCircleOutlined)
                            .inherit_color()
                            .w(px(metrics.font_size as f32))
                            .h(px(metrics.font_size as f32)),
                    );
                label_content = label_content.child(
                    Tooltip::new("label-tooltip")
                        .scope(scope.clone())
                        .title(tooltip)
                        .child(hint),
                );
            }
            if !required && required_mark == FormRequiredMark::Optional {
                label_content = label_content.child(
                    div()
                        .id("optional-mark")
                        .text_color(metrics.description_color)
                        .when(direction == Direction::Ltr, |e| {
                            e.ml(px(metrics.mark_margin as f32))
                        })
                        .when(direction == Direction::Rtl, |e| {
                            e.mr(px(metrics.mark_margin as f32))
                        })
                        .child(config.locale.form.optional.clone()),
                );
            }
            let colon_element = div()
                .id("colon")
                .flex_shrink_0()
                .when(direction == Direction::Ltr, |e| {
                    e.ml(px(metrics.colon_margin_start as f32))
                        .mr(px(metrics.colon_margin_end as f32))
                })
                .when(direction == Direction::Rtl, |e| {
                    e.mr(px(metrics.colon_margin_start as f32))
                        .ml(px(metrics.colon_margin_end as f32))
                })
                .when(vertical, |e| e.invisible())
                .child(FractionalText {
                    text: SharedString::new_static(if colon || vertical { ":" } else { "\u{a0}" }),
                    direction,
                });
            label_content = label_content.child(
                Accessibility {
                    hidden: true,
                    ..Default::default()
                }
                .apply(colon_element),
            );
            let label_content = if let Some(text) = literal_label {
                let mark = if has_required_mark {
                    let mut style = gpui::TextStyleRefinement {
                        font_family: Some(tokens.required_mark_font().family.clone()),
                        font_size: Some(px(metrics.font_size as f32).into()),
                        ..Default::default()
                    };
                    style.refine(&styles.required_mark.text);
                    Some(IntrinsicLabelMark {
                        text: SharedString::new_static("*"),
                        style,
                        margin: metrics.mark_margin as f32,
                    })
                } else if !required && required_mark == FormRequiredMark::Optional {
                    Some(IntrinsicLabelMark {
                        text: config.locale.form.optional.clone(),
                        style: Default::default(),
                        margin: metrics.mark_margin as f32,
                    })
                } else {
                    None
                };
                IntrinsicLabel {
                    element: label_content,
                    text,
                    punctuation: if colon || vertical { ":" } else { "\u{a0}" },
                    margin: (metrics.colon_margin_start + metrics.colon_margin_end) as f32,
                    mark,
                }
                .into_any_element()
            } else {
                label_content.into_any_element()
            };
            #[cfg(any(test, feature = "test-support"))]
            let label_content = if let Some(inspection) = self.label_layout_inspection {
                FormElement {
                    element: label_content,
                    field: None,
                    required: false,
                    live: false,
                    on_layout: Some(Rc::new(move |bounds, _, _| inspection(bounds))),
                    scroll: FormScrollTarget::default(),
                }
                .into_any_element()
            } else {
                label_content
            };
            row = row.child(label_slot.child(label_content));
        }
        let mut control = div()
            .id("control")
            .flex()
            .flex_col()
            .min_w_0()
            .when(explicit_vertical, |e| e.w_full().flex_none())
            .when(!explicit_vertical && layout != FormLayout::Inline, |e| {
                e.flex_1()
            });
        if !explicit_vertical {
            control = apply_column(control, wrapper_col, direction);
        } else if let Some(span) = wrapper_col.span {
            control = control.max_w(relative(f32::from(span) / 24.));
        }
        control.style().refine(&styles.control);
        let mut input = div()
            .id("input")
            .flex()
            .items_center()
            .min_w_0()
            .min_h(px(metrics.control_height as f32));
        input.style().refine(&styles.input);
        let mut input_content = div()
            .id("input-content")
            .flex()
            .flex_col()
            .flex_1()
            .min_w_0()
            .max_w_full()
            .when(direction == Direction::Ltr, |content| content.items_start())
            .when(direction == Direction::Rtl, |content| content.items_end());
        input_content.style().refine(&styles.content);
        control = control.child(input.child(input_content.child(child)));
        let help_height = Rc::new(Cell::new(None));
        if visible_help {
            let margin = f64::from(item_margin);
            let mut explain = div()
                .id("explain")
                .flex()
                .flex_col()
                .flex_shrink_0()
                .min_w_0()
                .relative()
                .top(px(-5. * (1. - help_sample.opacity)))
                .text_color(
                    match help_sample
                        .content
                        .as_ref()
                        .map_or(status, |snapshot| snapshot.status)
                    {
                        ValidationStatus::Error => metrics.error_color,
                        ValidationStatus::Warning => metrics.warning_color,
                        _ => metrics.description_color,
                    },
                );
            explain.style().refine(&styles.explain);
            let display = help_sample.content;
            let display_help = display
                .as_ref()
                .and_then(|display| display.help.as_ref().and_then(Content::repeat))
                .or_else(|| self.help.take());
            if let Some(help) = display_help {
                if !help.is_empty() {
                    explain = explain.child(help.into_element(direction, window, cx));
                }
            } else {
                let display_errors = display
                    .as_ref()
                    .map_or(errors, |display| display.errors.clone());
                let display_warnings = display
                    .as_ref()
                    .map_or(warnings, |display| display.warnings.clone());
                for (index, error) in display_errors.into_iter().enumerate() {
                    explain = explain.child(
                        div()
                            .id(("error", index))
                            .text_color(metrics.error_color)
                            .child(normal_paragraph_text(error, direction)),
                    );
                }
                for (index, warning) in display_warnings.into_iter().enumerate() {
                    explain = explain.child(
                        div()
                            .id(("warning", index))
                            .text_color(metrics.warning_color)
                            .child(normal_paragraph_text(warning, direction)),
                    );
                }
            }
            let mut viewport = div()
                .id("help-viewport")
                .flex()
                .flex_col()
                .min_w_0()
                .overflow_hidden()
                .opacity(help_sample.opacity);
            if let Some(height) = help_sample.height {
                viewport = viewport.h(height);
            }
            let viewport = viewport.child(FormElement {
                element: Accessibility::new(Role::Status).apply(explain),
                field: None,
                required: false,
                live: true,
                on_layout: Some(Rc::new(move |bounds, window, cx| {
                    let _ = help_geometry.update(cx, |state, cx| {
                        state.measured(bounds.size.height, window, cx)
                    });
                })),
                scroll: FormScrollTarget::default(),
            });
            control = control.child(
                div()
                    .id("explain-budget")
                    .min_h(px(margin.max(0.) as f32))
                    .child(FormElement {
                        element: viewport,
                        field: None,
                        required: false,
                        live: false,
                        on_layout: Some({
                            let help_height = help_height.clone();
                            Rc::new(move |bounds, _, _| help_height.set(Some(bounds.size.height)))
                        }),
                        scroll: FormScrollTarget::default(),
                    }),
            );
        }
        if let Some(extra) = self.extra.filter(|extra| !extra.is_empty()) {
            let mut slot = div()
                .id("extra")
                .min_w_0()
                .min_h(px(metrics.extra_min_height as f32))
                .text_color(metrics.description_color);
            slot.style().refine(&styles.extra);
            let extra = slot
                .child(extra.into_element(direction, window, cx))
                .into_any_element();
            control = if visible_help {
                control.child(ExtraAfterHelp {
                    child: extra,
                    help_height,
                    margin: px(item_margin),
                })
            } else {
                control.child(extra)
            };
        }
        self.accessibility.label = label;
        self.accessibility.description = description;
        self.accessibility.invalid |= invalid;
        self.accessibility.busy |= busy;
        self.accessibility.busy_allows_activation |= busy;
        let top = root.style().margin.top;
        let bottom = root.style().margin.bottom;
        let on_layout = self.on_layout.take().map(|callback| {
            Rc::new(
                move |mut bounds: Bounds<Pixels>, window: &mut Window, cx: &mut App| {
                    let length = |value: Option<gpui::Length>| match value {
                        Some(gpui::Length::Definite(value)) => {
                            value.to_pixels(bounds.size.width.into(), window.rem_size())
                        }
                        _ => px(0.),
                    };
                    bounds.size.height =
                        (bounds.size.height + length(top) + length(bottom)).max(px(0.));
                    callback(bounds, window, cx);
                },
            ) as LayoutCallback
        });
        FormElement {
            element: self.accessibility.apply(root.child(row.child(control))),
            field: self.field,
            required,
            live: false,
            on_layout,
            scroll: parent.scroll.clone(),
        }
        .into_any_element()
    }
}

#[derive(IntoElement)]
pub struct FormErrorList {
    id: ElementId,
    scope: Option<ConfigScope>,
    field: Option<FieldHandle>,
    errors: Vec<SharedString>,
    warnings: Vec<SharedString>,
    style: StyleRefinement,
}
impl FormErrorList {
    pub fn new(id: impl Into<ElementId>) -> Self {
        Self {
            id: id.into(),
            scope: None,
            field: None,
            errors: Vec::new(),
            warnings: Vec::new(),
            style: StyleRefinement::default(),
        }
    }
    pub fn scope(mut self, value: ConfigScope) -> Self {
        self.scope = Some(value);
        self
    }
    pub fn field(mut self, field: FieldHandle) -> Self {
        self.field = Some(field);
        self
    }
    pub fn errors(mut self, values: impl IntoIterator<Item = impl Into<SharedString>>) -> Self {
        self.errors = values.into_iter().map(Into::into).collect();
        self
    }
    pub fn warnings(mut self, values: impl IntoIterator<Item = impl Into<SharedString>>) -> Self {
        self.warnings = values.into_iter().map(Into::into).collect();
        self
    }
}
impl Styled for FormErrorList {
    fn style(&mut self) -> &mut StyleRefinement {
        &mut self.style
    }
}
impl RenderOnce for FormErrorList {
    fn render(mut self, window: &mut Window, cx: &mut App) -> impl IntoElement {
        let scope = self
            .scope
            .take()
            .unwrap_or_else(|| ConfigScope::application(cx));
        observe(&self.id, &scope, self.field.as_ref(), false, window, cx);
        let config = scope.snapshot(cx);
        let metrics = FormStyle::resolve(&config.theme, config.size);
        if let Some(field) = self.field {
            let meta = field.meta(cx);
            self.errors.extend(meta.errors);
            self.warnings.extend(meta.warnings);
        }
        let empty = self.errors.is_empty() && self.warnings.is_empty();
        let mut root = div()
            .id(self.id)
            .flex()
            .flex_col()
            .min_w_0()
            .text_size(px(metrics.font_size as f32))
            .line_height(relative(metrics.line_height as f32))
            .when(empty, |e| e.hidden());
        root.style().refine(&config.form.styles.explain);
        root.style().refine(&self.style);
        for (index, error) in self.errors.into_iter().enumerate() {
            root = root.child(
                div()
                    .id(("error", index))
                    .text_color(metrics.error_color)
                    .child(normal_paragraph_text(error, config.direction)),
            );
        }
        for (index, warning) in self.warnings.into_iter().enumerate() {
            root = root.child(
                div()
                    .id(("warning", index))
                    .text_color(metrics.warning_color)
                    .child(normal_paragraph_text(warning, config.direction)),
            );
        }
        FormElement {
            element: Accessibility::new(Role::Status).apply(root),
            field: None,
            required: false,
            live: true,
            on_layout: None,
            scroll: FormScrollTarget::default(),
        }
    }
}
