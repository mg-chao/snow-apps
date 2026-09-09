# snow-selected-text-c

Apache-2.0 C ABI for `snow-selected-text`. The authoritative declarations and pointer/lifetime
contracts are in `include/snow_selected_text.h`. Link the `snow_selected_text_c` CMake target to
obtain the header and the existing Snow Shot Rust static-library bundle.

Typical event-loop integration:

1. Create a service and initialize options with `snow_selected_text_options_init`. Set `strategy`
   to `SNOW_SELECTED_TEXT_STRATEGY_CLIPBOARD` to capture through Copy alone, as Snow Shot does.
2. Call `snow_selected_text_start` while the source application is still foreground. Inputs are
   copied during submission, and a successful submission returns a request handle.
3. Poll `snow_selected_text_request_poll` on a timer. Do not activate your translation popup before
   acquisition finishes. All service handles share the same one-request-at-a-time runtime.
4. Once terminal, call `snow_selected_text_request_result` once to acquire an owned result.
5. Inspect status, text, method, clipboard status, and errors. Copy returned UTF-8 views into Qt if
   needed, then destroy the result and request. Service destruction does not invalidate them.

No callbacks execute in the host. Poll/cancel are thread-safe; destruction must be exclusive.
Each successful `request_result` call returns an independent handle requiring its own destruction.
Output text is pointer-plus-length UTF-8, not a C string, and can contain embedded NUL bytes.
Use `QString::fromUtf8(data, length)` after checking that length fits Qt's size type.

`struct_size` and `abi_version` must match the v1 header. Flags and statuses are `uint32_t`, so an
unknown numeric input is validated rather than converted into an invalid Rust enum. Null options
select defaults, including `SNOW_SELECTED_TEXT_STRATEGY_AUTO`, Copy fallback enabled and a
two-second timeout. Explicit `STRATEGY_UIA`, `STRATEGY_NATIVE_EDIT`, and `STRATEGY_CLIPBOARD`
attempt only the chosen method; `copy_fallback` affects only `STRATEGY_AUTO`. Unknown strategy
values are rejected before reading exclusion arrays. The unreleased v1 options structure now
includes `strategy`; rebuild C/C++ consumers against the matching header. Error operation strings have
static lifetime. No thread-local last-error state is used.

See the Rust crate README for clipboard side effects, compatibility limits, timeout semantics,
verification commands, and the manual application matrix.
