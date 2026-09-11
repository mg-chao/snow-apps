//! Stream multiplexer: merges multiple `StreamHandle` sources into one output stream.
//!
//! The multiplexer uses a builder pattern for source registration and runs one
//! forwarding thread per source. A main loop merges per-source bounded channels
//! into a single output channel with audio-priority pre-drain, command fan-out,
//! timeout-based disconnect detection, and completion signaling.

use std::sync::Arc;
use std::sync::atomic::{AtomicU64, Ordering};
use std::thread;
use std::time::Duration;

use crossbeam_channel::{self as cbc, Receiver, Sender};
use smallvec::SmallVec;

use snow_core::stream_queue::StreamQueue;
use snow_core::streaming::StreamHandle;

use crate::event::{DeliveryLane, SourceId, StreamEvent, TaggedEvent};

// ---------------------------------------------------------------------------
// Configuration types
// ---------------------------------------------------------------------------

/// Configuration for the multiplexer runtime.
pub struct MultiplexerConfig {
    /// Maximum time the main loop blocks waiting for events from any source.
    /// Default: 25 ms.
    pub select_timeout: Duration,
    /// Source that receives priority pre-drain before each blocking wait.
    /// Typically the audio source. Default: `None`.
    pub priority_source: Option<SourceId>,
    /// Maximum number of priority-source events drained per iteration before
    /// the blocking select. Default: 8.
    pub priority_drain_batch: usize,
    /// Capacity of the merged output channel. Default: 16.
    pub output_capacity: usize,
    /// Per-item send timeout when the output channel is full. Items that
    /// exceed this timeout are dropped. Default: 10 ms.
    pub output_send_timeout: Duration,
}

impl Default for MultiplexerConfig {
    fn default() -> Self {
        Self {
            select_timeout: Duration::from_millis(25),
            priority_source: None,
            priority_drain_batch: 8,
            output_capacity: 16,
            output_send_timeout: Duration::from_millis(10),
        }
    }
}

/// Configuration for a single source registered with the multiplexer.
pub struct SourceConfig {
    /// Unique identifier for this source.
    pub source_id: SourceId,
    /// Capacity of the per-source bounded channel between the forwarding
    /// thread and the main loop.
    pub channel_capacity: usize,
    /// Timeout for sends into the per-source channel. Events that exceed
    /// this timeout are dropped by the forwarding thread.
    pub send_timeout: Duration,
}

// ---------------------------------------------------------------------------
// Command / Status enums
// ---------------------------------------------------------------------------

/// Commands that can be sent to the multiplexer for fan-out to all sources.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum MuxCommand {
    /// Pause all active sources.
    Pause,
    /// Resume all active sources.
    Resume,
    /// Stop all sources and shut down the multiplexer.
    Stop,
}

/// Status events reported by the multiplexer about individual sources or
/// overall completion.
#[derive(Clone, Debug, PartialEq, Eq)]
pub enum MuxStatus {
    /// The source emitted a terminal `StreamEnded` event and closed cleanly.
    SourceEnded(SourceId),
    /// The source's forwarding channel disconnected without a terminal event.
    SourceDisconnected(SourceId),
    /// The source's forwarding thread panicked.
    SourceForwarderPanicked(SourceId),
    /// All registered sources have terminated (ended, disconnected, or panicked).
    Completed,
}

// ---------------------------------------------------------------------------
// Internal: per-source channel message
// ---------------------------------------------------------------------------

/// Message sent from a forwarding thread to the main loop.
enum SourceMsg<O> {
    /// One or more mapped output events.
    Events(SmallVec<[O; 2]>),
    /// The source stream ended cleanly (`is_stream_ended()` was true).
    StreamEnded,
}

// ---------------------------------------------------------------------------
// Internal: type-erased source registration
// ---------------------------------------------------------------------------

/// A type-erased source registration collected by the builder.
struct SourceRegistration<O: StreamEvent> {
    config: SourceConfig,
    /// Spawns the forwarding thread and returns the per-source receiver.
    spawn_fn: Box<dyn FnOnce(SourceId) -> SourceHandle<O> + Send>,
}

/// Handle returned by spawning a forwarding thread.
struct SourceHandle<O: StreamEvent> {
    rx: Receiver<SourceMsg<O>>,
    cmd_tx: Sender<MuxCommand>,
    join: thread::JoinHandle<()>,
}

// ---------------------------------------------------------------------------
// Builder
// ---------------------------------------------------------------------------

/// Builder for constructing a [`StreamMultiplexer`].
pub struct StreamMultiplexerBuilder<O: StreamEvent> {
    config: MultiplexerConfig,
    sources: Vec<SourceRegistration<O>>,
}

impl<O: StreamEvent> StreamMultiplexerBuilder<O> {
    /// Create a new builder with the given multiplexer configuration.
    pub fn new(config: MultiplexerConfig) -> Self {
        Self {
            config,
            sources: Vec::new(),
        }
    }

    /// Register a source stream with the multiplexer.
    ///
    /// - `source`: per-source configuration (id, channel capacity, send timeout).
    /// - `handle`: the leaf streaming handle that produces events of type `E`.
    /// - `mapper`: transforms a `TaggedEvent<E>` into zero or more output events.
    pub fn register<E, H, F>(&mut self, source: SourceConfig, handle: H, mapper: F) -> &mut Self
    where
        E: StreamEvent,
        H: StreamHandle<E> + 'static,
        F: Fn(TaggedEvent<E>) -> SmallVec<[O; 2]> + Send + 'static,
    {
        let send_timeout = source.send_timeout;
        let channel_capacity = source.channel_capacity;

        let spawn_fn = Box::new(move |sid: SourceId| {
            let (tx, rx) = cbc::bounded::<SourceMsg<O>>(channel_capacity);
            let (cmd_tx, cmd_rx) = cbc::unbounded::<MuxCommand>();
            let join = thread::Builder::new()
                .name(format!("mux-fwd-{}", sid.0))
                .spawn(move || {
                    forwarding_thread(sid, handle, mapper, tx, send_timeout, cmd_rx);
                })
                .expect("failed to spawn multiplexer forwarding thread");
            SourceHandle { rx, cmd_tx, join }
        });

        self.sources.push(SourceRegistration {
            config: source,
            spawn_fn,
        });
        self
    }

    /// Consume the builder and start the multiplexer.
    ///
    /// Spawns one forwarding thread per registered source and a main loop
    /// thread that merges per-source channels into the output channel.
    pub fn build(self) -> StreamMultiplexer<O> {
        let output_queue = Arc::new(StreamQueue::new(self.config.output_capacity));
        let (cmd_tx, cmd_rx) = cbc::unbounded::<MuxCommand>();
        let (status_tx, status_rx) = cbc::unbounded::<MuxStatus>();
        let drop_counter = Arc::new(AtomicU64::new(0));

        // Spawn forwarding threads and collect receivers + join handles.
        let mut source_entries: Vec<(SourceId, Receiver<SourceMsg<O>>)> = Vec::new();
        let mut join_handles: Vec<(SourceId, thread::JoinHandle<()>)> = Vec::new();
        let mut source_cmd_txs: Vec<(SourceId, Sender<MuxCommand>)> = Vec::new();

        for reg in self.sources {
            let sid = reg.config.source_id;
            let sh = (reg.spawn_fn)(sid);
            source_entries.push((sid, sh.rx));
            source_cmd_txs.push((sid, sh.cmd_tx));
            join_handles.push((sid, sh.join));
        }

        let config = self.config;
        let drop_counter_clone = Arc::clone(&drop_counter);
        let output_queue_worker = Arc::clone(&output_queue);

        let main_handle = thread::Builder::new()
            .name("mux-main".to_string())
            .spawn(move || {
                main_loop(
                    config,
                    source_entries,
                    source_cmd_txs,
                    join_handles,
                    output_queue_worker,
                    cmd_rx,
                    status_tx,
                    drop_counter_clone,
                );
            })
            .expect("failed to spawn multiplexer main loop thread");

        StreamMultiplexer {
            output_queue,
            cmd_tx,
            status_rx,
            drop_counter,
            _main_handle: Some(main_handle),
        }
    }
}

// ---------------------------------------------------------------------------
// StreamMultiplexer runtime handle
// ---------------------------------------------------------------------------

/// Runtime handle for a running stream multiplexer.
///
/// Provides access to the merged output channel, command input, and status
/// reporting.
pub struct StreamMultiplexer<O> {
    output_queue: Arc<StreamQueue<O>>,
    cmd_tx: Sender<MuxCommand>,
    status_rx: Receiver<MuxStatus>,
    drop_counter: Arc<AtomicU64>,
    _main_handle: Option<thread::JoinHandle<()>>,
}

impl<O: StreamEvent> StreamMultiplexer<O> {
    /// Receive the next merged output event, blocking.
    pub fn recv(&self) -> Result<O, snow_core::error::RecvError> {
        self.output_queue.recv().map(|(event, _)| event)
    }

