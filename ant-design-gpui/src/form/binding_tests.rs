// Copyright (C) 2026 mg-chao
// SPDX-License-Identifier: Apache-2.0

//! Exercise adapters through native editors, focus and keyboard dispatch.

use super::*;
use crate::{
    Checkbox, CheckboxGroup, CheckboxOption, ColorPicker, ColorPickerState, ColorPickerValue,
    ConfigScope, Input, InputNumber, InputNumberState, InputState, OtpInput, OtpState,
    PasswordInput, RadioGroup, RadioOption, Segmented, SegmentedOption, Select, SelectOption,
    SelectOptions, SelectSettings, SelectState, SelectValue, Slider, SliderState, SliderValue,
    Switch, UiRoot, Variant,
    date_picker::{
        DateRangeSelection, DateSelection, DateTimeRangeSelection, DateTimeSelection,
        MultipleDateSelection, NaiveDate, NaiveDateTime, NaiveTime, Picker, PickerRange,
        PickerSelection, PickerState, TimeRangeSelection, TimeSelection,
    },
    input_number::{ExactDecimal, NumberOptions, NumericMode, NumericValue},
};
use gpui::{
    AnyElement, App, AppContext, Background, Bounds, Context, Entity, EntityInputHandler,
    IntoElement, KeyDownEvent, KeyUpEvent, Keystroke, MouseButton, MouseDownEvent, MouseUpEvent,
    Pixels, PlatformInput, Render, SharedString, Subscription, TestAppContext, Window,
    WindowHandle, div, point, prelude::*, px, rgb, size,
};
use std::{
    cell::{Cell, RefCell},
    rc::Rc,
};

type Renderer = Rc<dyn Fn(&mut Window, &mut App) -> AnyElement>;
const LABEL_BACKGROUND: u32 = 0x713a85;
struct Host {
    renderer: Renderer,
    shown: bool,
    events: Rc<RefCell<Vec<FormEvent>>>,
    _events: Subscription,
}
impl Render for Host {
    fn render(&mut self, window: &mut Window, cx: &mut Context<Self>) -> impl IntoElement {
        div()
            .w(px(480.))
            .p(px(16.))
            .when(self.shown, |root| root.child((self.renderer)(window, cx)))
    }
}
struct Fixture<D> {
    cx: TestAppContext,
    window: WindowHandle<UiRoot>,
    host: Entity<Host>,
    form: Entity<FormState>,
    scope: ConfigScope,
    data: D,
    events: Rc<RefCell<Vec<FormEvent>>>,
}
impl<D: 'static> Fixture<D> {
    fn new(
        create: impl FnOnce(&Entity<FormState>, &ConfigScope, &mut Window, &mut App) -> (D, Renderer),
    ) -> Self {
        let mut cx = TestAppContext::single();
        let mut result = None;
        let window = cx.add_window(|window, cx| {
            crate::init(cx);
            window.activate_window();
            let scope = ConfigScope::new_window(cx);
            scope
                .update_patch(cx, |patch| patch.motion = Some(false))
                .unwrap();
            let form = cx.new(|cx| FormState::new(window, cx));
            form.update(cx, |form, cx| form.set_scope(scope.clone(), cx));
            let (data, renderer) = create(&form, &scope, window, cx);
            let events = Rc::new(RefCell::new(Vec::new()));
            let host = cx.new(|cx| Host {
                renderer,
                shown: true,
                events: events.clone(),
                _events: cx.subscribe(&form, |host: &mut Host, _, event, _| {
                    host.events.borrow_mut().push(event.clone())
                }),
            });
            result = Some((host.clone(), form, scope.clone(), data, events));
            UiRoot::new(scope, host, window, cx)
        });
        let (host, form, scope, data, events) = result.unwrap();
        let mut fixture = Self {
            cx,
            window,
            host,
            form,
            scope,
            data,
            events,
        };
        fixture.draw();
        fixture
    }
    fn app<R>(&mut self, work: impl FnOnce(&mut Window, &mut App) -> R) -> R {
        let result = self
            .cx
            .update_window(self.window.into(), |_, window, cx| work(window, cx))
            .unwrap();
        self.draw();
        result
    }
    fn draw(&mut self) {
        self.cx.run_until_parked();
        self.cx
            .update_window(self.window.into(), |_, window, cx| {
                window.draw(cx).clear(cx)
            })
            .unwrap();
        self.cx.run_until_parked();
    }
    fn key(&mut self, key: &'static str) {
        self.app(|window, cx| {
            let keystroke = Keystroke::parse(key).unwrap();
            window.dispatch_event(
                PlatformInput::KeyDown(KeyDownEvent {
                    keystroke: keystroke.clone(),
                    is_held: false,
                    prefer_character_input: false,
                }),
                cx,
            );
            window.dispatch_event(PlatformInput::KeyUp(KeyUpEvent { keystroke }), cx);
        });
    }
    fn type_text(&mut self, text: &str) {
        self.cx.simulate_input(self.window.into(), text);
        self.draw();
    }
    fn label_bounds(&mut self) -> Bounds<Pixels> {
        self.cx
            .update_window(self.window.into(), |_, window, _| {
                let scale = window.scale_factor();
                let background: Background = rgb(LABEL_BACKGROUND).into();
                let quad = window
                    .rendered_scene()
                    .quads
                    .iter()
                    .find(|quad| quad.background == background)
                    .expect("the native Form label has a measured background");
                Bounds::new(
                    point(
                        px(quad.bounds.origin.x.0 / scale),
                        px(quad.bounds.origin.y.0 / scale),
                    ),
                    size(
                        px(quad.bounds.size.width.0 / scale),
                        px(quad.bounds.size.height.0 / scale),
                    ),
                )
            })
            .unwrap()
    }
    fn click_label(&mut self) {
        let position = self.label_bounds().center();
        self.app(|window, cx| {
            window.dispatch_event(
                PlatformInput::MouseDown(MouseDownEvent {
                    button: MouseButton::Left,
                    position,
                    ..Default::default()
                }),
                cx,
            );
            window.dispatch_event(
                PlatformInput::MouseUp(MouseUpEvent {
                    button: MouseButton::Left,
                    position,
                    ..Default::default()
                }),
                cx,
            );
        });
    }
    fn focus<T: 'static>(&mut self, field: &FormField<T>) {
        self.app(|window, cx| assert!(field.handle().focus(window, cx)));
    }
    fn value<T: 'static>(&self, field: &FormField<T>) -> FormPresence<T> {
        field
            .handle()
            .state()
            .read_with(&self.cx, |_, cx| field.read(cx).unwrap())
    }
    fn reset(&mut self) {
        let form = self.form.clone();
        self.app(|window, cx| form.update(cx, |form, cx| form.reset(window, cx)));
    }
    fn changes(&self) -> usize {
        self.events
            .borrow()
            .iter()
            .filter(|event| matches!(event, FormEvent::ValuesChange { .. }))
            .count()
    }
}
fn control<T: Clone + PartialEq + 'static, C: FormControl<T>>(
    form: &Entity<FormState>,
    scope: &ConfigScope,
    field: &FormField<T>,
    factory: impl Fn(FormItemContext, &mut Window, &mut App) -> C + 'static,
) -> Renderer {
    control_with_form_disabled(form, scope, field, None, factory)
}
fn control_with_form_disabled<T: Clone + PartialEq + 'static, C: FormControl<T>>(
    form: &Entity<FormState>,
    scope: &ConfigScope,
    field: &FormField<T>,
    disabled: Option<bool>,
    factory: impl Fn(FormItemContext, &mut Window, &mut App) -> C + 'static,
) -> Renderer {
    let form = form.clone();
    let scope = scope.clone();
    let field = field.clone();
    let factory = Rc::new(factory);
    Rc::new(move |_, _| {
        let factory = factory.clone();
        let mut form = Form::new("adapter-form", &form)
            .scope(scope.clone())
            .styles(label_styles());
        if let Some(disabled) = disabled {
            form = form.disabled(disabled);
        }
        form.item(
            FormItem::new("field")
                .label("Field")
                .has_feedback(true)
                .control_with_app(&field, move |context, window, cx| {
                    factory(context, window, cx)
                }),
        )
        .into_any_element()
    })
}
fn label_styles() -> FormStyles {
    let mut styles = FormStyles::default();
    styles.label.background = Some(rgb(LABEL_BACKGROUND).into());
    styles
}
fn text_fixture(options: FieldOptions) -> Fixture<(FormField<SharedString>, Entity<InputState>)> {
    Fixture::new(|form, scope, window, cx| {
        let field = form.update(cx, |form, cx| {
            form.register("value", FormCodec::text(), options, cx)
                .unwrap()
        });
        let editor = cx.new(|cx| InputState::new(window, cx));
        let state = editor.clone();
        let renderer = control(form, scope, &field, move |context, _, _| {
            Input::new(context.id, &state)
        });
        ((field, editor), renderer)
    })
}

