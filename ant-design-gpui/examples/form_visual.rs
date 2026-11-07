// Copyright (C) 2026 mg-chao
// SPDX-License-Identifier: Apache-2.0
//! Actual hidden native Form captures of the independently defined upstream scenes.
use ant_design_gpui::{
    Button, Checkbox, ConfigPatch, ConfigScope, Direction, Input, InputState, PasswordInput, Size,
    Switch, TextArea, UiRoot, ValidationStatus, Variant,
    form::{
        FieldOptions, FieldPatch, Form, FormArray, FormCodec, FormColumn, FormControl, FormField,
        FormItem, FormItemContext, FormLayout, FormList, FormRequiredMark, FormState, FormStyles,
        FormTheme, FormTokenPatch, FormValue, NamePath,
    },
    theme::{AlgorithmChain, Color},
};
use gpui::{
    AnyElement, App, AppContext, Bounds, Context, Element, ElementId, Entity, GlobalElementId,
    InspectorElementId, IntoElement, LayoutId, Pixels, Render, SharedString, StyleRefinement,
    Styled, Window, WindowBounds, WindowOptions, div, prelude::*, px, size,
};
use serde_json::{Value, json};
use std::{cell::RefCell, collections::HashMap, path::PathBuf, rc::Rc, time::Duration};

type Observations = Rc<RefCell<HashMap<String, Bounds<Pixels>>>>;
type Fields = Rc<HashMap<String, FormField<SharedString>>>;
type Editors = Rc<HashMap<String, Entity<InputState>>>;
type MarkerIds = Rc<HashMap<String, usize>>;

// Transparent delegates record the actual element layout and add no layout node.
struct Observed {
    child: AnyElement,
    observations: Observations,
    key: String,
}
impl IntoElement for Observed {
    type Element = Self;
    fn into_element(self) -> Self {
        self
    }
}
impl Element for Observed {
    type RequestLayoutState = ();
    type PrepaintState = ();
    fn id(&self) -> Option<ElementId> {
        None
    }
    fn source_location(&self) -> Option<&'static std::panic::Location<'static>> {
        None
    }
    fn request_layout(
        &mut self,
        _: Option<&GlobalElementId>,
        _: Option<&InspectorElementId>,
        window: &mut Window,
        cx: &mut App,
    ) -> (LayoutId, ()) {
        (self.child.request_layout(window, cx), ())
    }
    fn prepaint(
        &mut self,
        _: Option<&GlobalElementId>,
        _: Option<&InspectorElementId>,
        bounds: Bounds<Pixels>,
        _: &mut (),
        window: &mut Window,
        cx: &mut App,
    ) {
        self.observations
            .borrow_mut()
            .insert(self.key.clone(), bounds);
        self.child.prepaint(window, cx);
    }
    fn paint(
        &mut self,
        _: Option<&GlobalElementId>,
        _: Option<&InspectorElementId>,
        _: Bounds<Pixels>,
        _: &mut (),
        _: &mut (),
        window: &mut Window,
        cx: &mut App,
    ) {
        self.child.paint(window, cx);
    }
}
struct ObservedControl<C> {
    inner: C,
    observations: Observations,
    key: String,
}
impl<T: Clone + PartialEq + 'static, C: FormControl<T>> FormControl<T> for ObservedControl<C> {
    fn into_form_element(
        self,
        field: FormField<T>,
        context: FormItemContext,
        window: &mut Window,
        cx: &mut App,
    ) -> AnyElement {
        Observed {
            child: self.inner.into_form_element(field, context, window, cx),
            observations: self.observations,
            key: self.key,
        }
        .into_any_element()
    }
}
enum NativeText {
    Input(Input),
    Password(PasswordInput),
    Area(TextArea),
}
impl FormControl<SharedString> for NativeText {
    fn into_form_element(
        self,
        field: FormField<SharedString>,
        context: FormItemContext,
        window: &mut Window,
        cx: &mut App,
    ) -> AnyElement {
        match self {
            Self::Input(value) => value.into_form_element(field, context, window, cx),
            Self::Password(value) => value.into_form_element(field, context, window, cx),
            Self::Area(value) => value.into_form_element(field, context, window, cx),
        }
    }
}

