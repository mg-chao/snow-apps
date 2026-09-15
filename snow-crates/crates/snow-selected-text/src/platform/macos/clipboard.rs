use std::sync::atomic::{AtomicU64, Ordering};
use std::time::{Duration, Instant};

use objc2::rc::Retained;
use objc2::runtime::ProtocolObject;
use objc2_app_kit::{NSPasteboard, NSPasteboardItem, NSPasteboardTypeString, NSPasteboardWriting};
use objc2_core_foundation::CFRetained;
use objc2_core_graphics::{
    CGEvent, CGEventFlags, CGEventSource, CGEventSourceStateID, CGPreflightPostEventAccess,
};
use objc2_foundation::{NSArray, NSData, NSString};

use super::{accessibility, validate};
use crate::model::*;
use crate::policy::Context;

const MAX_SNAPSHOT: usize = 64 * 1024 * 1024;
const MAX_ITEMS: usize = 128;
const MAX_REPRESENTATIONS: usize = 256;
const MAX_TYPE_BYTES: usize = 4096;
const COPY_KEY_CODE: u16 = 8;
const MARKER_TYPE: &str = "org.snow-shot.selected-text.marker";

static NEXT_MARKER: AtomicU64 = AtomicU64::new(1);

#[derive(Clone)]
struct Representation {
    name: String,
    data: Vec<u8>,
}

#[derive(Clone)]
struct SavedItem(Vec<Representation>);

struct Snapshot {
    items: Vec<SavedItem>,
    sequence: isize,
    complete: bool,
}

struct ReadCopy {
    sequence: isize,
    text: Result<String, SelectionError>,
}

struct NativeClipboard {
    pasteboard: Retained<NSPasteboard>,
}

impl NativeClipboard {
    fn general() -> Self {
        Self {
            pasteboard: NSPasteboard::generalPasteboard(),
        }
    }

    #[cfg(test)]
    fn unique() -> Self {
        Self {
            pasteboard: NSPasteboard::pasteboardWithUniqueName(),
        }
    }