    /// Try to receive a merged output event without blocking.
    pub fn try_recv(&self) -> Result<O, snow_core::error::TryRecvError> {
        self.output_queue.try_recv().map(|(event, _)| event)
    }

    /// Receive with timeout.
    pub fn recv_timeout(&self, timeout: Duration) -> Result<O, snow_core::error::RecvTimeoutError> {
        self.output_queue
            .recv_timeout(timeout)
            .map(|(event, _)| event)
    }

    /// Send a command to all active sources.
    pub fn send_command(&self, cmd: MuxCommand) -> Result<(), cbc::SendError<MuxCommand>> {
        self.cmd_tx.send(cmd)
    }

    /// Try to receive a status event without blocking.
    pub fn try_recv_status(&self) -> Result<MuxStatus, cbc::TryRecvError> {
        self.status_rx.try_recv()
    }

    /// Receive the next status event, blocking.
    pub fn recv_status(&self) -> Result<MuxStatus, cbc::RecvError> {
        self.status_rx.recv()
    }

    /// Number of output events dropped due to send timeout.
    pub fn dropped_count(&self) -> u64 {
        self.drop_counter.load(Ordering::Relaxed)
    }
}

// ---------------------------------------------------------------------------
// Forwarding thread
// ---------------------------------------------------------------------------

/// How long the forwarding thread waits for an event before checking
/// for pending commands. Short enough for responsive command handling,
/// long enough to avoid busy-spinning.
const FWD_CMD_POLL_INTERVAL: Duration = Duration::from_millis(5);

/// Per-source forwarding thread.
///
/// Owns the leaf `StreamHandle` and a bounded channel sender. Reads events
/// from the handle, wraps them in `TaggedEvent`, maps them, and sends the
/// results to the main loop. Detects terminal events via `is_stream_ended()`.
fn forwarding_thread<E, H, F, O>(
    source_id: SourceId,
    handle: H,
    mapper: F,
    tx: Sender<SourceMsg<O>>,
    send_timeout: Duration,
    cmd_rx: Receiver<MuxCommand>,
) where
    E: StreamEvent,
    H: StreamHandle<E>,
    F: Fn(TaggedEvent<E>) -> SmallVec<[O; 2]> + Send,
    O: StreamEvent,
{
    loop {
        // Process any pending commands. A disconnected command channel means
        // the main loop is gone, so stop the source and exit instead of
        // polling the handle forever.
        let commands_disconnected = loop {
            match cmd_rx.try_recv() {
                Ok(MuxCommand::Pause) => handle.pause(),
                Ok(MuxCommand::Resume) => handle.resume(),
                Ok(MuxCommand::Stop) => handle.stop(),
                Err(cbc::TryRecvError::Empty) => break false,
                Err(cbc::TryRecvError::Disconnected) => {
                    handle.stop();
                    break true;
                }
            }
        };
        if commands_disconnected {
            break;
        }

        // Use recv_timeout so we periodically wake up to check for commands.
        let event = match handle.recv_timeout(FWD_CMD_POLL_INTERVAL) {
            Ok(e) => e,
            Err(e) => {
                let e: snow_core::error::RecvTimeoutError = e.into();
                match e {
                    snow_core::error::RecvTimeoutError::Timeout => continue,
                    snow_core::error::RecvTimeoutError::Disconnected => break,
                }
            }
        };

        let is_terminal = event.is_stream_ended();
        let mapped = mapper(TaggedEvent {
            source: source_id,
            event,
        });

        if !mapped.is_empty() {
            let has_control = mapped
                .iter()
                .any(|item| item.delivery_lane() == DeliveryLane::Control);
            let send_ok = if has_control {
                tx.send(SourceMsg::Events(mapped)).is_ok()
            } else {
                tx.send_timeout(SourceMsg::Events(mapped), send_timeout)
                    .is_ok()
            };

            if !send_ok {
                if is_terminal {
                    send_stream_ended(&tx);
                    break;
                }
                continue;
            }
        }

        if is_terminal {
            send_stream_ended(&tx);
            break;
        }
    }
}

// ---------------------------------------------------------------------------
// Main loop
// ---------------------------------------------------------------------------

/// Main multiplexer loop.
#[allow(clippy::too_many_arguments)]
fn main_loop<O: StreamEvent>(
    config: MultiplexerConfig,
    mut alive: Vec<(SourceId, Receiver<SourceMsg<O>>)>,
    mut alive_cmd_txs: Vec<(SourceId, Sender<MuxCommand>)>,
    mut joins: Vec<(SourceId, thread::JoinHandle<()>)>,
    output_queue: Arc<StreamQueue<O>>,
    cmd_rx: Receiver<MuxCommand>,
    status_tx: Sender<MuxStatus>,
    drop_counter: Arc<AtomicU64>,
) {
    let total_sources = alive.len();
    let mut terminated_count: usize = 0;
    let mut stop_requested = false;
    let mut priority_bootstrap = config.priority_source.is_some();

    let priority_source = config.priority_source;

    loop {
        // Check for commands (non-blocking) and fan out to all alive sources.
        // A disconnected command channel means the consumer dropped the
        // multiplexer without sending Stop; treat that abandonment as a stop
        // request so sources and threads shut down instead of running forever.
        loop {
            match cmd_rx.try_recv() {
                Ok(cmd) => {
                    fanout_command(&alive_cmd_txs, cmd);
                    if cmd == MuxCommand::Stop {
                        stop_requested = true;
                    }
                }
                Err(cbc::TryRecvError::Empty) => break,
                Err(cbc::TryRecvError::Disconnected) => {
                    fanout_command(&alive_cmd_txs, MuxCommand::Stop);
                    stop_requested = true;
                    break;
                }
            }
        }

        if stop_requested {
            shutdown_drain(
                &config,
                &alive,
                &output_queue,
                &status_tx,
                &drop_counter,
                &mut terminated_count,
            );
            // Check for panicked forwarders among remaining joins.
            check_panicked_forwarders(&mut joins, &status_tx, &mut terminated_count);
            let _ = complete_if_done(total_sources, terminated_count, alive.len(), &status_tx);
            break;
        }

        let per_source_timeout = if alive.is_empty() {
            config.select_timeout.max(Duration::from_millis(1))
        } else {
            (config.select_timeout / (alive.len() as u32)).max(Duration::from_millis(1))
        };

        // Audio-priority pre-drain.
        if let Some(prio_id) = priority_source
            && let Some(prio_idx) = alive.iter().position(|(id, _)| *id == prio_id)
        {
            let mut drained = 0usize;
            while drained < config.priority_drain_batch {
                let prio_msg = match alive[prio_idx].1.try_recv() {
                    Ok(msg) => Some(msg),
                    // On startup, wait once for the priority source so
                    // non-priority events don't win an initial scheduling race.
                    Err(cbc::TryRecvError::Empty) if priority_bootstrap && drained == 0 => {
                        match alive[prio_idx].1.recv_timeout(per_source_timeout) {
                            Ok(msg) => Some(msg),
                            Err(cbc::RecvTimeoutError::Timeout) => None,
                            Err(cbc::RecvTimeoutError::Disconnected) => {
                                check_forwarder_panic(&mut joins, prio_id, &status_tx);
                                terminated_count += 1;
                                remove_source_at(&mut alive, &mut alive_cmd_txs, prio_idx);
                                break;
                            }
                        }
                    }
                    Err(cbc::TryRecvError::Empty) => None,
                    Err(cbc::TryRecvError::Disconnected) => {
                        check_forwarder_panic(&mut joins, prio_id, &status_tx);
                        terminated_count += 1;
                        remove_source_at(&mut alive, &mut alive_cmd_txs, prio_idx);
                        break;
                    }
                };

                let Some(prio_msg) = prio_msg else {
                    break;
                };

                match prio_msg {
                    SourceMsg::Events(events) => {
                        forward_events(
                            events,
                            &output_queue,
                            config.output_send_timeout,
                            &drop_counter,
                        );
                        drained += 1;
                    }
                    SourceMsg::StreamEnded => {
                        let _ = status_tx.send(MuxStatus::SourceEnded(prio_id));
                        terminated_count += 1;
                        remove_source_at(&mut alive, &mut alive_cmd_txs, prio_idx);
                        break;
                    }
                }
            }
        }
        priority_bootstrap = false;

        // Check completion after priority drain.
        if complete_if_done(total_sources, terminated_count, alive.len(), &status_tx) {
            break;
        }

        let mut to_remove: SmallVec<[usize; 4]> = SmallVec::new();

        for (idx, (sid, rx)) in alive.iter().enumerate() {
            match rx.recv_timeout(per_source_timeout) {
                Ok(SourceMsg::Events(events)) => {
                    forward_events(
                        events,
                        &output_queue,
                        config.output_send_timeout,
                        &drop_counter,
                    );
                }
                Ok(SourceMsg::StreamEnded) => {
                    let _ = status_tx.send(MuxStatus::SourceEnded(*sid));
                    terminated_count += 1;
                    to_remove.push(idx);
                }
                Err(cbc::RecvTimeoutError::Timeout) => {}
                Err(cbc::RecvTimeoutError::Disconnected) => {
                    check_forwarder_panic(&mut joins, *sid, &status_tx);
                    terminated_count += 1;
                    to_remove.push(idx);
                }
            }
        }

        // Remove terminated sources (reverse order to preserve indices).
        for &idx in to_remove.iter().rev() {
            remove_source_at(&mut alive, &mut alive_cmd_txs, idx);
        }

        // Check completion.
        if complete_if_done(total_sources, terminated_count, alive.len(), &status_tx) {
            break;
        }
    }

    output_queue.close();
}

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