#[test]
fn form_binding_native_text_changes_once_and_enter_submits_once() {
    let mut fixture = text_fixture(FieldOptions::default().rule(FormRule::required()));
    let (field, editor) = fixture.data.clone();
    fixture.focus(&field);
    fixture.app(|window, cx| {
        editor.update(cx, |editor, cx| {
            editor.replace_text_in_range(None, "Ada", window, cx)
        })
    });
    assert_eq!(fixture.value(&field), FormPresence::Value("Ada".into()));
    assert_eq!(fixture.changes(), 1);
    fixture.key("enter");
    assert_eq!(
        fixture
            .events
            .borrow()
            .iter()
            .filter(|event| matches!(event, FormEvent::Finish(_)))
            .count(),
        1
    );
    let changes = fixture.changes();
    fixture.app(|window, cx| {
        field
            .set(FormPresence::Value("Grace".into()), window, cx)
            .unwrap()
    });
    assert_eq!(
        editor.read_with(&fixture.cx, |editor, _| editor.value().to_owned()),
        "Grace"
    );
    assert_eq!(fixture.changes(), changes);
}

#[test]
fn form_binding_textarea_enter_edits_while_search_enter_submits_once() {
    for multiline in [true, false] {
        let mut fixture = Fixture::new(move |form, scope, window, cx| {
            let field = form.update(cx, |form, cx| {
                form.register("value", FormCodec::text(), FieldOptions::default(), cx)
                    .unwrap()
            });
            let editor = cx.new(|cx| InputState::new(window, cx));
            let state = editor.clone();
            let renderer: Renderer = if multiline {
                control(form, scope, &field, move |context, _, _| {
                    crate::TextArea::new(context.id, &state)
                })
            } else {
                control(form, scope, &field, move |context, _, _| {
                    crate::SearchInput::new(context.id, &state)
                })
            };
            ((field, editor), renderer)
        });
        let (field, _) = fixture.data.clone();
        fixture.focus(&field);
        fixture.type_text("native");
        fixture.key("enter");
        assert_eq!(
            fixture.value(&field),
            FormPresence::Value(if multiline { "native\n" } else { "native" }.into())
        );
        assert_eq!(
            fixture
                .events
                .borrow()
                .iter()
                .filter(|event| matches!(event, FormEvent::Finish(_)))
                .count(),
            usize::from(!multiline)
        );
    }
}

#[test]
fn form_binding_ime_defers_collection_until_native_composition_commits() {
    let mut fixture = text_fixture(FieldOptions::default());
    let (field, editor) = fixture.data.clone();
    fixture.focus(&field);
    fixture.app(|window, cx| {
        editor.update(cx, |editor, cx| {
            editor.replace_and_mark_text_in_range(None, "ni", Some(2..2), window, cx)
        })
    });
    assert_eq!(fixture.value(&field), FormPresence::Unset);
    assert_eq!(fixture.changes(), 0);
    fixture.app(|window, cx| {
        editor.update(cx, |editor, cx| {
            editor.replace_text_in_range(None, "你", window, cx)
        })
    });
    assert_eq!(fixture.value(&field), FormPresence::Value("你".into()));
    assert_eq!(fixture.changes(), 1);
}

#[test]
fn form_binding_rejected_normalization_restores_editor_without_user_change() {
    let mut fixture = text_fixture(FieldOptions::default().initial_value("A").normalize(
        |value, previous, _| {
            if value
                .as_str()
                .is_some_and(|text| text.chars().any(|c| c.is_ascii_digit()))
            {
                previous.clone()
            } else {
                value
            }
        },
    ));
    let (field, editor) = fixture.data.clone();
    fixture.focus(&field);
    fixture.type_text("1");
    assert_eq!(fixture.value(&field), FormPresence::Value("A".into()));
    assert_eq!(
        editor.read_with(&fixture.cx, |editor, _| editor.value().to_owned()),
        "A"
    );
    assert_eq!(fixture.changes(), 0);
}

#[test]
fn form_binding_equal_reset_clears_history_composition_and_preserves_entities() {
    let mut fixture = text_fixture(FieldOptions::default().initial_value("A"));
    let (field, editor) = fixture.data.clone();
    let entity = editor.entity_id();
    fixture.focus(&field);
    fixture.app(|window, cx| {
        editor.update(cx, |editor, cx| {
            editor.replace_text_in_range(None, "B", window, cx)
        })
    });
    fixture.app(|window, cx| editor.update(cx, |editor, cx| editor.undo(window, cx)));
    assert!(editor.read_with(&fixture.cx, |editor, _| editor.can_redo()));
    fixture.reset();
    assert!(!editor.read_with(&fixture.cx, |editor, _| editor.can_redo()
        || editor.can_undo()
        || editor.is_composing()));
    assert_eq!(editor.entity_id(), entity);
    fixture.focus(&field);
    fixture.app(|window, cx| {
        editor.update(cx, |editor, cx| {
            editor.replace_and_mark_text_in_range(None, "x", Some(1..1), window, cx)
        })
    });
    fixture.reset();
    assert!(!editor.read_with(&fixture.cx, |editor, _| editor.is_composing()));
    assert_eq!(fixture.value(&field), FormPresence::Value("A".into()));
}

