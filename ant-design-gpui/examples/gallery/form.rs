// Copyright (C) 2026 mg-chao
// SPDX-License-Identifier: Apache-2.0
// Examples adapted from Ant Design 6.6.7 (MIT), using retained native GPUI models.

use super::input_examples::{self as examples, Coverage, Demo};
use ant_design_gpui::{
    Button, Checkbox, ConfigScope, DatePicker, DatePickerState, Input, InputNumber,
    InputNumberState, InputState, Modal, ModalState, PasswordInput, PickerRange, RangePicker,
    RangePickerState, Select, SelectOption, SelectOptions, SelectState, SelectValue, Size, Switch,
    TimePicker, TimePickerState, ValidationStatus, Variant,
    button::ButtonType,
    date_picker::{NaiveDate, NaiveTime},
    form::*,
    input_number::NumericValue,
    modal::{ModalContent, ModalFooter, ModalOptions},
};
use gpui::{
    AnyElement, App, AppContext, Context, Entity, IntoElement, Render, ScrollAnchor, ScrollHandle,
    SharedString, StyleRefinement, Styled, Subscription, Window, div, prelude::*, px,
};
use std::{collections::BTreeMap, time::Duration};

macro_rules! demo {
    ($id:literal,$title:literal,$description:literal) => {
        Demo {
            id: $id,
            title: $title,
            description: $description,
            coverage: Coverage::Complete,
        }
    };
    ($id:literal,$title:literal,$description:literal,$note:literal) => {
        Demo {
            id: $id,
            title: $title,
            description: $description,
            coverage: Coverage::Partial($note),
        }
    };
}
pub(super) const DEMOS: &[Demo] = &[
    demo!(
        "basic",
        "Basic Usage",
        "Collect a username and password; submit validates the required fields."
    ),
    demo!(
        "control-hooks",
        "Form methods",
        "Fill fields, reset their values, and reveal a conditional control."
    ),
    demo!(
        "layout",
        "Form Layout",
        "Switch between horizontal, vertical, and inline forms."
    ),
    demo!(
        "layout-multiple",
        "Form mix layout",
        "Individual items can use a different layout from their form."
    ),
    demo!(
        "disabled",
        "Form disabled",
        "A form setting disables the controls together.",
        "Unavailable Cascader, TreeSelect, Upload, Rate, Mentions, Transfer and Tree controls are represented by supported native inputs."
    ),
    demo!(
        "variant",
        "Form variants",
        "Outlined, filled, borderless, and underlined controls inherit the form variant.",
        "Cascader, TreeSelect and Mentions are represented by Select and Input."
    ),
    demo!(
        "required-mark",
        "Required style",
        "Choose required marks, optional marks, or no marks."
    ),
    demo!(
        "size",
        "Form size",
        "Choose small, medium, or large controls.",
        "Cascader and TreeSelect are represented by the supported native Select."
    ),
    demo!(
        "layout-can-wrap",
        "label can wrap",
        "Long labels wrap within a fixed label track."
    ),
    demo!(
        "warning-only",
        "No block rule",
        "A URL warning remains visible while allowing submission."
    ),
    demo!(
        "useWatch",
        "Watch Hooks",
        "A field watcher updates a dependent preview without observing the whole form."
    ),
    demo!(
        "validate-trigger",
        "Validate Trigger",
        "Compare change, blur, and debounced validation."
    ),
    demo!(
        "validate-only",
        "Validate Only",
        "Check validity without displaying validation messages."
    ),
    demo!(
        "form-item-path",
        "Path Prefix",
        "Compose independent fields under a shared nested object path."
    ),
    demo!(
        "dynamic-form-item",
        "Dynamic Form Item",
        "Add and remove rows with stable identities."
    ),
    demo!(
        "dynamic-form-items",
        "Dynamic Form nest Items",
        "Each row contains independently validated first and last names."
    ),
    demo!(
        "dynamic-form-items-drag-sorting",
        "Drag sorting",
        "Drag rows to reorder them while preserving their retained editors."
    ),
    demo!(
        "dynamic-form-items-complex",
        "Complex Dynamic Form Item",
        "Compose nested editable lists and remove rows independently."
    ),
    demo!(
        "nest-messages",
        "Nest",
        "Nested values use scoped validation messages."
    ),
    demo!(
        "complex-form-control",
        "complex form control",
        "Compose multiple no-style fields under one labeled item."
    ),
    demo!(
        "customized-form-controls",
        "Customized Form Controls",
        "A native numeric control and a currency selector form a custom price value."
    ),
    demo!(
        "global-state",
        "Store Form Data into Upper Component",
        "A parent subscription displays the current field values."
    ),
    demo!(
        "form-context",
        "Control between forms",
        "A provider coordinates separately retained parent and child forms."
    ),
    demo!(
        "inline-login",
        "Inline Login Form",
        "A compact inline login form with native keyboard submission."
    ),
    demo!(
        "login",
        "Login Form",
        "Password editing, remembered login, and validation use the existing native controls."
    ),
    demo!(
        "register",
        "Registration",
        "Validate email and dependent confirmation fields.",
        "Residence Cascader and website AutoComplete are represented by supported Select and Input."
    ),
    demo!(
        "advanced-search",
        "Advanced search",
        "Expand additional criteria, search, or reset the form."
    ),
    demo!(
        "form-in-modal",
        "Form in Modal to Create",
        "Open a native dialog containing an independently retained form."
    ),
    demo!(
        "time-related-controls",
        "Time-related Controls",
        "Civil date values remain typed throughout form submission."
    ),
    demo!(
        "without-form-create",
        "Handle Form Data Manually",
        "A custom validation status can be supplied directly to an item."
    ),
    demo!(
        "validate-static",
        "Customized Validation",
        "Display success, warning, error, and validating feedback.",
        "Cascader, TreeSelect and Mentions are represented by native Input and Select."
    ),
    demo!(
        "dynamic-rule",
        "Dynamic Rules",
        "Toggle whether the username rule requires a value."
    ),
    demo!(
        "form-dependencies",
        "Dependencies",
        "Changing a password revalidates its confirmation."
    ),
    demo!(
        "getValueProps-normalize",
        "getValueProps + normalize",
        "A synchronous normalizer transforms user text before it enters the store."
    ),
    demo!(
        "validate-scroll-to-field",
        "Slide to error field",
        "Submission scrolls to and focuses the first invalid field."
    ),
    demo!(
        "validate-other",
        "Other Form Controls",
        "Bind selection, switches, numbers, and text through the same form engine.",
        "Rate and Upload/Dragger use an app-owned native attachment-name input to demonstrate custom value collection."
    ),
    demo!(
        "style-class",
        "Custom semantic dom styling",
        "Refine label, input, help, and other form slots in a local scope."
    ),
];

