// Copyright (C) 2026 mg-chao
// SPDX-License-Identifier: Apache-2.0

use super::*;
use crate::{
    ConfigScope, UiRoot,
    input_number::{ExactDecimal, NumericValue},
    locale::Locale,
};
use gpui::{
    App, AppContext, Context, Entity, IntoElement, Render, SharedString, Subscription,
    TestAppContext, Window, WindowHandle, div,
};
use std::{
    cell::RefCell,
    future::Future,
    pin::Pin,
    rc::Rc,
    task::{Context as TaskContext, Poll, Waker},
    time::Duration,
};

#[test]
fn form_values_preserve_nested_paths_native_types_and_snapshot_sharing() {
    #[derive(Clone, PartialEq)]
    struct Domain(u64);
    let path = NamePath::from("users").index(2).child("role");
    let old: FormValue = FormObject::new().into();
    let value = old
        .with_path(&path, FormValue::native(Domain(u64::MAX)))
        .unwrap();
    assert!(old.get(&path).is_none());
    assert_eq!(
        value.get(&path).unwrap().native_ref::<Domain>().unwrap().0,
        u64::MAX
    );
    assert!(
        value
            .get(&NamePath::from("users").index(0))
            .unwrap()
            .is_undefined()
    );
    assert!(value.get(&path).unwrap().native_ref::<u64>().is_err());
    let literal = value
        .with_path(&NamePath::from("users.2.role"), "literal".into())
        .unwrap();
    assert_eq!(
        literal
            .get(&NamePath::from("users.2.role"))
            .unwrap()
            .as_str(),
        Some("literal")
    );
    assert!(
        literal
            .get(&path)
            .unwrap()
            .ptr_eq(value.get(&path).unwrap())
    );
    assert!(
        FormValue::from("scalar")
            .with_path(&NamePath::from("child"), true.into())
            .is_err()
    );
}

#[test]
fn form_wide_persistent_snapshots_share_unaffected_native_leaves() {
    let original = FormValue::object(
        (0..2048).map(|index| (format!("field-{index}"), FormValue::native(vec![index; 8]))),
    );
    let unchanged = original.get(&NamePath::from("field-2000")).unwrap().clone();
    let mut current = original.clone();
    for index in 0..128 {
        current = current
            .with_path(&NamePath::from("field-1"), FormValue::native(index))
            .unwrap();
    }
    assert!(
        current
            .get(&NamePath::from("field-2000"))
            .unwrap()
            .ptr_eq(&unchanged)
    );
    assert_eq!(
        original
            .get(&NamePath::from("field-1"))
            .unwrap()
            .native_ref::<Vec<i32>>()
            .unwrap()[0],
        1
    );
}

#[test]
fn form_codecs_distinguish_unset_null_and_type_errors() {
    let codec = FormCodec::text();
    assert_eq!(codec.decode_presence(None).unwrap(), FormPresence::Unset);
    assert_eq!(
        codec.decode_presence(Some(&FormValue::null())).unwrap(),
        FormPresence::Null
    );
    assert_eq!(
        codec.decode_presence(Some(&"".into())).unwrap(),
        FormPresence::Value(SharedString::default())
    );
    assert!(codec.decode(&true.into()).is_err());
    let numbers = FormCodec::numeric().array();
    let precise: ExactDecimal = "9007199254740993.123456789".parse().unwrap();
    let values = vec![NumericValue::Decimal(precise)];
    assert_eq!(
        numbers.decode(&numbers.encode(values.clone())).unwrap(),
        values
    );
}

fn request(value: FormValue) -> FormValidationRequest {
    FormValidationRequest {
        field: FieldId(1),
        name: "value".into(),
        label: "Value".into(),
        value,
        values: FormSnapshot::empty(),
        trigger: ValidationTrigger::Programmatic,
        messages: Locale::en_us().form.clone(),
        variables: Default::default(),
    }
}

#[test]
fn form_rules_validate_exact_numbers_unicode_lengths_types_and_messages() {
    assert!(FormRule::required().check(&request(false.into())).is_ok());
    assert!(
        FormRule::required()
            .check(&request(FormValue::null()))
            .is_err()
    );
    assert!(
        FormRule::string()
            .len(2.)
            .check(&request("😀a".into()))
            .is_ok()
    );
    assert!(
        FormRule::string()
            .whitespace(true)
            .check(&request("  \t".into()))
            .is_err()
    );
    assert!(
        FormRule::string()
            .whitespace(true)
            .check(&request("\u{feff}".into()))
            .is_err()
    );
    assert!(
        FormRule::string()
            .whitespace(true)
            .check(&request("\u{85}".into()))
            .is_ok()
    );
    assert!(
        FormRule::email()
            .check(&request("person@example.com".into()))
            .is_ok()
    );
    assert!(
        FormRule::email()
            .check(&request("not-an-email".into()))
            .is_err()
    );
    assert!(
        FormRule::url()
            .check(&request("https://example.com/path".into()))
            .is_ok()
    );
    for valid in [
        "www.example.com",
        "//example.com/path",
        "custom://example.com",
        "http://localhost:3000",
    ] {
        assert!(
            FormRule::url().check(&request(valid.into())).is_ok(),
            "{valid}"
        );
    }
    for invalid in [
        "example.com",
        "https://example",
        "https://example.c",
        "https://example.com/a b",
    ] {
        assert!(
            FormRule::url().check(&request(invalid.into())).is_err(),
            "{invalid}"
        );
    }
    let bound: ExactDecimal = "9007199254740993.1".parse().unwrap();
    let below: ExactDecimal = "9007199254740993.09".parse().unwrap();
    assert!(
        FormRule::number()
            .decimal_min(bound)
            .check(&request(below.into()))
            .is_err()
    );
    let rule = FormRule::required().message("${label} needs a value");
    assert_eq!(
        rule.check(&request(FormValue::undefined()))
            .unwrap_err()
            .as_ref(),
        "Value needs a value"
    );
    assert!(
        FormRule::new()
            .enum_values(["red", "blue"])
            .check(&request("green".into()))
            .is_err()
    );
}