#[test]
fn form_binding_reset_does_not_run_blur_rules_but_next_real_blur_does() {
    let calls = Rc::new(Cell::new(0));
    let counter = calls.clone();
    let mut fixture = text_fixture(
        FieldOptions::default()
            .validate_trigger([ValidationTrigger::Blur])
            .rule(FormRule::custom(move |_, _, _| {
                counter.set(counter.get() + 1);
                FormRuleResult::Ready(Err("Required".into()))
            })),
    );
    let (field, _) = fixture.data.clone();
    fixture.focus(&field);
    fixture.reset();
    assert_eq!(calls.get(), 0);
    fixture.focus(&field);
    fixture.app(|window, cx| window.blur(cx));
    assert_eq!(calls.get(), 1);
    assert!(
        field
            .handle()
            .state()
            .read_with(&fixture.cx, |field, _| !field.meta().errors.is_empty())
    );
}

#[test]
fn form_binding_password_reset_restores_hidden_default() {
    let mut fixture = Fixture::new(|form, scope, window, cx| {
        let field = form.update(cx, |form, cx| {
            form.register(
                "password",
                FormCodec::text(),
                FieldOptions::default().initial_value("secret"),
                cx,
            )
            .unwrap()
        });
        let editor = cx.new(|cx| InputState::new(window, cx));
        let state = editor.clone();
        let renderer = control(form, scope, &field, move |context, _, _| {
            PasswordInput::new(context.id, &state)
        });
        ((field, editor), renderer)
    });
    let (_, editor) = fixture.data.clone();
    fixture.app(|window, cx| {
        editor.update(cx, |editor, cx| {
            editor.set_password_visible(true, window, cx)
        })
    });
    assert!(editor.read_with(&fixture.cx, |editor, _| editor.password_visible()));
    fixture.reset();
    assert!(!editor.read_with(&fixture.cx, |editor, _| editor.password_visible()));
}

#[test]
fn form_binding_checkbox_and_switch_keyboard_changes_compose_callbacks_once() {
    for switch in [false, true] {
        let calls = Rc::new(Cell::new(0));
        let count = calls.clone();
        let mut fixture = Fixture::new(move |form, scope, _, cx| {
            let field = form.update(cx, |form, cx| {
                form.register(
                    "enabled",
                    FormCodec::boolean(),
                    FieldOptions::default().initial_value(false),
                    cx,
                )
                .unwrap()
            });
            let renderer: Renderer = if switch {
                let count = count.clone();
                control(form, scope, &field, move |context, _, _| {
                    let count = count.clone();
                    Switch::new(context.id).on_change(move |_, _, _| count.set(count.get() + 1))
                })
            } else {
                control(form, scope, &field, move |context, _, _| {
                    let count = count.clone();
                    Checkbox::new(context.id)
                        .label("Enabled")
                        .on_change(move |_, _, _| count.set(count.get() + 1))
                })
            };
            (field, renderer)
        });
        let field = fixture.data.clone();
        fixture.focus(&field);
        fixture.key("space");
        assert_eq!(fixture.value(&field), FormPresence::Value(true));
        assert_eq!(calls.get(), 1);
        assert_eq!(fixture.changes(), 1);
        fixture.reset();
        assert_eq!(fixture.value(&field), FormPresence::Value(false));
    }
}

#[test]
fn form_binding_checkbox_group_keyboard_allows_clearing_last_selection() {
    let mut fixture = Fixture::new(|form, scope, _, cx| {
        let field = form.update(cx, |form, cx| {
            form.register(
                "values",
                FormCodec::<&str>::native().array(),
                FieldOptions::default(),
                cx,
            )
            .unwrap()
        });
        let renderer = control(form, scope, &field, move |context, _, _| {
            CheckboxGroup::new(context.id).options([
                CheckboxOption::new("one", "one", "One"),
                CheckboxOption::new("two", "two", "Two"),
            ])
        });
        (field, renderer)
    });
    let field = fixture.data.clone();
    fixture.focus(&field);
    fixture.key("space");
    assert_eq!(fixture.value(&field), FormPresence::Value(vec!["one"]));
    fixture.key("space");
    assert_eq!(fixture.value(&field), FormPresence::Value(Vec::new()));
}

#[test]
fn form_binding_radio_and_segmented_keyboard_keep_typed_domain_values() {
    for segmented in [false, true] {
        let mut fixture = Fixture::new(move |form, scope, _, cx| {
            let field = form.update(cx, |form, cx| {
                form.register(
                    "value",
                    FormCodec::<u64>::native(),
                    FieldOptions::default().initial_value(FormValue::native(7_u64)),
                    cx,
                )
                .unwrap()
            });
            let renderer: Renderer = if segmented {
                control(form, scope, &field, move |context, _, _| {
                    Segmented::new(context.id).options([
                        SegmentedOption::new("a", 7_u64, "One"),
                        SegmentedOption::new("b", u64::MAX, "Two"),
                    ])
                })
            } else {
                control(form, scope, &field, move |context, _, _| {
                    RadioGroup::new(context.id).options([
                        RadioOption::new("a", 7_u64, "One"),
                        RadioOption::new("b", u64::MAX, "Two"),
                    ])
                })
            };
            (field, renderer)
        });
        let field = fixture.data.clone();
        fixture.focus(&field);
        fixture.key("right");
        assert_eq!(fixture.value(&field), FormPresence::Value(u64::MAX));
        fixture.reset();
        assert_eq!(fixture.value(&field), FormPresence::Value(7));
    }
}