#[cfg(target_os = "windows")]
fn assert_background() -> u32 {
    #[link(name = "user32")]
    unsafe extern "system" {
        fn GetForegroundWindow() -> isize;
        fn GetWindowThreadProcessId(window: isize, process: *mut u32) -> u32;
    }
    let mut process = 0;
    unsafe {
        GetWindowThreadProcessId(GetForegroundWindow(), &mut process);
    }
    assert_ne!(
        process,
        std::process::id(),
        "hidden Form capture must preserve foreground focus"
    );
    process
}
#[cfg(not(target_os = "windows"))]
fn assert_background() -> u32 {
    0
}
#[cfg(target_os = "windows")]
fn set_native_scale(title: &str, scale: f32, canvas: (f32, f32)) {
    #[repr(C)]
    #[derive(Default)]
    struct Rect {
        left: i32,
        top: i32,
        right: i32,
        bottom: i32,
    }
    #[link(name = "user32")]
    unsafe extern "system" {
        fn FindWindowW(class: *const u16, title: *const u16) -> isize;
        fn GetWindowRect(window: isize, rect: *mut Rect) -> i32;
        fn GetClientRect(window: isize, rect: *mut Rect) -> i32;
        fn SendMessageW(window: isize, message: u32, wparam: usize, lparam: isize) -> isize;
        fn SetWindowPos(
            window: isize,
            after: isize,
            x: i32,
            y: i32,
            width: i32,
            height: i32,
            flags: u32,
        ) -> i32;
    }
    let title: Vec<u16> = title.encode_utf16().chain(Some(0)).collect();
    unsafe {
        assert_background();
        let handle = FindWindowW(std::ptr::null(), title.as_ptr());
        assert_ne!(handle, 0);
        let mut rect = Rect::default();
        assert_ne!(GetWindowRect(handle, &mut rect), 0);
        let dpi = (96. * scale).round() as usize;
        SendMessageW(
            handle,
            0x02E0,
            dpi | (dpi << 16),
            (&rect as *const Rect) as isize,
        );
        let mut client = Rect::default();
        assert_ne!(GetWindowRect(handle, &mut rect), 0);
        assert_ne!(GetClientRect(handle, &mut client), 0);
        let width = (canvas.0 * scale).round() as i32;
        let height = (canvas.1 * scale).round() as i32;
        assert_ne!(
            SetWindowPos(
                handle,
                0,
                0,
                0,
                width + rect.right - rect.left - client.right + client.left,
                height + rect.bottom - rect.top - client.bottom + client.top,
                0x2 | 0x4 | 0x10
            ),
            0
        );
        assert_ne!(GetClientRect(handle, &mut client), 0);
        assert_eq!(
            (client.right - client.left, client.bottom - client.top),
            (width, height)
        );
        assert_background();
    }
}
#[cfg(not(target_os = "windows"))]
fn set_native_scale(_: &str, _: f32, _: (f32, f32)) {}