#[test]
fn form_message_variables_escape_once_and_do_not_reinterpret_replacement_text() {
    let mut value = request(FormValue::undefined());
    value.variables.insert("alias".into(), "${label}".into());
    let error = FormRule::required()
        .message(r"${label} / \${label} / ${alias} / ${missing}")
        .check(&value)
        .unwrap_err();
    assert_eq!(error.as_ref(), "Value / ${label} / ${label} / undefined");
}

struct Consumer {
    events: Rc<RefCell<Vec<FormEvent>>>,
    _subscription: Subscription,
}
impl Render for Consumer {
    fn render(&mut self, _: &mut Window, _: &mut Context<Self>) -> impl IntoElement {
        div()
    }
}
struct Fixture {
    cx: TestAppContext,
    window: WindowHandle<UiRoot>,
    form: Entity<FormState>,
    events: Rc<RefCell<Vec<FormEvent>>>,
}
impl Fixture {
    fn new() -> Self {
        let mut cx = TestAppContext::single();
        let mut result = None;
        let window = cx.add_window(|window, cx| {
            let scope = ConfigScope::new_window(cx);
            let form = cx.new(|cx| FormState::new(window, cx));
            form.update(cx, |form, cx| form.set_scope(scope.clone(), cx));
            let events = Rc::new(RefCell::new(Vec::new()));
            let consumer = cx.new(|cx| Consumer {
                events: events.clone(),
                _subscription: cx.subscribe(&form, |this: &mut Consumer, _, event, _| {
                    this.events.borrow_mut().push(event.clone())
                }),
            });
            result = Some((form, events));
            UiRoot::new(scope, consumer, window, cx)
        });
        let (form, events) = result.unwrap();
        Self {
            cx,
            window,
            form,
            events,
        }
    }
    fn update<R>(
        &mut self,
        f: impl FnOnce(&mut FormState, &mut Window, &mut Context<FormState>) -> R,
    ) -> R {
        let form = self.form.clone();
        let result = self
            .cx
            .update_window(self.window.into(), |_, window, cx| {
                form.update(cx, |form, cx| f(form, window, cx))
            })
            .unwrap();
        self.cx.run_until_parked();
        result
    }
    fn app<R>(&mut self, f: impl FnOnce(&mut Window, &mut App) -> R) -> R {
        let result = self
            .cx
            .update_window(self.window.into(), |_, window, cx| f(window, cx))
            .unwrap();
        self.cx.run_until_parked();
        result
    }
    fn field(&mut self, name: &str, options: FieldOptions) -> FormField<SharedString> {
        self.update(|form, _, cx| form.register(name, FormCodec::text(), options, cx).unwrap())
    }
    fn meta<T: 'static>(&self, field: &FormField<T>) -> FieldMeta {
        field
            .handle()
            .state()
            .read_with(&self.cx, |field, _| field.meta().clone())
    }
    fn advance(&mut self, millis: u64) {
        self.cx
            .executor()
            .advance_clock(Duration::from_millis(millis));
        self.cx.run_until_parked();
    }
}
fn outcome(mut handle: ValidationHandle) -> Poll<Result<FormSnapshot, FormValidationError>> {
    Pin::new(&mut handle).poll(&mut TaskContext::from_waker(Waker::noop()))
}

#[test]
fn form_initial_precedence_silent_programmatic_changes_and_reset_metadata() {
    let mut fixture = Fixture::new();
    fixture.update(|form, _, cx| {
        form.set_initial_values(FormValue::object([("name", "Form")]), cx)
            .unwrap()
    });
    let field = fixture.field("name", FieldOptions::default().initial_value("Item"));
    fixture.app(|_, cx| assert_eq!(field.read(cx).unwrap(), FormPresence::Value("Form".into())));
    assert!(!fixture.meta(&field).touched);
    assert!(fixture.meta(&field).dirty);
    fixture.app(|window, cx| {
        field
            .set(FormPresence::Value("Form".into()), window, cx)
            .unwrap()
    });
    assert!(fixture.meta(&field).touched);
    assert!(
        !fixture
            .events
            .borrow()
            .iter()
            .any(|event| matches!(event, FormEvent::ValuesChange { .. }))
    );
    let handle = field.handle();
    let before = handle
        .state()
        .read_with(&fixture.cx, |state, _| state.reset_epoch());
    fixture.update(|form, window, cx| form.reset(window, cx));
    assert!(!fixture.meta(&field).touched);
    assert_eq!(
        handle
            .state()
            .read_with(&fixture.cx, |state, _| state.reset_epoch()),
        before + 1
    );
}