#[derive(Clone)]
struct TextBinding {
    field: FormField<SharedString>,
    editor: Entity<InputState>,
    label: &'static str,
}
fn text(
    form: &Entity<FormState>,
    path: NamePath,
    label: &'static str,
    options: FieldOptions,
    window: &mut Window,
    cx: &mut App,
) -> TextBinding {
    let field = form.update(cx, |form, cx| {
        form.register(path, FormCodec::text(), options.label(label), cx)
            .unwrap()
    });
    TextBinding {
        field,
        editor: cx.new(|cx| InputState::new(window, cx)),
        label,
    }
}
struct NestedRows {
    list: FormList,
    fields: BTreeMap<FormListKey, TextBinding>,
    order: Vec<FormListKey>,
    _subscription: Subscription,
}
struct DynamicRow {
    first: TextBinding,
    last: TextBinding,
    nested: Option<NestedRows>,
}
#[derive(Clone)]
struct RowDrag {
    demo: &'static str,
    key: FormListKey,
}
struct DragPreview;
impl Render for DragPreview {
    fn render(&mut self, _: &mut Window, _: &mut Context<Self>) -> impl IntoElement {
        div().p(px(8.)).child("Move form row")
    }
}

type TextWatch = FormWatch<Result<FormPresence<SharedString>, FormTypeError>>;