    fn snapshot(&self, context: &Context) -> Result<Snapshot, SelectionError> {
        context.check()?;
        let sequence = self.pasteboard.changeCount();
        let Some(items) = self.pasteboard.pasteboardItems() else {
            return Ok(Snapshot {
                items: Vec::new(),
                sequence,
                complete: true,
            });
        };
        let item_count = items.len();
        let mut complete = item_count <= MAX_ITEMS;
        let mut saved_items = Vec::new();
        let mut representations = 0usize;
        let mut total = 0usize;
        'items: for item in items.iter().take(MAX_ITEMS) {
            context.check()?;
            let mut saved = Vec::new();
            for data_type in item.types().iter() {
                representations += 1;
                if representations > MAX_REPRESENTATIONS {
                    complete = false;
                    break 'items;
                }
                if data_type.len() > MAX_TYPE_BYTES {
                    complete = false;
                    continue;
                }
                let name = data_type.to_string();
                if name.is_empty() {
                    complete = false;
                    continue;
                }
                let Some(data) = item.dataForType(&data_type) else {
                    complete = false;
                    continue;
                };
                let Some(next) = total
                    .checked_add(name.len())
                    .and_then(|total| total.checked_add(data.len()))
                else {
                    complete = false;
                    break 'items;
                };
                if next > MAX_SNAPSHOT {
                    complete = false;
                    break 'items;
                }
                let bytes = data.to_vec();
                total = next;
                saved.push(Representation { name, data: bytes });
            }
            if saved.is_empty() {
                complete = false;
            }
            saved_items.push(SavedItem(saved));
        }
        if self.pasteboard.changeCount() != sequence {
            return Err(SelectionError::new(
                ErrorKind::ClipboardAmbiguous,
                "pasteboard changed during snapshot",
            ));
        }
        Ok(Snapshot {
            items: saved_items,
            sequence,
            complete,
        })
    }

    fn make_items(
        items: &[SavedItem],
    ) -> Result<Vec<Retained<ProtocolObject<dyn NSPasteboardWriting>>>, SelectionError> {
        let mut output = Vec::with_capacity(items.len());
        for item in items {
            let native = NSPasteboardItem::new();
            for representation in &item.0 {
                let data_type = NSString::from_str(&representation.name);
                let data = NSData::with_bytes(&representation.data);
                if !native.setData_forType(&data, &data_type) {
                    return Err(SelectionError::new(
                        ErrorKind::NativeApi,
                        "NSPasteboardItem setData",
                    ));
                }
            }
            output.push(ProtocolObject::from_retained(native));
        }
        Ok(output)
    }

    fn write_items(&self, items: &[SavedItem]) -> Result<isize, SelectionError> {
        let native = Self::make_items(items)?;
        self.pasteboard.clearContents();
        if !native.is_empty() {
            let array = NSArray::from_retained_slice(&native);
            if !self.pasteboard.writeObjects(&array) {
                return Err(SelectionError::new(
                    ErrorKind::NativeApi,
                    "NSPasteboard writeObjects",
                ));
            }
        }
        Ok(self.pasteboard.changeCount())
    }

    fn install_marker(&self, token: &str) -> Result<isize, SelectionError> {
        self.write_items(&[SavedItem(vec![Representation {
            name: MARKER_TYPE.into(),
            data: token.as_bytes().to_vec(),
        }])])
    }

    fn marker_present(&self, token: &str) -> bool {
        let marker_type = NSString::from_str(MARKER_TYPE);
        self.pasteboard
            .dataForType(&marker_type)
            .is_some_and(|value| value.len() == token.len() && value.to_vec() == token.as_bytes())
    }

    fn copied_text(
        &self,
        marker: Option<&str>,
        max_text_bytes: usize,
    ) -> Result<String, SelectionError> {
        if marker.is_some_and(|marker| self.marker_present(marker)) {
            return Err(SelectionError::new(
                ErrorKind::ClipboardAmbiguous,
                "pasteboard marker remained after Copy",
            ));
        }
        let value = self
            .pasteboard
            .stringForType(unsafe { NSPasteboardTypeString })
            .ok_or_else(|| {
                SelectionError::new(ErrorKind::ClipboardAmbiguous, "Copy produced no plain text")
            })?;
        if value.len() > max_text_bytes {
            return Err(SelectionError::new(
                ErrorKind::LimitExceeded,
                "pasteboard text",
            ));
        }
        validate_text(value.to_string(), max_text_bytes)
    }

    fn restore(&self, snapshot: &Snapshot, expected: isize) -> ClipboardStatus {
        restoration_status(expected, self.pasteboard.changeCount(), || {
            self.write_items(&snapshot.items).map(|_| ())
        })
    }

    fn read_copy(
        &self,
        context: &Context,
        target: &accessibility::CopyTarget,
        baseline: isize,
        marker: Option<&str>,
    ) -> Result<ReadCopy, SelectionError> {
        let deadline = context
            .state
            .deadline
            .checked_sub(Duration::from_millis(100))
            .unwrap_or(context.state.deadline);
        loop {
            let sequence = self.pasteboard.changeCount();
            if copy_changed(baseline, sequence) {
                let text = target
                    .validate(context, context.stage_deadline(Duration::from_millis(200)))
                    .and_then(|()| self.copied_text(marker, context.options.max_text_bytes));
                let final_sequence = self.pasteboard.changeCount();
                if final_sequence != sequence {
                    continue;
                }
                return Ok(ReadCopy {
                    sequence: final_sequence,
                    text,
                });
            }
            context.check()?;
            validate(context)?;
            if Instant::now() >= deadline {
                return Err(SelectionError::new(ErrorKind::TimedOut, "Copy response"));
            }
            std::thread::sleep(
                deadline
                    .saturating_duration_since(Instant::now())
                    .min(Duration::from_millis(5)),
            );
        }
    }
}