/// Remove a source from both the alive receivers and command sender lists.
///
/// Uses index-based removal so duplicate `SourceId` registrations are handled
/// independently.
fn remove_source_at<O: StreamEvent>(
    alive: &mut Vec<(SourceId, Receiver<SourceMsg<O>>)>,
    alive_cmd_txs: &mut Vec<(SourceId, Sender<MuxCommand>)>,
    idx: usize,
) {
    if idx >= alive.len() {
        return;
    }

    let (sid, _) = alive.remove(idx);
    remove_one_cmd_tx(alive_cmd_txs, idx, sid);
}

/// Returns true when all sources have terminated or no sources remain alive.
fn should_complete(total_sources: usize, terminated_count: usize, alive_len: usize) -> bool {
    terminated_count >= total_sources || alive_len == 0
}

/// Emit `Completed` when all sources have terminated.
fn complete_if_done(
    total_sources: usize,
    terminated_count: usize,
    alive_len: usize,
    status_tx: &Sender<MuxStatus>,
) -> bool {
    if should_complete(total_sources, terminated_count, alive_len) {
        let _ = status_tx.send(MuxStatus::Completed);
        return true;
    }
    false
}

/// Remove one command sender entry for the removed source.
fn remove_one_cmd_tx(
    alive_cmd_txs: &mut Vec<(SourceId, Sender<MuxCommand>)>,
    idx: usize,
    sid: SourceId,
) {
    if idx < alive_cmd_txs.len() {
        let _ = alive_cmd_txs.remove(idx);
    } else if let Some(cmd_idx) = alive_cmd_txs.iter().position(|(id, _)| *id == sid) {
        // Vectors can be temporarily out of sync; remove only one entry.
        let _ = alive_cmd_txs.remove(cmd_idx);
    }
}

/// Drain remaining events during shutdown, prioritizing the audio source.
fn shutdown_drain<O: StreamEvent>(
    config: &MultiplexerConfig,
    alive: &[(SourceId, Receiver<SourceMsg<O>>)],
    output_queue: &Arc<StreamQueue<O>>,
    status_tx: &Sender<MuxStatus>,
    drop_counter: &Arc<AtomicU64>,
    terminated_count: &mut usize,
) {
    // Phase 1: Drain priority source first.
    let priority_idx = config
        .priority_source
        .and_then(|prio_id| alive.iter().position(|(id, _)| *id == prio_id));
    if let Some(prio_idx) = priority_idx {
        drain_source(
            &alive[prio_idx],
            output_queue,
            status_tx,
            drop_counter,
            config.output_send_timeout,
            terminated_count,
        );
    }

    // Phase 2: Drain remaining sources.
    for (idx, source) in alive.iter().enumerate() {
        if Some(idx) == priority_idx {
            continue;
        }
        drain_source(
            source,
            output_queue,
            status_tx,
            drop_counter,
            config.output_send_timeout,
            terminated_count,
        );
    }
}

/// Drain all pending events from a single source channel.
fn drain_source<O: StreamEvent>(
    source: &(SourceId, Receiver<SourceMsg<O>>),
    output_queue: &Arc<StreamQueue<O>>,
    status_tx: &Sender<MuxStatus>,
    drop_counter: &Arc<AtomicU64>,
    output_send_timeout: Duration,
    terminated_count: &mut usize,
) {
    let (sid, rx) = source;
    loop {
        match rx.try_recv() {
            Ok(SourceMsg::Events(events)) => {
                forward_events(events, output_queue, output_send_timeout, drop_counter);
            }
            Ok(SourceMsg::StreamEnded) => {
                let _ = status_tx.send(MuxStatus::SourceEnded(*sid));
                *terminated_count += 1;
                break;
            }
            Err(cbc::TryRecvError::Empty) => break,
            Err(cbc::TryRecvError::Disconnected) => {
                let _ = status_tx.send(MuxStatus::SourceDisconnected(*sid));
                *terminated_count += 1;
                break;
            }
        }
    }
}

/// Check if a specific forwarder thread panicked and report appropriate status.
fn check_forwarder_panic(
    joins: &mut Vec<(SourceId, thread::JoinHandle<()>)>,
    sid: SourceId,
    status_tx: &Sender<MuxStatus>,
) {
    if let Some(idx) = joins.iter().position(|(id, _)| *id == sid) {
        let (_, handle) = joins.remove(idx);
        // The channel is already disconnected, so the thread is either done or
        // finishing its unwind. Calling join() blocks until it completes, which
        // avoids the race where is_finished() returns false while the thread is
        // still unwinding from a panic.
        match handle.join() {
            Ok(()) => {
                // Clean disconnect -forwarder exited normally.
                let _ = status_tx.send(MuxStatus::SourceDisconnected(sid));
            }
            Err(_) => {
                let _ = status_tx.send(MuxStatus::SourceForwarderPanicked(sid));
            }
        }
    } else {
        let _ = status_tx.send(MuxStatus::SourceDisconnected(sid));
    }
}

/// Check all remaining join handles for panicked forwarders.
fn check_panicked_forwarders(
    joins: &mut Vec<(SourceId, thread::JoinHandle<()>)>,
    status_tx: &Sender<MuxStatus>,
    terminated_count: &mut usize,
) {
    // drain(..) yields owned items directly -no intermediate Vec needed.
    // We must collect unfinished handles to put them back.
    let mut unfinished = Vec::new();
    for (sid, handle) in joins.drain(..) {
        if handle.is_finished() {
            if handle.join().is_err() {
                let _ = status_tx.send(MuxStatus::SourceForwarderPanicked(sid));
                *terminated_count += 1;
            }
        } else {
            unfinished.push((sid, handle));
        }
    }
    *joins = unfinished;
}

fn forward_events<O: StreamEvent>(
    events: SmallVec<[O; 2]>,
    output_queue: &Arc<StreamQueue<O>>,
    output_send_timeout: Duration,
    drop_counter: &Arc<AtomicU64>,
) {
    for item in events {
        send_output(output_queue, item, output_send_timeout, drop_counter);
    }
}

/// Fan out one command to all active source forwarding threads.
fn fanout_command(alive_cmd_txs: &[(SourceId, Sender<MuxCommand>)], cmd: MuxCommand) {
    for (_sid, cmd_tx) in alive_cmd_txs {
        let _ = cmd_tx.send(cmd);
    }
}

fn send_stream_ended<O>(tx: &Sender<SourceMsg<O>>) {
    let _ = tx.send(SourceMsg::StreamEnded);
}

/// Send an output item into the merged queue. Data-plane drops are counted,
/// while control-plane items stay reliable via `StreamQueue`.
fn send_output<O: StreamEvent>(
    output_queue: &Arc<StreamQueue<O>>,
    item: O,
    _timeout: Duration,
    drop_counter: &Arc<AtomicU64>,
) {
    if output_queue.push(item).dropped.is_some() {
        drop_counter.fetch_add(1, Ordering::Relaxed);
    }
}

#[cfg(test)]
#[allow(clippy::while_let_loop)]
mod tests {
    use super::*;
    use snow_core::streaming::StreamHandle;

    use crate::event::{DeliveryLane, SourceId, TaggedEvent};
    use crossbeam_channel as cbc;
    use proptest::prelude::*;
    use std::collections::HashMap;
    use std::time::Duration;

    // -----------------------------------------------------------------------
    // Mock event and stream handle
    // -----------------------------------------------------------------------

    /// A simple event carrying a sequence number and a terminal flag.
    #[derive(Clone, Debug, PartialEq, Eq)]
    struct MockEvent {
        seq: u32,
        terminal: bool,
    }

    impl snow_core::event::StreamEvent for MockEvent {
        fn is_stream_ended(&self) -> bool {
            self.terminal
        }
    }

    /// Output type collected from the multiplexer.
    #[derive(Clone, Debug)]
    struct TestOutput {
        source: SourceId,
        seq: u32,
    }

    impl snow_core::event::StreamEvent for TestOutput {
        fn delivery_lane(&self) -> DeliveryLane {
            DeliveryLane::Data
        }
    }