pub(super) struct FormPanel {
    pub id: &'static str,
    pub form: Entity<FormState>,
    scope: ConfigScope,
    scroll: ScrollHandle,
    texts: Vec<TextBinding>,
    checked: FormField<bool>,
    select: FormField<SelectValue<&'static str>>,
    selector: Entity<SelectState<&'static str>>,
    number: FormField<NumericValue>,
    numeric: Entity<InputNumberState>,
    date: FormField<Option<NaiveDate>>,
    date_state: Entity<DatePickerState>,
    time: Option<(FormField<Option<NaiveTime>>, Entity<TimePickerState>)>,
    range: Option<(FormField<PickerRange<NaiveDate>>, Entity<RangePickerState>)>,
    list: Option<FormList>,
    rows: BTreeMap<FormListKey, DynamicRow>,
    order: Vec<FormListKey>,
    layout: FormLayout,
    size: Size,
    variant: Variant,
    mark: FormRequiredMark,
    disabled: bool,
    expanded: bool,
    valid: bool,
    feedback: String,
    watched: Option<Entity<TextWatch>>,
    peer: Option<(Entity<FormState>, TextBinding)>,
    modal: Entity<ModalState>,
    _provider: Option<Entity<FormProvider>>,
    _subscriptions: Vec<Subscription>,
}
impl FormPanel {
    fn new(
        id: &'static str,
        scope: ConfigScope,
        scroll: ScrollHandle,
        window: &mut Window,
        cx: &mut Context<Self>,
    ) -> Self {
        let form = cx.new(|cx| FormState::new(window, cx));
        form.update(cx, |form, cx| {
            form.set_scope(scope.clone(), cx);
            if id == "validate-scroll-to-field" {
                form.set_scroll_to_first_error(true, true);
            }
        });
        let names: &[(&str, &str)] = match id {
            "control-hooks" => &[("note", "Note"), ("other", "Other")],
            "warning-only" => &[("url", "URL")],
            "layout-can-wrap" => &[
                ("name", "This is a very long label that can wrap"),
                ("description", "Description"),
            ],
            "validate-trigger" => &[
                ("change", "On change"),
                ("blur", "On blur"),
                ("debounce", "Debounced"),
            ],
            "register" => &[
                ("email", "E-mail"),
                ("password", "Password"),
                ("confirm", "Confirm password"),
                ("nickname", "Nickname"),
                ("website", "Website"),
            ],
            "form-dependencies" => &[("password", "Password"), ("confirm", "Confirm password")],
            "advanced-search" => &[
                ("name", "Name"),
                ("email", "E-mail"),
                ("company", "Company"),
                ("city", "City"),
            ],
            "validate-static" => &[
                ("success", "Success"),
                ("warning", "Warning"),
                ("error", "Error"),
                ("validating", "Validating"),
            ],
            "dynamic-form-item"
            | "dynamic-form-items"
            | "dynamic-form-items-drag-sorting"
            | "dynamic-form-items-complex" => &[],
            "time-related-controls" | "customized-form-controls" => &[],
            "without-form-create" => &[("prime", "Prime number")],
            "nest-messages" => &[
                ("name", "Name"),
                ("email", "E-mail"),
                ("website", "Website"),
            ],
            _ => &[("username", "Username"), ("password", "Password")],
        };
        let mut texts = Vec::new();
        for &(name, label) in names {
            let mut options = FieldOptions::default().rule(FormRule::required());
            if id == "required-mark" && name == "password" {
                options = FieldOptions::default();
            }
            if id == "advanced-search" || id == "control-hooks" && name == "other" {
                options = FieldOptions::default();
            }
            if name == "email" {
                options = options.rule(FormRule::email());
            }
            if id == "warning-only" {
                options = options.rule(FormRule::url().warning_only(true));
            }
            if id == "validate-trigger" {
                if name == "blur" {
                    options = options.validate_trigger([ValidationTrigger::Blur]);
                }
                if name == "debounce" {
                    options = options.debounce(Duration::from_millis(700));
                }
                options = options.rule(FormRule::string().min(3.));
            }
            if name == "confirm" {
                options =
                    options
                        .dependencies(["password"])
                        .rule(FormRule::custom(|request, _, _| {
                            FormRuleResult::Ready(
                                if request.values.get(&NamePath::from("password"))
                                    == Some(&request.value)
                                {
                                    Ok(())
                                } else {
                                    Err("The two passwords do not match".into())
                                },
                            )
                        }));
            }
            if id == "dynamic-rule" && name == "username" {
                options =
                    FieldOptions::default()
                        .dependencies(["enabled"])
                        .rule(FormRule::dynamic(|values| {
                            if values
                                .get(&NamePath::from("enabled"))
                                .and_then(FormValue::as_bool)
                                .unwrap_or(true)
                            {
                                FormRule::required()
                            } else {
                                FormRule::new()
                            }
                        }));
            }
            if id == "getValueProps-normalize" {
                options = options.normalize(|value, _, _| {
                    FormValue::text(value.as_str().unwrap_or("").trim().to_uppercase())
                });
            }
            let path = if matches!(id, "form-item-path" | "nest-messages") {
                NamePath::from("user").child(name)
            } else {
                name.into()
            };
            texts.push(text(&form, path, label, options, window, cx));
        }
        let checked = form.update(cx, |form, cx| {
            form.register(
                if matches!(id, "basic" | "login" | "register") {
                    "remember"
                } else {
                    "enabled"
                },
                FormCodec::boolean(),
                if matches!(
                    id,
                    "basic" | "login" | "register" | "dynamic-rule" | "validate-other"
                ) {
                    FieldOptions::default().initial_value(true)
                } else {
                    FieldOptions::default()
                },
                cx,
            )
            .unwrap()
        });
        let select = form.update(cx, |form, cx| {
            form.register(
                "gender",
                FormCodec::selection(),
                FieldOptions::default(),
                cx,
            )
            .unwrap()
        });
        let options = SelectOptions::new([
            SelectOption::new("male", "male", "Male"),
            SelectOption::new("female", "female", "Female"),
            SelectOption::new("other", "other", "Other"),
        ])
        .unwrap();
        let selector = cx.new(|cx| SelectState::new(options, window, cx));
        let number = form.update(cx, |form, cx| {
            form.register(
                "age",
                FormCodec::numeric(),
                FieldOptions::default().rule(FormRule::number().min(0.).max(150.)),
                cx,
            )
            .unwrap()
        });
        let numeric = cx.new(|cx| InputNumberState::new(window, cx));
        let date = form.update(cx, |form, cx| {
            form.register(
                "date",
                FormCodec::optional_native(),
                FieldOptions::default(),
                cx,
            )
            .unwrap()
        });
        let date_state = cx.new(|cx| DatePickerState::new(window, cx));
        let time = if id == "time-related-controls" {
            Some((
                form.update(cx, |form, cx| {
                    form.register(
                        "time",
                        FormCodec::optional_native(),
                        FieldOptions::default(),
                        cx,
                    )
                    .unwrap()
                }),
                cx.new(|cx| TimePickerState::new(window, cx)),
            ))
        } else {
            None
        };
        let range = if id == "time-related-controls" {
            Some((
                form.update(cx, |form, cx| {
                    form.register("range", FormCodec::range(), FieldOptions::default(), cx)
                        .unwrap()
                }),
                cx.new(|cx| RangePickerState::new(window, cx)),
            ))
        } else {
            None
        };
        let watched = if id == "useWatch" {
            Some(form.update(cx, |form, cx| {
                form.watch(
                    texts
                        .first()
                        .map(|t| t.field.name(cx))
                        .unwrap_or_else(|| "username".into()),
                    FormCodec::text(),
                    cx,
                )
            }))
        } else {
            None
        };
        let modal = cx.new(|cx| ModalState::new(window, cx));
        modal.update(cx, |modal, cx| modal.set_scope(scope.clone(), cx));
        if id == "form-in-modal" {
            let form = form.clone();
            let texts = texts.clone();
            let scope = scope.clone();
            modal.update(cx, |modal, cx| {
                modal.set_options(
                    ModalOptions {
                        title: Some(ModalContent::text("Create a collection")),
                        content: Some(ModalContent::render(move |_, _, _| {
                            let submit = form.clone();
                            Form::new("modal-form", &form)
                                .scope(scope.clone())
                                .layout(FormLayout::Vertical)
                                .items(
                                    texts
                                        .iter()
                                        .enumerate()
                                        .map(|(index, text)| Self::text_item(text, index, false)),
                                )
                                .item(FormItem::new("create-action").content(
                                    Button::new("create").label("Create").on_click(
                                        move |_, window, cx| {
                                            submit.update(cx, |form, cx| {
                                                form.submit(window, cx);
                                            })
                                        },
                                    ),
                                ))
                        })),
                        footer: ModalFooter::None,
                        ..Default::default()
                    },
                    cx,
                )
            });
        }
        let mut subscriptions =
            vec![
                cx.subscribe_in(&form, window, |this: &mut Self, _, event, window, cx| {
                    match event {
                        FormEvent::Finish(values) => {
                            this.feedback = format!("Submitted: {:?}", values.values());
                            this.valid = true;
                            if this.id == "form-in-modal" {
                                this.modal
                                    .update(cx, |modal, cx| modal.set_open(false, window, cx));
                            }
                        }
                        FormEvent::FinishFailed(error) => {
                            this.feedback =
                                format!("{} field(s) need attention", error.error_fields.len());
                            this.valid = false;
                        }
                        FormEvent::ValuesChange { values, .. } if this.id == "global-state" => {
                            this.feedback = format!("{:?}", values.values())
                        }
                        _ => {}
                    }
                    if matches!(event, FormEvent::Finish(_) | FormEvent::FinishFailed(_))
                        || matches!(event, FormEvent::ValuesChange { .. })
                            && this.id == "global-state"
                    {
                        cx.notify();
                    }
                }),
            ];
        if let Some(watched) = &watched {
            subscriptions.push(cx.observe(watched, |_: &mut Self, _, cx| cx.notify()));
        }
        if id == "without-form-create" {
            subscriptions.push(
                cx.subscribe(&texts[0].editor, |_: &mut Self, _, event, cx| {
                    if matches!(event, ant_design_gpui::InputEvent::Change(_)) {
                        cx.notify();
                    }
                }),
            );
        }
        let list = if id.starts_with("dynamic-form-item") {
            Some(form.update(cx, |form, cx| {
                form.register_list(
                    "users",
                    Some(
                        [FormValue::object([
                            ("first", FormValue::from("")),
                            ("last", FormValue::from("")),
                            (
                                "items",
                                FormValue::array([FormValue::object([("value", "")])]),
                            ),
                        ])]
                        .into_iter()
                        .collect(),
                    ),
                    cx,
                )
                .unwrap()
            }))
        } else {
            None
        };
        if let Some(list) = &list {
            subscriptions.push(cx.observe_in(
                list.field().state(),
                window,
                |this: &mut Self, _, window, cx| {
                    if this.sync_rows(window, cx) {
                        cx.notify();
                    }
                },
            ));
        }
        let (provider, peer) = if id == "form-context" {
            let provider = cx.new(FormProvider::new);
            let peer_form = cx.new(|cx| FormState::new(window, cx));
            let peer_text = text(
                &peer_form,
                "name".into(),
                "Saved user",
                FieldOptions::default(),
                window,
                cx,
            );
            provider.update(cx, |provider, cx| {
                provider.register("child", &form, cx);
                provider.register("parent", &peer_form, cx);
            });
            subscriptions.push(cx.subscribe_in(
                &provider,
                window,
                |this: &mut Self, _, event, window, cx| {
                    if let FormProviderEvent::Finish { name, values, .. } = event
                        && name.as_ref() == "child"
                        && let Some((_, binding)) = &this.peer
                    {
                        let value = values
                            .get(&NamePath::from("username"))
                            .and_then(FormValue::as_text)
                            .cloned()
                            .unwrap_or_default();
                        let _ = binding.field.set(FormPresence::Value(value), window, cx);
                        this.feedback = "Child form saved into parent form".into();
                        cx.notify();
                    }
                },
            ));
            (Some(provider), Some((peer_form, peer_text)))
        } else {
            (None, None)
        };
        let mut panel = Self {
            id,
            form,
            scope,
            scroll,
            texts,
            checked,
            select,
            selector,
            number,
            numeric,
            date,
            date_state,
            time,
            range,
            list,
            rows: BTreeMap::new(),
            order: Vec::new(),
            layout: FormLayout::Horizontal,
            size: Size::Medium,
            variant: Variant::Outlined,
            mark: FormRequiredMark::Required,
            disabled: false,
            expanded: false,
            valid: false,
            feedback: String::new(),
            watched,
            peer,
            modal,
            _provider: provider,
            _subscriptions: subscriptions,
        };
        panel.sync_rows(window, cx);
        panel
    }
    fn sync_rows(&mut self, window: &mut Window, cx: &mut Context<Self>) -> bool {
        let Some(list) = &self.list else {
            return false;
        };
        let items = list.items(cx);
        let order = items.iter().map(|r| r.key).collect::<Vec<_>>();
        let mut changed = order != self.order;
        self.rows.retain(|key, _| order.contains(key));
        for item in items {
            if !self.rows.contains_key(&item.key) {
                let first = text(
                    &self.form,
                    item.field("first"),
                    "First name",
                    FieldOptions::default().rule(FormRule::required()),
                    window,
                    cx,
                );
                let last = text(
                    &self.form,
                    item.field("last"),
                    "Last name",
                    if self.id == "dynamic-form-item" {
                        FieldOptions::default()
                    } else {
                        FieldOptions::default().rule(FormRule::required())
                    },
                    window,
                    cx,
                );
                let nested = if self.id == "dynamic-form-items-complex" {
                    let list = self.form.update(cx, |form, cx| {
                        form.register_list(item.field("items"), None, cx).unwrap()
                    });
                    let subscription = cx.observe_in(
                        list.field().state(),
                        window,
                        |this: &mut Self, _, window, cx| {
                            if this.sync_rows(window, cx) {
                                cx.notify();
                            }
                        },
                    );
                    Some(NestedRows {
                        list,
                        fields: BTreeMap::new(),
                        order: Vec::new(),
                        _subscription: subscription,
                    })
                } else {
                    None
                };
                self.rows.insert(
                    item.key,
                    DynamicRow {
                        first,
                        last,
                        nested,
                    },
                );
                changed = true;
            }
            if let Some(nested) = self
                .rows
                .get_mut(&item.key)
                .and_then(|row| row.nested.as_mut())
            {
                let items = nested.list.items(cx);
                let order = items.iter().map(|r| r.key).collect::<Vec<_>>();
                changed |= order != nested.order;
                nested.fields.retain(|key, _| order.contains(key));
                for item in items {
                    nested.fields.entry(item.key).or_insert_with(|| {
                        text(
                            &self.form,
                            item.field("value"),
                            "Item",
                            FieldOptions::default().rule(FormRule::required()),
                            window,
                            cx,
                        )
                    });
                }
                nested.order = order;
            }
        }
        self.order = order;
        changed
    }
    fn text_item(binding: &TextBinding, index: usize, password: bool) -> FormItem {
        let editor = binding.editor.clone();
        if password {
            FormItem::new(("text", index))
                .label(binding.label)
                .has_feedback(true)
                .control(&binding.field, move |context| {
                    PasswordInput::new(context.id, &editor)
                })
        } else {
            FormItem::new(("text", index))
                .label(binding.label)
                .has_feedback(true)
                .control(&binding.field, move |context| {
                    Input::new(context.id, &editor)
                })
        }
    }
    fn action(&self, id: &'static str, label: &'static str, cx: &Context<Self>) -> Button {
        let form = self.form.clone();
        Button::new(id)
            .scope(self.scope.clone())
            .label(label)
            .automation_id(format!("gallery.form.{}.{}", self.id, id))
            .button_type(if id == "submit" {
                ButtonType::Primary
            } else {
                ButtonType::Default
            })
            .on_click(cx.listener(move |this, _, window, cx| {
                match id {
                    "submit" => {
                        form.update(cx, |form, cx| {
                            form.submit(window, cx);
                        });
                    }
                    "reset" => {
                        form.update(cx, |form, cx| form.reset(window, cx));
                        this.feedback.clear();
                    }
                    "fill" => {
                        form.update(cx, |form, cx| {
                            form.set_fields_value(
                                FormValue::object([
                                    ("note", "Hello world!"),
                                    ("username", "Ada"),
                                    ("password", "native-gpui"),
                                ]),
                                window,
                                cx,
                            )
                            .unwrap()
                        });
                    }
                    "validate" => {
                        let validation = form.update(cx, |form, cx| {
                            form.validate_fields(
                                ValidateOptions {
                                    validate_only: true,
                                    ..Default::default()
                                },
                                window,
                                cx,
                            )
                        });
                        cx.spawn_in(window, async move |weak, cx| {
                            let valid = validation.await.is_ok();
                            let _ = cx.update(|_, cx| {
                                let _ = weak.update(cx, |this, cx| {
                                    this.valid = valid;
                                    this.feedback = if valid {
                                        "Ready to submit"
                                    } else {
                                        "Complete the required fields"
                                    }
                                    .into();
                                    cx.notify();
                                });
                            });
                        })
                        .detach();
                    }
                    "layout" => {
                        this.layout = match this.layout {
                            FormLayout::Horizontal => FormLayout::Vertical,
                            FormLayout::Vertical => FormLayout::Inline,
                            FormLayout::Inline => FormLayout::Horizontal,
                        }
                    }
                    "size" => {
                        this.size = match this.size {
                            Size::Small => Size::Medium,
                            Size::Medium => Size::Large,
                            Size::Large => Size::Small,
                        }
                    }
                    "variant" => {
                        this.variant = match this.variant {
                            Variant::Outlined => Variant::Filled,
                            Variant::Filled => Variant::Borderless,
                            Variant::Borderless => Variant::Underlined,
                            Variant::Underlined => Variant::Outlined,
                        }
                    }
                    "mark" => {
                        this.mark = match this.mark {
                            FormRequiredMark::Required => FormRequiredMark::Optional,
                            FormRequiredMark::Optional => FormRequiredMark::Hidden,
                            FormRequiredMark::Hidden => FormRequiredMark::Required,
                        }
                    }
                    "disabled" => this.disabled = !this.disabled,
                    "expand" => this.expanded = !this.expanded,
                    "add" => {
                        if let Some(list) = &this.list {
                            list.add(
                                FormValue::object([
                                    ("first", FormValue::from("")),
                                    ("last", FormValue::from("")),
                                    (
                                        "items",
                                        FormValue::array([FormValue::object([("value", "")])]),
                                    ),
                                ]),
                                None,
                                window,
                                cx,
                            )
                            .unwrap();
                        }
                    }
                    "open" => {
                        this.modal
                            .update(cx, |modal, cx| modal.set_open(true, window, cx));
                    }
                    _ => {}
                }
                cx.notify();
            }))
    }
    fn body(&self, cx: &Context<Self>) -> AnyElement {
        if self.id == "without-form-create" {
            let value = self.texts[0].editor.read(cx).value();
            let valid = value == "11";
            return FormItem::new("manual-prime")
                .scope(self.scope.clone())
                .label("Prime number")
                .validate_status(if value.is_empty() {
                    ValidationStatus::None
                } else if valid {
                    ValidationStatus::Success
                } else {
                    ValidationStatus::Error
                })
                .help(if valid {
                    "11 is a prime number"
                } else {
                    "Enter a prime number between 8 and 12"
                })
                .content(Input::new("prime", &self.texts[0].editor))
                .into_any_element();
        }
        let mut form = Form::new(self.id, &self.form)
            .scope(self.scope.clone())
            .scroll_handle(&self.scroll)
            .layout(if self.id == "inline-login" {
                FormLayout::Inline
            } else {
                self.layout
            })
            .size(self.size)
            .variant(self.variant)
            .disabled(self.disabled)
            .required_mark(self.mark)
            .label_col(FormColumn::width(if self.id == "layout-can-wrap" {
                110.
            } else {
                100.
            }))
            .label_wrap(self.id == "layout-can-wrap");
        for (index, binding) in self.texts.iter().enumerate() {
            if self.id == "complex-form-control" {
                continue;
            }
            if self.id == "advanced-search" && index >= 2 && !self.expanded
                || self.id == "control-hooks" && index == 1 && !self.expanded
            {
                continue;
            }
            let mut item = Self::text_item(
                binding,
                index,
                binding.label.contains("Password") || binding.label == "Confirm password",
            );
            if self.id == "layout-multiple" && index == 1 {
                item = item.layout(FormLayout::Vertical);
            }
            if self.id == "without-form-create" {
                item = item
                    .validate_status(ValidationStatus::Warning)
                    .help("Enter a prime number between 8 and 12");
            }
            if self.id == "validate-static" {
                item = item
                    .validate_status(
                        [
                            ValidationStatus::Success,
                            ValidationStatus::Warning,
                            ValidationStatus::Error,
                            ValidationStatus::Validating,
                        ][index],
                    )
                    .help(
                        [
                            "Looks good",
                            "Please check this value",
                            "This field needs attention",
                            "Checking this value",
                        ][index],
                    );
            }
            if self.id == "style-class" {
                item = item
                    .styles(FormStyles {
                        label: StyleRefinement::default().text_color(gpui::rgb(0x722ed1)),
                        extra: StyleRefinement::default().text_size(px(12.)),
                        ..Default::default()
                    })
                    .extra("A locally styled help message");
            }
            form = form.item(item);
        }
        if matches!(self.id, "basic" | "login" | "register" | "dynamic-rule") {
            let required_toggle = self.id == "dynamic-rule";
            form = form.item(FormItem::new("enabled").label_placeholder().control(
                &self.checked,
                move |context| {
                    Checkbox::new(context.id).label(if required_toggle {
                        "Require username"
                    } else {
                        "Remember me"
                    })
                },
            ));
        }
        if matches!(
            self.id,
            "control-hooks"
                | "disabled"
                | "variant"
                | "size"
                | "register"
                | "validate-other"
                | "customized-form-controls"
        ) {
            let selector = self.selector.clone();
            form = form.item(
                FormItem::new("gender")
                    .label(if self.id == "customized-form-controls" {
                        "Currency"
                    } else {
                        "Gender"
                    })
                    .control(&self.select, move |context| {
                        Select::new(context.id, &selector).placeholder("Select an option")
                    }),
            );
        }
        if matches!(
            self.id,
            "disabled"
                | "variant"
                | "size"
                | "useWatch"
                | "validate-other"
                | "customized-form-controls"
        ) {
            let numeric = self.numeric.clone();
            form = form.item(
                FormItem::new("number")
                    .label(if self.id == "customized-form-controls" {
                        "Price"
                    } else {
                        "Age"
                    })
                    .control_with_app(&self.number, move |context, _, cx| {
                        InputNumber::new(context.id, &numeric, cx)
                    }),
            );
        }
        if matches!(self.id, "time-related-controls" | "getValueProps-normalize") {
            let state = self.date_state.clone();
            form = form.item(
                FormItem::new("date")
                    .label("Date")
                    .control(&self.date, move |context| {
                        DatePicker::new(context.id, &state)
                    }),
            );
        }
        if let Some((field, state)) = &self.time {
            let state = state.clone();
            form = form.item(
                FormItem::new("time")
                    .label("Time")
                    .control(field, move |context| TimePicker::new(context.id, &state)),
            );
        }
        if let Some((field, state)) = &self.range {
            let state = state.clone();
            form = form.item(
                FormItem::new("range")
                    .label("Date range")
                    .control(field, move |context| RangePicker::new(context.id, &state)),
            );
        }
        if self.id == "validate-other" {
            form = form.item(
                FormItem::new("switch")
                    .label("Switch")
                    .control(&self.checked, move |context| Switch::new(context.id)),
            );
        }
        if self.id == "complex-form-control" {
            let first = self.texts[0].clone();
            let last = self.texts[1].clone();
            form = form.item(
                FormItem::new("combined").label("Address").content(
                    div()
                        .flex()
                        .gap(px(8.))
                        .child(Self::text_item(&first, 10, false).no_style(true))
                        .child(Self::text_item(&last, 11, false).no_style(true)),
                ),
            );
        }
        if let Some(list) = &self.list {
            for (index, key) in self.order.iter().enumerate() {
                let row = &self.rows[key];
                let list_for_remove = list.clone();
                let remove_key = *key;
                let mut content = div()
                    .id(("row", key.0))
                    .flex()
                    .flex_col()
                    .gap(px(8.))
                    .child(Self::text_item(&row.first, index * 2, false));
                if self.id != "dynamic-form-item" {
                    content = content.child(Self::text_item(&row.last, index * 2 + 1, false));
                }
                if let Some(nested) = &row.nested {
                    for (inner_index, key) in nested.order.iter().enumerate() {
                        content = content.child(Self::text_item(
                            &nested.fields[key],
                            100 + inner_index,
                            false,
                        ));
                        let list = nested.list.clone();
                        let key = *key;
                        content = content.child(
                            Button::new(("remove-inner", key.0))
                                .label("Remove nested item")
                                .on_click(move |_, window, cx| {
                                    if let Some(index) =
                                        list.items(cx).iter().position(|row| row.key == key)
                                    {
                                        list.remove([index], window, cx).unwrap();
                                    }
                                }),
                        );
                    }
                    let inner = nested.list.clone();
                    content =
                        content.child(Button::new("add-inner").label("Add nested item").on_click(
                            move |_, window, cx| {
                                inner
                                    .add(FormValue::object([("value", "")]), None, window, cx)
                                    .unwrap();
                            },
                        ));
                }
                content = content.child(Button::new("remove").label("Remove").on_click(
                    move |_, window, cx| {
                        if let Some(index) = list_for_remove
                            .items(cx)
                            .iter()
                            .position(|item| item.key == remove_key)
                        {
                            list_for_remove.remove([index], window, cx).unwrap();
                        }
                    },
                ));
                if self.id == "dynamic-form-items-drag-sorting" {
                    let target = *key;
                    let list = list.clone();
                    let demo = self.id;
                    content = content
                        .cursor_move()
                        .on_drag(RowDrag { demo, key: *key }, |_, _, _, cx| {
                            cx.new(|_| DragPreview)
                        })
                        .on_drop(move |drag: &RowDrag, window, cx| {
                            if drag.demo == demo {
                                let items = list.items(cx);
                                if let (Some(from), Some(to)) = (
                                    items.iter().position(|r| r.key == drag.key),
                                    items.iter().position(|r| r.key == target),
                                ) {
                                    list.move_item(from, to, window, cx).unwrap();
                                }
                            }
                        });
                }
                form = form.child(content);
            }
            form = form.item(
                FormItem::new("list-actions")
                    .label_placeholder()
                    .content(self.action("add", "Add field", cx)),
            );
        }
        let mut actions = examples::row(&self.scope, cx)
            .child(
                self.action(
                    "submit",
                    if self.id == "advanced-search" {
                        "Search"
                    } else {
                        "Submit"
                    },
                    cx,
                )
                .disabled(self.id == "validate-only" && !self.valid),
            )
            .child(self.action("reset", "Reset", cx));
        if self.id == "control-hooks" {
            actions = actions
                .child(self.action("fill", "Fill form", cx))
                .child(self.action("expand", "Toggle other", cx));
        }
        if self.id == "validate-only" {
            actions = actions.child(self.action("validate", "Check validity", cx));
        }
        if self.id == "advanced-search" {
            actions = actions.child(self.action("expand", "Expand criteria", cx));
        }
        form.item(
            FormItem::new("actions")
                .label_placeholder()
                .content(actions),
        )
        .into_any_element()
    }
}