#[test]
fn form_binding_select_single_and_multiple_keep_labels_and_native_ids() {
    let mut fixture = Fixture::new(|form, scope, window, cx| {
        let field = form.update(cx, |form, cx| {
            form.register(
                "choice",
                FormCodec::<SelectValue<u64>>::selection(),
                FieldOptions::default(),
                cx,
            )
            .unwrap()
        });
        let options = SelectOptions::new([
            SelectOption::new("a", u64::MAX, "Remote label"),
            SelectOption::new("b", 2, "Second"),
        ])
        .unwrap();
        let state = cx.new(|cx| SelectState::new(options, window, cx));
        let controller = state.clone();
        let renderer = control(form, scope, &field, move |context, _, _| {
            Select::new(context.id, &controller)
        });
        ((field, state), renderer)
    });
    let (field, state) = fixture.data.clone();
    fixture.focus(&field);
    fixture.key("down");
    fixture.key("enter");
    assert_eq!(
        fixture.value(&field),
        FormPresence::Value(SelectValue::new(u64::MAX, "Remote label"))
    );
    fixture.app(|window, cx| {
        field
            .set(
                FormPresence::Value(SelectValue::new(3, "Missing remote option")),
                window,
                cx,
            )
            .unwrap()
    });
    assert_eq!(
        state.read_with(&fixture.cx, |state, _| state.values()[0].label.clone()),
        SharedString::from("Missing remote option")
    );
    let mut multiple = Fixture::new(|form, scope, window, cx| {
        let field = form.update(cx, |form, cx| {
            form.register(
                "choices",
                FormCodec::<Vec<SelectValue<u64>>>::selections(),
                FieldOptions::default(),
                cx,
            )
            .unwrap()
        });
        let options = SelectOptions::new([
            SelectOption::new("a", u64::MAX, "One"),
            SelectOption::new("b", 2, "Two"),
        ])
        .unwrap();
        let state = cx.new(|cx| SelectState::new(options, window, cx));
        state.update(cx, |state, cx| {
            state.set_settings(
                SelectSettings {
                    mode: crate::SelectMode::Multiple,
                    ..Default::default()
                },
                cx,
            )
        });
        let controller = state.clone();
        let renderer = control(form, scope, &field, move |context, _, _| {
            Select::new(context.id, &controller)
        });
        ((field, state), renderer)
    });
    let (field, _) = multiple.data.clone();
    multiple.focus(&field);
    multiple.key("down");
    multiple.key("enter");
    assert_eq!(multiple.value(&field).value().unwrap()[0].value, u64::MAX);
    multiple.app(|window, cx| {
        field
            .set(FormPresence::Value(Vec::new()), window, cx)
            .unwrap()
    });
    assert_eq!(multiple.value(&field), FormPresence::Value(Vec::new()));
}

#[test]
fn form_binding_numeric_exact_input_and_equal_reset_clear_incomplete_draft() {
    let mut fixture = Fixture::new(|form, scope, window, cx| {
        let field = form.update(cx, |form, cx| {
            form.register(
                "amount",
                FormCodec::numeric(),
                FieldOptions::default().initial_value("1".parse::<ExactDecimal>().unwrap()),
                cx,
            )
            .unwrap()
        });
        let state = cx.new(|cx| {
            InputNumberState::with_options(
                NumberOptions {
                    mode: NumericMode::ExactDecimal,
                    ..Default::default()
                },
                window,
                cx,
            )
            .unwrap()
        });
        let controller = state.clone();
        let renderer = control(form, scope, &field, move |context, _, cx| {
            InputNumber::new(context.id, &controller, cx)
        });
        ((field, state), renderer)
    });
    let (field, state) = fixture.data.clone();
    fixture.focus(&field);
    fixture.key("ctrl-a");
    fixture.type_text("9007199254740993.125");
    fixture.key("enter");
    let expected: ExactDecimal = "9007199254740993.125".parse().unwrap();
    assert_eq!(
        fixture.value(&field),
        FormPresence::Value(NumericValue::Decimal(expected))
    );
    fixture.app(|window, cx| {
        field
            .set(
                FormPresence::Value(NumericValue::Decimal("1".parse().unwrap())),
                window,
                cx,
            )
            .unwrap()
    });
    fixture.focus(&field);
    fixture.key("ctrl-a");
    fixture.type_text("1.");
    fixture.reset();
    assert_eq!(
        state.read_with(&fixture.cx, |state, cx| state.text(cx).to_owned()),
        "1"
    );
}

fn picker_round_trip<S: PickerSelection>(codec: FormCodec<S::Value>, value: S::Value)
where
    Picker<S>: FormControl<S::Value>,
{
    let mut fixture = Fixture::new(|form, scope, window, cx| {
        let field = form.update(cx, |form, cx| {
            form.register("date", codec, FieldOptions::default(), cx)
                .unwrap()
        });
        let state = cx.new(|cx| PickerState::<S>::new(window, cx));
        let controller = state.clone();
        let renderer = control(form, scope, &field, move |context, _, _| {
            Picker::<S>::new(context.id, &controller)
        });
        ((field, state), renderer)
    });
    let (field, state) = fixture.data.clone();
    fixture.app(|window, cx| {
        field
            .set(FormPresence::Value(value.clone()), window, cx)
            .unwrap()
    });
    assert_eq!(
        state.read_with(&fixture.cx, |state, _| state.value().clone()),
        value
    );
    assert_eq!(fixture.changes(), 0);
    fixture.reset();
    assert_eq!(
        state.read_with(&fixture.cx, |state, _| state.value().clone()),
        S::empty()
    );
}
#[test]
fn form_binding_all_picker_shapes_sync_and_reset_without_string_conversion() {
    let date = NaiveDate::from_ymd_opt(2026, 10, 30).unwrap();
    let time = NaiveTime::from_hms_opt(13, 14, 15).unwrap();
    let datetime = NaiveDateTime::new(date, time);
    picker_round_trip::<DateSelection>(FormCodec::optional_native(), Some(date));
    picker_round_trip::<DateTimeSelection>(FormCodec::optional_native(), Some(datetime));
    picker_round_trip::<TimeSelection>(FormCodec::optional_native(), Some(time));
    picker_round_trip::<MultipleDateSelection>(FormCodec::native_array(), [date].into());
    picker_round_trip::<DateRangeSelection>(FormCodec::range(), PickerRange::new(Some(date), None));
    picker_round_trip::<DateTimeRangeSelection>(
        FormCodec::range(),
        PickerRange::new(Some(datetime), Some(datetime)),
    );
    picker_round_trip::<TimeRangeSelection>(FormCodec::range(), PickerRange::new(None, Some(time)));
}

#[test]
fn form_binding_native_date_typing_commits_a_civil_date() {
    let mut fixture = Fixture::new(|form, scope, window, cx| {
        let field = form.update(cx, |form, cx| {
            form.register(
                "date",
                FormCodec::<Option<NaiveDate>>::optional_native(),
                FieldOptions::default(),
                cx,
            )
            .unwrap()
        });
        let state = cx.new(|cx| PickerState::<DateSelection>::new(window, cx));
        let controller = state.clone();
        let renderer = control(form, scope, &field, move |context, _, _| {
            Picker::<DateSelection>::new(context.id, &controller)
        });
        ((field, state), renderer)
    });
    let (field, _) = fixture.data.clone();
    fixture.focus(&field);
    fixture.type_text("2026-10-30");
    fixture.key("enter");
    assert_eq!(
        fixture.value(&field),
        FormPresence::Value(Some(NaiveDate::from_ymd_opt(2026, 10, 30).unwrap()))
    );
}

