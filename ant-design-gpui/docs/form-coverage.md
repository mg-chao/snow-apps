# Form coverage map

This map identifies the implementation and focused checks for the agreed native
Form scope. Final execution results belong in [form-verification.md](form-verification.md);
the existence of a fixture or test does not establish that it passed.

| Reference capability | Native ownership/API | Focused evidence |
| --- | --- | --- |
| Horizontal, vertical, inline and per-item layout | `Form`, `FormItem`, `FormLayout` | `form::view_tests`, 19-scene visual matrix |
| Responsive labels, 24-column grid, offsets, wrapping, RTL | `FormColumn`, `FormLabelAlign`, native label elements | View layout regressions; 520/575/576-pixel and wrapped/absent-label captures |
| Required/optional/custom marks, colon, tooltip, help, extra, feedback | Item presentation and repeatable factories | Marks/statuses/semantic-style captures; renderer regressions |
| Component tokens, four themes, three sizes, variants, disabled inheritance | `FormTheme`, `ConfigScope.form`, explicit control adapters | Scoped config/locale tests, original-source inheritance oracle, visual matrix |
| Compound fields and `noStyle` | `FormItem.item/items`, nearest styled-parent ownership | Native nested/no-style metadata and width regressions; paired captures |
| Exact paths, unset/null, native typed leaves, persistent snapshots | `NamePath`, `FormPresence`, `FormCodec`, `FormValue` | Value, codec, structural-sharing and exact-decimal tests |
| Initial values, silent setters, touched/dirty metadata | `FormState`, `FieldHandle` | Initial-precedence, setter and normalization tests; original-source behavior traces |
| Conditional lifecycle, hidden fields, preservation, clear-on-destroy | Retained Form/Item mounts and weak registrations | Unmount, release-order and control-release regressions |
| Reset equal values, native drafts, history, IME and pending work | Explicit reset protocol in control bridges | Native text/password/number/picker/OTP binding tests |
| Required/type/length/range/enum/pattern/whitespace/URL rules | `FormRule`, native `FormRegexp`/`FormMethod` | Rule edge cases and locale/message-template tests |
| Nested object/array rules and multiple messages | `field`, `default_field`, nested rule modes | Nested sync/async, explicit/default precedence and full-path tests |
| Async validation, warning-only, debounce and first-error modes | Foreground tasks, field generations, cancellation | Async stale/reset/list cancellation, debounce, serial/parallel and warning tests |
| Validate-only, dirty and recursive selection, submit | `ValidateOptions`, `ValidationHandle`, `FormEvent` | Validation selection and native Enter/submit integration |
| Dependencies and watchers | Indexed paths and retained selector/watch entities | Narrow notification and dependent validation tests; Release benchmark |
| Dynamic and nested lists | Stable row keys independent of numeric paths | List move/reset/replacement tests and native gallery examples |
| Coordination across forms | Weak `FormProvider` registrations | Named snapshots, finish/change, rename/move and release tests |
| All existing data-entry controls | `FormControl<T>` adapters | Native binding tests: text, password, textarea, search, OTP, number, checkbox/group, switch, radio/group, segmented, select, every picker shape, color and slider |
| Focus, labels, scroll-to-field and scroll-to-first-error | Live native control actions, explicit `ScrollHandle` ownership and GPUI List autoscroll | Actual pointer-label activation, current availability and real scroll-container tests |
| Accessibility and locale | Exact native control metadata and locale catalogs | Native AccessKit-node tests and six-profile Windows UIA fixture |
| Motion and reduced motion | Retained help/feedback transitions | Deterministic time/height/leave/retarget tests and zero settled idle requests |
| Gallery and integration guidance | 37 public demos, retained controller examples | Focused gallery regressions and guide |
| Bounded editing/rendering cost | Persistent nodes, path indexes, cached named views | Release CPU/allocation distributions; 10/100/1,000 native controls; isolated factory counts |

The independent source evidence lives under `fixtures/form/`. Screenshot pixels
and machine-specific executable/performance artifacts remain in the ignored build
tree. Source and executable hashes bind final results to the code that ran.

Native content factories replace React context, hooks and child cloning. Custom
Rust values replace JavaScript values without string/JSON conversion. Unsupported
gallery controls are explicitly identified substitutions; their Form behavior is
demonstrated, and no substituted-control appearance claim is made. See the guide
for these API boundaries and platform/backend limitations.