impl Render for FormPanel {
    fn render(&mut self, _: &mut Window, cx: &mut Context<Self>) -> impl IntoElement {
        let mut column = examples::column();
        let toggle = match self.id {
            "layout" => Some(("layout", "Change layout")),
            "size" => Some(("size", "Change size")),
            "variant" => Some(("variant", "Change variant")),
            "required-mark" => Some(("mark", "Change required marks")),
            "disabled" => Some(("disabled", "Toggle disabled")),
            _ => None,
        };
        if let Some((id, label)) = toggle {
            column = column.child(self.action(id, label, cx));
        }
        if self.id == "form-in-modal" {
            column = column
                .child(self.action("open", "New collection", cx))
                .child(Modal::new("form-dialog", &self.modal, cx));
        } else {
            column = column.child(self.body(cx));
        }
        if self.id == "useWatch" {
            let text = self
                .watched
                .as_ref()
                .expect("watch demo owns a field watch")
                .read(cx)
                .value()
                .as_ref()
                .ok()
                .cloned()
                .and_then(FormPresence::value)
                .unwrap_or_default();
            column = column.child(format!("Username: {text}"));
        }
        if let Some((form, binding)) = &self.peer {
            column = column.child(
                Form::new("parent-form", form)
                    .layout(FormLayout::Vertical)
                    .item(Self::text_item(binding, 90, false)),
            );
        }
        if !self.feedback.is_empty() {
            column = column.child(
                div()
                    .id("submission-feedback")
                    .role(gpui::Role::Status)
                    .text_size(px(12.))
                    .child(self.feedback.clone()),
            );
        }
        column
    }
}