#[test]
fn form_validation_warning_only_validate_only_and_dirty_selection() {
    let mut fixture = Fixture::new();
    let field = fixture.field("name", FieldOptions::default().rule(FormRule::required()));
    let hidden = fixture.update(|form, window, cx| {
        form.validate_fields(
            ValidateOptions {
                validate_only: true,
                ..Default::default()
            },
            window,
            cx,
        )
    });
    assert!(matches!(outcome(hidden), Poll::Ready(Err(_))));
    assert!(fixture.meta(&field).errors.is_empty());
    assert!(!fixture.meta(&field).validated);
    let visible = fixture
        .update(|form, window, cx| form.validate_fields(ValidateOptions::default(), window, cx));
    assert!(matches!(outcome(visible), Poll::Ready(Err(_))));
    assert!(!fixture.meta(&field).errors.is_empty());
    fixture.update(|form, _, cx| {
        form.set_field_options(
            field.handle().id(),
            FieldOptions::default().rule(FormRule::required().warning_only(true)),
            cx,
        )
        .unwrap()
    });
    let warning = fixture.update(|form, window, cx| form.submit(window, cx));
    assert!(matches!(outcome(warning), Poll::Ready(Ok(_))));
    assert!(fixture.meta(&field).errors.is_empty());
    assert!(!fixture.meta(&field).warnings.is_empty());
}

#[test]
fn form_async_validation_discards_changed_and_reset_values() {
    let mut fixture = Fixture::new();
    let rule = FormRule::custom(|request, _, cx| {
        let timer = cx.background_executor().timer(Duration::from_millis(100));
        FormRuleResult::Pending(cx.spawn(async move |_| {
            timer.await;
            if request.value.as_str() == Some("taken") {
                Err("Already used".into())
            } else {
                Ok(())
            }
        }))
    });
    let field = fixture.field("name", FieldOptions::default().rule(rule));
    fixture.app(|window, cx| {
        field
            .change(
                FormPresence::Value("taken".into()),
                ValidationTrigger::Change,
                window,
                cx,
            )
            .unwrap()
    });
    assert!(fixture.meta(&field).validating);
    fixture.app(|window, cx| {
        field
            .change(
                FormPresence::Value("free".into()),
                ValidationTrigger::Change,
                window,
                cx,
            )
            .unwrap()
    });
    fixture.advance(100);
    assert!(fixture.meta(&field).errors.is_empty());
    assert!(!fixture.meta(&field).validating);
    fixture.app(|window, cx| {
        field
            .change(
                FormPresence::Value("taken".into()),
                ValidationTrigger::Change,
                window,
                cx,
            )
            .unwrap()
    });
    let pending = fixture
        .update(|form, window, cx| form.validate_fields(ValidateOptions::default(), window, cx));
    fixture.update(|form, window, cx| form.reset(window, cx));
    fixture.advance(100);
    assert!(matches!(
        outcome(pending),
        Poll::Ready(Err(FormValidationError {
            out_of_date: true,
            ..
        }))
    ));
    assert!(fixture.meta(&field).errors.is_empty());
    assert!(!fixture.meta(&field).validated);
}

#[test]
fn form_watchers_are_narrow_and_silent_setters_notify_them() {
    let mut fixture = Fixture::new();
    let first = fixture.field("first", FieldOptions::default());
    let second = fixture.field("second", FieldOptions::default());
    let watch = fixture.update(|form, _, cx| form.watch("first", FormCodec::text(), cx));
    fixture.app(|window, cx| {
        second
            .set(FormPresence::Value("other".into()), window, cx)
            .unwrap()
    });
    assert_eq!(watch.read_with(&fixture.cx, |watch, _| watch.revision()), 0);
    fixture.app(|window, cx| {
        first
            .set(FormPresence::Value("selected".into()), window, cx)
            .unwrap()
    });
    assert_eq!(watch.read_with(&fixture.cx, |watch, _| watch.revision()), 1);
    assert_eq!(
        watch
            .read_with(&fixture.cx, |watch, _| watch.value().clone())
            .unwrap(),
        FormPresence::Value("selected".into())
    );
}

