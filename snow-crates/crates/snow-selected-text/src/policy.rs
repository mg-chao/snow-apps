use std::sync::Arc;
use std::time::{Duration, Instant};

use crate::model::*;
use crate::runtime::RequestState;

#[derive(Debug)]
pub(crate) enum Probe {
    Selected(Vec<SelectedRange>),
    Empty,
    Unsupported,
}

#[derive(Clone)]
pub(crate) struct Context {
    pub source: SourceWindow,
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
    fn uia(&mut self, context: &Context, deadline: Instant) -> Result<Probe, SelectionError>;
    fn native(&mut self, context: &Context, deadline: Instant) -> Result<Probe, SelectionError>;
    fn copy(&mut self, context: &Context) -> CaptureResult;
}

pub(crate) fn acquire(backend: &mut impl Backend, context: &Context) -> CaptureResult {
    context.check()?;
    backend.validate_target(context)?;
    for (method, budget) in [
        (RetrievalMethod::Uia, Duration::from_millis(600)),
        (RetrievalMethod::NativeEdit, Duration::from_millis(150)),
    ] {
        let enabled = match context.options.strategy {
            CaptureStrategy::Auto => true,
            CaptureStrategy::Uia => method == RetrievalMethod::Uia,
            CaptureStrategy::NativeEdit => method == RetrievalMethod::NativeEdit,
            CaptureStrategy::Clipboard => false,
        };
        if !enabled {
            continue;
        }
        context.check()?;
        let deadline = context.stage_deadline(budget);
        let probe = match method {
            RetrievalMethod::Uia => backend.uia(context, deadline),
            RetrievalMethod::NativeEdit => backend.native(context, deadline),
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
        }
    }
    let copy_enabled = match context.options.strategy {
        CaptureStrategy::Auto => context.options.copy_fallback,
        CaptureStrategy::Clipboard => true,
        CaptureStrategy::Uia | CaptureStrategy::NativeEdit => false,
    };
    if !copy_enabled {
        return Ok(SelectionOutcome::Unsupported);
    }
    context.check()?;
    backend.validate_target(context)?;
    backend.copy(context)
}

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
