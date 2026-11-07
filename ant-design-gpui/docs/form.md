# Form

Form provides native field layout, lossless data ownership, validation, dynamic
lists and coordination between independently retained forms. The reference is
Ant Design 6.6.7, revision `f8f5595ab5e891fff6a2a06c5b3b98af643d008e`, with
`@rc-component/form` 1.8.6.

Create `Entity<FormState>` and each native control entity once. Register typed
fields outside rendering, then rebuild lightweight presentation builders:

```rust,ignore
let form = cx.new(|cx| FormState::new(window, cx));
let email = form.update(cx, |form, cx| {
    form.register(
        "email",
        FormCodec::text(),
        FieldOptions::default()
            .label("Email")
            .rule(FormRule::required())
            .rule(FormRule::email()),
        cx,
    ).unwrap()
});
let input = cx.new(|cx| InputState::new(window, cx));

// During rendering; capture the existing entity, not a new editor.
Form::new("account", &form)
    .scope(scope.clone())
    .label_col(FormColumn::span(6))
    .wrapper_col(FormColumn::span(18))
    .item(FormItem::new("email")
        .label("Email")
        .control(&email, move |_| Input::new("email-input", &input)))
```

Use the library's `UiRoot` for native typography, focus traversal and popup
ownership. Applications own labels and custom validation copy; Form's optional
mark and default validation templates are supplied in English, Simplified Chinese
and Traditional Chinese through `Locale`.

## Values and notifications

`NamePath` distinguishes literal string keys from numeric indexes. `"user.name"`
is one key; `NamePath::new(["user", "name"])` selects a nested property. Use
`.index(n)` for array positions.

`FormPresence<T>` distinguishes unset, explicit null and a typed value. A
`FormSnapshot` is immutable and cheap to clone. Object/array nodes share persistent
storage; native leaves preserve custom Rust values without JSON conversion.
Native leaves must be treated as immutable values. They deliberately require no
`Send`, `Sync` or `Debug` bound.

| Control | Recommended codec |
| --- | --- |
| Input, TextArea, PasswordInput, SearchInput, OtpInput | `FormCodec::text()` |
| Checkbox, Switch, standalone Radio | `FormCodec::boolean()` |
| InputNumber | `FormCodec::numeric()`; exact decimal values remain exact |
| RadioGroup, Segmented | `FormCodec::<T>::native()` |
| CheckboxGroup | A scalar codec followed by `.array()` |
| Select, single | `FormCodec::<SelectValue<T>>::selection()` |
| Select, multiple/tags | `FormCodec::<Vec<SelectValue<T>>>::selections()` |
| DatePicker, DateTimePicker, TimePicker | `FormCodec::<Option<T>>::optional_native()` |
| Range pickers | `FormCodec::<PickerRange<T>>::range()` |
| MultipleDatePicker | `FormCodec::<Arc<[NaiveDate]>>::native_array()` |
| ColorPicker | `FormCodec::color()` |
| Slider | `FormCodec::slider()` |

Single cleared controls use null; cleared multiple selections use empty arrays.
Unset/null text displays empty text without rewriting its stored presence. Type
errors are returned instead of coercing or erasing incompatible stored values.
`FormCodec::new` supplies a custom lossless structural mapping.

Programmatic field/value setters update field observers and watchers without
emitting user value-change events. User interactions emit typed `FormEvent`
notifications. Form initial values take precedence over Item defaults. Updating
the reset baseline does not overwrite current edited values.

## Composition and lifecycle

`FormItem::control` takes a repeatable native control factory. Use
`control_with_app` when a constructor requires `Window` or `App`, such as
`InputNumber::new`. Use `control_with` and `FormItemContext` for an app-owned
control; `FormControl<T>` and `FormControlBinding` expose the same binding/focus/
reset contract used by library adapters.

Named items own retained views. Factories should capture retained entities and
read changing data when invoked. External data also needs explicit invalidation:
set `render_revision` when it changes, or return a retained child view that
observes its model. Merely reading an external entity inside a factory does not
subscribe the cached item to that entity. Field values, metadata, scope and
native presentation changes invalidate automatically.
Repeatable `content_with`, `label_content_with`, `help_content_with` and
`extra_content_with` support reactive custom native content. One-shot `.content`
is available for ordinary layout composition.

Horizontal, vertical and inline layouts support per-item overrides, responsive
24-column spans/offsets, fixed/flexible label widths, wrapping, optional/required
marks, tooltips, feedback and semantic styles. `label_placeholder` reproduces
`label={null}`: span-grid layouts infer an action row's wrapper offset from the
inherited label column. An omitted label does not infer that offset. Fixed or
flexible label columns can use explicit action-row spacing when needed.