#[test]
fn form_list_moves_preserve_field_identity_and_metadata() {
    let mut fixture = Fixture::new();
    let list = fixture.update(|form, _, cx| {
        form.register_list(
            "users",
            Some(
                [
                    FormValue::object([("name", "A")]),
                    FormValue::object([("name", "B")]),
                ]
                .into_iter()
                .collect(),
            ),
            cx,
        )
        .unwrap()
    });
    let first = fixture.update(|form, _, cx| {
        form.register(
            NamePath::from("users").index(0).child("name"),
            FormCodec::text(),
            FieldOptions::default(),
            cx,
        )
        .unwrap()
    });
    let before = fixture.app(|_, cx| list.items(cx));
    fixture.update(|form, window, cx| {
        form.set_fields(
            [FieldPatch {
                name: first.name(cx),
                touched: Some(true),
                errors: Some(vec!["Check A".into()]),
                ..Default::default()
            }],
            window,
            cx,
        )
        .unwrap()
    });
    fixture.app(|window, cx| list.move_item(0, 1, window, cx).unwrap());
    fixture.app(|_, cx| {
        assert_eq!(
            first.name(cx),
            NamePath::from("users").index(1).child("name")
        );
        assert_eq!(first.read(cx).unwrap(), FormPresence::Value("A".into()));
        assert_eq!(list.items(cx)[1].key, before[0].key);
    });
    assert_eq!(
        fixture.meta(&first).errors,
        vec![SharedString::from("Check A")]
    );
    fixture.app(|window, cx| list.remove([1, 1, 99], window, cx).unwrap());
    assert_eq!(fixture.app(|_, cx| list.items(cx).len()), 1);
}

#[test]
fn form_path_index_only_returns_overlapping_paths() {
    let mut index = super::model::PathIndex::default();
    index.insert(&NamePath::from("user").child("name"), FieldId(1));
    index.insert(&NamePath::from("other"), FieldId(2));
    assert_eq!(
        index
            .intersects(&"user".into())
            .into_iter()
            .collect::<Vec<_>>(),
        vec![FieldId(1)]
    );
    assert!(index.intersects(&"missing".into()).is_empty());
    index.remove(&NamePath::from("user").child("name"), FieldId(1));
    assert!(
        index
            .intersects(&NamePath::root())
            .iter()
            .all(|id| *id == FieldId(2))
    );
    index.insert(&NamePath::from("user").child("name"), FieldId(3));
    index.insert(&NamePath::from("user").child("age"), FieldId(3));
    index.remove(&NamePath::from("user").child("name"), FieldId(3));
    assert!(index.intersects(&NamePath::root()).contains(&FieldId(3)));
}

#[test]
fn form_conditional_fields_preserve_values_and_unregister_metadata() {
    let mut fixture = Fixture::new();
    let preserved = fixture.field(
        "preserved",
        FieldOptions::default().rule(FormRule::required()),
    );
    let discarded = fixture.field("discarded", FieldOptions::default().preserve(false));
    fixture.app(|window, cx| {
        preserved.handle().mount(cx);
        discarded.handle().mount(cx);
        preserved
            .set(FormPresence::Value("saved".into()), window, cx)
            .unwrap();
        discarded
            .set(FormPresence::Value("temporary".into()), window, cx)
            .unwrap();
        preserved.handle().unmount(cx);
        discarded.handle().unmount(cx);
    });
    fixture.update(|form, _, _| {
        assert_eq!(form.get_field_value("preserved").as_str(), Some("saved"));
        assert!(form.get_field_value("discarded").is_undefined());
        assert!(form.field_errors().is_empty());
        assert!(form.get_fields_value(false).as_object().unwrap().is_empty());
    });
    fixture.app(|_, cx| preserved.handle().mount(cx));
    assert!(!fixture.meta(&preserved).touched);
    fixture.app(|_, cx| {
        assert_eq!(
            preserved.read(cx).unwrap(),
            FormPresence::Value("saved".into())
        )
    });
}

#[test]
fn form_error_rules_run_before_warnings_and_serial_first_stops() {
    let mut fixture = Fixture::new();
    let calls = Rc::new(RefCell::new(Vec::new()));
    let error_calls = calls.clone();
    let warning_calls = calls.clone();
    let options = FieldOptions::default()
        .validate_first(RuleMode::SerialFirst)
        .rule(
            FormRule::custom(move |_, _, _| {
                warning_calls.borrow_mut().push("warning");
                FormRuleResult::Ready(Err("Warning".into()))
            })
            .warning_only(true),
        )
        .rule(FormRule::custom(move |_, _, _| {
            error_calls.borrow_mut().push("error");
            FormRuleResult::Ready(Err("Error".into()))
        }));
    let field = fixture.field("name", options);
    fixture.update(|form, window, cx| form.validate_fields(ValidateOptions::default(), window, cx));
    assert_eq!(&*calls.borrow(), &["error"]);
    assert_eq!(
        fixture.meta(&field).errors,
        vec![SharedString::from("Error")]
    );
    assert!(fixture.meta(&field).warnings.is_empty());
}

#[test]
fn form_debounce_is_event_only_and_explicit_validation_bypasses_it() {
    let mut fixture = Fixture::new();
    let calls = Rc::new(RefCell::new(0));
    let count = calls.clone();
    let field = fixture.field(
        "name",
        FieldOptions::default()
            .debounce(Duration::from_secs(1))
            .rule(FormRule::custom(move |_, _, _| {
                *count.borrow_mut() += 1;
                FormRuleResult::Ready(Ok(()))
            })),
    );
    fixture.app(|window, cx| {
        field
            .change(
                FormPresence::Value("first".into()),
                ValidationTrigger::Change,
                window,
                cx,
            )
            .unwrap()
    });
    assert_eq!(*calls.borrow(), 0);
    fixture.update(|form, window, cx| form.validate_fields(ValidateOptions::default(), window, cx));
    assert_eq!(*calls.borrow(), 1);
    fixture.advance(1000);
    assert_eq!(*calls.borrow(), 1);
}