fn color(value: &Value) -> Option<Color> {
    value
        .as_str()
        .and_then(|v| u32::from_str_radix(v.trim_start_matches('#'), 16).ok())
        .map(Color::rgb)
}
fn tokens(value: &Value) -> FormTokenPatch {
    FormTokenPatch {
        label_required_mark_color: color(&value["labelRequiredMarkColor"]),
        label_color: color(&value["labelColor"]),
        label_font_size: value["labelFontSize"].as_f64(),
        label_height: value["labelHeight"].as_f64(),
        label_colon_margin_inline_start: value["labelColonMarginInlineStart"].as_f64(),
        label_colon_margin_inline_end: value["labelColonMarginInlineEnd"].as_f64(),
        item_margin_bottom: value["itemMarginBottom"].as_f64(),
        inline_item_margin_bottom: value["inlineItemMarginBottom"].as_f64(),
        ..Default::default()
    }
}
fn column(value: &Value) -> Option<FormColumn> {
    let mut result = if let Some(span) = value["span"].as_u64() {
        FormColumn::span(span as u8)
    } else if let Some(width) = value["flex"]
        .as_str()
        .and_then(|v| v.trim_end_matches("px").parse::<f32>().ok())
    {
        FormColumn::width(width)
    } else {
        FormColumn::flex(value["flex"].as_f64()? as f32)
    };
    if let Some(offset) = value["offset"].as_u64() {
        result = result.offset(offset as u8);
    }
    Some(result)
}
fn layout(value: &Value) -> FormLayout {
    match value.as_str() {
        Some("vertical") => FormLayout::Vertical,
        Some("inline") => FormLayout::Inline,
        _ => FormLayout::Horizontal,
    }
}
fn size_value(value: &Value) -> Size {
    match value.as_str() {
        Some("small") => Size::Small,
        Some("large") => Size::Large,
        _ => Size::Medium,
    }
}
fn variant(value: &Value) -> Variant {
    match value.as_str() {
        Some("filled") => Variant::Filled,
        Some("borderless") => Variant::Borderless,
        Some("underlined") => Variant::Underlined,
        _ => Variant::Outlined,
    }
}
fn status(value: &Value) -> ValidationStatus {
    match value.as_str() {
        Some("error") => ValidationStatus::Error,
        Some("warning") => ValidationStatus::Warning,
        Some("success") => ValidationStatus::Success,
        Some("validating") => ValidationStatus::Validating,
        _ => ValidationStatus::None,
    }
}
fn value(source: &Value) -> FormValue {
    match source {
        Value::Null => FormValue::null(),
        Value::Bool(v) => (*v).into(),
        Value::Number(v) => v.as_f64().unwrap().into(),
        Value::String(v) => v.clone().into(),
        Value::Array(v) => v.iter().map(value).collect::<FormArray>().into(),
        Value::Object(v) => FormValue::object(v.iter().map(|(key, v)| (key.clone(), value(v)))),
    }
}
fn marker(index: usize, slot: usize) -> u32 {
    0x400000 + (index as u32) * 16 + slot as u32
}
fn background(code: u32) -> StyleRefinement {
    div().bg(gpui::rgb(code)).style().clone()
}
fn styles(path: &str, ids: &MarkerIds, marked: bool) -> FormStyles {
    if !marked {
        return FormStyles::default();
    }
    let i = ids[path];
    FormStyles {
        item: background(marker(i, 0)),
        label_column: background(marker(i, 1)),
        label: background(marker(i, 6)),
        control: background(marker(i, 2)),
        explain: background(marker(i, 3)),
        extra: background(marker(i, 4)),
        feedback: background(marker(i, 5)),
        ..Default::default()
    }
}
fn semantic_style(source: &Value, direction: Direction) -> StyleRefinement {
    let mut style = div();
    if let Some(value) = color(&source["backgroundColor"]) {
        style = style.bg(value.to_hsla());
    }
    if let Some(value) = color(&source["color"]) {
        style = style.text_color(value.to_hsla());
    }
    if let Some(value) = source["paddingInlineStart"].as_f64() {
        style = if direction == Direction::Ltr {
            style.pl(px(value as f32))
        } else {
            style.pr(px(value as f32))
        };
    }
    if let Some(value) = source["paddingInlineEnd"].as_f64() {
        style = if direction == Direction::Ltr {
            style.pr(px(value as f32))
        } else {
            style.pl(px(value as f32))
        };
    }
    style.style().clone()
}
fn marker_bounds(window: &Window, index: usize, slot: usize) -> Value {
    let expected = gpui::rgb(marker(index, slot));
    let scale = window.scale_factor();
    window
        .rendered_scene()
        .quads
        .iter()
        .find(|quad| {
            quad.background.as_solid().is_some_and(|color| {
                let c = color.to_rgb();
                (c.r - expected.r).abs() < 0.0001
                    && (c.g - expected.g).abs() < 0.0001
                    && (c.b - expected.b).abs() < 0.0001
            })
        })
        .map_or(Value::Null, |quad| {
            json!({"x":quad.bounds.origin.x.0/scale,"y":quad.bounds.origin.y.0/scale,
        "width":quad.bounds.size.width.0/scale,"height":quad.bounds.size.height.0/scale})
        })
}
fn observed(bounds: &Observations, key: &str) -> Value {
    bounds.borrow().get(key).map_or(Value::Null, |r| {
        json!({"x":f32::from(r.origin.x),"y":f32::from(r.origin.y),
        "width":f32::from(r.size.width),"height":f32::from(r.size.height)})
    })
}
fn font_probes(scope: &ConfigScope, window: &Window, cx: &App) -> Vec<Value> {
    let config = scope.snapshot(cx);
    ["Username", "Password", ":", "*", "(optional)"]
        .into_iter()
        .map(|text| {
            let run = gpui::TextRun {
                len: text.len(),
                font: config.font.clone(),
                color: config.theme.tokens.color_text.to_hsla(),
                background_color: None,
                underline: None,
                strikethrough: None,
            };
            let line = window.text_system().shape_line(
                text.into(),
                px(config.theme.tokens.font_size as f32),
                &[run],
                None,
            );
            json!({"text":text,"width":f32::from(line.width()),"font_size":config.theme.tokens.font_size,"font":format!("{:?}",config.font)})
        })
        .collect()
}
fn sprite_metrics(window: &Window) -> Vec<Value> {
    let scale = window.scale_factor();
    window
        .rendered_scene()
        .polychrome_sprites
        .iter()
        .map(|sprite| {
            json!({"x":sprite.bounds.origin.x.0/scale,"y":sprite.bounds.origin.y.0/scale,
            "width":sprite.bounds.size.width.0/scale,"height":sprite.bounds.size.height.0/scale,
            "opacity":sprite.opacity})
        })
        .collect()
}