    /// A mock `StreamHandle` that delivers a pre-loaded sequence of events
    /// through a crossbeam channel.
    struct MockHandle {
        rx: cbc::Receiver<MockEvent>,
        _tx: cbc::Sender<MockEvent>,
    }

    impl MockHandle {
        /// Create a handle pre-loaded with the given events.
        fn with_events(events: Vec<MockEvent>) -> Self {
            let (tx, rx) = cbc::bounded(events.len() + 1);
            for e in events {
                tx.send(e).unwrap();
            }
            Self { rx, _tx: tx }
        }
    }

    impl StreamHandle<MockEvent> for MockHandle {
        type RecvError = snow_core::error::RecvError;
        type TryRecvError = snow_core::error::TryRecvError;
        type RecvTimeoutError = snow_core::error::RecvTimeoutError;

        fn recv(&self) -> Result<MockEvent, Self::RecvError> {
            self.rx
                .recv()
                .map_err(|_| snow_core::error::RecvError::Disconnected)
        }

        fn try_recv(&self) -> Result<MockEvent, Self::TryRecvError> {
            self.rx.try_recv().map_err(|e| match e {
                cbc::TryRecvError::Empty => snow_core::error::TryRecvError::Empty,
                cbc::TryRecvError::Disconnected => snow_core::error::TryRecvError::Disconnected,
            })
        }

        fn recv_timeout(&self, timeout: Duration) -> Result<MockEvent, Self::RecvTimeoutError> {
            self.rx.recv_timeout(timeout).map_err(|e| match e {
                cbc::RecvTimeoutError::Timeout => snow_core::error::RecvTimeoutError::Timeout,
                cbc::RecvTimeoutError::Disconnected => {
                    snow_core::error::RecvTimeoutError::Disconnected
                }
            })
        }

        fn stop(&self) {}
        fn pause(&self) {}
        fn resume(&self) {}
        fn is_paused(&self) -> bool {
            false
        }
        fn is_running(&self) -> bool {
            true
        }
    }

    // -----------------------------------------------------------------------
    // Property test
    // -----------------------------------------------------------------------

    /// Strategy: generate event counts for 2..=5 sources, each with 1..=20 events.
    fn arb_source_event_counts() -> impl Strategy<Value = Vec<u32>> {
        prop::collection::vec(1u32..=20, 2..=5)
    }

    proptest! {
        #![proptest_config(ProptestConfig::with_cases(100))]

        /// **Validates: Requirements 10.1**
        ///
        /// Property 7: Multiplexer preserves per-source event order.
        ///
        /// For any set of sources each emitting a numbered sequence of events,
        /// the multiplexer output must contain those events in the same
        /// per-source relative order, regardless of interleaving across sources.
        #[test]
        fn prop_multiplexer_preserves_per_source_order(
            event_counts in arb_source_event_counts(),
        ) {
            let mut builder = StreamMultiplexerBuilder::new(MultiplexerConfig {
                select_timeout: Duration::from_millis(50),
                priority_source: None,
                priority_drain_batch: 8,
                output_capacity: 256,
                output_send_timeout: Duration::from_millis(100),
            });

            for (idx, &count) in event_counts.iter().enumerate() {
                let sid = SourceId(idx as u8);

                // Build event sequence: `count` data events + 1 terminal.
                let mut events: Vec<MockEvent> = (0..count)
                    .map(|seq| MockEvent { seq, terminal: false })
                    .collect();
                events.push(MockEvent { seq: count, terminal: true });

                let handle = MockHandle::with_events(events);

                builder.register(
                    SourceConfig {
                        source_id: sid,
                        channel_capacity: 64,
                        send_timeout: Duration::from_millis(100),
                    },
                    handle,
                    move |te: TaggedEvent<MockEvent>| {
                        smallvec::smallvec![TestOutput {
                            source: te.source,
                            seq: te.event.seq,
                        }]
                    },
                );
            }

            let mux = builder.build();

            // Collect all output events until the output channel disconnects.
            let mut collected: Vec<TestOutput> = Vec::new();
            loop {
                match mux.recv_timeout(Duration::from_secs(5)) {
                    Ok(item) => collected.push(item),
                    Err(_) => break,
                }
            }

            // Group collected events by source.
            let mut per_source: HashMap<u8, Vec<u32>> = HashMap::new();
            for item in &collected {
                per_source
                    .entry(item.source.0)
                    .or_default()
                    .push(item.seq);
            }

            // Verify per-source order is preserved.
            for (src_idx, &count) in event_counts.iter().enumerate() {
                let sid = src_idx as u8;
                let seqs = per_source.get(&sid).cloned().unwrap_or_default();

                // Filter to non-terminal events for order check.
                let data_seqs: Vec<u32> = seqs.iter()
                    .copied()
                    .filter(|&s| s < count)
                    .collect();

                // The data sequence numbers must be strictly increasing
                // (they were emitted as 0, 1, 2, ..., count-1).
                for window in data_seqs.windows(2) {
                    prop_assert!(
                        window[0] < window[1],
                        "Per-source order violated for source {}: seq {} appeared before {}",
                        sid,
                        window[0],
                        window[1],
                    );
                }

                // All data events should be present (none dropped with our
                // generous capacity/timeout settings).
                let expected: Vec<u32> = (0..count).collect();
                prop_assert_eq!(
                    &data_seqs,
                    &expected,
                    "Source {} missing events: expected {:?}, got {:?}",
                    sid,
                    expected,
                    data_seqs,
                );
            }
        }
    }

    // -----------------------------------------------------------------------
    // Abandonment safety: dropping the handle must stop the sources
    // -----------------------------------------------------------------------

    /// Dropping the multiplexer handle without ever sending Stop must stop
    /// every registered source instead of leaving the main and forwarding
    /// threads (and the sources they own) running forever.
    #[test]
    fn dropping_handle_without_stop_stops_sources() {
        let mut command_logs = Vec::new();
        let mut builder = StreamMultiplexerBuilder::new(MultiplexerConfig {
            select_timeout: Duration::from_millis(25),
            priority_source: None,
            priority_drain_batch: 8,
            output_capacity: 16,
            output_send_timeout: Duration::from_millis(10),
        });

        for idx in 0..2 {
            let sid = SourceId(idx as u8);
            let (handle, log) = CommandTrackingHandle::new();
            command_logs.push(log);
            builder.register(
                SourceConfig {
                    source_id: sid,
                    channel_capacity: 8,
                    send_timeout: Duration::from_millis(50),
                },
                handle,
                move |te: TaggedEvent<MockEvent>| {
                    smallvec::smallvec![TestOutput {
                        source: te.source,
                        seq: te.event.seq,
                    }]
                },
            );
        }

        // Abandon the multiplexer without a Stop command. The handles keep
        // their event senders alive and never emit a terminal event, so the
        // only way these sources can be stopped is the abandonment path.
        drop(builder.build());

        let deadline = std::time::Instant::now() + Duration::from_secs(5);
        for (idx, log) in command_logs.iter().enumerate() {
            loop {
                let received = log.lock().unwrap().clone();
                if received.last() == Some(&MuxCommand::Stop) {
                    break;
                }
                assert!(
                    std::time::Instant::now() < deadline,
                    "source {} was not stopped after the multiplexer was dropped; \
                     commands received: {:?}",
                    idx,
                    received,
                );
                std::thread::sleep(Duration::from_millis(10));
            }
        }
    }

    // -----------------------------------------------------------------------
    // Command-tracking mock handle for command propagation tests
    // -----------------------------------------------------------------------

    /// A mock `StreamHandle` that records commands (stop/pause/resume) it receives.
    ///
    /// Events are delivered via a channel. The handle keeps the event sender
    /// alive so the forwarding thread blocks on `recv()` until we explicitly
    /// send a terminal event or drop the sender.
    struct CommandTrackingHandle {
        rx: cbc::Receiver<MockEvent>,
        tx: cbc::Sender<MockEvent>,
        commands: Arc<std::sync::Mutex<Vec<MuxCommand>>>,
    }

    impl CommandTrackingHandle {
        fn new() -> (Self, Arc<std::sync::Mutex<Vec<MuxCommand>>>) {
            let (tx, rx) = cbc::bounded(64);
            let commands = Arc::new(std::sync::Mutex::new(Vec::new()));
            let handle = Self {
                rx,
                tx,
                commands: Arc::clone(&commands),
            };
            (handle, commands)
        }

        /// Get a sender to inject events into this handle.
        fn sender(&self) -> cbc::Sender<MockEvent> {
            self.tx.clone()
        }
    }

    impl StreamHandle<MockEvent> for CommandTrackingHandle {
        type RecvError = snow_core::error::RecvError;
        type TryRecvError = snow_core::error::TryRecvError;
        type RecvTimeoutError = snow_core::error::RecvTimeoutError;

        fn recv(&self) -> Result<MockEvent, Self::RecvError> {
            self.rx
                .recv()
                .map_err(|_| snow_core::error::RecvError::Disconnected)
        }