#[test]
fn form_dependencies_revalidate_only_affected_dirty_fields() {
    let mut fixture = Fixture::new();
    let password = fixture.field("password", FieldOptions::default());
    let calls = Rc::new(RefCell::new(0));
    let count = calls.clone();
    let confirm = fixture.field(
        "confirm",
        FieldOptions::default()
            .initial_value("initial")
            .dependencies(["password"])
            .rule(FormRule::custom(move |request, _, _| {
                *count.borrow_mut() += 1;
                FormRuleResult::Ready(
                    if request.values.get(&NamePath::from("password")) == Some(&request.value) {
                        Ok(())
                    } else {
                        Err("Passwords differ".into())
                    },
                )
            })),
    );
    let unrelated = fixture.field("other", FieldOptions::default());
    fixture.app(|window, cx| {
        unrelated
            .change(
                FormPresence::Value("ignored".into()),
                ValidationTrigger::Change,
                window,
                cx,
            )
            .unwrap()
    });
    assert_eq!(*calls.borrow(), 0);
    fixture.app(|window, cx| {
        password
            .change(
                FormPresence::Value("new".into()),
                ValidationTrigger::Change,
                window,
                cx,
            )
            .unwrap()
    });
    assert_eq!(*calls.borrow(), 1);
    assert!(!fixture.meta(&confirm).errors.is_empty());
}

#[test]
fn form_provider_coordinates_independent_forms_with_named_snapshots() {
    let mut fixture = Fixture::new();
    let form = fixture.form.clone();
    let provider = fixture.app(|_, cx| cx.new(FormProvider::new));
    fixture
        .app(|_, cx| provider.update(cx, |provider, cx| provider.register("profile", &form, cx)));
    fixture.update(|form, window, cx| {
        form.set_fields_value(FormValue::object([("name", "Ada")]), window, cx)
            .unwrap()
    });
    fixture.app(|_, cx| {
        let provider = provider.read(cx);
        assert_eq!(provider.form("profile"), Some(form));
        assert_eq!(
            provider.snapshots(cx)["profile"]
                .get(&NamePath::from("name"))
                .unwrap()
                .as_str(),
            Some("Ada")
        );
    });
}

#[test]
fn form_normalization_is_synchronous_and_only_applies_to_user_changes() {
    let mut fixture = Fixture::new();
    let field = fixture.field(
        "name",
        FieldOptions::default().normalize(|value, previous, values| {
            assert_eq!(
                values.get(&NamePath::from("name")),
                (!previous.is_undefined()).then_some(previous)
            );
            FormValue::text(value.as_str().unwrap().trim().to_uppercase())
        }),
    );
    fixture.app(|window, cx| {
        field
            .change(
                FormPresence::Value("  ada  ".into()),
                ValidationTrigger::Change,
                window,
                cx,
            )
            .unwrap()
    });
    fixture.app(|_, cx| assert_eq!(field.read(cx).unwrap(), FormPresence::Value("ADA".into())));
    fixture.app(|window, cx| {
        field
            .set(FormPresence::Value("  direct  ".into()), window, cx)
            .unwrap()
    });
    fixture.app(|_, cx| {
        assert_eq!(
            field.read(cx).unwrap(),
            FormPresence::Value("  direct  ".into())
        )
    });
}

#[test]
fn form_dynamic_required_marks_ignore_warning_only_rules() {
    let mut fixture = Fixture::new();
    let enabled = fixture.update(|form, _, cx| {
        form.register(
            "enabled",
            FormCodec::boolean(),
            FieldOptions::default().initial_value(false),
            cx,
        )
        .unwrap()
    });
    let field = fixture.field(
        "value",
        FieldOptions::default()
            .dependencies(["enabled"])
            .rule(FormRule::dynamic(|snapshot| {
                if snapshot
                    .get(&NamePath::from("enabled"))
                    .and_then(FormValue::as_bool)
                    == Some(true)
                {
                    FormRule::required()
                } else {
                    FormRule::new()
                }
            })),
    );
    assert!(!fixture.meta(&field).required);
    fixture.app(|window, cx| {
        enabled
            .change(
                FormPresence::Value(true),
                ValidationTrigger::Change,
                window,
                cx,
            )
            .unwrap()
    });
    assert!(fixture.meta(&field).required);
    fixture.update(|form, _, cx| {
        form.set_field_options(
            field.handle().id(),
            FieldOptions::default().rule(FormRule::required().warning_only(true)),
            cx,
        )
        .unwrap()
    });
    assert!(!fixture.meta(&field).required);
}

#[test]
fn form_validation_templates_accept_scoped_messages_and_field_variables() {
    let mut fixture = Fixture::new();
    fixture.update(|form, _, cx| {
        form.set_validate_messages(
            crate::locale::FormMessagePatch {
                required: Some("${label}: ${purpose}".into()),
                ..Default::default()
            },
            cx,
        )
    });
    let field = fixture.field(
        "value",
        FieldOptions::default()
            .label("Name")
            .message_variable("purpose", "required for this profile")
            .rule(FormRule::required()),
    );
    fixture.update(|form, window, cx| form.validate_fields(ValidateOptions::default(), window, cx));
    assert_eq!(
        fixture.meta(&field).errors,
        vec![SharedString::from("Name: required for this profile")]
    );
}

