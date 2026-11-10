//! One active full-display producer and one preparing/retiring producer. Native
//! discovery, startup, first-frame readiness and teardown never block encoding.
use super::*;
use crossbeam_channel::{Receiver, TryRecvError};

pub(super) struct Prepared {
    pub display: DisplayInfo,
    pub stream: VideoStream,
    pub frame: NativeFrame,
    topology: u64,
    exclusions: u64,
    layout: Vec<DisplayInfo>,
}

enum Flight {
    Preparing {
        result: Receiver<MacResult<Prepared>>,
        cancellation: crate::CancellationToken,
        topology: u64,
        exclusions: u64,
        destination: Option<DesktopRect>,
    },
    Retiring(Receiver<()>),
}

pub(super) struct FollowState {
    pub nominal: DesktopRect,
    pub display: DisplayInfo,
    pub changed_at: Instant,
    flight: Option<Flight>,
    retry_at: Instant,
    pub recover: bool,
    pub published: bool,
    layout: Vec<DisplayInfo>,
    layout_topology: u64,
}

pub(super) fn cursor_position() -> Option<(f64, f64)> {
    let event = objc2_core_graphics::CGEvent::new(None)?;
    let point = objc2_core_graphics::CGEvent::location(Some(&event));
    (point.x.is_finite() && point.y.is_finite()).then_some((point.x, point.y))
}

pub(super) fn capture_config(config: &DesktopConfig, display: &DisplayInfo) -> CaptureConfig {
    CaptureConfig {
        cancellation: config.cancellation.clone(),
        target: Target::Display(display.id),
        source: None,
        output: Some(display.pixels),
        dynamic_range: config.dynamic_range,
        cursor: config.cursor,
        frames_per_second: config.fps,
        persistent_window: false,
        excluded_windows: config.excluded_windows.clone(),
        excluded_processes: config.excluded_processes.clone(),
        timeout: config.timeout,
    }
}

impl FollowState {
    pub fn new(config: &DesktopConfig, plan: &Plan) -> MacResult<(Self, SharedSnapshot)> {
        let DesktopTarget::Region(nominal) = config.target else {
            return Err(geometry("follow mouse requires a desktop region"));
        };
        let layout_topology = super::topology()?;
        let content = content::shareable_content_cancelable(config.timeout, &config.cancellation)?;
        let displays = content::displays_from(&content)?;
        let point = cursor_position().unwrap_or((
            nominal.x + nominal.width * 0.5,
            nominal.y + nominal.height * 0.5,
        ));
        let display = displays
            .iter()
            .find(|display| super::follow_policy::contains(display.bounds, point))
            .cloned()
            .ok_or(MacError::TargetUnavailable)?;
        // Validate before acquiring any native producer.
        super::follow_policy::viewport(nominal, display.bounds, point).map_err(geometry)?;
        plan.transform.output.byte_len(8).map_err(geometry)?;
        Ok((
            Self {
                nominal,
                display,
                changed_at: Instant::now(),
                flight: None,
                retry_at: Instant::now(),
                recover: false,
                published: false,
                layout: displays,
                layout_topology,
            },
            SharedSnapshot::new(content),
        ))
    }

    pub fn viewport(&self, cursor: (f64, f64)) -> MacResult<DesktopRect> {
        super::follow_policy::viewport(self.nominal, self.display.bounds, cursor).map_err(geometry)
    }

    pub fn poll(
        &mut self,
        config: &DesktopConfig,
        topology: u64,
        cursor: (f64, f64),
        exclusions: u64,
        filters_settled: bool,
    ) -> Option<Prepared> {
        let flight = self.flight.take()?;
        match flight {
            Flight::Retiring(result) => {
                if matches!(result.try_recv(), Err(TryRecvError::Empty)) {
                    self.flight = Some(Flight::Retiring(result));
                }
            }
            Flight::Preparing {
                result,
                cancellation,
                topology: requested_topology,
                exclusions: requested_exclusions,
                destination,
            } => {
                if topology != requested_topology
                    || exclusions != requested_exclusions
                    || !filters_settled
                    || super::follow_policy::contains(self.display.bounds, cursor) && !self.recover
                    || destination
                        .is_some_and(|bounds| !super::follow_policy::contains(bounds, cursor))
                {
                    cancellation.cancel();
                }
                match result.try_recv() {
                    Ok(Ok(prepared))
                        if !cancellation.is_canceled()
                            && super::follow_policy::can_commit(
                                prepared.topology,
                                topology,
                                prepared.exclusions,
                                exclusions,
                                filters_settled,
                                prepared.display.bounds,
                                cursor,
                            ) =>
                    {
                        self.layout.clone_from(&prepared.layout);
                        self.layout_topology = topology;
                        return Some(prepared);
                    }
                    Ok(Ok(prepared)) => self.retire(vec![prepared.stream]),
                    Ok(Err(_)) | Err(TryRecvError::Disconnected) => {
                        self.retry_at = Instant::now() + Duration::from_millis(250);
                    }
                    Err(TryRecvError::Empty) => {
                        if config.cancellation.is_canceled() {
                            cancellation.cancel();
                        }
                        self.flight = Some(Flight::Preparing {
                            result,
                            cancellation,
                            topology: requested_topology,
                            exclusions: requested_exclusions,
                            destination,
                        });
                    }
                }
            }
        }
        None
    }