fn validate_text(text: String, max_text_bytes: usize) -> Result<String, SelectionError> {
    if text.is_empty() {
        Err(SelectionError::new(
            ErrorKind::ClipboardAmbiguous,
            "Copy returned empty text",
        ))
    } else if text.len() > max_text_bytes {
        Err(SelectionError::new(
            ErrorKind::LimitExceeded,
            "pasteboard text",
        ))
    } else {
        Ok(text)
    }
}

fn copy_changed(baseline: isize, current: isize) -> bool {
    baseline != current
}

fn restoration_status<E>(
    expected: isize,
    current: isize,
    restore: impl FnOnce() -> Result<(), E>,
) -> ClipboardStatus {
    if current != expected {
        ClipboardStatus::Superseded
    } else if restore().is_ok() {
        ClipboardStatus::Restored
    } else {
        ClipboardStatus::RestorationFailed
    }
}

fn require_sequence(current: isize, expected: isize) -> Result<(), SelectionError> {
    if current == expected {
        Ok(())
    } else {
        let mut error = SelectionError::new(
            ErrorKind::ClipboardAmbiguous,
            "pasteboard changed before Copy",
        );
        error.clipboard_status = ClipboardStatus::Superseded;
        Err(error)
    }
}

fn wait_for_keys(context: &Context) -> Result<(), SelectionError> {
    let state = CGEventSourceStateID::CombinedSessionState;
    let mask = CGEventFlags::MaskShift
        | CGEventFlags::MaskControl
        | CGEventFlags::MaskAlternate
        | CGEventFlags::MaskCommand
        | CGEventFlags::MaskSecondaryFn;
    loop {
        context.check()?;
        let flags = CGEventSource::flags_state(state);
        if !flags.intersects(mask) && !CGEventSource::key_state(state, COPY_KEY_CODE) {
            return Ok(());
        }
        std::thread::sleep(
            context
                .state
                .deadline
                .saturating_duration_since(Instant::now())
                .min(Duration::from_millis(5)),
        );
    }
}

struct CopyEvents {
    down: CFRetained<CGEvent>,
    up: CFRetained<CGEvent>,
}

trait EventPoster {
    fn post_copy(&self, process_id: u32);
}

impl CopyEvents {
    fn new() -> Result<Self, SelectionError> {
        let source = CGEventSource::new(CGEventSourceStateID::HIDSystemState).ok_or_else(|| {
            SelectionError::new(ErrorKind::InputInjectionFailed, "CGEventSourceCreate")
        })?;
        let down =
            CGEvent::new_keyboard_event(Some(&source), COPY_KEY_CODE, true).ok_or_else(|| {
                SelectionError::new(
                    ErrorKind::InputInjectionFailed,
                    "CGEventCreateKeyboardEvent key down",
                )
            })?;
        let up =
            CGEvent::new_keyboard_event(Some(&source), COPY_KEY_CODE, false).ok_or_else(|| {
                SelectionError::new(
                    ErrorKind::InputInjectionFailed,
                    "CGEventCreateKeyboardEvent key up",
                )
            })?;
        CGEvent::set_flags(Some(&down), CGEventFlags::MaskCommand);
        CGEvent::set_flags(Some(&up), CGEventFlags::MaskCommand);
        Ok(Self { down, up })
    }
}

impl EventPoster for CopyEvents {
    fn post_copy(&self, process_id: u32) {
        CGEvent::post_to_pid(process_id as i32, Some(&self.down));
        CGEvent::post_to_pid(process_id as i32, Some(&self.up));
    }
}

fn marker(context: &Context) -> String {
    format!(
        "{}:{}:{}",
        std::process::id(),
        context.source.process_id,
        NEXT_MARKER.fetch_add(1, Ordering::Relaxed)
    )
}