#[test]
fn form_provider_moves_and_renames_without_stale_registrations() {
    let mut fixture = Fixture::new();
    let form = fixture.form.clone();
    let first = fixture.app(|_, cx| cx.new(FormProvider::new));
    let second = fixture.app(|_, cx| cx.new(FormProvider::new));
    fixture.app(|_, cx| {
        first.update(cx, |provider, cx| provider.register("old", &form, cx));
        first.update(cx, |provider, cx| provider.register("renamed", &form, cx));
        assert!(first.read(cx).form("old").is_none());
        second.update(cx, |provider, cx| provider.register("new", &form, cx));
        assert!(first.read(cx).form("renamed").is_none());
        assert_eq!(second.read(cx).form("new"), Some(form));
    });
}

#[test]
fn form_no_rule_validation_keeps_pristine_field_metadata_pristine() {
    let mut fixture = Fixture::new();
    let field = fixture.field("plain", FieldOptions::default());
    let validation = fixture
        .update(|form, window, cx| form.validate_fields(ValidateOptions::default(), window, cx));
    assert!(matches!(outcome(validation), Poll::Ready(Ok(_))));
    assert!(!fixture.meta(&field).validated);
    assert!(!fixture.meta(&field).dirty);
}

#[test]
fn form_list_reset_and_silent_replacement_reconcile_keys_and_lengths() {
    let mut fixture = Fixture::new();
    let list = fixture.update(|form, _, cx| {
        form.register_list(
            "rows",
            Some(
                [FormValue::object([("name", "Initial")])]
                    .into_iter()
                    .collect(),
            ),
            cx,
        )
        .unwrap()
    });
    fixture.app(|window, cx| {
        list.add(FormValue::object([("name", "Added")]), None, window, cx)
            .unwrap();
    });
    let added = fixture.update(|form, _, cx| {
        form.register(
            NamePath::from("rows").index(1).child("name"),
            FormCodec::text(),
            FieldOptions::default().initial_value("Ignored inside list"),
            cx,
        )
        .unwrap()
    });
    fixture.update(|form, window, cx| form.reset(window, cx));
    assert_eq!(fixture.app(|_, cx| list.items(cx).len()), 1);
    fixture
        .update(|form, _, _| assert_eq!(form.get_field_value("rows").as_array().unwrap().len(), 1));
    fixture.app(|_, cx| assert_eq!(added.read(cx).unwrap(), FormPresence::Unset));
    fixture.update(|form, window, cx| {
        form.set_fields_value(
            FormValue::object([(
                "rows",
                FormValue::array([
                    FormValue::object([("name", "One")]),
                    FormValue::object([("name", "Two")]),
                    FormValue::object([("name", "Three")]),
                ]),
            )]),
            window,
            cx,
        )
        .unwrap()
    });
    assert_eq!(fixture.app(|_, cx| list.items(cx).len()), 3);
}

#[test]
fn form_clear_on_destroy_survives_parent_before_child_release_order() {
    let mut fixture = Fixture::new();
    fixture.update(|form, _, cx| {
        form.connect();
        form.clear_on_destroy(true);
        form.set_initial_values(FormValue::object([("name", "Initial")]), cx)
            .unwrap();
    });
    let field = fixture.field("name", FieldOptions::default().preserve(false));
    fixture.app(|_, cx| field.handle().mount(cx));
    fixture.update(|form, _, cx| form.disconnect(cx));
    fixture.app(|_, cx| field.handle().unmount(cx));
    fixture
        .update(|form, _, _| assert!(form.get_fields_value(true).as_object().unwrap().is_empty()));
    fixture.update(|form, _, _| form.connect());
    fixture.app(|_, cx| field.handle().mount(cx));
    fixture.app(|_, cx| {
        assert_eq!(
            field.read(cx).unwrap(),
            FormPresence::Value("Initial".into())
        )
    });
}

#[test]
fn form_exact_setter_touches_only_exact_metadata_and_syncs_nested_values() {
    let mut fixture = Fixture::new();
    let parent = fixture.update(|form, _, cx| {
        form.register("user", FormCodec::value(), FieldOptions::default(), cx)
            .unwrap()
    });
    let child = fixture.update(|form, _, cx| {
        form.register(
            NamePath::from("user").child("name"),
            FormCodec::text(),
            FieldOptions::default(),
            cx,
        )
        .unwrap()
    });
    fixture.update(|form, window, cx| {
        form.set_field_value("user", FormValue::object([("name", "Ada")]), window, cx)
            .unwrap()
    });
    assert!(fixture.meta(&parent).touched);
    assert!(!fixture.meta(&child).touched);
    fixture.app(|_, cx| assert_eq!(child.read(cx).unwrap(), FormPresence::Value("Ada".into())));
    fixture.update(|form, window, cx| {
        form.set_fields(
            [FieldPatch {
                name: "user".into(),
                value: Some(FormValue::object([("name", "Grace")])),
                ..Default::default()
            }],
            window,
            cx,
        )
        .unwrap()
    });
    fixture.app(|_, cx| assert_eq!(child.read(cx).unwrap(), FormPresence::Value("Grace".into())));
}