fn gpu_metrics(window: &Window) -> Value {
    window.gpu_service().ok().map_or(Value::Null, |service| {
        let capabilities = service.capabilities();
        json!({"backend":format!("{:?}",capabilities.backend),
            "adapter":capabilities.adapter_name.as_ref(),"vendor_id":capabilities.vendor_id,
            "device_id":capabilities.device_id,"max_texture_size":capabilities.max_texture_size})
    })
}

struct PreparedForm {
    sample: Value,
    state: Entity<FormState>,
    scope: ConfigScope,
    fields: Fields,
    editors: Editors,
    ids: MarkerIds,
}
struct FixtureFields {
    state: Entity<FormState>,
    fields: HashMap<String, FormField<SharedString>>,
    editors: HashMap<String, Entity<InputState>>,
    ids: HashMap<String, usize>,
}
impl FixtureFields {
    fn register(
        &mut self,
        item: &Value,
        path: &str,
        name: Option<NamePath>,
        window: &mut Window,
        cx: &mut App,
    ) {
        self.ids.insert(path.to_owned(), self.ids.len());
        if let Some(children) = item["children"].as_array() {
            for child in children {
                let id = child["id"].as_str().unwrap();
                self.register(
                    child,
                    &format!("{path}/{id}"),
                    name.as_ref().map(|prefix| prefix.child(id)),
                    window,
                    cx,
                );
            }
            return;
        }
        if matches!(
            item["control"].as_str(),
            Some("button" | "checkbox" | "switch")
        ) {
            return;
        }
        let mut options = FieldOptions::default().required(item["required"] == true);
        if let Some(initial) = item.get("value") {
            options = options.initial_value(value(initial));
        }
        let name = name.unwrap_or_else(|| NamePath::from(item["id"].as_str().unwrap()));
        let field = self
            .state
            .update(cx, |state, cx| {
                state.register(name, FormCodec::text(), options, cx)
            })
            .unwrap();
        if let Some(errors) = item["errors"].as_array() {
            let patch = FieldPatch {
                name: field.name(cx),
                errors: Some(
                    errors
                        .iter()
                        .map(|v| v.as_str().unwrap().to_owned().into())
                        .collect(),
                ),
                ..Default::default()
            };
            self.state
                .update(cx, |state, cx| state.set_fields([patch], window, cx))
                .unwrap();
        }
        self.fields.insert(path.to_owned(), field);
        self.editors
            .insert(path.to_owned(), cx.new(|cx| InputState::new(window, cx)));
    }
}
fn prepare(
    mut sample: Value,
    parent: &ConfigScope,
    marker_base: usize,
    window: &mut Window,
    cx: &mut App,
) -> PreparedForm {
    let mut scope = parent.clone();
    if sample["tokens"].is_object() {
        let mut patch = ConfigPatch::default();
        patch
            .theme
            .get_or_insert_default()
            .components
            .set::<FormTheme>(tokens(&sample["tokens"]));
        scope = scope.child(patch, cx).unwrap();
    }
    let state = cx.new(|cx| FormState::new(window, cx));
    state.update(cx, |s, cx| s.set_scope(scope.clone(), cx));
    let mut registrations = FixtureFields {
        state: state.clone(),
        fields: HashMap::new(),
        editors: HashMap::new(),
        ids: HashMap::new(),
    };
    let form_id = sample["id"].as_str().unwrap().to_owned();
    for item in sample["items"].as_array().unwrap() {
        let path = format!("{form_id}/{}", item["id"].as_str().unwrap());
        registrations.register(item, &path, None, window, cx);
    }
    if sample["list"].is_object() {
        let config = sample["list"].clone();
        let name = NamePath::from(config["name"].as_str().unwrap());
        let initial = value(&config["initialValue"]).as_array().cloned().unwrap();
        let list = FormList::new(&state, name.clone(), Some(initial), cx).unwrap();
        let original = list.items(cx);
        let key_map = original
            .iter()
            .enumerate()
            .map(|(index, row)| (row.key, index))
            .collect::<HashMap<_, _>>();
        for row in &original {
            let id = format!("row-{}", key_map[&row.key]);
            let group = json!({"id":id,"control":"group","children":config["children"]});
            registrations.register(
                &group,
                &format!("{form_id}/{id}"),
                Some(row.name.clone()),
                window,
                cx,
            );
        }
        if let Some(movement) = config["move"].as_array() {
            list.move_item(
                movement[0].as_u64().unwrap() as usize,
                movement[1].as_u64().unwrap() as usize,
                window,
                cx,
            )
            .unwrap();
        }
        for row in list.items(cx) {
            let id = format!("row-{}", key_map[&row.key]);
            sample["items"].as_array_mut().unwrap().push(json!({"id":id,
            "label":format!("User {}",row.index+1),"control":"group","gap":config["gap"],"children":config["children"]}));
        }
    }
    for index in registrations.ids.values_mut() {
        *index += marker_base;
    }
    PreparedForm {
        sample,
        state,
        scope,
        fields: Rc::new(registrations.fields),
        editors: Rc::new(registrations.editors),
        ids: Rc::new(registrations.ids),
    }
}

