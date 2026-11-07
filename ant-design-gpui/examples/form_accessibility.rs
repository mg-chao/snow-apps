// Copyright (C) 2026 mg-chao
// SPDX-License-Identifier: Apache-2.0

//! Hidden native Form fixture inspected through the real Windows UIA adapter.

use std::{
    cell::RefCell,
    collections::{BTreeMap, VecDeque},
    path::PathBuf,
    rc::Rc,
    time::Duration,
};

use ant_design_gpui::{
    ConfigScope, Direction, FieldOptions, Form, FormCodec, FormField, FormItem, FormLayout,
    FormRule, FormState, Input, InputState, PasswordInput, UiRoot, ValidateOptions,
    form::FormRuleResult, locale::Locale, theme::AlgorithmChain,
};
use gpui::{
    App, AppContext, Bounds, Context, Entity, IntoElement, Render, SharedString, Subscription,
    Window, WindowBounds, WindowOptions, div, point, prelude::*, px, size,
};
use serde_json::json;

#[derive(Clone, Copy, PartialEq, Eq)]
enum Kind {
    Required,
    Optional,
    Password,
    Disabled,
    Enabled,
    Pending,
    Hidden,
}
struct Field {
    kind: Kind,
    name: &'static str,
    label: &'static str,
    value: FormField<SharedString>,
    editor: Entity<InputState>,
}
struct Fixture {
    scope: ConfigScope,
    form: Entity<FormState>,
    fields: Vec<Field>,
    inspections: Rc<RefCell<BTreeMap<&'static str, serde_json::Value>>>,
    ever_native_active: bool,
    _activation: Subscription,
}
impl Render for Fixture {
    fn render(&mut self, _: &mut Window, _: &mut Context<Self>) -> impl IntoElement {
        let mut form = Form::new("account", &self.form)
            .scope(self.scope.clone())
            .layout(FormLayout::Vertical)
            .accessibility_label("Account form")
            .w(px(560.));
        for field in &self.fields {
            let editor = field.editor.clone();
            let name = field.name;
            let kind = field.kind;
            let inspections = self.inspections.clone();
            let mut item = FormItem::new(name)
                .label(field.label)
                .has_feedback(matches!(kind, Kind::Required | Kind::Pending));
            if kind == Kind::Optional {
                item = item
                    .help("Optional account note")
                    .extra("Use a work address");
            }
            if matches!(kind, Kind::Disabled | Kind::Enabled) {
                item = item.disabled(true);
            }
            if kind == Kind::Hidden {
                item = item.hidden(true);
            }
            item = if kind == Kind::Password {
                item.control(&field.value, move |_| {
                    let inspections = inspections.clone();
                    PasswordInput::new(name, &editor)
                        .automation_id(format!("form.{name}"))
                        .configure(|input| {
                            input.inspect_accessibility(move |node| {
                                inspect_node(&inspections, name, node)
                            })
                        })
                })
            } else {
                item.control(&field.value, move |_| {
                    let inspections = inspections.clone();
                    let input = Input::new(name, &editor)
                        .automation_id(format!("form.{name}"))
                        .inspect_accessibility(move |node| inspect_node(&inspections, name, node));
                    if kind == Kind::Enabled {
                        input.disabled(false)
                    } else {
                        input
                    }
                })
            };
            form = form.item(item);
        }
        div().p(px(20.)).child(form)
    }
}

fn inspect_node(
    inspections: &RefCell<BTreeMap<&'static str, serde_json::Value>>,
    name: &'static str,
    node: &gpui::accesskit::Node,
) {
    inspections.borrow_mut().insert(
        name,
        json!({
            "role":format!("{:?}", node.role()), "label":node.label(),
            "description":node.description(), "required":node.is_required(),
            "busy":node.is_busy(), "invalid":node.invalid().map(|value|format!("{value:?}")),
        }),
    );
}