#[test]
fn form_touched_queries_use_descendant_groups_and_ignore_list_records_for_global_all() {
    let mut fixture = Fixture::new();
    fixture.update(|form, _, _| {
        assert!(form.is_fields_touched(None, true));
        assert!(!form.is_fields_touched(None, false));
        assert!(form.is_fields_touched(Some(&[]), true));
        assert!(!form.is_fields_touched(Some(&[]), false));
    });
    fixture.update(|form, _, cx| {
        form.register_list("users", None, cx).unwrap();
        form.register(
            NamePath::from("user").child("first"),
            FormCodec::text(),
            FieldOptions::default(),
            cx,
        )
        .unwrap();
        form.register(
            NamePath::from("user").child("last"),
            FormCodec::text(),
            FieldOptions::default(),
            cx,
        )
        .unwrap();
        form.register(
            NamePath::from("preferences").child("color"),
            FormCodec::text(),
            FieldOptions::default(),
            cx,
        )
        .unwrap();
    });
    fixture.update(|form, window, cx| {
        form.set_fields(
            [FieldPatch {
                name: NamePath::from("user").child("first"),
                touched: Some(true),
                ..Default::default()
            }],
            window,
            cx,
        )
        .unwrap();
        assert!(form.is_fields_touched(Some(&["user".into()]), true));
        assert!(!form.is_fields_touched(Some(&["user".into(), "preferences".into()]), true));
        assert!(form.is_fields_touched(Some(&["user".into(), "preferences".into()]), false));
        assert!(!form.is_fields_touched(Some(&["missing".into()]), true));
        assert!(!form.is_fields_touched(Some(&["users".into()]), true));
        assert!(!form.is_fields_touched(None, true));
        form.set_fields(
            [
                FieldPatch {
                    name: NamePath::from("user").child("last"),
                    touched: Some(true),
                    ..Default::default()
                },
                FieldPatch {
                    name: NamePath::from("preferences").child("color"),
                    touched: Some(true),
                    ..Default::default()
                },
            ],
            window,
            cx,
        )
        .unwrap();
        assert!(
            form.is_fields_touched(None, true),
            "the untouched list container does not block global all"
        );
        assert!(form.is_fields_touched(Some(&["user".into(), "preferences".into()]), true));
    });
}

#[test]
fn form_sync_and_async_validator_messages_apply_scoped_variables_and_escapes_once() {
    for asynchronous in [false, true] {
        let mut fixture = Fixture::new();
        let field = fixture.field(
            "account",
            FieldOptions::default()
                .label("Account")
                .message_variable("alias", "${name}")
                .initial_value("present")
                .rule(FormRule::custom(move |_, _, cx| {
                    let error = SharedString::from(r"${label}: \${label} / ${alias}");
                    if asynchronous {
                        FormRuleResult::Pending(cx.spawn(async move |cx| {
                            cx.background_executor()
                                .timer(Duration::from_millis(10))
                                .await;
                            Err(error)
                        }))
                    } else {
                        FormRuleResult::Ready(Err(error))
                    }
                })),
        );
        let result = fixture.update(|form, window, cx| {
            form.validate_fields(ValidateOptions::default(), window, cx)
        });
        fixture.advance(20);
        assert!(matches!(outcome(result), Poll::Ready(Err(_))));
        assert_eq!(
            fixture.meta(&field).errors,
            vec![SharedString::from("Account: ${label} / ${name}")]
        );
    }
}

#[test]
fn form_native_float_hex_phone_regexp_and_method_rules_are_type_aware() {
    assert!(
        FormRule::new()
            .enum_values(Vec::<FormValue>::new())
            .check(&request("anything".into()))
            .is_err()
    );
    assert!(FormRule::float().check(&request(1.25.into())).is_ok());
    assert!(FormRule::float().check(&request(1.0.into())).is_err());
    assert!(FormRule::hex().check(&request("#AbC123".into())).is_ok());
    assert!(FormRule::hex().check(&request("#abcd".into())).is_err());
    assert!(
        FormRule::tel()
            .check(&request("+86 13800000000".into()))
            .is_ok()
    );
    assert!(
        FormRule::tel()
            .check(&request("not a telephone".into()))
            .is_err()
    );
    assert!(FormRule::regexp().check(&request("[a-z]+".into())).is_ok());
    assert!(FormRule::regexp().check(&request("(".into())).is_err());
    assert!(
        FormRule::regexp()
            .check(&request(FormValue::native(
                FormRegexp::new("(?i)^native$").unwrap()
            )))
            .is_ok()
    );
    let calls = Rc::new(RefCell::new(0));
    let counter = calls.clone();
    let method = FormMethod::new(move |_| {
        *counter.borrow_mut() += 1;
        true.into()
    });
    assert!(
        FormRule::method()
            .check(&request(FormValue::native(method)))
            .is_ok()
    );
    assert_eq!(*calls.borrow(), 0);
    assert!(
        FormRule::method()
            .check(&request("function".into()))
            .is_err()
    );
    assert!(
        FormRule::email()
            .check(&request("a..b@example.com".into()))
            .is_err()
    );
    assert!(
        FormRule::url()
            .check(&request("//example.com/path".into()))
            .is_ok()
    );
}