#[derive(Clone)]
struct ItemContext {
    scope: ConfigScope,
    form_config: Rc<Value>,
    fields: Fields,
    editors: Editors,
    ids: MarkerIds,
    observations: Observations,
    marked: bool,
}
fn item(source: &Value, path: String, context: ItemContext, child: bool) -> FormItem {
    let ItemContext {
        scope,
        form_config,
        fields,
        editors,
        ids,
        observations,
        marked,
    } = context;
    let mut result = FormItem::new(SharedString::from(path.clone()))
        .scope(scope.clone())
        .styles(styles(&path, &ids, marked));
    #[cfg(feature = "test-support")]
    {
        let observations = observations.clone();
        let key = format!("{path}/label_text");
        result = result.inspect_label_layout(move |bounds| {
            observations.borrow_mut().insert(key.clone(), bounds);
        });
    }
    if let Some(label) = source.get("label") {
        if label.is_null() {
            result = result.label_placeholder();
        } else if let Some(label) = label.as_str() {
            result = result.label(label.to_owned());
        }
    }
    if let Some(v) = source["required"].as_bool() {
        result = result.required(v);
    }
    if source["status"].is_string() {
        result = result.validate_status(status(&source["status"]));
    }
    if let Some(v) = source["feedback"].as_bool() {
        result = result.has_feedback(v);
    }
    if let Some(v) = source["noStyle"].as_bool() {
        result = result.no_style(v);
    }
    if source["layout"].is_string() {
        result = result.layout(layout(&source["layout"]));
    }
    if let Some(v) = source["colon"].as_bool() {
        result = result.colon(v);
    }
    if let Some(v) = source["tooltip"].as_str() {
        result = result.tooltip(v.to_owned());
    }
    if let Some(v) = source["help"].as_str() {
        result = result.help(v.to_owned());
    }
    if let Some(v) = source["extra"].as_str() {
        result = result.extra(v.to_owned());
    }
    let form_size = size_value(&form_config["size"]);
    let disabled = source["disabled"]
        .as_bool()
        .unwrap_or(form_config["disabled"] == true);
    if child {
        result = result
            .size(form_size)
            .variant(variant(&form_config["variant"]));
        if source["noStyle"] != true
            && let Some(width) = source["controlWidth"].as_f64()
        {
            result = result.w(px(width as f32)).mb(px(0.));
        }
    }
    if source["control"] == "group" {
        let children = source["children"].as_array().unwrap().clone();
        let gap = source["gap"].as_f64().unwrap_or(8.) as f32;
        result
            .group_gap(gap)
            .items(children.iter().map(|child_source| {
                let child_path = format!("{path}/{}", child_source["id"].as_str().unwrap());
                item(
                    child_source,
                    child_path,
                    ItemContext {
                        scope: scope.clone(),
                        form_config: form_config.clone(),
                        fields: fields.clone(),
                        editors: editors.clone(),
                        ids: ids.clone(),
                        observations: observations.clone(),
                        marked,
                    },
                    true,
                )
            }))
    } else if matches!(
        source["control"].as_str(),
        Some("button" | "checkbox" | "switch")
    ) {
        let source = source.clone();
        result.content_with(move |_, _| {
            let id = SharedString::from(format!("{path}/control"));
            let control = match source["control"].as_str() {
                Some("button") => Button::new(id)
                    .scope(scope.clone())
                    .primary()
                    .size(form_size)
                    .disabled(disabled)
                    .label(source["text"].as_str().unwrap_or_default().to_owned())
                    .into_any_element(),
                Some("checkbox") => Checkbox::new(id)
                    .scope(scope.clone())
                    .disabled(disabled)
                    .checked(source["value"] == true)
                    .label(source["text"].as_str().unwrap_or_default().to_owned())
                    .into_any_element(),
                _ => {
                    let mut control = Switch::new(id)
                        .scope(scope.clone())
                        .size(source.get("size").map_or(form_size, size_value))
                        .loading(source["loading"] == true)
                        .disabled(disabled)
                        .checked(source["value"] == true);
                    if let Some(alpha) = source["opacity"].as_f64() {
                        control = control.opacity(alpha as f32);
                    }
                    if let Some(backdrop) = color(&form_config["opacityBackdrop"]) {
                        control = control.opacity_backdrop(backdrop);
                    }
                    if source["customContent"] == true {
                        control = control
                            .checked_content(div().w(px(12.)).h(px(22.)).bg(gpui::rgb(0xe54242)))
                            .unchecked_content(div().w(px(12.)).h(px(22.)).bg(gpui::rgb(0xe54242)));
                    }
                    control.into_any_element()
                }
            };
            Observed {
                child: control,
                observations: observations.clone(),
                key: format!("{path}/input"),
            }
            .into_any_element()
        })
    } else {
        let source = source.clone();
        let field = fields[&path].clone();
        let editor = editors[&path].clone();
        result.control(&field, move |_| {
            let id = SharedString::from(format!("{path}/control"));
            let placeholder = source["placeholder"]
                .as_str()
                .unwrap_or_default()
                .to_owned();
            macro_rules! configure {
                ($value:expr) => {{
                    let mut control = $value.placeholder(placeholder.clone());
                    if let Some(disabled) = source["disabled"].as_bool() {
                        control = control.disabled(disabled);
                    }
                    if let Some(width) = source["controlWidth"].as_f64() {
                        control = control.w(px(width as f32));
                    }
                    control
                }};
            }
            let inner = match source["control"].as_str() {
                Some("password") => {
                    NativeText::Password(configure!(PasswordInput::new(id, &editor)))
                }
                Some("textarea") => NativeText::Area(configure!(
                    TextArea::new(id, &editor).rows(source["rows"].as_u64().unwrap_or(3) as usize)
                )),
                _ => NativeText::Input(configure!(Input::new(id, &editor))),
            };
            ObservedControl {
                inner,
                observations: observations.clone(),
                key: format!("{path}/input"),
            }
        })
    }
}