    pub fn prepare(
        &mut self,
        config: &DesktopConfig,
        topology: u64,
        cursor: (f64, f64),
        exclusions: u64,
        required: &[u32],
    ) -> MacResult<()> {
        if self.flight.is_some() || Instant::now() < self.retry_at {
            return Ok(());
        }
        let cancellation = crate::CancellationToken::default();
        let mut worker_config = config.clone();
        worker_config.cancellation = cancellation.clone();
        let session_cancellation = config.cancellation.clone();
        let required = required.to_vec();
        let destination = (topology == self.layout_topology)
            .then(|| {
                self.layout
                    .iter()
                    .find(|display| super::follow_policy::contains(display.bounds, cursor))
                    .map(|display| display.bounds)
            })
            .flatten();
        let (sender, result) = crossbeam_channel::bounded(1);
        let worker = std::thread::Builder::new()
            .name("snow-follow-prepare".into())
            .spawn(move || {
                snow_core::qos::apply_current_thread();
                let prepared = (|| {
                    if session_cancellation.is_canceled() {
                        return Err(MacError::Canceled);
                    }
                    let content = content::shareable_content_with_required_windows(
                        worker_config.timeout,
                        &worker_config.cancellation,
                        &required,
                    )?;
                    let displays = content::displays_from(&content)?;
                    let display = displays
                        .iter()
                        .find(|display| super::follow_policy::contains(display.bounds, cursor))
                        .cloned()
                        .ok_or(MacError::TargetUnavailable)?;
                    let stream = VideoStream::start_with(
                        &capture_config(&worker_config, &display),
                        Some(&content),
                    )?;
                    let frame = stream.next_frame(worker_config.timeout)?;
                    if session_cancellation.is_canceled()
                        || worker_config.cancellation.is_canceled()
                    {
                        return Err(MacError::Canceled);
                    }
                    if frame.image.size() != display.pixels || super::topology()? != topology {
                        return Err(MacError::Inactive);
                    }
                    Ok(Prepared {
                        display,
                        stream,
                        frame,
                        topology,
                        exclusions,
                        layout: displays,
                    })
                })();
                let _ = sender.send(prepared);
            });
        if worker.is_ok() {
            self.flight = Some(Flight::Preparing {
                result,
                cancellation,
                topology,
                exclusions,
                destination,
            });
        } else {
            self.retry_at = Instant::now() + Duration::from_millis(250);
        }
        Ok(())
    }

    pub fn retire(&mut self, streams: Vec<VideoStream>) {
        if streams.is_empty() {
            return;
        }
        debug_assert!(self.flight.is_none());
        let (sender, result) = crossbeam_channel::bounded(1);
        // Keep teardown in the same bounded resource slot as preparation. A
        // rapid crossing cannot accumulate old SCStreams waiting to stop.
        let streams = Arc::new(Mutex::new(Some(streams)));
        let retiring = streams.clone();
        if std::thread::Builder::new()
            .name("snow-follow-retire".into())
            .spawn(move || {
                snow_core::qos::apply_current_thread();
                drop(retiring.lock().unwrap_or_else(|e| e.into_inner()).take());
                let _ = sender.send(());
            })
            .is_ok()
        {
            self.flight = Some(Flight::Retiring(result));
        }
        // Only thread-creation failure falls back to local native teardown.
    }

    pub fn cancel_prepare(&mut self) {
        if let Some(Flight::Preparing { cancellation, .. }) = &self.flight {
            cancellation.cancel();
        }
    }

    pub fn wait_for_progress(&self, deadline: Instant, cancellation: &crate::CancellationToken) {
        // Merely wait for readiness: the next refresh owns the result and its
        // latest topology/filter/pointer checks. Do not drain it here.
        match &self.flight {
            Some(Flight::Preparing { result, .. }) => {
                super::follow_wait::wait_for_progress(
                    Some(result),
                    cancellation.receiver(),
                    deadline,
                    None,
                );
            }
            Some(Flight::Retiring(result)) => {
                super::follow_wait::wait_for_progress(
                    Some(result),
                    cancellation.receiver(),
                    deadline,
                    None,
                );
            }
            None => {
                super::follow_wait::wait_for_progress::<()>(
                    None,
                    cancellation.receiver(),
                    deadline,
                    Some(self.retry_at),
                );
            }
        }
    }
}
impl Drop for FollowState {
    fn drop(&mut self) {
        self.cancel_prepare();
    }
}