#[test]
fn form_nested_object_rules_collect_all_messages_and_explicit_fields_override_defaults() {
    let mut fixture = Fixture::new();
    let field = fixture.update(|form, _, cx| {
        form.register(
            "profile",
            FormCodec::value(),
            FieldOptions::default()
                .initial_value(FormValue::object([
                    ("name", FormValue::from("x")),
                    ("count", FormValue::from(7.)),
                ]))
                .rule(
                    FormRule::object()
                        .default_field(FormRule::string().min(3.))
                        .field("count", [FormRule::number().min(0.)])
                        .field("email", [FormRule::required()]),
                ),
            cx,
        )
        .unwrap()
    });
    let validation = fixture
        .update(|form, window, cx| form.validate_fields(ValidateOptions::default(), window, cx));
    assert!(matches!(outcome(validation), Poll::Ready(Err(_))));
    let errors = fixture.meta(&field).errors;
    assert_eq!(errors.len(), 2);
    assert!(errors.iter().any(|error| error.contains("profile.name")));
    assert!(errors.iter().any(|error| error.contains("profile.email")));
    assert!(!errors.iter().any(|error| error.contains("count")));
}

#[test]
fn form_nested_async_array_rules_receive_full_paths_and_cancel_stale_results() {
    let mut fixture = Fixture::new();
    let paths = Rc::new(RefCell::new(Vec::new()));
    let seen = paths.clone();
    let child = FormRule::custom(move |request, _, cx| {
        seen.borrow_mut().push(request.name.to_string());
        let timer = cx.background_executor().timer(Duration::from_millis(50));
        FormRuleResult::Pending(cx.spawn(async move |_| {
            timer.await;
            if request.value.as_str() == Some("bad") {
                Err("Email is unavailable".into())
            } else {
                Ok(())
            }
        }))
    });
    let field = fixture.update(|form, _, cx| {
        form.register(
            "users",
            FormCodec::value(),
            FieldOptions::default()
                .initial_value(FormValue::array([
                    FormValue::object([("email", "good")]),
                    FormValue::object([("email", "bad")]),
                ]))
                .rule(
                    FormRule::array()
                        .default_field(FormRule::object().field("email", [child]))
                        .warning_only(true),
                ),
            cx,
        )
        .unwrap()
    });
    let first = fixture
        .update(|form, window, cx| form.validate_fields(ValidateOptions::default(), window, cx));
    fixture.advance(50);
    assert!(matches!(outcome(first), Poll::Ready(Ok(_))));
    assert_eq!(&*paths.borrow(), &["users[0].email", "users[1].email"]);
    assert_eq!(fixture.meta(&field).warnings.len(), 1);
    let stale = fixture
        .update(|form, window, cx| form.validate_fields(ValidateOptions::default(), window, cx));
    fixture.app(|window, cx| {
        field
            .set(
                FormPresence::Value(FormValue::array([FormValue::object([("email", "new")])])),
                window,
                cx,
            )
            .unwrap()
    });
    fixture.advance(50);
    assert!(matches!(
        outcome(stale),
        Poll::Ready(Err(FormValidationError {
            out_of_date: true,
            ..
        }))
    ));
    assert!(fixture.meta(&field).warnings.is_empty());
}

#[test]
fn form_nested_serial_first_skips_later_validator_and_optional_missing_parent() {
    let mut fixture = Fixture::new();
    let calls = Rc::new(RefCell::new(Vec::new()));
    let first = calls.clone();
    let second = calls.clone();
    let rule = FormRule::object()
        .nested_mode(RuleMode::SerialFirst)
        .field(
            "a",
            [FormRule::custom(move |_, _, _| {
                first.borrow_mut().push("a");
                FormRuleResult::Ready(Err("First".into()))
            })],
        )
        .field(
            "b",
            [FormRule::custom(move |_, _, _| {
                second.borrow_mut().push("b");
                FormRuleResult::Ready(Err("Second".into()))
            })],
        );
    let field = fixture.update(|form, _, cx| {
        form.register(
            "object",
            FormCodec::value(),
            FieldOptions::default().rule(rule),
            cx,
        )
        .unwrap()
    });
    let missing = fixture
        .update(|form, window, cx| form.validate_fields(ValidateOptions::default(), window, cx));
    assert!(matches!(outcome(missing), Poll::Ready(Ok(_))));
    assert!(calls.borrow().is_empty());
    fixture.app(|window, cx| {
        field
            .set(
                FormPresence::Value(FormValue::object([("a", "one"), ("b", "two")])),
                window,
                cx,
            )
            .unwrap()
    });
    fixture.update(|form, window, cx| form.validate_fields(ValidateOptions::default(), window, cx));
    assert_eq!(&*calls.borrow(), &["a"]);
    assert_eq!(
        fixture.meta(&field).errors,
        vec![SharedString::from("First")]
    );
}