fn main() {
    let mut arguments = std::env::args().skip(1);
    let output = PathBuf::from(arguments.next().expect("state output path"));
    let locale = arguments.next().unwrap_or_else(|| "en_US".into());
    let profile = arguments.next().unwrap_or_else(|| "default".into());
    assert!(["en_US", "zh_CN", "zh_TW"].contains(&locale.as_str()));
    assert!(["default", "dark-compact-rtl"].contains(&profile.as_str()));
    gpui_platform::application().run(move |cx: &mut App| {
        ant_design_gpui::init(cx);
        cx.open_window(WindowOptions {
            window_bounds: Some(WindowBounds::Windowed(Bounds::new(point(px(0.), px(0.)), size(px(900.), px(850.))))),
            show: false, focus: false, ..Default::default()
        }, move |window, cx| {
            window.set_window_title("Form accessibility fixture");
            let scope = ConfigScope::new_window(cx);
            scope.update_patch(cx, |patch| {
                patch.motion = Some(false);
                patch.locale.locale = Some(match locale.as_str() { "zh_CN" => Locale::zh_cn(), "zh_TW" => Locale::zh_tw(), _ => Locale::en_us() });
                if profile == "dark-compact-rtl" {
                    patch.direction = Some(Direction::Rtl);
                    patch.theme = Some(ant_design_gpui::theme::ThemeConfig { algorithms: Some(AlgorithmChain::dark_compact()), ..Default::default() });
                }
            }).unwrap();
            let form = cx.new(|cx| FormState::new(window, cx));
            form.update(cx, |form, cx| form.set_scope(scope.clone(), cx));
            let mut fields = Vec::new();
            for (kind, name, label) in [
                (Kind::Required, "username", "Username"), (Kind::Optional, "note", "Account note"),
                (Kind::Password, "password", "Password"), (Kind::Disabled, "disabled", "Disabled field"),
                (Kind::Enabled, "enabled", "Enabled override"), (Kind::Pending, "pending", "Pending verification"),
                (Kind::Hidden, "hidden", "Hidden field"),
            ] {
                let mut options = FieldOptions::default().label(label);
                if kind == Kind::Required { options = options.rule(FormRule::required()); }
                if kind == Kind::Password { options = options.initial_value("synthetic-form-password-do-not-export"); }
                if kind == Kind::Pending {
                    options = options.initial_value("Checking").rule(FormRule::custom(|_, _, cx| {
                        FormRuleResult::Pending(cx.spawn(async move |cx| {
                            cx.background_executor().timer(Duration::from_secs(120)).await;
                            Ok(())
                        }))
                    }));
                }
                let value = form.update(cx, |form, cx| form.register(name, FormCodec::text(), options, cx)).unwrap();
                let editor = cx.new(|cx| InputState::new(window, cx));
                fields.push(Field { kind, name, label, value, editor });
            }
            let fixture = cx.new(|cx| Fixture {
                scope: scope.clone(), form: form.clone(), fields, inspections: Rc::default(),
                ever_native_active: window.is_window_active(),
                _activation: cx.observe_window_activation(window, |fixture: &mut Fixture, window, _| {
                    fixture.ever_native_active |= window.is_window_active();
                }),
            });
            let validate = form.clone();
            let observation = fixture.downgrade();
            window.spawn(cx, async move |cx| {
                let mut validation_started = false;
                let mut settled_fields = None;
                let mut native_frames = VecDeque::new();
                for _ in 0..600 {
                    cx.background_executor().timer(Duration::from_millis(100)).await;
                    if cx.update(|window, cx| {
                        window.draw(cx).clear(cx);
                        // Hidden windows do not receive automatic animation
                        // frames. Validate only after an actual first layout.
                        if !validation_started {
                            validation_started = true;
                            validate.update(cx, |form, cx| { form.validate_fields(ValidateOptions::default(), window, cx); });
                        }
                        let Some(fixture) = observation.upgrade() else { return; };
                        let fixture = fixture.read(cx);
                        let fields = fixture.fields.iter().map(|field| {
                            let meta = field.value.handle().meta(cx);
                            json!({"name":field.name,"required":meta.required,"validating":meta.validating,"errors":meta.errors.iter().map(|s|s.as_ref()).collect::<Vec<_>>()})
                        }).collect::<Vec<_>>();
                        if settled_fields.as_ref() != Some(&fields) {
                            native_frames.clear();
                            settled_fields = Some(fields.clone());
                        }
                        let tree = window.debug_a11y_tree_json().and_then(|tree| serde_json::from_str::<serde_json::Value>(&tree).ok());
                        if window.is_a11y_active()
                            && let Some(tree) = &tree
                            && native_frames.back().is_none_or(|last: &serde_json::Value| last["frame"]["frame_number"] != tree["frame"]["frame_number"])
                        {
                            native_frames.push_back(tree.clone());
                            if native_frames.len() > 4 { native_frames.pop_front(); }
                        }
                        let state = json!({"process_id":std::process::id(),"locale":locale,"profile":profile,
                            "ever_native_active":fixture.ever_native_active,"fields":fields,
                            "native_nodes":fixture.inspections.borrow().clone(),
                            "a11y_active":window.is_a11y_active(),"settled_native_frames":native_frames,"tree":tree});
                        let temporary = output.with_extension("tmp");
                        if let Err(error) = std::fs::write(&temporary, serde_json::to_vec_pretty(&state).unwrap())
                            .and_then(|_|std::fs::rename(temporary, &output)) { eprintln!("Form accessibility state publication: {error}"); }
                    }).is_err() { break; }
                }
                cx.update(|_, cx| cx.quit()).ok();
            }).detach();
            cx.new(|cx| UiRoot::new(scope, fixture, window, cx))
        }).unwrap();
    });
}