        fn try_recv(&self) -> Result<MockEvent, Self::TryRecvError> {
            self.rx.try_recv().map_err(|e| match e {
                cbc::TryRecvError::Empty => snow_core::error::TryRecvError::Empty,
                cbc::TryRecvError::Disconnected => snow_core::error::TryRecvError::Disconnected,
            })
        }

        fn recv_timeout(&self, timeout: Duration) -> Result<MockEvent, Self::RecvTimeoutError> {
            self.rx.recv_timeout(timeout).map_err(|e| match e {
                cbc::RecvTimeoutError::Timeout => snow_core::error::RecvTimeoutError::Timeout,
                cbc::RecvTimeoutError::Disconnected => {
                    snow_core::error::RecvTimeoutError::Disconnected
                }
            })
        }

        fn stop(&self) {
            self.commands.lock().unwrap().push(MuxCommand::Stop);
        }

        fn pause(&self) {
            self.commands.lock().unwrap().push(MuxCommand::Pause);
        }

        fn resume(&self) {
            self.commands.lock().unwrap().push(MuxCommand::Resume);
        }

        fn is_paused(&self) -> bool {
            false
        }
        fn is_running(&self) -> bool {
            true
        }
    }

    // -----------------------------------------------------------------------
    // Strategy for command sequences
    // -----------------------------------------------------------------------

    fn arb_mux_command() -> impl Strategy<Value = MuxCommand> {
        prop_oneof![Just(MuxCommand::Pause), Just(MuxCommand::Resume),]
    }

    fn arb_command_sequence() -> impl Strategy<Value = Vec<MuxCommand>> {
        prop::collection::vec(arb_mux_command(), 1..=8)
    }

    fn arb_source_count() -> impl Strategy<Value = usize> {
        2usize..=5
    }

    proptest! {
        #![proptest_config(ProptestConfig::with_cases(100))]

        /// **Validates: Requirements 5.5**
        ///
        /// Property 8: Multiplexer command propagation.
        ///
        /// For any set of N active source handles registered with the
        /// multiplexer, sending a sequence of Pause/Resume commands SHALL
        /// propagate each command to every active source handle exactly once
        /// per command issued.
        #[test]
        fn prop_multiplexer_command_propagation(
            num_sources in arb_source_count(),
            commands in arb_command_sequence(),
        ) {
            let mut builder = StreamMultiplexerBuilder::new(MultiplexerConfig {
                select_timeout: Duration::from_millis(50),
                priority_source: None,
                priority_drain_batch: 8,
                output_capacity: 256,
                output_send_timeout: Duration::from_millis(100),
            });

            // Create command-tracking handles and collect their command logs
            // and event senders.
            let mut command_logs: Vec<Arc<std::sync::Mutex<Vec<MuxCommand>>>> = Vec::new();
            let mut event_senders: Vec<cbc::Sender<MockEvent>> = Vec::new();

            for idx in 0..num_sources {
                let sid = SourceId(idx as u8);
                let (handle, log) = CommandTrackingHandle::new();
                let sender = handle.sender();
                command_logs.push(log);
                event_senders.push(sender);

                builder.register(
                    SourceConfig {
                        source_id: sid,
                        channel_capacity: 64,
                        send_timeout: Duration::from_millis(100),
                    },
                    handle,
                    move |te: TaggedEvent<MockEvent>| {
                        smallvec::smallvec![TestOutput {
                            source: te.source,
                            seq: te.event.seq,
                        }]
                    },
                );
            }

            let mux = builder.build();

            // Send all commands. The main loop fans each out to all source
            // forwarding threads via per-source command channels.
            for &cmd in &commands {
                mux.send_command(cmd).unwrap();
            }

            // Give the main loop time to fan out all commands, and the
            // forwarding threads time to process them (they poll every 5ms).
            // We wait long enough for multiple poll cycles.
            std::thread::sleep(Duration::from_millis(
                50 + (commands.len() as u64) * 15,
            ));

            // Now send terminal events to cleanly shut down.
            for sender in &event_senders {
                let _ = sender.send(MockEvent {
                    seq: 0,
                    terminal: true,
                });
            }

            // Drain output to let the multiplexer complete.
            loop {
                match mux.recv_timeout(Duration::from_secs(2)) {
                    Ok(_) => {}
                    Err(_) => break,
                }
            }

            // Verify each handle received exactly the expected commands.
            for (idx, log) in command_logs.iter().enumerate() {
                let received = log.lock().unwrap().clone();
                prop_assert_eq!(
                    &received,
                    &commands,
                    "Source {} received commands {:?}, expected {:?}",
                    idx,
                    received,
                    commands,
                );
            }
        }
    }

    // -----------------------------------------------------------------------
    // Strategy for completion test: source count + termination order
    // -----------------------------------------------------------------------

    /// Strategy: generate a source count (2..=5) and a random permutation
    /// representing the order in which sources will be terminated.
    fn arb_completion_scenario() -> impl Strategy<Value = (usize, Vec<usize>)> {
        (2usize..=5).prop_flat_map(|n| {
            let indices: Vec<usize> = (0..n).collect();
            (Just(n), Just(indices).prop_shuffle())
        })
    }

    /// Strategy: per-source data event counts (1..=10) for a given number of sources.
    fn arb_data_counts(n: usize) -> impl Strategy<Value = Vec<u32>> {
        prop::collection::vec(1u32..=10, n..=n)
    }

    proptest! {
        #![proptest_config(ProptestConfig::with_cases(100))]

        /// **Validates: Requirements 5.6, 5.7**
        ///
        /// Property 9: Multiplexer completion after all sources end.
        ///
        /// For any set of registered sources (2 to 5), when every source
        /// ends (by emitting a terminal event) in a random order, the
        /// multiplexer SHALL:
        /// 1. Report `SourceEnded(sid)` for each source that terminates.
        /// 2. Signal `Completed` after all sources have ended.
        #[test]
        fn prop_multiplexer_completion_after_all_sources_end(
            (num_sources, termination_order) in arb_completion_scenario(),
            data_counts in arb_data_counts(5).prop_map(|v| v),
        ) {
            // Trim data_counts to actual source count.
            let data_counts: Vec<u32> = data_counts.into_iter().take(num_sources).collect();

            let mut builder = StreamMultiplexerBuilder::new(MultiplexerConfig {
                select_timeout: Duration::from_millis(50),
                priority_source: None,
                priority_drain_batch: 8,
                output_capacity: 256,
                output_send_timeout: Duration::from_millis(100),
            });

            // Use CommandTrackingHandle so we can inject events on our schedule.
            let mut event_senders: Vec<(SourceId, cbc::Sender<MockEvent>)> = Vec::new();

            for idx in 0..num_sources {
                let sid = SourceId(idx as u8);
                let (handle, _log) = CommandTrackingHandle::new();
                let sender = handle.sender();
                event_senders.push((sid, sender));

                builder.register(
                    SourceConfig {
                        source_id: sid,
                        channel_capacity: 64,
                        send_timeout: Duration::from_millis(100),
                    },
                    handle,
                    move |te: TaggedEvent<MockEvent>| {
                        smallvec::smallvec![TestOutput {
                            source: te.source,
                            seq: te.event.seq,
                        }]
                    },
                );
            }

            let mux = builder.build();

            // Send data events for each source first.
            for (idx, &count) in data_counts.iter().enumerate() {
                let (_, ref sender) = event_senders[idx];
                for seq in 0..count {
                    let _ = sender.send(MockEvent { seq, terminal: false });
                }
            }

            // End sources in the random termination order.
            for &src_idx in &termination_order {
                let (_, ref sender) = event_senders[src_idx];
                let _ = sender.send(MockEvent {
                    seq: data_counts[src_idx],
                    terminal: true,
                });
                // Small delay to allow the multiplexer to process the
                // terminal event before ending the next source.
                std::thread::sleep(Duration::from_millis(15));
            }

            // Drain all output events.
            loop {
                match mux.recv_timeout(Duration::from_secs(2)) {
                    Ok(_) => {}
                    Err(_) => break,
                }
            }

            // Collect all status events.
            let mut statuses: Vec<MuxStatus> = Vec::new();
            loop {
                match mux.try_recv_status() {
                    Ok(s) => statuses.push(s),
                    Err(_) => break,
                }
            }

            // Verify: each source has a SourceEnded status.
            let mut ended_sources: std::collections::HashSet<u8> = std::collections::HashSet::new();
            for status in &statuses {
                if let MuxStatus::SourceEnded(sid) = status {
                    ended_sources.insert(sid.0);
                }
            }

            for idx in 0..num_sources {
                prop_assert!(
                    ended_sources.contains(&(idx as u8)),
                    "Source {} did not receive SourceEnded status. Statuses: {:?}",
                    idx,
                    statuses,
                );
            }

            // Verify: Completed is the last status event.
            let last = statuses.last();
            prop_assert!(
                matches!(last, Some(MuxStatus::Completed)),
                "Expected Completed as last status, got {:?}. All statuses: {:?}",
                last,
                statuses,
            );

            // Verify: Completed appears exactly once.
            let completed_count = statuses.iter()
                .filter(|s| matches!(s, MuxStatus::Completed))
                .count();
            prop_assert_eq!(
                completed_count,
                1,
                "Expected exactly 1 Completed status, got {}. Statuses: {:?}",
                completed_count,
                statuses,
            );
        }
    }