fn with_permissions<T>(
    event_access: bool,
    prepare_target: impl FnOnce() -> Result<T, SelectionError>,
    capture: impl FnOnce(T) -> CaptureResult,
) -> CaptureResult {
    if !event_access {
        return Err(SelectionError::new(
            ErrorKind::AccessDenied,
            "macOS event-posting permission",
        ));
    }
    capture(prepare_target()?)
}

pub(super) fn capture(context: &Context) -> CaptureResult {
    context.check()?;
    validate(context)?;
    with_permissions(
        CGPreflightPostEventAccess(),
        || {
            accessibility::prepare_copy_target(
                context,
                context.stage_deadline(Duration::from_millis(200)),
            )
        },
        |target| capture_authorized(context, target),
    )
}

fn capture_authorized(context: &Context, target: accessibility::CopyTarget) -> CaptureResult {
    target.validate(context, context.stage_deadline(Duration::from_millis(200)))?;
    wait_for_keys(context)?;
    let events = CopyEvents::new()?;
    let clipboard = NativeClipboard::general();
    let snapshot = clipboard.snapshot(context)?;
    validate(context)?;
    require_sequence(clipboard.pasteboard.changeCount(), snapshot.sequence)?;

    let token = snapshot.complete.then(|| marker(context));
    let baseline = if let Some(token) = token.as_deref() {
        context.state.copy_started.store(true, Ordering::Release);
        match clipboard.install_marker(token) {
            Ok(sequence) => sequence,
            Err(mut error) => {
                error.clipboard_status =
                    clipboard.restore(&snapshot, clipboard.pasteboard.changeCount());
                return Err(error);
            }
        }
    } else {
        snapshot.sequence
    };

    if let Err(mut error) =
        target.validate(context, context.stage_deadline(Duration::from_millis(200)))
    {
        error.clipboard_status = if snapshot.complete {
            clipboard.restore(&snapshot, baseline)
        } else {
            ClipboardStatus::Unchanged
        };
        return Err(error);
    }
    require_sequence(clipboard.pasteboard.changeCount(), baseline)?;
    context.state.copy_started.store(true, Ordering::Release);
    events.post_copy(context.source.process_id);
    let copied = clipboard.read_copy(context, &target, baseline, token.as_deref());

    match copied {
        Ok(copied) => {
            let status = if snapshot.complete {
                clipboard.restore(&snapshot, copied.sequence)
            } else {
                ClipboardStatus::PreservationIncomplete
            };
            let text = context.check().and(copied.text).map_err(|mut error| {
                error.clipboard_status = status;
                error
            })?;
            assemble(
                vec![SelectedRange {
                    text,
                    bounds: Vec::new(),
                }],
                context.source.clone(),
                RetrievalMethod::Clipboard,
                status,
                context.options.max_text_bytes,
            )
        }
        Err(mut error) => {
            error.clipboard_status = if snapshot.complete {
                clipboard.restore(&snapshot, baseline)
            } else {
                ClipboardStatus::Unknown
            };
            Err(error)
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::runtime::RequestState;
    use std::cell::Cell;
    use std::sync::Arc;

    fn context() -> Context {
        Context {
            source: SourceApplication {
                native_window: None,
                process_id: std::process::id(),
                native_focus: None,
                executable: "test".into(),
            },
            options: CaptureOptions::default(),
            state: Arc::new(RequestState::new(Instant::now() + Duration::from_secs(2))),
        }
    }

    #[test]
    fn unique_pasteboard_snapshot_marker_and_restore_preserve_items_and_types() {
        let clipboard = NativeClipboard::unique();
        let original = vec![
            SavedItem(vec![
                Representation {
                    name: "public.utf8-plain-text".into(),
                    data: b"hello".to_vec(),
                },
                Representation {
                    name: "org.snow-shot.test".into(),
                    data: vec![0, 1, 2, 3],
                },
            ]),
            SavedItem(vec![Representation {
                name: "public.utf8-plain-text".into(),
                data: b"second".to_vec(),
            }]),
        ];
        clipboard.write_items(&original).unwrap();
        let snapshot = clipboard.snapshot(&context()).unwrap();
        assert!(snapshot.complete);
        assert_eq!(snapshot.items.len(), 2);
        let marker_sequence = clipboard.install_marker("token").unwrap();
        assert!(clipboard.marker_present("token"));
        assert_eq!(
            clipboard.restore(&snapshot, marker_sequence),
            ClipboardStatus::Restored
        );
        let restored = clipboard.snapshot(&context()).unwrap();
        assert!(restored.complete);
        assert_eq!(restored.items.len(), 2);
        assert_eq!(restored.items[0].0[0].data, b"hello");
        assert_eq!(restored.items[0].0[1].data, [0, 1, 2, 3]);
        assert_eq!(restored.items[1].0[0].data, b"second");
    }

    #[test]
    fn restoration_never_overwrites_a_newer_pasteboard_writer() {
        let clipboard = NativeClipboard::unique();
        clipboard
            .write_items(&[SavedItem(vec![Representation {
                name: "public.utf8-plain-text".into(),
                data: b"old".to_vec(),
            }])])
            .unwrap();
        let snapshot = clipboard.snapshot(&context()).unwrap();
        let marker_sequence = clipboard.install_marker("token").unwrap();
        clipboard
            .write_items(&[SavedItem(vec![Representation {
                name: "public.utf8-plain-text".into(),
                data: b"newer".to_vec(),
            }])])
            .unwrap();
        assert_eq!(
            clipboard.restore(&snapshot, marker_sequence),
            ClipboardStatus::Superseded
        );
        assert_eq!(
            clipboard
                .pasteboard
                .stringForType(unsafe { NSPasteboardTypeString })
                .unwrap()
                .to_string(),
            "newer"
        );
    }

    #[test]
    fn snapshot_limits_mark_preservation_incomplete_without_clearing() {
        let clipboard = NativeClipboard::unique();
        let original: Vec<_> = (0..=MAX_ITEMS)
            .map(|index| {
                SavedItem(vec![Representation {
                    name: "public.utf8-plain-text".into(),
                    data: index.to_string().into_bytes(),
                }])
            })
            .collect();
        let sequence = clipboard.write_items(&original).unwrap();
        let snapshot = clipboard.snapshot(&context()).unwrap();
        assert!(!snapshot.complete);
        assert_eq!(snapshot.sequence, sequence);
        assert_eq!(clipboard.pasteboard.changeCount(), sequence);
        assert_eq!(snapshot.items.len(), MAX_ITEMS);
    }

    #[test]
    fn marker_empty_non_text_and_oversized_results_are_rejected() {
        let clipboard = NativeClipboard::unique();
        clipboard.install_marker("request-token").unwrap();
        assert_eq!(
            clipboard
                .copied_text(Some("request-token"), 1024)
                .unwrap_err()
                .kind,
            ErrorKind::ClipboardAmbiguous
        );

        clipboard
            .write_items(&[SavedItem(vec![Representation {
                name: "org.snow-shot.binary".into(),
                data: vec![1, 2, 3],
            }])])
            .unwrap();
        assert_eq!(
            clipboard.copied_text(None, 1024).unwrap_err().kind,
            ErrorKind::ClipboardAmbiguous
        );

        clipboard
            .write_items(&[SavedItem(vec![Representation {
                name: "public.utf8-plain-text".into(),
                data: Vec::new(),
            }])])
            .unwrap();
        assert_eq!(
            clipboard.copied_text(None, 1024).unwrap_err().kind,
            ErrorKind::ClipboardAmbiguous
        );

        clipboard
            .write_items(&[SavedItem(vec![Representation {
                name: "public.utf8-plain-text".into(),
                data: "你好😀".as_bytes().to_vec(),
            }])])
            .unwrap();
        assert_eq!(clipboard.copied_text(None, 10).unwrap(), "你好😀");
        assert_eq!(
            clipboard.copied_text(None, 9).unwrap_err().kind,
            ErrorKind::LimitExceeded
        );
    }

    #[test]
    fn no_op_copy_and_mocked_cleanup_statuses_are_exact() {
        assert!(!copy_changed(7, 7));
        assert!(copy_changed(7, 8));
        assert!(require_sequence(7, 7).is_ok());
        assert_eq!(
            require_sequence(8, 7).unwrap_err().clipboard_status,
            ClipboardStatus::Superseded
        );

        let called = Cell::new(false);
        assert_eq!(
            restoration_status(7, 8, || {
                called.set(true);
                Ok::<(), ()>(())
            }),
            ClipboardStatus::Superseded
        );
        assert!(
            !called.get(),
            "a superseding writer must never be overwritten"
        );
        assert_eq!(
            restoration_status(8, 8, || Ok::<(), ()>(())),
            ClipboardStatus::Restored
        );
        assert_eq!(
            restoration_status(8, 8, || Err::<(), ()>(())),
            ClipboardStatus::RestorationFailed
        );
    }

    #[test]
    fn mocked_permissions_stop_before_pasteboard_mutation() {
        let prepared = Cell::new(0);
        let mutations = Cell::new(0);
        let denied = with_permissions(
            false,
            || {
                prepared.set(prepared.get() + 1);
                Ok(())
            },
            |_| {
                mutations.set(mutations.get() + 1);
                Ok(SelectionOutcome::Unsupported)
            },
        );
        assert_eq!(denied.unwrap_err().kind, ErrorKind::AccessDenied);
        assert_eq!(prepared.get(), 0);
        assert_eq!(mutations.get(), 0);

        let ax_denied = with_permissions(
            true,
            || {
                prepared.set(prepared.get() + 1);
                Err(SelectionError::new(
                    ErrorKind::AccessDenied,
                    "mock Accessibility permission",
                ))
            },
            |_: ()| {
                mutations.set(mutations.get() + 1);
                Ok(SelectionOutcome::Unsupported)
            },
        );
        assert_eq!(ax_denied.unwrap_err().kind, ErrorKind::AccessDenied);
        assert_eq!(prepared.get(), 1);
        assert_eq!(mutations.get(), 0);
    }

    #[test]
    fn mocked_event_interface_posts_once_to_the_captured_process() {
        struct MockEvents {
            posts: Cell<usize>,
            process_id: Cell<u32>,
        }
        impl EventPoster for MockEvents {
            fn post_copy(&self, process_id: u32) {
                self.posts.set(self.posts.get() + 1);
                self.process_id.set(process_id);
            }
        }

        let events = MockEvents {
            posts: Cell::new(0),
            process_id: Cell::new(0),
        };
        events.post_copy(4242);
        assert_eq!(events.posts.get(), 1);
        assert_eq!(events.process_id.get(), 4242);
    }

    #[test]
    #[ignore = "requires an interactive foreground application, AX/event permission, and the general pasteboard"]
    fn foreground_copy_uses_the_general_pasteboard() {
        eprintln!("Select text in another application within five seconds.");
        std::thread::sleep(Duration::from_secs(5));
        let source = super::super::capture_source().unwrap();
        let context = Context {
            source,
            options: CaptureOptions {
                strategy: CaptureStrategy::Clipboard,
                ..Default::default()
            },
            state: Arc::new(RequestState::new(Instant::now() + Duration::from_secs(2))),
        };
        let SelectionOutcome::Selected(text) = capture(&context).unwrap() else {
            panic!("interactive Copy did not return selected text")
        };
        assert!(!text.text.is_empty());
        assert_eq!(text.method, RetrievalMethod::Clipboard);
        assert!(matches!(
            text.clipboard_status,
            ClipboardStatus::Restored | ClipboardStatus::PreservationIncomplete
        ));
    }
}