#[test]
fn form_binding_color_gradient_and_slider_values_sync_reset_and_keyboard() {
    let mut color = Fixture::new(|form, scope, window, cx| {
        let field = form.update(cx, |form, cx| {
            form.register("color", FormCodec::color(), FieldOptions::default(), cx)
                .unwrap()
        });
        let state = cx.new(|cx| ColorPickerState::new(window, cx));
        let controller = state.clone();
        let renderer = control(form, scope, &field, move |context, _, _| {
            ColorPicker::new(context.id, &controller)
        });
        ((field, state), renderer)
    });
    let (field, state) = color.data.clone();
    let value = ColorPickerValue::gradient([
        crate::ColorStop::new(crate::PickerColor::parse("#1677ff").unwrap(), 0.).unwrap(),
        crate::ColorStop::new(crate::PickerColor::parse("#722ed1").unwrap(), 100.).unwrap(),
    ])
    .unwrap();
    color.app(|window, cx| {
        field
            .set(FormPresence::Value(value.clone()), window, cx)
            .unwrap()
    });
    assert_eq!(
        state.read_with(&color.cx, |state, _| state.value().clone()),
        value
    );
    color.reset();
    assert!(state.read_with(&color.cx, |state, _| state.value().is_cleared()));
    let mut slider = Fixture::new(|form, scope, window, cx| {
        let field = form.update(cx, |form, cx| {
            form.register(
                "slider",
                FormCodec::slider(),
                FieldOptions::default().initial_value(10.),
                cx,
            )
            .unwrap()
        });
        let state = cx.new(|cx| SliderState::new(window, cx));
        let controller = state.clone();
        let renderer = control(form, scope, &field, move |context, _, _| {
            Slider::new(context.id, &controller)
        });
        ((field, state), renderer)
    });
    let (field, _) = slider.data.clone();
    slider.focus(&field);
    slider.key("right");
    assert_eq!(
        slider.value(&field),
        FormPresence::Value(SliderValue::Single(11.))
    );
    slider.reset();
    assert_eq!(
        slider.value(&field),
        FormPresence::Value(SliderValue::Single(10.))
    );
}

#[test]
fn form_binding_native_otp_typing_and_reset_use_aggregate_editor_state() {
    let mut fixture = Fixture::new(|form, scope, window, cx| {
        let field = form.update(cx, |form, cx| {
            form.register("code", FormCodec::text(), FieldOptions::default(), cx)
                .unwrap()
        });
        let state = cx.new(|cx| OtpState::new(window, cx));
        let controller = state.clone();
        let renderer = control(form, scope, &field, move |context, _, _| {
            OtpInput::new(context.id, &controller)
        });
        ((field, state), renderer)
    });
    let (field, state) = fixture.data.clone();
    fixture.focus(&field);
    fixture.type_text("123456");
    assert_eq!(fixture.value(&field), FormPresence::Value("123456".into()));
    assert!(state.read_with(&fixture.cx, |state, _| state.is_complete()));
    fixture.reset();
    assert!(
        state.read_with(&fixture.cx, |state, _| state.value().is_empty()
            && !state.can_undo())
    );
}

#[test]
fn form_binding_scope_disabled_inherits_and_explicit_false_wins() {
    let mut fixture = Fixture::new(|form, scope, window, cx| {
        scope
            .update_patch(cx, |patch| {
                patch.disabled = Some(true);
                patch.variant = Some(Variant::Filled);
            })
            .unwrap();
        let field = form.update(cx, |form, cx| {
            form.register("value", FormCodec::text(), FieldOptions::default(), cx)
                .unwrap()
        });
        let editor = cx.new(|cx| InputState::new(window, cx));
        let state = editor.clone();
        let renderer = control(form, scope, &field, move |context, _, _| {
            Input::new(context.id, &state)
                .disabled(false)
                .variant(Variant::Outlined)
        });
        ((field, editor), renderer)
    });
    let (field, editor) = fixture.data.clone();
    assert!(!editor.read_with(&fixture.cx, |editor, _| editor.is_disabled()));
    fixture.focus(&field);
    fixture.type_text("enabled");
    assert_eq!(fixture.value(&field), FormPresence::Value("enabled".into()));
    let scope = fixture.scope.clone();
    fixture.app(|_, cx| {
        scope
            .update_patch(cx, |patch| patch.disabled = Some(false))
            .unwrap()
    });
    assert!(!editor.read_with(&fixture.cx, |editor, _| editor.is_disabled()));
}

#[test]
fn form_binding_disabled_scope_blocks_native_editing_until_reenabled() {
    let mut fixture = text_fixture(FieldOptions::default());
    let (field, editor) = fixture.data.clone();
    let scope = fixture.scope.clone();
    fixture.app(|_, cx| {
        scope
            .update_patch(cx, |patch| patch.disabled = Some(true))
            .unwrap()
    });
    assert!(editor.read_with(&fixture.cx, |editor, _| editor.is_disabled()));
    fixture.app(|window, cx| assert!(!field.handle().focus(window, cx)));
    fixture.type_text("ignored");
    assert_eq!(fixture.value(&field), FormPresence::Unset);
    fixture.app(|_, cx| {
        scope
            .update_patch(cx, |patch| patch.disabled = Some(false))
            .unwrap()
    });
    fixture.focus(&field);
    fixture.type_text("enabled");
    assert_eq!(fixture.value(&field), FormPresence::Value("enabled".into()));
}

#[test]
fn form_binding_controller_disabled_preserves_native_input_and_explicit_priority() {
    // An ordinary retained editor owns this request even before Form binds it.
    let mut standalone = Fixture::new(|form, scope, window, cx| {
        let field = form.update(cx, |form, cx| {
            form.register("value", FormCodec::text(), FieldOptions::default(), cx)
                .unwrap()
        });
        let editor = cx.new(|cx| InputState::new(window, cx));
        editor.update(cx, |editor, cx| editor.set_disabled(true, window, cx));
        let state = editor.clone();
        let scope = scope.clone();
        let render: Renderer = Rc::new(move |_, _| {
            Input::new("standalone-disabled", &state)
                .scope(scope.clone())
                .into_any_element()
        });
        ((field, editor), render)
    });
    let editor = standalone.data.1.clone();
    standalone.app(|window, cx| editor.update(cx, |editor, cx| editor.focus(window, cx)));
    standalone.type_text("blocked");
    assert!(
        editor.read_with(&standalone.cx, |editor, _| editor.is_disabled()
            && editor.value().is_empty())
    );

    for (global, form_disabled, requested, child, disabled) in [
        (false, None, true, None, true),
        (false, Some(true), true, Some(false), false),
        (true, Some(false), false, None, false),
        (true, Some(false), true, None, true),
    ] {
        let mut fixture = Fixture::new(move |form, scope, window, cx| {
            scope
                .update_patch(cx, |patch| patch.disabled = Some(global))
                .unwrap();
            let field = form.update(cx, |form, cx| {
                form.register("value", FormCodec::text(), FieldOptions::default(), cx)
                    .unwrap()
            });
            let editor = cx.new(|cx| InputState::new(window, cx));
            editor.update(cx, |editor, cx| editor.set_disabled(requested, window, cx));
            let state = editor.clone();
            let renderer = control_with_form_disabled(
                form,
                scope,
                &field,
                form_disabled,
                move |context, _, _| {
                    let input = Input::new(context.id, &state);
                    if let Some(disabled) = child {
                        input.disabled(disabled)
                    } else {
                        input
                    }
                },
            );
            ((field, editor), renderer)
        });
        let (field, editor) = fixture.data.clone();
        fixture.click_label();
        fixture.type_text("edit");
        assert_eq!(
            fixture.value(&field),
            if disabled {
                FormPresence::Unset
            } else {
                FormPresence::Value("edit".into())
            },
            "controller request must survive inherited enabled Form"
        );
        assert_eq!(
            editor.read_with(&fixture.cx, |editor, _| editor.is_disabled()),
            disabled
        );
        fixture.app(|window, cx| assert_eq!(field.handle().focus(window, cx), !disabled));
        // GPUI simulate_input dispatches one native keystroke per character.
        assert_eq!(fixture.changes(), if disabled { 0 } else { "edit".len() });
    }
}

