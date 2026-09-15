use std::sync::Arc;
use std::time::{Duration, Instant};

use crate::model::*;
use crate::runtime::RequestState;

#[derive(Debug)]
pub(crate) enum Probe {
    Selected(Vec<SelectedRange>),
    Empty,
    Unsupported,
    /// The provider failed in a way that Auto may safely route around.
    #[cfg(any(target_os = "macos", test))]
    Recoverable(SelectionError),
}

#[derive(Clone)]
pub(crate) struct Context {
    pub source: SourceApplication,
    pub options: CaptureOptions,
    pub state: Arc<RequestState>,
}

impl Context {
    pub fn check(&self) -> Result<(), SelectionError> {
        self.state.check()
    }

    pub fn stage_deadline(&self, duration: Duration) -> Instant {
        self.state.deadline.min(Instant::now() + duration)
    }
}

pub(crate) trait Backend {
    fn validate_target(&mut self, context: &Context) -> Result<(), SelectionError>;
    fn accessibility(
        &mut self,
        context: &Context,
        deadline: Instant,
    ) -> Result<Probe, SelectionError>;
    fn native_control(
        &mut self,
        context: &Context,
        deadline: Instant,
    ) -> Result<Probe, SelectionError>;
    fn copy(&mut self, context: &Context) -> CaptureResult;
}

pub(crate) fn acquire(backend: &mut impl Backend, context: &Context) -> CaptureResult {
    context.check()?;
    backend.validate_target(context)?;
    for (method, budget) in [
        (RetrievalMethod::Accessibility, Duration::from_millis(600)),
        (RetrievalMethod::NativeControl, Duration::from_millis(150)),
    ] {
        let enabled = match context.options.strategy {
            CaptureStrategy::Auto => true,
            CaptureStrategy::Accessibility => method == RetrievalMethod::Accessibility,
            CaptureStrategy::NativeControl => method == RetrievalMethod::NativeControl,
            CaptureStrategy::Clipboard => false,
        };
        if !enabled {
            continue;
        }
        context.check()?;
        let deadline = context.stage_deadline(budget);
        let probe = match method {
            RetrievalMethod::Accessibility => backend.accessibility(context, deadline),
            RetrievalMethod::NativeControl => backend.native_control(context, deadline),
            RetrievalMethod::Clipboard => unreachable!(),
        }?;
        context.check()?;
        backend.validate_target(context)?;
        match probe {
            Probe::Selected(ranges) => {
                return assemble(
                    ranges,
                    context.source.clone(),
                    method,
                    ClipboardStatus::Unchanged,
                    context.options.max_text_bytes,
                );
            }
            Probe::Empty => return Ok(SelectionOutcome::NoSelection),
            Probe::Unsupported => {}
            #[cfg(any(target_os = "macos", test))]
            Probe::Recoverable(error) => {
                if context.options.strategy != CaptureStrategy::Auto {
                    return Err(error);
                }
                if !context.options.copy_fallback {
                    return Err(error);
                }
            }
        }
    }
    let copy_enabled = match context.options.strategy {
        CaptureStrategy::Auto => context.options.copy_fallback,
        CaptureStrategy::Clipboard => true,
        CaptureStrategy::Accessibility | CaptureStrategy::NativeControl => false,
    };
    if !copy_enabled {
        return Ok(SelectionOutcome::Unsupported);
    }
    context.check()?;
    backend.validate_target(context)?;
    backend.copy(context)
}

#[cfg(any(windows, test))]
pub(crate) fn terminal_executable(name: &str) -> bool {
    [
        "windowsterminal.exe",
        "windowsterminalpreview.exe",
        "openconsole.exe",
        "conhost.exe",
        "powershell.exe",
        "pwsh.exe",
        "cmd.exe",
        "wsl.exe",
        "bash.exe",
        "mintty.exe",
    ]
    .iter()
    .any(|terminal| name.eq_ignore_ascii_case(terminal))
}