impl PreparedForm {
    fn render(&self, observations: Observations, marked: bool, cx: &App) -> AnyElement {
        let source = &self.sample;
        let id = source["id"].as_str().unwrap();
        let mut form = Form::new(SharedString::from(format!("{id}/form")), &self.state)
            .scope(self.scope.clone())
            .layout(layout(&source["layout"]))
            .size(size_value(&source["size"]))
            .variant(variant(&source["variant"]))
            .disabled(source["disabled"] == true);
        let direction = self.scope.snapshot(cx).direction;
        form = form.styles(FormStyles {
            label: semantic_style(&source["styles"]["label"], direction),
            content: semantic_style(&source["styles"]["content"], direction),
            explain: semantic_style(&source["styles"]["help"], direction),
            extra: semantic_style(&source["styles"]["extra"], direction),
            ..Default::default()
        });
        if let Some(v) = column(&source["labelCol"]) {
            form = form.label_col(v);
        }
        if let Some(v) = column(&source["wrapperCol"]) {
            form = form.wrapper_col(v);
        }
        if let Some(v) = source["colon"].as_bool() {
            form = form.colon(v);
        }
        if let Some(v) = source["labelWrap"].as_bool() {
            form = form.label_wrap(v);
        }
        if source["labelAlign"] == "left" {
            form = form.label_align(ant_design_gpui::form::FormLabelAlign::Left);
        }
        if source.get("requiredMark").is_some() {
            form = form.required_mark(if source["requiredMark"] == false {
                FormRequiredMark::Hidden
            } else if source["requiredMark"] == "optional" {
                FormRequiredMark::Optional
            } else {
                FormRequiredMark::Required
            });
        }
        let context = ItemContext {
            scope: self.scope.clone(),
            form_config: Rc::new(source.clone()),
            fields: self.fields.clone(),
            editors: self.editors.clone(),
            ids: self.ids.clone(),
            observations: observations.clone(),
            marked,
        };
        for source_item in source["items"].as_array().unwrap() {
            form = form.item(item(
                source_item,
                format!("{id}/{}", source_item["id"].as_str().unwrap()),
                context.clone(),
                false,
            ));
        }
        let child = Observed {
            child: form.into_any_element(),
            observations: observations.clone(),
            key: format!("{id}/root"),
        };
        Observed {
            child: div()
                .w(px(source["width"].as_f64().unwrap() as f32))
                .max_w_full()
                .flex_none()
                .child(child)
                .into_any_element(),
            observations,
            key: format!("{id}/wrapper"),
        }
        .into_any_element()
    }
}
fn item_metrics(
    source: &Value,
    path: &str,
    ids: &MarkerIds,
    observations: &Observations,
    window: &Window,
) -> Value {
    let children = source["children"]
        .as_array()
        .map(|values| {
            values
                .iter()
                .map(|child| {
                    item_metrics(
                        child,
                        &format!("{path}/{}", child["id"].as_str().unwrap()),
                        ids,
                        observations,
                        window,
                    )
                })
                .collect::<Vec<_>>()
        })
        .unwrap_or_default();
    let input = if let Some(child) = children.first() {
        child["input"].clone()
    } else {
        observed(observations, &format!("{path}/input"))
    };
    let chrome = |slot| {
        if source["noStyle"] == true {
            Value::Null
        } else {
            marker_bounds(window, ids[path], slot)
        }
    };
    let label_text = if cfg!(feature = "test-support") {
        observed(observations, &format!("{path}/label_text"))
    } else {
        chrome(6)
    };
    json!({"id":source["id"],"root":chrome(0),"label":chrome(1),"label_text":label_text,"painted_label_text":chrome(6),"control":chrome(2),"help":chrome(3),"extra":chrome(4),
        "feedback":chrome(5),"input":input,"children":children})
}
struct Capture {
    scope: ConfigScope,
    manifest: Value,
    scene: String,
    forms: Vec<PreparedForm>,
    observations: Observations,
    marked: bool,
}
impl Capture {
    fn configure(&mut self, scene: &str, window: &mut Window, cx: &mut Context<Self>) {
        self.scene = scene.to_owned();
        self.forms = self.manifest[scene]["forms"]
            .as_array()
            .unwrap()
            .iter()
            .cloned()
            .enumerate()
            .map(|(index, v)| prepare(v, &self.scope, index * 1024, window, cx))
            .collect();
        self.marked = false;
        cx.notify();
    }
    fn metrics(&self, window: &Window, cx: &App) -> Vec<Value> {
        self.forms.iter().map(|form|{let id=form.sample["id"].as_str().unwrap();
            let editors=form.editors.iter().map(|(path,editor)| {
                let geometry=editor.read(cx).geometry().map_or(Value::Null,|g|json!({
                    "x":f32::from(g.viewport.origin.x),"y":f32::from(g.viewport.origin.y),
                    "width":f32::from(g.viewport.size.width),"height":f32::from(g.viewport.size.height)}));
                (path.clone(),geometry)
            }).collect::<serde_json::Map<_,_>>();
            json!({"id":id,"root":observed(&self.observations,&format!("{id}/root")),"wrapper":observed(&self.observations,&format!("{id}/wrapper")),"editors":editors,
            "items":form.sample["items"].as_array().unwrap().iter().map(|item|item_metrics(item,&format!("{id}/{}",item["id"].as_str().unwrap()),&form.ids,&self.observations,window)).collect::<Vec<_>>()})}).collect()
    }
}
impl Render for Capture {
    fn render(&mut self, _: &mut Window, cx: &mut Context<Self>) -> impl IntoElement {
        self.observations.borrow_mut().clear();
        let config = self.scope.snapshot(cx);
        let scene = &self.manifest[&self.scene];
        let mut page = div()
            .w(px(scene["viewport"]["width"].as_f64().unwrap() as f32))
            .h(px(scene["viewport"]["height"].as_f64().unwrap() as f32))
            .p(px(20.))
            .flex()
            .flex_col()
            .items_start()
            .gap(px(24.))
            .bg(color(&scene["background"])
                .unwrap_or(config.theme.tokens.color_bg_container)
                .to_hsla());
        if config.direction == Direction::Rtl {
            page = page.items_end();
        }
        for form in &self.forms {
            page = page.child(form.render(self.observations.clone(), self.marked, cx));
        }
        page
    }
}