#[test]
fn form_binding_controller_disabled_changes_remain_live_after_mount() {
    let mut fixture = text_fixture(FieldOptions::default());
    let (field, editor) = fixture.data.clone();
    fixture.focus(&field);
    fixture.app(|window, cx| editor.update(cx, |editor, cx| editor.set_disabled(true, window, cx)));
    fixture.click_label();
    fixture.type_text("blocked");
    assert_eq!(fixture.value(&field), FormPresence::Unset);
    fixture.app(|window, cx| assert!(!field.handle().focus(window, cx)));
    fixture
        .app(|window, cx| editor.update(cx, |editor, cx| editor.set_disabled(false, window, cx)));
    fixture.click_label();
    fixture.type_text("enabled");
    assert_eq!(fixture.value(&field), FormPresence::Value("enabled".into()));
    assert_eq!(fixture.changes(), "enabled".len());
}

#[test]
fn form_binding_controller_disabled_number_and_otp_block_native_editing() {
    let mut number = Fixture::new(|form, scope, window, cx| {
        let field = form.update(cx, |form, cx| {
            form.register("number", FormCodec::numeric(), FieldOptions::default(), cx)
                .unwrap()
        });
        let state = cx.new(|cx| InputNumberState::new(window, cx));
        state.update(cx, |state, cx| state.set_disabled(true, window, cx));
        let controller = state.clone();
        let renderer = control(form, scope, &field, move |context, _, cx| {
            InputNumber::new(context.id, &controller, cx)
        });
        ((field, state), renderer)
    });
    let (field, state) = number.data.clone();
    number.click_label();
    number.type_text("13");
    number.key("up");
    assert_eq!(number.value(&field), FormPresence::Unset);
    assert!(state.read_with(&number.cx, |state, cx| state.is_disabled(cx)));
    number.app(|window, cx| assert!(!field.handle().focus(window, cx)));

    let mut otp = Fixture::new(|form, scope, window, cx| {
        let field = form.update(cx, |form, cx| {
            form.register("code", FormCodec::text(), FieldOptions::default(), cx)
                .unwrap()
        });
        let state = cx.new(|cx| OtpState::new(window, cx));
        state.update(cx, |state, cx| state.set_disabled(true, window, cx));
        let controller = state.clone();
        let renderer = control(form, scope, &field, move |context, _, _| {
            OtpInput::new(context.id, &controller)
        });
        ((field, state), renderer)
    });
    let (field, state) = otp.data.clone();
    otp.click_label();
    otp.type_text("123456");
    assert_eq!(otp.value(&field), FormPresence::Unset);
    assert!(state.read_with(&otp.cx, |state, _| state.is_disabled()));
    otp.app(|window, cx| assert!(!field.handle().focus(window, cx)));
}

#[test]
fn form_binding_controller_disabled_slider_preserves_handle_policy_and_explicit_override() {
    use crate::slider::{SliderDisabled, SliderOptions, SliderRange};
    for (global, form_disabled, options_disabled, builder_disabled, expected) in [
        (false, None, SliderDisabled::All(true), None, vec![10., 20.]),
        (
            false,
            None,
            SliderDisabled::from([true, false]),
            None,
            vec![10., 21.],
        ),
        (
            false,
            Some(true),
            SliderDisabled::All(true),
            Some(false),
            vec![11., 20.],
        ),
        (
            true,
            Some(false),
            SliderDisabled::All(false),
            None,
            vec![11., 20.],
        ),
    ] {
        let mut fixture = Fixture::new(move |form, scope, window, cx| {
            scope
                .update_patch(cx, |patch| patch.disabled = Some(global))
                .unwrap();
            let field = form.update(cx, |form, cx| {
                form.register(
                    "range",
                    FormCodec::slider(),
                    FieldOptions::default().initial_value(FormValue::array([10., 20.])),
                    cx,
                )
                .unwrap()
            });
            let state = cx.new(|cx| {
                SliderState::with_options(
                    SliderOptions {
                        range: Some(SliderRange::default()),
                        disabled: options_disabled,
                        ..Default::default()
                    },
                    window,
                    cx,
                )
                .unwrap()
            });
            let controller = state.clone();
            let renderer = control_with_form_disabled(
                form,
                scope,
                &field,
                form_disabled,
                move |context, _, _| {
                    let slider = Slider::new(context.id, &controller);
                    if let Some(disabled) = builder_disabled {
                        slider.disabled(disabled)
                    } else {
                        slider
                    }
                },
            );
            (field, renderer)
        });
        let field = fixture.data.clone();
        fixture.click_label();
        fixture.key("right");
        assert_eq!(
            fixture.value(&field),
            FormPresence::Value(SliderValue::Range(expected))
        );
    }
}

#[test]
fn form_binding_color_native_hex_editor_collects_typed_color() {
    let mut fixture = Fixture::new(|form, scope, window, cx| {
        let field = form.update(cx, |form, cx| {
            form.register("color", FormCodec::color(), FieldOptions::default(), cx)
                .unwrap()
        });
        let state = cx.new(|cx| ColorPickerState::new(window, cx));
        let controller = state.clone();
        let renderer = control(form, scope, &field, move |context, _, _| {
            ColorPicker::new(context.id, &controller)
        });
        ((field, state), renderer)
    });
    let (field, state) = fixture.data.clone();
    fixture.app(|window, cx| state.update(cx, |state, cx| state.set_open(true, window, cx)));
    fixture.app(|window, cx| state.update(cx, |state, cx| state.focus_input(0, window, cx)));
    fixture.key("ctrl-a");
    fixture.type_text("00ff00");
    fixture.key("enter");
    let value = fixture
        .value(&field)
        .value()
        .expect("native color edit collected");
    assert_eq!(value.active_color(0).to_hex_string(), "#00ff00");
    assert!(fixture.changes() > 0);
}