Explicit child `.disabled(...)` overrides inherited Form settings and controller
requests. Otherwise Input, InputNumber and OTP keep their controller's disabled
request, and Slider keeps its disabled handle policy. An enabled Form does not
clear those requests. `Form.disabled(false)` overrides global scope disabling;
controller requests still apply and remain live after mounting. Availability
changes update native editing and field focus behavior.

As in the reference, Form's `disabled` setting does not disable Segmented;
disable that control explicitly. The existing native `ConfigScope.disabled`
behavior remains available for application-wide disabling. This differs from
the React Segmented's handling of global `componentDisabled`.

Hidden fields retain data and validation. `no_style` fields render without Item
chrome. Use an enclosing item's `.item` or `.items` to compose child fields and
aggregate their validation messages at the nearest styled parent; `group_gap`
sets the native group's spacing. Arbitrary custom content uses a repeatable
content factory. Values preserve across conditional unmount by default. Disable
preservation to restore the initial value/remove that path when its final mounted
registration disappears.

Reset is a dedicated operation: it clears errors, touch/validation metadata,
composition, editing history, drafts and pending work even when values are equal.
Affected controls blur; their reusable entities remain intact. Unrelated fields
retain their state. Ordinary updates and list moves retain editing identity.

## Validation, lists and coordination

Rules support native scalar/array constraints, patterns, exact numeric bounds,
warning-only failures, transforms and custom synchronous/asynchronous validators.
Nested object/array rules use explicit `fields` and a fallback `default_field`.
`FormRegexp` and `FormMethod` preserve native regular-expression and callable
values for type validation. Pattern rules use Rust regex syntax. Normalization
changes stored values synchronously; a rule transform changes only its validation
copy.

Templates interpolate once. Use `\${label}` to display a literal `${label}`;
placeholder-looking text inside replacement values is retained literally.
Unknown variable names produce `undefined`, following the reference formatter.

A custom validator returns `FormRuleResult::Ready` or `Pending(Task<...>)`.
Requests contain an owned value and immutable form snapshot. Foreground tasks
may capture native values; background tasks must extract their own `Send` data.
Replaced, reset, removed or relocated fields reject obsolete results. Explicit
validation/submit bypass event debounce. `ValidateOptions` supports validate-only,
dirty filtering and recursive path selection.

`FormList` owns stable row keys separately from numeric paths. Use each row key
as its GPUI identity and retain its field/control entities under that key. Add,
remove and move update names and metadata together. Nested lists use the row's
current name/path. `FormErrorList` renders list-level messages.

`watch` and `watch_selector` return retained observer entities. Declare selector
dependency paths so unrelated edits do not reevaluate the selector. `FormProvider`
registers named forms with weak ownership and emits coordinated change/finish
events. Call `submit`, `reset`, `FieldHandle::focus` or `scroll_to_field` from native
actions. For a tracked GPUI `Div` or library `ScrollArea`, pass its existing
`ScrollHandle` through `Form::scroll_handle(&handle)`. Form reveals the field at
the nearest viewport edge without replacing the owner's scroll state. GPUI Lists
also consume native automatic scroll requests when no explicit handle is supplied.
Field and label focus resolve the mounted control's current availability,
including explicit child overrides and enabled options within groups. Checkbox,
Switch and Radio labels use their native activation behavior.

TextArea uses a native vertical resize grip by default. `resize(TextAreaResize::None)`
disables it; horizontal and both-axis modes are also available. Content-driven
`auto_size` takes precedence. Dragging retains editing identity and the existing
focus/selection, including when the textarea is disabled or read-only.

Switch opacity composites against the theme's uniform container surface. On a
different opaque uniform surface, supply `Switch::opacity_backdrop`. This GPUI
revision cannot isolate an arbitrary scene subtree into an offscreen texture;
nonuniform backgrounds, ancestor opacity and unbounded custom paint remain a
group-opacity limitation.

Required, invalid and validating metadata reach the actual native control even
when its visual status is overridden. Plain-text help, errors and extra content
contribute to its description. Supply `accessibility_label`/`accessibility_description`
for custom rich content whose text cannot be inferred from an arbitrary native
element. Background validation leaves editing available. Windows UIA
exposes required, busy and full descriptions; this GPUI AccessKit backend does
not expose the UIA `IsDataValidForForm` property. The accessibility fixture also
inspects the exact native AccessKit node to verify invalid state.

The pinned GPUI scene cache does not replay accessibility subtrees. Form keeps
its field entities but bypasses named-item scene caching while assistive
technology is active, so unchanged fields remain in every native tree update.
Scene caching resumes when accessibility is inactive. This increases rendering
work on active accessibility frames; it does not schedule additional idle frames.

Open the gallery with `--component form`. Verification status and limitations
are recorded in [form-verification.md](form-verification.md); the
[coverage map](form-coverage.md) relates reference capabilities to native APIs and
focused evidence.