fn main() {
    if std::env::args().any(|argument| argument == "--build-info") {
        println!(
            "{}",
            json!({"label_layout_inspection":cfg!(feature="test-support"),
            "features":if cfg!(feature="test-support"){vec!["test-support"]}else{Vec::<&str>::new()}})
        );
        return;
    }
    let mut options = HashMap::new();
    let mut args = std::env::args().skip(1);
    while let Some(key) = args.next() {
        options.insert(key, args.next().expect("argument value"));
    }
    let output = PathBuf::from(options.get("--output").expect("--output directory"));
    std::fs::create_dir_all(&output).unwrap();
    gpui_platform::application().run(move|cx:&mut App|{
        ant_design_gpui::init(cx);let scope=ConfigScope::new_window(cx);scope.update_patch(cx,|patch|patch.motion=Some(false)).unwrap();
        let manifest:Value=options.get("--manifest").map_or_else(
            ||serde_json::from_str(include_str!("../fixtures/form/visual-cases.json")).unwrap(),
            |file|serde_json::from_slice(&std::fs::read(file).expect("capture manifest")).unwrap());
        let names=manifest.as_object().unwrap().keys().filter(|name|options.get("--scene").is_none_or(|wanted|wanted==*name)).cloned().collect::<Vec<_>>();assert!(!names.is_empty());
        cx.open_window(WindowOptions{window_bounds:Some(WindowBounds::Windowed(Bounds::centered(None,size(px(900.),px(900.)),cx))),show:false,focus:false,..Default::default()},|window,cx|{
            let title=format!("Ant Design Form hidden capture {}",std::process::id());window.set_window_title(&title);assert_background();
            let capture=cx.new(|cx|{let mut state=Capture{scope:scope.clone(),manifest,scene:names[0].clone(),forms:Vec::new(),observations:Rc::default(),marked:false};state.configure(&names[0],window,cx);state});
            let root=cx.new(|cx|UiRoot::new(scope.clone(),capture.clone(),window,cx));
            window.spawn(cx,async move|cx|{
                let mut cases:Vec<Value>=if options.len()>1{std::fs::read(output.join("native-metrics.json")).ok().map(|bytes|serde_json::from_slice(&bytes).unwrap()).unwrap_or_default()}else{Vec::new()};
                cx.background_executor().timer(Duration::from_millis(150)).await;
                for scale in [1.,1.5,2.]{if options.get("--scale").is_some_and(|v|v.parse::<f32>().unwrap()!=scale){continue;}
                    for scene in &names{
                        let canvas=cx.update(|window,cx|{capture.update(cx,|state,cx|state.configure(scene,window,cx));let sample=&capture.read(cx).manifest[scene];
                            (sample["viewport"]["width"].as_f64().unwrap()as f32,sample["viewport"]["height"].as_f64().unwrap()as f32)}).unwrap();
                        set_native_scale(&title,scale,canvas);cx.background_executor().timer(Duration::from_millis(40)).await;
                        cx.update(|window,cx|{window.bounds_changed(cx);#[cfg(not(target_os="windows"))]window.set_scale_factor(scale);assert!((window.scale_factor()-scale).abs()<0.001);}).unwrap();
                        for (theme,algorithms) in [("default",AlgorithmChain::default()),("dark",AlgorithmChain::dark()),("compact",AlgorithmChain::compact()),("dark-compact",AlgorithmChain::dark_compact())]{
                            if options.get("--theme").is_some_and(|v|v!=theme){continue;}
                            for (direction,dir) in [("ltr",Direction::Ltr),("rtl",Direction::Rtl)]{if options.get("--direction").is_some_and(|v|v!=direction){continue;}
                                cx.update(|_,cx|scope.update_patch(cx,|patch|{patch.direction=Some(dir);patch.theme.get_or_insert_default().algorithms=Some(algorithms.clone());}).unwrap()).unwrap();
                                cx.background_executor().timer(Duration::from_millis(20)).await;
                                let record=cx.update(|window,cx|{assert_background();window.draw(cx).clear(cx);window.draw(cx).clear(cx);
                                    let name=format!("{scene}-{theme}-{direction}-{}",(scale*100.)as u32);let image=window.render_to_image().expect("native Form GPU frame");
                                    assert_eq!((image.width(),image.height()),((canvas.0*scale).round()as u32,(canvas.1*scale).round()as u32));image.save(output.join(format!("{name}.png"))).unwrap();
                                    let sprites=sprite_metrics(window);
                                    capture.update(cx,|state,cx|{state.marked=true;cx.notify();});window.draw(cx).clear(cx);window.draw(cx).clear(cx);
                                    let forms=capture.read(cx).metrics(window,cx);capture.update(cx,|state,cx|{state.marked=false;cx.notify();});window.draw(cx).clear(cx);
                                    json!({"name":name,"scene":scene,"theme":theme,"direction":direction,"scale":scale,"viewport":{"width":canvas.0,"height":canvas.1},"forms":forms,
                                        "font_probes":font_probes(&scope,window,cx),
                                        "label_geometry_source":if cfg!(feature="test-support"){"logical-prepaint"}else{"pixel-snapped-quad"},
                                        "gpu":gpu_metrics(window),
                                        "sprites":sprites,
                                        "foreground_pid":assert_background(),"capture_pid":std::process::id()})}).unwrap();
                                let name=record["name"].as_str().unwrap();cases.retain(|old|old["name"]!=name);cases.push(record);
                                std::fs::write(output.join("native-metrics.json"),serde_json::to_vec_pretty(&cases).unwrap()).unwrap();
                            }
                        }
                        println!("Captured native Form {scene} at {}%",(scale*100.)as u32);
                    }
                }
                cx.update(|_,cx|cx.quit()).unwrap();
            }).detach();root
        }).expect("hidden native Form capture window");
    });
}