#[test]
fn form_binding_conditional_unmount_releases_control_registrations_and_entities() {
    let mut fixture = text_fixture(FieldOptions::default());
    let (field, editor) = fixture.data.clone();
    let state = field.handle().state().downgrade();
    let controller = editor.downgrade();
    let form = fixture.form.downgrade();
    fixture.host.update(&mut fixture.cx, |host, cx| {
        host.shown = false;
        cx.notify();
    });
    fixture.draw();
    assert!(
        field
            .handle()
            .state()
            .read_with(&fixture.cx, |field, _| field.meta().errors.is_empty())
    );
    drop(field);
    drop(editor);
    drop(fixture);
    assert!(state.upgrade().is_none());
    assert!(controller.upgrade().is_none());
    assert!(form.upgrade().is_none());
}

#[test]
fn form_binding_pointer_labels_activate_native_checkbox_switch_and_radio_once() {
    for kind in 0..3 {
        let mut fixture = Fixture::new(move |form, scope, _, cx| {
            let field = form.update(cx, |form, cx| {
                form.register(
                    "checked",
                    FormCodec::boolean(),
                    FieldOptions::default().initial_value(false),
                    cx,
                )
                .unwrap()
            });
            let calls = Rc::new(Cell::new(0));
            let callback = calls.clone();
            let renderer = match kind {
                0 => control(form, scope, &field, move |context, _, _| {
                    let callback = callback.clone();
                    Checkbox::new(context.id)
                        .on_change(move |_, _, _| callback.set(callback.get() + 1))
                }),
                1 => control(form, scope, &field, move |context, _, _| {
                    let callback = callback.clone();
                    Switch::new(context.id)
                        .on_change(move |_, _, _| callback.set(callback.get() + 1))
                }),
                _ => control(form, scope, &field, move |context, _, _| {
                    let callback = callback.clone();
                    crate::Radio::new(context.id)
                        .on_change(move |_, _, _| callback.set(callback.get() + 1))
                }),
            };
            ((field, calls), renderer)
        });
        let (field, calls) = fixture.data.clone();
        fixture.click_label();
        assert_eq!(
            fixture.value(&field),
            FormPresence::Value(true),
            "native label kind {kind}"
        );
        assert_eq!(calls.get(), 1);
        assert_eq!(fixture.changes(), 1);
        fixture.key("space");
        assert_eq!(fixture.value(&field), FormPresence::Value(kind == 2));
        assert_eq!(calls.get(), if kind == 2 { 1 } else { 2 });
    }
}

#[test]
fn form_binding_label_and_field_focus_respect_live_child_disabled_override() {
    for enabled_override in [false, true] {
        let mut fixture = Fixture::new(move |form, scope, _, cx| {
            let field = form.update(cx, |form, cx| {
                form.register(
                    "checked",
                    FormCodec::boolean(),
                    FieldOptions::default().initial_value(false),
                    cx,
                )
                .unwrap()
            });
            let renderer = control_with_form_disabled(
                form,
                scope,
                &field,
                Some(true),
                move |context, _, _| {
                    let checkbox = Checkbox::new(context.id);
                    if enabled_override {
                        checkbox.disabled(false)
                    } else {
                        checkbox
                    }
                },
            );
            (field, renderer)
        });
        let field = fixture.data.clone();
        fixture.click_label();
        assert_eq!(fixture.value(&field), FormPresence::Value(enabled_override));
        fixture.app(|window, cx| assert_eq!(field.handle().focus(window, cx), enabled_override));
        fixture.key("space");
        assert_eq!(fixture.value(&field), FormPresence::Value(false));
        assert_eq!(fixture.changes(), if enabled_override { 2 } else { 0 });
    }
}

#[test]
fn form_binding_text_label_focuses_without_editing_and_group_focus_skips_disabled_option() {
    let mut text = text_fixture(FieldOptions::default());
    let (field, _) = text.data.clone();
    text.click_label();
    assert_eq!(text.changes(), 0);
    text.type_text("focused by the native label");
    assert_eq!(
        text.value(&field),
        FormPresence::Value("focused by the native label".into())
    );

    let mut group = Fixture::new(|form, scope, _, cx| {
        let field = form.update(cx, |form, cx| {
            form.register(
                "values",
                FormCodec::<u64>::native().array(),
                FieldOptions::default(),
                cx,
            )
            .unwrap()
        });
        let renderer = control(form, scope, &field, |context, _, _| {
            CheckboxGroup::new(context.id).options([
                CheckboxOption::new("disabled", 11, "Disabled").disabled(true),
                CheckboxOption::new("enabled", 22, "Enabled"),
            ])
        });
        (field, renderer)
    });
    let field = group.data.clone();
    group.click_label();
    assert_eq!(group.changes(), 0);
    group.key("space");
    assert_eq!(group.value(&field), FormPresence::Value(vec![22]));
    assert_eq!(group.changes(), 1);
}

#[test]
fn form_binding_native_label_actions_release_after_conditional_unmount() {
    for switch in [false, true] {
        let mut fixture = Fixture::new(move |form, scope, _, cx| {
            let field = form.update(cx, |form, cx| {
                form.register("checked", FormCodec::boolean(), FieldOptions::default(), cx)
                    .unwrap()
            });
            let renderer = if switch {
                control(form, scope, &field, |context, _, _| Switch::new(context.id))
            } else {
                control(form, scope, &field, |context, _, _| {
                    Checkbox::new(context.id)
                })
            };
            (field, renderer)
        });
        let field = fixture.data.clone();
        fixture.click_label();
        let weak_field = field.handle().state().downgrade();
        let weak_form = fixture.form.downgrade();
        fixture.host.update(&mut fixture.cx, |host, cx| {
            host.shown = false;
            cx.notify();
        });
        fixture.draw();
        fixture.app(|window, cx| assert!(!field.handle().focus(window, cx)));
        drop(field);
        drop(fixture);
        assert!(weak_field.upgrade().is_none());
        assert!(weak_form.upgrade().is_none());
    }
}

