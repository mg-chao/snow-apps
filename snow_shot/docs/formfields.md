# Form fields

Snow Shot's form fields live in `snow_shot::presentation::components::form_fields` and are
compiled into `snow_shot_settings`. Include `snow_shot/presentation/components/formfields.h`.
They compose Ant Design Qt editors and `AdFormItem`; domain validation, persistence, fetching,
shortcut capture, and submission remain with the owning controller.

## Creating a field

```cpp
namespace fields = snow_shot::presentation::components::form_fields;

fields::Metadata metadata;
metadata.id = QStringLiteral("accountName");
metadata.label = {"AccountEditor", QT_TRANSLATE_NOOP("AccountEditor", "Account name")};
metadata.description = {
    "AccountEditor", QT_TRANSLATE_NOOP("AccountEditor", "A name for this account.")};

fields::Options options;
options.parent = body;
options.allowClear = true;
options.required = true;
options.commitPolicy = fields::CommitPolicy::OnFinish;
const auto name = fields::text(metadata, options);
layout->addWidget(name.viewWidget());
name.field->syncValue(existingName);
```

Place `viewWidget()`, never the editor separately. The view owns the controller and editor.
Parentless views stay hidden until placed or explicitly shown; construction never displays a
temporary top-level form item.
Use `editor` for control-specific capabilities, `item()` for an AdForm validator, and
`focusWidget()` for initial focus or the first invalid field. Guard retained handles with
`QPointer` when a modal can be destroyed asynchronously.

`VerticalDialog` is the default preset: labels above controls, descriptions in label tooltips,
and inline errors/warnings. `SettingsRow` places title/description beside a shared 230-pixel
control column. It owns an inner inline AdForm for feedback; never register that item with a
second form. The item inherits the inline layout so the settings section alone supplies row
spacing. Theme colors and metrics are shared, while compact controls keep their native size.

For a dialog-level AdForm, call `configureForm(form)` and pass `options.form = form` before
creating fields. This registers their items with the form. Use AdForm initial values/reset to
initialize its touched/dirty baseline. When arranging registered items into a grid, register all
items first: AdForm registration rebuilds its default layout. Call `configureTwoColumnGrid(grid)`
to give both columns equal stretch and use the current theme's gutter.
Complete field registration and initial values/reset before opening a centered modal, and settle
its layouts before the first paint.

## Values, feedback, and save timing

`valueEdited` reports draft edits. `valueCommitted` reports the selected commit boundary;
`Immediate` uses value changes, `OnFinish` uses editing completion, and `Explicit` waits for the
caller to invoke `notifyCommitted()`. Color fields default to picker editing completion,
preserving local previews. Path Clear commits immediately with `OnFinish`; a
browse caller sets the selected path under `synchronize()`, then calls `notifyEdited()` and
`notifyCommitted()` to publish one edit and one commit.

`syncValue()` and `synchronize(callback)` silence the shared field events. They preserve native
widget signals required for drawing and accessibility. Use `synchronize` when rebuilding a
caller-owned options/model list. Direct native setters retain their normal change notifications.
Synchronization does not reset AdForm's touched/dirty metadata.

For an explicit-save dialog, read values and run domain validation when Save/OK is requested;
connect no field commit signal to persistence. Cancel then leaves persisted values untouched.
For example, a single-column dialog can use the form's default layout:

```cpp
auto* form = new adqt::widgets::AdForm(body);
fields::configureForm(form);
fields::Options dialogOptions;
dialogOptions.form = form;
dialogOptions.commitPolicy = fields::CommitPolicy::Explicit;
const auto endpoint = fields::text(endpointMetadata, dialogOptions);
form->setInitialValues({{endpointMetadata.id, existingEndpoint}});
form->resetFields();
connect(modal, &adqt::widgets::AdModal::closeRequested, modal,
        [modal, endpoint](adqt::widgets::AdModal::CloseReason reason) {
            if (reason != adqt::widgets::AdModal::CloseReason::OkAction) {
                modal->reject();
                return;
            }
            // Validate endpoint.field->value(), persist only after validation succeeds,
            // then call endpoint.field->notifyCommitted() and modal->accept().
            // Domain errors use endpoint.field->setFeedback().
        });
```

Call `setFeedback(errors, warnings, busy)` to render inline feedback, and use
`reserveFeedbackHeight(height)` for dialogs whose geometry must stay stable while errors appear.
Use `setFieldEnabled` and `setFieldVisible` to affect the entire field.

Translated metadata and managed choices refresh on `LanguageChange` without changing values.
Runtime choice text can use `Choice::text`; use `Choice::label` for translatable options and a
stable `Choice::value` for identity. Keep the same translation contexts when migrating existing
copy. Assign new contexts in `i18n/modules.json`, extract with `snow_shot_update_translations`,
and fill all three catalogs. Callers can retain ownership of dynamically rebuilt options and
provider-specific labels; refresh them in their existing retranslation path.

## Settings binding

```cpp
#include "snow_shot/presentation/settings/settingsformfield.h"

fields::Options options;
options.parent = section;
options.presentation = fields::Presentation::SettingsRow;
auto* field = settings::SettingsFormField::create(descriptor, session, options);
layout->addWidget(field->viewWidget());
```

`SettingsFormField` maps catalog metadata/options and configuration-schema ranges to the shared
factories. It owns the commit connection to `SettingsRuntimeSession::submitDraft`, presents the
session's draft/error/enabled/visible/pending/conflict state, and retains local unfinished text
when unrelated refreshes occur. Settings dirtiness and write outcomes come from the runtime
session, not AdForm. The registry, descriptor, and session must outlive the view.

Text settings commit on editing completion. File/directory settings also commit Browse and
Clear immediately. Choices, switches, numeric fields, sliders, and radios commit immediately.
The settings adapter preserves lazy font options and picker popup construction.

## Custom editors

Pass a `CustomBinding` with the existing control, focus target, and value reader/writer to
`custom(metadata, binding, options)`. Connect the control's typed edit/finish signals to
`notifyEdited()` / `notifyCommitted()`. Put custom retranslation in `binding.retranslate`.
`isSynchronizing()` lets compound bridges suppress feedback during state updates. Existing
shortcut recorders remain responsible for capture, suspension, validation, and acceptance.

```cpp
fields::CustomBinding binding;
binding.control = recorder;
binding.focusWidget = recorder;
binding.readValue = [recorder] { return QVariant(recorder->draft()); };
binding.writeValue = [recorder](const QVariant& value) { recorder->setDraft(value); };
fields::Options recorderOptions;
recorderOptions.commitPolicy = fields::CommitPolicy::Explicit;
const auto shortcut = fields::custom(shortcutMetadata, binding, recorderOptions);
connect(recorder, &ShortcutRecorder::draftChanged, shortcut.field,
        [controller = shortcut.field] { controller->notifyEdited(); });
// On successful explicit confirmation: shortcut.field->notifyCommitted().
```

The built-in path, slider, radio, multi-select, and color adapters already bridge their value
notifications to AdForm. Do not add a second notification bridge in a consumer.

## Verification

Build and run `snow-shot-form-fields-tests` for primitive/preset behavior. Run only the tests for
the consumers changed by a migration. The narrow screenshot translation dialog registration is
`snow-shot-screenshot-translation-settings-dialog-tests`; it uses the existing executable's
`--screenshot-settings` switch. Preserve native object names and stable catalog IDs for test,
search, and accessibility clients.