    // -----------------------------------------------------------------------
    // Property 10: Per-source backpressure independence
    // -----------------------------------------------------------------------

    /// Strategy: generate a number of "free" sources (1..=3) that will deliver
    /// events normally, plus a data-event count for each free source (1..=10).
    fn arb_backpressure_scenario() -> impl Strategy<Value = (usize, Vec<u32>)> {
        (1usize..=3).prop_flat_map(|free_count| {
            let counts = prop::collection::vec(1u32..=10, free_count..=free_count);
            (Just(free_count), counts)
        })
    }

    proptest! {
        #![proptest_config(ProptestConfig::with_cases(100))]

        /// **Validates: Requirements 5.4**
        ///
        /// Property 10: Per-source backpressure independence.
        ///
        /// For any multiplexer with multiple registered sources, backpressure
        /// (full channel) on one source's bounded channel SHALL NOT block
        /// event delivery from other sources.
        ///
        /// Approach: register a "blocked" source with a very small channel
        /// capacity (1) and flood it so its per-source channel fills up,
        /// then verify that other "free" sources still deliver all their
        /// events promptly.
        #[test]
        fn prop_per_source_backpressure_independence(
            (free_count, free_data_counts) in arb_backpressure_scenario(),
        ) {
            // The "blocked" source gets channel_capacity=1 and a very short
            // send_timeout so events are dropped quickly when the channel is full.
            let blocked_sid = SourceId(0);

            let mut builder = StreamMultiplexerBuilder::new(MultiplexerConfig {
                select_timeout: Duration::from_millis(50),
                priority_source: None,
                priority_drain_batch: 8,
                output_capacity: 256,
                output_send_timeout: Duration::from_millis(100),
            });

            // Register the blocked source with capacity=1 and short send_timeout.
            let (blocked_handle, _blocked_cmds) = CommandTrackingHandle::new();
            let blocked_sender = blocked_handle.sender();

            builder.register(
                SourceConfig {
                    source_id: blocked_sid,
                    channel_capacity: 1,
                    send_timeout: Duration::from_millis(5),
                },
                blocked_handle,
                move |te: TaggedEvent<MockEvent>| {
                    smallvec::smallvec![TestOutput {
                        source: te.source,
                        seq: te.event.seq,
                    }]
                },
            );

            // Register free sources with generous capacity.
            let mut free_senders: Vec<(SourceId, cbc::Sender<MockEvent>)> = Vec::new();

            for i in 0..free_count {
                let sid = SourceId((i + 1) as u8);
                let (handle, _cmds) = CommandTrackingHandle::new();
                let sender = handle.sender();
                free_senders.push((sid, sender));

                builder.register(
                    SourceConfig {
                        source_id: sid,
                        channel_capacity: 64,
                        send_timeout: Duration::from_millis(100),
                    },
                    handle,
                    move |te: TaggedEvent<MockEvent>| {
                        smallvec::smallvec![TestOutput {
                            source: te.source,
                            seq: te.event.seq,
                        }]
                    },
                );
            }

            let mux = builder.build();

            // Flood the blocked source with many events to create backpressure.
            // The channel capacity is 1, so most of these will cause the
            // forwarding thread to hit send_timeout and drop events.
            for seq in 0..50 {
                let _ = blocked_sender.send(MockEvent { seq, terminal: false });
            }

            // Give the blocked source's forwarding thread time to fill its
            // per-source channel and start experiencing backpressure.
            std::thread::sleep(Duration::from_millis(30));

            // Now send events on the free sources and verify they deliver.
            for (i, (_, sender)) in free_senders.iter().enumerate() {
                let count = free_data_counts[i];
                for seq in 0..count {
                    sender.send(MockEvent { seq, terminal: false }).unwrap();
                }
            }

            // Collect output events with a reasonable timeout.
            // Free sources should deliver within this window despite the
            // blocked source experiencing backpressure.
            let deadline = std::time::Instant::now() + Duration::from_secs(3);
            let mut per_source: HashMap<u8, Vec<u32>> = HashMap::new();

            loop {
                let remaining = deadline.saturating_duration_since(std::time::Instant::now());
                if remaining.is_zero() {
                    break;
                }
                match mux.recv_timeout(remaining.min(Duration::from_millis(50))) {
                    Ok(item) => {
                        per_source
                            .entry(item.source.0)
                            .or_default()
                            .push(item.seq);

                        // Early exit as soon as all free sources delivered.
                        let all_free_delivered = free_senders.iter().enumerate().all(|(i, (sid, _))| {
                            let count = free_data_counts[i];
                            let delivered = per_source.get(&sid.0).map_or(0, |v| v.len());
                            delivered >= count as usize
                        });
                        if all_free_delivered {
                            break;
                        }
                    }
                    Err(_) => {
                        // Check if all free sources have delivered all events.
                        let all_free_delivered = free_senders.iter().enumerate().all(|(i, (sid, _))| {
                            let count = free_data_counts[i];
                            let delivered = per_source.get(&sid.0).map_or(0, |v| v.len());
                            delivered >= count as usize
                        });
                        if all_free_delivered {
                            break;
                        }
                    }
                }
            }

            // Verify: every free source delivered ALL its events.
            for (i, (sid, _)) in free_senders.iter().enumerate() {
                let expected_count = free_data_counts[i];
                let delivered = per_source.get(&sid.0).cloned().unwrap_or_default();

                prop_assert_eq!(
                    delivered.len() as u32,
                    expected_count,
                    "Free source {} delivered {} events, expected {}. \
                     Backpressure on source 0 should not block other sources.",
                    sid.0,
                    delivered.len(),
                    expected_count,
                );

                // Also verify order is preserved.
                let expected_seqs: Vec<u32> = (0..expected_count).collect();
                prop_assert_eq!(
                    &delivered,
                    &expected_seqs,
                    "Free source {} events out of order: got {:?}, expected {:?}",
                    sid.0,
                    delivered,
                    expected_seqs,
                );
            }

            // Clean up: terminate all sources.
            let _ = blocked_sender.send(MockEvent { seq: 999, terminal: true });
            for (_, sender) in &free_senders {
                let _ = sender.send(MockEvent { seq: 999, terminal: true });
            }

            // Drain remaining output.
            loop {
                match mux.recv_timeout(Duration::from_millis(500)) {
                    Ok(_) => {}
                    Err(_) => break,
                }
            }
        }
    }

    // -----------------------------------------------------------------------
    // Property 11: Audio-priority pre-drain
    // -----------------------------------------------------------------------

    /// Strategy: generate audio event count (batch_size..=batch_size*3) and
    /// video event count (batch_size..=batch_size*2), plus a variable batch size (4..=12).
    fn arb_priority_drain_scenario() -> impl Strategy<Value = (usize, usize, usize)> {
        (4usize..=12).prop_flat_map(|batch_size| {
            let audio_count = batch_size..=(batch_size * 3);
            let video_count = batch_size..=(batch_size * 2);
            (Just(batch_size), audio_count, video_count)
        })
    }

