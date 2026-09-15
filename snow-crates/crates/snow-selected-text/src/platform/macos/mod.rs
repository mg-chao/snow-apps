mod accessibility;
mod clipboard;

use std::path::Path;
use std::time::Instant;

use objc2::rc::autoreleasepool;
use objc2_app_kit::NSWorkspace;

use crate::model::*;
use crate::policy::{Backend, Context, Probe};

fn frontmost_process() -> Result<(u32, String), SelectionError> {
    autoreleasepool(|_| {
        let application = NSWorkspace::sharedWorkspace()
            .frontmostApplication()
            .ok_or_else(|| {
                SelectionError::new(ErrorKind::NoForegroundWindow, "frontmost application")
            })?;
        let process_id = application.processIdentifier();
        if process_id <= 0 {
            return Err(SelectionError::new(
                ErrorKind::TargetUnavailable,
                "frontmost process identifier",
            ));
        }
        let path = application
            .executableURL()
            .and_then(|url| url.path())
            .map(|path| path.to_string())
            .ok_or_else(|| {
                SelectionError::new(ErrorKind::TargetUnavailable, "frontmost executable")
            })?;
        let executable = Path::new(&path)
            .file_name()
            .and_then(|name| name.to_str())
            .filter(|name| !name.is_empty())
            .map(str::to_owned)
            .ok_or_else(|| {
                SelectionError::new(ErrorKind::MalformedData, "frontmost executable basename")
            })?;
        Ok((process_id as u32, executable))
    })
}

pub(crate) fn capture_source() -> Result<SourceApplication, SelectionError> {
    let (process_id, executable) = frontmost_process()?;
    Ok(SourceApplication {
        native_window: None,
        process_id,
        native_focus: None,
        executable,
    })
}

pub(super) fn validate(context: &Context) -> Result<(), SelectionError> {
    context.check()?;
    let (process_id, executable) = frontmost_process()?;
    if process_id != context.source.process_id
        || !executable.eq_ignore_ascii_case(&context.source.executable)
    {
        return Err(SelectionError::new(
            ErrorKind::TargetChanged,
            "frontmost application identity",
        ));
    }
    Ok(())
}

pub(crate) struct Worker;

impl Worker {
    pub fn new() -> Result<Self, SelectionError> {
        Ok(Self)
    }
}

impl Backend for Worker {
    fn validate_target(&mut self, context: &Context) -> Result<(), SelectionError> {
        validate(context)
    }

    fn accessibility(
        &mut self,
        context: &Context,
        deadline: Instant,
    ) -> Result<Probe, SelectionError> {
        autoreleasepool(|_| accessibility::capture(context, deadline))
    }

    fn native_control(
        &mut self,
        _context: &Context,
        _deadline: Instant,
    ) -> Result<Probe, SelectionError> {
        Ok(Probe::Unsupported)
    }

    fn copy(&mut self, context: &Context) -> CaptureResult {
        autoreleasepool(|_| clipboard::capture(context))
    }
}