#[test]
fn form_binding_equal_reset_restores_textarea_dimensions_without_recreating_editor() {
    let mut fixture = Fixture::new(|form, scope, window, cx| {
        let field = form.update(cx, |form, cx| {
            form.register(
                "text",
                FormCodec::text(),
                FieldOptions::default().initial_value("same"),
                cx,
            )
            .unwrap()
        });
        let editor = cx.new(|cx| InputState::new(window, cx));
        let control_state = editor.clone();
        let renderer = control(form, scope, &field, move |context, _, _| {
            crate::TextArea::new(context.id, &control_state)
        });
        ((field, editor), renderer)
    });
    let (field, editor) = fixture.data.clone();
    let identity = editor.entity_id();
    let initial = editor.read_with(&fixture.cx, |editor, _| {
        editor.geometry().unwrap().viewport.size.height
    });
    let frame = fixture.app(|window, _| {
        let scale = window.scale_factor();
        let quad = window
            .rendered_scene()
            .quads
            .iter()
            .find(|quad| quad.border_widths.top.0 > 0. && quad.bounds.size.height.0 / scale > 60.)
            .unwrap();
        Bounds::new(
            point(
                px(quad.bounds.origin.x.0 / scale),
                px(quad.bounds.origin.y.0 / scale),
            ),
            size(
                px(quad.bounds.size.width.0 / scale),
                px(quad.bounds.size.height.0 / scale),
            ),
        )
    });
    let start = point(frame.right() - px(3.), frame.bottom() - px(3.));
    let end = start + point(px(0.), px(36.));
    fixture.app(|window, cx| {
        window.dispatch_event(
            PlatformInput::MouseDown(MouseDownEvent {
                button: MouseButton::Left,
                position: start,
                click_count: 1,
                ..Default::default()
            }),
            cx,
        );
    });
    fixture.app(|window, cx| {
        window.dispatch_event(
            PlatformInput::MouseMove(gpui::MouseMoveEvent {
                position: end,
                pressed_button: Some(MouseButton::Left),
                ..Default::default()
            }),
            cx,
        );
    });
    fixture.app(|window, cx| {
        window.dispatch_event(
            PlatformInput::MouseUp(MouseUpEvent {
                button: MouseButton::Left,
                position: end,
                click_count: 1,
                ..Default::default()
            }),
            cx,
        );
    });
    assert_eq!(
        editor.read_with(&fixture.cx, |editor, _| editor
            .geometry()
            .unwrap()
            .viewport
            .size
            .height),
        initial + px(36.)
    );
    assert_eq!(fixture.value(&field), FormPresence::Value("same".into()));
    assert_eq!(fixture.changes(), 0);
    fixture.reset();
    assert_eq!(editor.entity_id(), identity);
    assert_eq!(
        editor.read_with(&fixture.cx, |editor, _| editor
            .geometry()
            .unwrap()
            .viewport
            .size
            .height),
        initial
    );
    assert_eq!(fixture.value(&field), FormPresence::Value("same".into()));
    assert_eq!(fixture.changes(), 0);
}

#[test]
fn form_binding_textarea_resize_reflows_cached_rows_with_one_bounded_geometry_rebuild() {
    let mut fixture = Fixture::new(|form, scope, window, cx| {
        let (notes, name) = form.update(cx, |form, cx| {
            (
                form.register(
                    "notes",
                    FormCodec::text(),
                    FieldOptions::default().initial_value("same"),
                    cx,
                )
                .unwrap(),
                form.register("name", FormCodec::text(), FieldOptions::default(), cx)
                    .unwrap(),
            )
        });
        let notes_editor = cx.new(|cx| InputState::new(window, cx));
        let name_editor = cx.new(|cx| InputState::new(window, cx));
        let counts = (Rc::new(Cell::new(0)), Rc::new(Cell::new(0)));
        let form = form.clone();
        let scope = scope.clone();
        let (first, second) = (notes_editor.clone(), name_editor.clone());
        let (first_field, second_field) = (notes.clone(), name.clone());
        let (count_a, count_b) = counts.clone();
        let renderer: Renderer =
            Rc::new(move |_, _| {
                let first = first.clone();
                let second = second.clone();
                let count_a = count_a.clone();
                let count_b = count_b.clone();
                Form::new("resize-cached-form", &form)
                    .scope(scope.clone())
                    .item(FormItem::new("notes").label("Notes").control(
                        &first_field,
                        move |context| {
                            count_a.set(count_a.get() + 1);
                            crate::TextArea::new(context.id, &first)
                        },
                    ))
                    .item(FormItem::new("name").label("Name").control(
                        &second_field,
                        move |context| {
                            count_b.set(count_b.get() + 1);
                            Input::new(context.id, &second)
                        },
                    ))
                    .into_any_element()
            });
        ((notes, notes_editor, name_editor, counts), renderer)
    });
    let (notes, editor, following, counts) = fixture.data.clone();
    let before = editor.read_with(&fixture.cx, |editor, _| {
        editor.geometry().unwrap().viewport.size.height
    });
    let next_top = following.read_with(&fixture.cx, |editor, _| {
        editor.geometry().unwrap().viewport.top()
    });
    let unrelated = counts.1.get();
    let frame = fixture.app(|window, _| {
        let scale = window.scale_factor();
        let quad = window
            .rendered_scene()
            .quads
            .iter()
            .find(|quad| quad.border_widths.top.0 > 0. && quad.bounds.size.height.0 / scale > 60.)
            .unwrap();
        Bounds::new(
            point(
                px(quad.bounds.origin.x.0 / scale),
                px(quad.bounds.origin.y.0 / scale),
            ),
            size(
                px(quad.bounds.size.width.0 / scale),
                px(quad.bounds.size.height.0 / scale),
            ),
        )
    });
    let start = point(frame.right() - px(3.), frame.bottom() - px(3.));
    let end = start + point(px(100.), px(36.));
    fixture.app(|window, cx| {
        window.dispatch_event(
            PlatformInput::MouseDown(MouseDownEvent {
                button: MouseButton::Left,
                position: start,
                click_count: 1,
                ..Default::default()
            }),
            cx,
        );
    });
    fixture.app(|window, cx| {
        window.dispatch_event(
            PlatformInput::MouseMove(gpui::MouseMoveEvent {
                position: end,
                pressed_button: Some(MouseButton::Left),
                ..Default::default()
            }),
            cx,
        );
    });
    fixture.app(|window, cx| {
        window.dispatch_event(
            PlatformInput::MouseUp(MouseUpEvent {
                button: MouseButton::Left,
                position: end,
                click_count: 1,
                ..Default::default()
            }),
            cx,
        );
    });
    fixture.draw();
    assert_eq!(
        editor.read_with(&fixture.cx, |editor, _| editor
            .geometry()
            .unwrap()
            .viewport
            .size
            .height),
        before + px(36.)
    );
    assert_eq!(
        following.read_with(&fixture.cx, |editor, _| editor
            .geometry()
            .unwrap()
            .viewport
            .top()),
        next_top + px(36.)
    );
    // GPUI's cached EntityElement includes absolute bounds in its cache key.
    // The displaced row redraws once for its new origin; ordinary value edits
    // still leave unrelated rows untouched (covered by field isolation tests).
    assert_eq!(counts.1.get(), unrelated + 1);
    assert_eq!(fixture.value(&notes), FormPresence::Value("same".into()));
    assert_eq!(fixture.changes(), 0);
    let settled = (counts.0.get(), counts.1.get());
    for _ in 0..3 {
        fixture.draw();
    }
    assert_eq!((counts.0.get(), counts.1.get()), settled);
}