    proptest! {
        #![proptest_config(ProptestConfig::with_cases(100))]

        /// **Validates: Requirements 12.1**
        ///
        /// Property 11: Audio-priority pre-drain.
        ///
        /// For any multiplexer loop iteration where the priority source (audio)
        /// channel has pending events, the multiplexer SHALL drain up to
        /// `priority_drain_batch` audio events via non-blocking receive before
        /// entering the blocking select wait.
        ///
        /// Approach: register an audio source (as priority) and a video source,
        /// pre-fill both channels with events before the multiplexer processes
        /// them, then verify that in the output the first `batch_size` events
        /// are all from the audio source.
        #[test]
        fn prop_audio_priority_pre_drain(
            (batch_size, audio_count, video_count) in arb_priority_drain_scenario(),
        ) {
            let audio_sid = SourceId(0);
            let video_sid = SourceId(1);

            // Create handles but do NOT build the multiplexer yet -we want
            // to fill channels before the main loop starts processing.
            let (audio_handle, _audio_cmds) = CommandTrackingHandle::new();
            let audio_sender = audio_handle.sender();

            let (video_handle, _video_cmds) = CommandTrackingHandle::new();
            let video_sender = video_handle.sender();

            // Pre-fill both channels with events BEFORE building the mux.
            // This ensures both channels have pending events when the main
            // loop first runs its priority pre-drain.
            for seq in 0..audio_count as u32 {
                audio_sender
                    .send(MockEvent { seq, terminal: false })
                    .unwrap();
            }
            for seq in 0..video_count as u32 {
                video_sender
                    .send(MockEvent { seq, terminal: false })
                    .unwrap();
            }

            let mut builder = StreamMultiplexerBuilder::new(MultiplexerConfig {
                select_timeout: Duration::from_millis(50),
                priority_source: Some(audio_sid),
                priority_drain_batch: batch_size,
                output_capacity: 256,
                output_send_timeout: Duration::from_millis(100),
            });

            builder.register(
                SourceConfig {
                    source_id: audio_sid,
                    channel_capacity: 64,
                    send_timeout: Duration::from_millis(100),
                },
                audio_handle,
                move |te: TaggedEvent<MockEvent>| {
                    smallvec::smallvec![TestOutput {
                        source: te.source,
                        seq: te.event.seq,
                    }]
                },
            );

            builder.register(
                SourceConfig {
                    source_id: video_sid,
                    channel_capacity: 64,
                    send_timeout: Duration::from_millis(100),
                },
                video_handle,
                move |te: TaggedEvent<MockEvent>| {
                    smallvec::smallvec![TestOutput {
                        source: te.source,
                        seq: te.event.seq,
                    }]
                },
            );

            let mux = builder.build();

            // Give forwarding threads time to read from the pre-filled
            // handle channels and forward events into the per-source
            // bounded channels. Without this, the main loop may start
            // its priority pre-drain before audio events are available,
            // causing video events to appear first due to a race.
            std::thread::sleep(Duration::from_millis(100));

            // Collect all output events.
            let total_expected = audio_count + video_count;
            let mut collected: Vec<TestOutput> = Vec::new();
            let deadline = std::time::Instant::now() + Duration::from_secs(5);

            while collected.len() < total_expected {
                let remaining = deadline.saturating_duration_since(std::time::Instant::now());
                if remaining.is_zero() {
                    break;
                }
                match mux.recv_timeout(remaining.min(Duration::from_millis(100))) {
                    Ok(item) => collected.push(item),
                    Err(_) => break,
                }
            }

            // Now terminate both sources and drain.
            let _ = audio_sender.send(MockEvent { seq: 999, terminal: true });
            let _ = video_sender.send(MockEvent { seq: 999, terminal: true });
            loop {
                match mux.recv_timeout(Duration::from_millis(500)) {
                    Ok(_) => {}
                    Err(_) => break,
                }
            }

            // Verify: the first `batch_size` events in the output should
            // all be from the audio (priority) source. The pre-drain loop
            // drains up to `priority_drain_batch` audio events before the
            // blocking select, so audio events must appear first.
            let first_batch: Vec<u8> = collected.iter()
                .take(batch_size)
                .map(|o| o.source.0)
                .collect();

            prop_assert!(
                collected.len() >= batch_size,
                "Expected at least {} output events, got {}",
                batch_size,
                collected.len(),
            );

            for (i, &src) in first_batch.iter().enumerate() {
                prop_assert_eq!(
                    src,
                    audio_sid.0,
                    "Event at position {} should be from audio source ({}), \
                     but was from source {}. First {} events: {:?}",
                    i,
                    audio_sid.0,
                    src,
                    batch_size,
                    first_batch,
                );
            }

            // Also verify that audio events in the first batch are in order.
            let audio_seqs_in_batch: Vec<u32> = collected.iter()
                .take(batch_size)
                .filter(|o| o.source == audio_sid)
                .map(|o| o.seq)
                .collect();

            for window in audio_seqs_in_batch.windows(2) {
                prop_assert!(
                    window[0] < window[1],
                    "Audio events in priority batch are out of order: {} before {}",
                    window[0],
                    window[1],
                );
            }
        }
    }

    // -----------------------------------------------------------------------
    // Unit tests for multiplexer builder and runtime (Task 1.10)
    // -----------------------------------------------------------------------

    /// A mock `StreamHandle` whose forwarding thread panics after receiving
    /// the first non-terminal event.
    struct PanickingHandle {
        rx: cbc::Receiver<MockEvent>,
        tx: cbc::Sender<MockEvent>,
    }

    impl PanickingHandle {
        fn new() -> Self {
            let (tx, rx) = cbc::bounded(64);
            Self { rx, tx }
        }

        fn sender(&self) -> cbc::Sender<MockEvent> {
            self.tx.clone()
        }
    }

    impl StreamHandle<MockEvent> for PanickingHandle {
        type RecvError = snow_core::error::RecvError;
        type TryRecvError = snow_core::error::TryRecvError;
        type RecvTimeoutError = snow_core::error::RecvTimeoutError;

        fn recv(&self) -> Result<MockEvent, Self::RecvError> {
            let event = self
                .rx
                .recv()
                .map_err(|_| snow_core::error::RecvError::Disconnected)?;
            if !event.terminal {
                panic!("PanickingHandle: intentional panic for testing");
            }
            Ok(event)
        }

        fn try_recv(&self) -> Result<MockEvent, Self::TryRecvError> {
            self.rx.try_recv().map_err(|e| match e {
                cbc::TryRecvError::Empty => snow_core::error::TryRecvError::Empty,
                cbc::TryRecvError::Disconnected => snow_core::error::TryRecvError::Disconnected,
            })
        }

        fn recv_timeout(&self, timeout: Duration) -> Result<MockEvent, Self::RecvTimeoutError> {
            let event = self.rx.recv_timeout(timeout).map_err(|e| match e {
                cbc::RecvTimeoutError::Timeout => snow_core::error::RecvTimeoutError::Timeout,
                cbc::RecvTimeoutError::Disconnected => {
                    snow_core::error::RecvTimeoutError::Disconnected
                }
            })?;
            if !event.terminal {
                panic!("PanickingHandle: intentional panic for testing");
            }
            Ok(event)
        }

        fn stop(&self) {}
        fn pause(&self) {}
        fn resume(&self) {}
        fn is_paused(&self) -> bool {
            false
        }
        fn is_running(&self) -> bool {
            true
        }
    }

    /// **Validates: Requirements 5.1, 5.6**
    ///
    /// Zero sources: building a multiplexer with no registered sources should
    /// signal Completed immediately since there are no sources to wait for.
    #[test]
    fn test_zero_sources_signals_completed() {
        let builder: StreamMultiplexerBuilder<TestOutput> =
            StreamMultiplexerBuilder::new(MultiplexerConfig::default());
        let mux = builder.build();

        // The output channel should disconnect quickly since there are no sources.
        let result = mux.recv_timeout(Duration::from_secs(2));
        assert!(
            result.is_err(),
            "Expected no output events from zero-source multiplexer"
        );

        // Should receive Completed status.
        let status = mux.recv_status();
        assert!(
            matches!(status, Ok(MuxStatus::Completed)),
            "Expected Completed status from zero-source multiplexer, got {:?}",
            status,
        );
    }

    /// **Validates: Requirements 5.1**
    ///
    /// Duplicate source IDs: registering two sources with the same SourceId.
    /// The multiplexer should still function -both forwarding threads run
    /// independently, and both sets of events appear in the output.
    #[test]
    fn test_duplicate_source_ids() {
        let sid = SourceId(42);

        let mut builder = StreamMultiplexerBuilder::new(MultiplexerConfig {
            select_timeout: Duration::from_millis(50),
            priority_source: None,
            priority_drain_batch: 8,
            output_capacity: 256,
            output_send_timeout: Duration::from_millis(100),
        });

        let handle_a = MockHandle::with_events(vec![
            MockEvent {
                seq: 1,
                terminal: false,
            },
            MockEvent {
                seq: 2,
                terminal: true,
            },
        ]);
        let handle_b = MockHandle::with_events(vec![
            MockEvent {
                seq: 10,
                terminal: false,
            },
            MockEvent {
                seq: 20,
                terminal: true,
            },
        ]);

        builder.register(
            SourceConfig {
                source_id: sid,
                channel_capacity: 64,
                send_timeout: Duration::from_millis(100),
            },
            handle_a,
            move |te: TaggedEvent<MockEvent>| {
                smallvec::smallvec![TestOutput {
                    source: te.source,
                    seq: te.event.seq,
                }]
            },
        );
        builder.register(
            SourceConfig {
                source_id: sid,
                channel_capacity: 64,
                send_timeout: Duration::from_millis(100),
            },
            handle_b,
            move |te: TaggedEvent<MockEvent>| {
                smallvec::smallvec![TestOutput {
                    source: te.source,
                    seq: te.event.seq,
                }]
            },
        );

        let mux = builder.build();

        // Collect all output events.
        let mut collected: Vec<TestOutput> = Vec::new();
        loop {
            match mux.recv_timeout(Duration::from_secs(2)) {
                Ok(item) => collected.push(item),
                Err(_) => break,
            }
        }

        // Both sources should have delivered their data events.
        let seqs: Vec<u32> = collected.iter().map(|o| o.seq).collect();
        assert!(seqs.contains(&1), "Missing seq 1 from source A");
        assert!(seqs.contains(&10), "Missing seq 10 from source B");

        // All events should be tagged with the same SourceId.
        for item in &collected {
            assert_eq!(item.source, sid, "All events should have source id 42");
        }

        // Should eventually get Completed.
        let mut statuses = Vec::new();
        loop {
            match mux.try_recv_status() {
                Ok(s) => statuses.push(s),
                Err(_) => break,
            }
        }
        assert!(
            statuses.iter().any(|s| matches!(s, MuxStatus::Completed)),
            "Expected Completed status, got {:?}",
            statuses,
        );
    }