pub(super) struct FormPage {
    pub scope: ConfigScope,
    pub panels: BTreeMap<&'static str, Entity<FormPanel>>,
    anchors: Vec<ScrollAnchor>,
    _config: Subscription,
}
impl FormPage {
    pub fn new(
        scope: ConfigScope,
        anchors: Vec<ScrollAnchor>,
        scroll: ScrollHandle,
        window: &mut Window,
        cx: &mut Context<Self>,
    ) -> Self {
        let panels = DEMOS
            .iter()
            .map(|demo| {
                (
                    demo.id,
                    cx.new(|cx| FormPanel::new(demo.id, scope.clone(), scroll.clone(), window, cx)),
                )
            })
            .collect();
        Self {
            _config: scope.observe(cx),
            scope,
            panels,
            anchors,
        }
    }
}
impl Render for FormPage {
    fn render(&mut self, window: &mut Window, cx: &mut Context<Self>) -> impl IntoElement {
        examples::grid(&self.scope, window, cx)
            .id("gallery.form.examples")
            .children(DEMOS.iter().zip(&self.anchors).map(|(demo, anchor)| {
                examples::card(
                    "form",
                    demo,
                    anchor.clone(),
                    self.panels[demo.id].clone(),
                    &self.scope,
                    cx,
                )
            }))
    }
}