    /// **Validates: Requirements 5.1**
    ///
    /// Select timeout: verify the multiplexer respects the configured
    /// select_timeout by checking that a source disconnect is detected
    /// within a reasonable time window.
    #[test]
    fn test_select_timeout_configuration() {
        let short_timeout = Duration::from_millis(10);

        let mut builder = StreamMultiplexerBuilder::new(MultiplexerConfig {
            select_timeout: short_timeout,
            priority_source: None,
            priority_drain_batch: 8,
            output_capacity: 16,
            output_send_timeout: Duration::from_millis(100),
        });

        // Create a channel-based handle where we control the sender.
        // By dropping the sender, the forwarding thread sees a disconnect.
        let (tx, rx) = cbc::bounded::<MockEvent>(1);
        // Drop tx immediately so the channel is disconnected from the start.
        drop(tx);

        // Build a MockHandle that uses the already-disconnected receiver.
        // We can't use MockHandle::with_events because it keeps _tx alive.
        // Instead, create a struct directly with a dummy tx that we won't use.
        let (dummy_tx, _dummy_rx) = cbc::bounded::<MockEvent>(1);
        let handle = MockHandle { rx, _tx: dummy_tx };
        let sid = SourceId(0);

        builder.register(
            SourceConfig {
                source_id: sid,
                channel_capacity: 64,
                send_timeout: Duration::from_millis(100),
            },
            handle,
            move |te: TaggedEvent<MockEvent>| {
                smallvec::smallvec![TestOutput {
                    source: te.source,
                    seq: te.event.seq,
                }]
            },
        );

        let start = std::time::Instant::now();
        let mux = builder.build();

        // Wait for the disconnect to be detected and Completed to be signaled.
        let mut statuses = Vec::new();
        loop {
            match mux.recv_status() {
                Ok(s) => {
                    statuses.push(s.clone());
                    if matches!(s, MuxStatus::Completed) {
                        break;
                    }
                }
                Err(_) => break,
            }
        }

        let elapsed = start.elapsed();

        // With a 10ms select_timeout, disconnect detection should happen
        // well within 1 second (forwarding thread polls every 5ms).
        assert!(
            elapsed < Duration::from_secs(1),
            "Disconnect detection took {:?}, expected < 1s with select_timeout={:?}",
            elapsed,
            short_timeout,
        );

        assert!(
            statuses.iter().any(|s| matches!(s, MuxStatus::Completed)),
            "Expected Completed status, got {:?}",
            statuses,
        );
    }

    /// **Validates: Requirements 5.6**
    ///
    /// MuxStatus::SourceEnded: when a source emits a terminal event
    /// (is_stream_ended() == true), the multiplexer reports SourceEnded.
    #[test]
    fn test_mux_status_source_ended() {
        let sid = SourceId(7);

        let mut builder = StreamMultiplexerBuilder::new(MultiplexerConfig {
            select_timeout: Duration::from_millis(25),
            priority_source: None,
            priority_drain_batch: 8,
            output_capacity: 64,
            output_send_timeout: Duration::from_millis(100),
        });

        let handle = MockHandle::with_events(vec![
            MockEvent {
                seq: 0,
                terminal: false,
            },
            MockEvent {
                seq: 1,
                terminal: true,
            },
        ]);

        builder.register(
            SourceConfig {
                source_id: sid,
                channel_capacity: 64,
                send_timeout: Duration::from_millis(100),
            },
            handle,
            move |te: TaggedEvent<MockEvent>| {
                smallvec::smallvec![TestOutput {
                    source: te.source,
                    seq: te.event.seq,
                }]
            },
        );

        let mux = builder.build();

        // Drain output.
        loop {
            match mux.recv_timeout(Duration::from_secs(2)) {
                Ok(_) => {}
                Err(_) => break,
            }
        }

        // Collect statuses.
        let mut statuses = Vec::new();
        loop {
            match mux.try_recv_status() {
                Ok(s) => statuses.push(s),
                Err(_) => break,
            }
        }

        assert!(
            statuses.contains(&MuxStatus::SourceEnded(sid)),
            "Expected SourceEnded({:?}), got {:?}",
            sid,
            statuses,
        );
        assert!(
            statuses.contains(&MuxStatus::Completed),
            "Expected Completed after source ended, got {:?}",
            statuses,
        );
    }

    /// **Validates: Requirements 5.6**
    ///
    /// MuxStatus::SourceDisconnected: when a source handle drops without
    /// emitting a terminal event, the multiplexer reports SourceDisconnected.
    #[test]
    fn test_mux_status_source_disconnected() {
        let sid = SourceId(3);

        let mut builder = StreamMultiplexerBuilder::new(MultiplexerConfig {
            select_timeout: Duration::from_millis(25),
            priority_source: None,
            priority_drain_batch: 8,
            output_capacity: 64,
            output_send_timeout: Duration::from_millis(100),
        });

        // Create a handle with an already-disconnected receiver so the
        // forwarding thread sees a disconnect without a terminal event.
        let (tx, rx) = cbc::bounded::<MockEvent>(1);
        drop(tx); // Disconnect immediately.
        let (dummy_tx, _dummy_rx) = cbc::bounded::<MockEvent>(1);
        let handle = MockHandle { rx, _tx: dummy_tx };

        builder.register(
            SourceConfig {
                source_id: sid,
                channel_capacity: 64,
                send_timeout: Duration::from_millis(100),
            },
            handle,
            move |te: TaggedEvent<MockEvent>| {
                smallvec::smallvec![TestOutput {
                    source: te.source,
                    seq: te.event.seq,
                }]
            },
        );

        let mux = builder.build();

        // Drain output (should be empty).
        loop {
            match mux.recv_timeout(Duration::from_secs(2)) {
                Ok(_) => {}
                Err(_) => break,
            }
        }

        // Collect statuses.
        let mut statuses = Vec::new();
        loop {
            match mux.try_recv_status() {
                Ok(s) => statuses.push(s),
                Err(_) => break,
            }
        }

        assert!(
            statuses.contains(&MuxStatus::SourceDisconnected(sid)),
            "Expected SourceDisconnected({:?}), got {:?}",
            sid,
            statuses,
        );
        assert!(
            statuses.contains(&MuxStatus::Completed),
            "Expected Completed after source disconnected, got {:?}",
            statuses,
        );
    }

    /// **Validates: Requirements 5.6**
    ///
    /// MuxStatus::SourceForwarderPanicked: when a source's forwarding thread
    /// panics, the multiplexer reports SourceForwarderPanicked.
    #[test]
    fn test_mux_status_source_forwarder_panicked() {
        let sid = SourceId(5);

        let mut builder = StreamMultiplexerBuilder::new(MultiplexerConfig {
            select_timeout: Duration::from_millis(25),
            priority_source: None,
            priority_drain_batch: 8,
            output_capacity: 64,
            output_send_timeout: Duration::from_millis(100),
        });

        let handle = PanickingHandle::new();
        let sender = handle.sender();

        builder.register(
            SourceConfig {
                source_id: sid,
                channel_capacity: 64,
                send_timeout: Duration::from_millis(100),
            },
            handle,
            move |te: TaggedEvent<MockEvent>| {
                smallvec::smallvec![TestOutput {
                    source: te.source,
                    seq: te.event.seq,
                }]
            },
        );

        let mux = builder.build();

        // Send a non-terminal event to trigger the panic in the forwarding thread.
        sender
            .send(MockEvent {
                seq: 0,
                terminal: false,
            })
            .unwrap();

        // Drain output.
        loop {
            match mux.recv_timeout(Duration::from_secs(2)) {
                Ok(_) => {}
                Err(_) => break,
            }
        }

        // Collect statuses.
        let mut statuses = Vec::new();
        loop {
            match mux.try_recv_status() {
                Ok(s) => statuses.push(s),
                Err(_) => break,
            }
        }

        assert!(
            statuses.contains(&MuxStatus::SourceForwarderPanicked(sid)),
            "Expected SourceForwarderPanicked({:?}), got {:?}",
            sid,
            statuses,
        );
        assert!(
            statuses.contains(&MuxStatus::Completed),
            "Expected Completed after forwarder panicked, got {:?}",
            statuses,
        );
    }
}
