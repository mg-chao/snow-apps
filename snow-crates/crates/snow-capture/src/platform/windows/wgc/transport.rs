use std::collections::VecDeque;
use std::sync::{Arc, Condvar, Mutex, MutexGuard};
use std::time::Instant;

use crossbeam_channel::Sender;
use windows::Graphics::Capture::{Direct3D11CaptureFrame, Direct3D11CaptureFramePool};
use windows::Win32::Foundation::E_POINTER;
use windows::core::HRESULT;

use crate::error::{CaptureError, CaptureResult};

fn is_empty_pool_result(error: &windows::core::Error) -> bool {
    error.code() == HRESULT(0) || error.code() == E_POINTER
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub(super) enum DrainPolicy {
    CompleteLatest,
    Ordered,
}

struct BoundedFrameQueue<T> {
    entries: VecDeque<T>,
    capacity: usize,
    overflowed: bool,
    paused: bool,
    closed: bool,
    failure: Option<CaptureError>,
    pool_busy: bool,
    drain_requested: bool,
}

impl<T> BoundedFrameQueue<T> {
    fn new(capacity: usize) -> Self {
        Self {
            entries: VecDeque::with_capacity(capacity),
            capacity: capacity.max(1),
            overflowed: false,
            paused: false,
            closed: false,
            failure: None,
            pool_busy: false,
            drain_requested: false,
        }
    }

    // Return ownership of rejected/evicted frames; native destruction must never
    // happen while the callback's queue mutex is held.
    #[must_use = "release rejected or evicted frames after unlocking the queue"]
    fn push(&mut self, value: T) -> (bool, Option<T>) {
        if self.paused || self.closed {
            return (false, Some(value));
        }
        let retired = if self.entries.len() == self.capacity {
            self.overflowed = true;
            self.entries.pop_front()
        } else {
            None
        };
        self.entries.push_back(value);
        (true, retired)
    }

    fn drain(&mut self) -> QueueDrain<T> {
        let overflowed = std::mem::take(&mut self.overflowed);
        QueueDrain {
            entries: self.entries.drain(..).collect(),
            overflowed,
            closed: self.closed,
        }
    }

    #[must_use = "release cleared frames after unlocking the queue"]
    fn clear(&mut self) -> Vec<T> {
        self.overflowed = false;
        self.entries.drain(..).collect()
    }

    fn pause(&mut self) {
        self.paused = true;
    }

    fn resume(&mut self) {
        if !self.closed {
            self.paused = false;
        }
    }

    #[must_use = "release failed frames after unlocking the queue"]
    fn record_failure(&mut self, error: CaptureError) -> Vec<T> {
        self.failure = Some(error);
        self.clear()
    }
}

#[must_use = "process or release drained frames after unlocking the queue"]
struct QueueDrain<T> {
    entries: Vec<T>,
    overflowed: bool,
    closed: bool,
}

pub(super) struct FramePacket {
    pub frame: Direct3D11CaptureFrame,
    pub system_relative_time_hns: i64,
    pub received_at: Instant,
}

impl Drop for FramePacket {
    fn drop(&mut self) {
        let _ = self.frame.Close();
    }
}

pub(super) struct FrameBatch<T = FramePacket> {
    pub frames: Vec<T>,
    pub overflowed: bool,
    pub discarded: usize,
    pub closed: bool,
}

struct SharedTransport<T> {
    queue: Mutex<BoundedFrameQueue<T>>,
    pool_idle: Condvar,
    notification: Sender<()>,
}

pub(super) struct FrameTransport<T = FramePacket> {
    shared: Arc<SharedTransport<T>>,
}

impl<T> Clone for FrameTransport<T> {
    fn clone(&self) -> Self {
        Self {
            shared: Arc::clone(&self.shared),
        }
    }
}

impl<T> FrameTransport<T> {
    pub fn new(capacity: usize, notification: Sender<()>) -> Self {
        Self {
            shared: Arc::new(SharedTransport {
                queue: Mutex::new(BoundedFrameQueue::new(capacity)),
                pool_idle: Condvar::new(),
                notification,
            }),
        }
    }

    pub fn mark_closed(&self) {
        if let Ok(mut queue) = self.shared.queue.lock() {
            queue.closed = true;
        }
        let _ = self.shared.notification.try_send(());
    }

    pub fn drain(&self, policy: DrainPolicy) -> CaptureResult<FrameBatch<T>> {
        let (mut drained, error) = {
            let mut queue = self.lock_queue()?;
            let error = queue.failure.take();
            (queue.drain(), error)
        };
        if let Some(error) = error {
            return Err(error);
        }
        let discarded = if policy == DrainPolicy::CompleteLatest {
            let latest = drained.entries.pop();
            let discarded = drained.entries.len();
            drained.entries.clear();
            drained.entries.extend(latest);
            discarded
        } else {
            0
        };
        Ok(FrameBatch {
            frames: drained.entries,
            overflowed: drained.overflowed,
            discarded,
            closed: drained.closed,
        })
    }

    pub fn pause_and_clear(&self) -> CaptureResult<()> {
        let frames = {
            let mut queue = self.lock_queue()?;
            queue.pause();
            self.wait_for_pool(queue)?.clear()
        };
        drop(frames);
        Ok(())
    }

    pub fn pause(&self) -> CaptureResult<()> {
        let mut queue = self.lock_queue()?;
        queue.pause();
        drop(self.wait_for_pool(queue)?);
        Ok(())
    }

    pub fn shutdown(&self) {
        let frames = {
            let mut queue = self.shared.queue.lock().unwrap_or_else(|e| e.into_inner());
            queue.closed = true;
            queue.paused = true;
            // Seal admission before waiting. Callbacks must be able to return
            // while native WGC holds its locks, so this wait releases the mutex.
            while queue.pool_busy {
                queue = self
                    .shared
                    .pool_idle
                    .wait(queue)
                    .unwrap_or_else(|e| e.into_inner());
            }
            queue.failure = None;
            queue.clear()
        };
        // Closing a native frame may invoke WGC work. Do it without holding
        // the mutex that a late FrameArrived/Closed callback needs.
        drop(frames);
    }

    fn lock_queue(&self) -> CaptureResult<MutexGuard<'_, BoundedFrameQueue<T>>> {
        self.shared.queue.lock().map_err(|_| {
            CaptureError::platform(anyhow::anyhow!("WGC frame transport mutex was poisoned"))
        })
    }

    fn wait_for_pool<'a>(
        &self,
        mut queue: MutexGuard<'a, BoundedFrameQueue<T>>,
    ) -> CaptureResult<MutexGuard<'a, BoundedFrameQueue<T>>> {
        while queue.pool_busy {
            queue = self.shared.pool_idle.wait(queue).map_err(|_| {
                CaptureError::platform(anyhow::anyhow!("WGC frame transport mutex was poisoned"))
            })?;
        }
        Ok(queue)
    }

    fn try_acquire_pool(&self) -> Option<FramePoolAccess<'_, T>> {
        let mut queue = self.shared.queue.lock().ok()?;
        if queue.paused || queue.closed {
            return None;
        }
        if queue.pool_busy {
            // Never wait for another pool user from FrameArrived: WGC invokes
            // it while holding a native lock that TryGetNextFrame/Close need.
            queue.drain_requested = true;
            return None;
        }
        queue.pool_busy = true;
        Some(FramePoolAccess {
            transport: self,
            active: true,
        })
    }

    fn acquire_pool(&self) -> CaptureResult<Option<FramePoolAccess<'_, T>>> {
        let mut queue = self.wait_for_pool(self.lock_queue()?)?;
        if queue.closed {
            return Ok(None);
        }
        queue.pool_busy = true;
        Ok(Some(FramePoolAccess {
            transport: self,
            active: true,
        }))
    }
}

// The lease serializes acquisition and ordered admission, without holding any
// mutex across native WGC calls. Pause and shutdown wait for its release; native
// callbacks only request another drain when a lease is already active.
struct FramePoolAccess<'a, T> {
    transport: &'a FrameTransport<T>,
    active: bool,
}

impl<T> FramePoolAccess<'_, T> {
    fn accepting(&self) -> CaptureResult<bool> {
        let queue = self.transport.lock_queue()?;
        Ok(!queue.paused && !queue.closed)
    }

    fn admit(&self, frame: T) -> CaptureResult<bool> {
        let (queued, retired) = self.transport.lock_queue()?.push(frame);
        drop(retired);
        Ok(queued)
    }

    fn resume(&self) -> CaptureResult<()> {
        self.transport.lock_queue()?.resume();
        Ok(())
    }

    fn record_failure(&self, error: CaptureError) {
        let frames = self
            .transport
            .shared
            .queue
            .lock()
            .unwrap_or_else(|e| e.into_inner())
            .record_failure(error);
        drop(frames);
    }

    fn finish_or_retry(&mut self) -> CaptureResult<bool> {
        let mut queue = self.transport.lock_queue()?;
        if std::mem::take(&mut queue.drain_requested) && !queue.paused && !queue.closed {
            return Ok(true);
        }
        queue.pool_busy = false;
        self.active = false;
        self.transport.shared.pool_idle.notify_all();
        Ok(false)
    }

    fn drain(&mut self, mut next: impl FnMut() -> CaptureResult<Option<T>>) -> CaptureResult<bool> {
        let mut queued = false;
        loop {
            while self.accepting()? {
                let Some(frame) = next()? else { break };
                queued |= self.admit(frame)?;
            }
            // Check missed callbacks and release admission atomically, so a
            // final arrival cannot be stranded between an empty pool and unlock.
            if !self.finish_or_retry()? {
                return Ok(queued);
            }
        }
    }

    fn discard(&self, mut next: impl FnMut() -> CaptureResult<Option<T>>) -> CaptureResult<()> {
        while !self.transport.lock_queue()?.closed {
            let Some(frame) = next()? else { break };
            drop(frame);
        }
        Ok(())
    }
}

impl<T> Drop for FramePoolAccess<'_, T> {
    fn drop(&mut self) {
        if self.active {
            let mut queue = self
                .transport
                .shared
                .queue
                .lock()
                .unwrap_or_else(|e| e.into_inner());
            queue.pool_busy = false;
            queue.drain_requested = false;
            self.transport.shared.pool_idle.notify_all();
        }
    }
}

impl FrameTransport {
    pub fn drain_frame_pool(&self, frame_pool: &Direct3D11CaptureFramePool) {
        let Some(mut access) = self.try_acquire_pool() else {
            return;
        };
        let notify = match access.drain(|| next_frame_packet(frame_pool)) {
            Ok(queued) => queued,
            Err(error) => {
                access.record_failure(error);
                true
            }
        };
        drop(access);
        if notify {
            let _ = self.shared.notification.try_send(());
        }
    }

    pub fn discard_and_resume(&self, frame_pool: &Direct3D11CaptureFramePool) -> CaptureResult<()> {
        let Some(mut access) = self.acquire_pool()? else {
            return Ok(());
        };
        let discarded = access.discard(|| acquire_frame_packet(frame_pool));
        access.resume()?;
        discarded?;
        self.drain_and_notify(&mut access, frame_pool)
    }

    pub fn resume_and_drain(&self, frame_pool: &Direct3D11CaptureFramePool) -> CaptureResult<()> {
        let Some(mut access) = self.acquire_pool()? else {
            return Ok(());
        };
        access.resume()?;
        self.drain_and_notify(&mut access, frame_pool)
    }

    fn drain_and_notify(
        &self,
        access: &mut FramePoolAccess<'_, FramePacket>,
        frame_pool: &Direct3D11CaptureFramePool,
    ) -> CaptureResult<()> {
        if access.drain(|| next_frame_packet(frame_pool))? {
            let _ = self.shared.notification.try_send(());
        }
        Ok(())
    }
}

fn next_frame_packet(
    frame_pool: &Direct3D11CaptureFramePool,
) -> CaptureResult<Option<FramePacket>> {
    let Some(mut packet) = acquire_frame_packet(frame_pool)? else {
        return Ok(None);
    };
    packet.system_relative_time_hns = packet
        .frame
        .SystemRelativeTime()
        .map_err(|error| {
            super::map_platform_error(error, "Direct3D11CaptureFrame::SystemRelativeTime failed")
        })?
        .Duration;
    Ok(Some(packet))
}

fn acquire_frame_packet(
    frame_pool: &Direct3D11CaptureFramePool,
) -> CaptureResult<Option<FramePacket>> {
    let frame = match frame_pool.TryGetNextFrame() {
        Ok(frame) => frame,
        Err(error) if is_empty_pool_result(&error) => return Ok(None),
        Err(error) => {
            return Err(super::map_platform_error(
                error,
                "Direct3D11CaptureFramePool::TryGetNextFrame failed",
            ));
        }
    };
    // Own the native frame before reading metadata, so every error also closes
    // it outside the queue mutex and before the acquisition lease is released.
    Ok(Some(FramePacket {
        frame,
        system_relative_time_hns: 0,
        received_at: Instant::now(),
    }))
}

#[cfg(test)]
mod tests {
    use super::*;
    use std::sync::Weak;
    use std::sync::atomic::{AtomicBool, AtomicUsize, Ordering};
    use std::time::Duration;

    struct TrackedFrame {
        transport: Weak<SharedTransport<TrackedFrame>>,
        released: Arc<AtomicUsize>,
        released_without_lock: Arc<AtomicBool>,
    }

    impl TrackedFrame {
        fn new(
            transport: &FrameTransport<Self>,
            released: &Arc<AtomicUsize>,
            released_without_lock: &Arc<AtomicBool>,
        ) -> Self {
            Self {
                transport: Arc::downgrade(&transport.shared),
                released: Arc::clone(released),
                released_without_lock: Arc::clone(released_without_lock),
            }
        }
    }

    impl Drop for TrackedFrame {
        fn drop(&mut self) {
            let transport = self.transport.upgrade().expect("frame transport is alive");
            self.released_without_lock
                .fetch_and(transport.queue.try_lock().is_ok(), Ordering::SeqCst);
            self.released.fetch_add(1, Ordering::SeqCst);
        }
    }

    struct TrackedTransport {
        transport: FrameTransport<TrackedFrame>,
        released: Arc<AtomicUsize>,
        released_without_lock: Arc<AtomicBool>,
    }

    impl TrackedTransport {
        fn new(capacity: usize, count: usize) -> Self {
            let (notifications, _) = crossbeam_channel::bounded(1);
            let fixture = Self {
                transport: FrameTransport::new(capacity, notifications),
                released: Arc::new(AtomicUsize::new(0)),
                released_without_lock: Arc::new(AtomicBool::new(true)),
            };
            let access = fixture.transport.acquire_pool().unwrap().unwrap();
            for _ in 0..count {
                assert!(access.admit(fixture.frame()).unwrap());
            }
            drop(access);
            fixture
        }

        fn frame(&self) -> TrackedFrame {
            TrackedFrame::new(&self.transport, &self.released, &self.released_without_lock)
        }

        fn assert_released(&self, count: usize) {
            assert_eq!(self.released.load(Ordering::SeqCst), count);
            assert!(self.released_without_lock.load(Ordering::SeqCst));
        }
    }

    impl Drop for TrackedTransport {
        fn drop(&mut self) {
            self.transport.shutdown();
        }
    }

    #[test]
    fn frame_close_can_wait_for_a_callback_that_enters_the_queue() {
        struct NativeFrame {
            native_lock: Arc<Mutex<()>>,
            closing: Option<Sender<()>>,
        }
        impl Drop for NativeFrame {
            fn drop(&mut self) {
                if let Some(closing) = self.closing.take() {
                    let _ = closing.send(());
                }
                drop(self.native_lock.lock().unwrap_or_else(|e| e.into_inner()));
            }
        }

        let (notifications, _) = crossbeam_channel::bounded(1);
        let transport = FrameTransport::new(4, notifications);
        let native_lock = Arc::new(Mutex::new(()));
        let (ready, entered) = crossbeam_channel::bounded(1);
        let (closing, release) = crossbeam_channel::bounded(1);
        let callback_transport = transport.clone();
        let callback_lock = Arc::clone(&native_lock);
        let callback = std::thread::spawn(move || {
            let _native = callback_lock.lock().unwrap();
            ready.send(()).unwrap();
            release.recv_timeout(Duration::from_secs(5)).unwrap();
            // try_lock makes a regression fail without leaving the test in the
            // exact permanent native-lock/queue-lock cycle seen in the dump.
            assert!(callback_transport.shared.queue.try_lock().is_ok());
        });
        entered.recv_timeout(Duration::from_secs(5)).unwrap();
        let access = transport.acquire_pool().unwrap().unwrap();
        access
            .admit(NativeFrame {
                native_lock: Arc::clone(&native_lock),
                closing: Some(closing),
            })
            .unwrap();
        access
            .admit(NativeFrame {
                native_lock,
                closing: None,
            })
            .unwrap();
        drop(access);

        let batch = transport.drain(DrainPolicy::CompleteLatest).unwrap();

        callback.join().unwrap();
        assert_eq!(batch.frames.len(), 1);
        assert_eq!(batch.discarded, 1);
        drop(batch);
    }

    #[test]
    fn overflow_releases_evicted_frames_outside_callback_lock() {
        let fixture = TrackedTransport::new(2, 2);
        let access = fixture.transport.acquire_pool().unwrap().unwrap();
        assert!(access.admit(fixture.frame()).unwrap());
        fixture.assert_released(1);
        drop(access);
        let batch = fixture.transport.drain(DrainPolicy::Ordered).unwrap();
        assert!(batch.overflowed);
        assert_eq!(batch.frames.len(), 2);
        drop(batch);
        fixture.assert_released(3);
    }

    #[test]
    fn pause_and_failure_release_frames_outside_callback_lock() {
        let fixture = TrackedTransport::new(4, 3);
        fixture.transport.pause_and_clear().unwrap();
        fixture.assert_released(3);
        assert!(fixture.transport.try_acquire_pool().is_none());
        let access = fixture.transport.acquire_pool().unwrap().unwrap();
        assert!(!access.admit(fixture.frame()).unwrap());
        fixture.assert_released(4);
        access.resume().unwrap();
        assert!(access.admit(fixture.frame()).unwrap());
        access.record_failure(CaptureError::Timeout);
        fixture.assert_released(5);
        // A late admission after failure must also be retired outside the lock
        // when the consumer observes that error.
        assert!(access.admit(fixture.frame()).unwrap());
        drop(access);
        assert!(matches!(
            fixture.transport.drain(DrainPolicy::Ordered),
            Err(CaptureError::Timeout)
        ));
        fixture.assert_released(6);
    }

    #[test]
    fn pause_seals_admission_and_waits_for_native_acquisition_to_finish() {
        for clear in [false, true] {
            let fixture = TrackedTransport::new(4, 3);
            let access = fixture.transport.try_acquire_pool().unwrap();
            let pause_transport = fixture.transport.clone();
            let (finished, completion) = crossbeam_channel::bounded(1);
            let pause = std::thread::spawn(move || {
                if clear {
                    pause_transport.pause_and_clear().unwrap();
                } else {
                    pause_transport.pause().unwrap();
                }
                finished.send(()).unwrap();
            });
            // Observe the actual admission transition, rather than relying on
            // a delay or on when the pause thread was scheduled.
            let deadline = Instant::now() + Duration::from_secs(5);
            while access.accepting().unwrap() {
                assert!(Instant::now() < deadline, "pause must seal frame admission");
                std::thread::yield_now();
            }
            assert!(completion.try_recv().is_err());
            assert!(fixture.transport.try_acquire_pool().is_none());
            assert!(!access.admit(fixture.frame()).unwrap());
            fixture.assert_released(1);
            drop(access);
            completion.recv_timeout(Duration::from_secs(5)).unwrap();
            pause.join().unwrap();
            fixture.assert_released(if clear { 4 } else { 1 });
            let batch = fixture.transport.drain(DrainPolicy::Ordered).unwrap();
            assert_eq!(batch.frames.len(), if clear { 0 } else { 3 });
            drop(batch);
            fixture.assert_released(4);
        }
    }

    #[test]
    fn source_closed_during_acquisition_rejects_frames_without_reopening() {
        let fixture = TrackedTransport::new(4, 0);
        let access = fixture.transport.try_acquire_pool().unwrap();
        fixture.transport.mark_closed();
        access.resume().unwrap();
        assert!(!access.admit(fixture.frame()).unwrap());
        fixture.assert_released(1);
        drop(access);
        assert!(fixture.transport.try_acquire_pool().is_none());
        assert!(fixture.transport.acquire_pool().unwrap().is_none());
        let batch = fixture.transport.drain(DrainPolicy::Ordered).unwrap();
        assert!(batch.closed);
        assert!(batch.frames.is_empty());
    }

    #[test]
    fn pool_operations_release_frames_and_call_native_code_without_queue_lock() {
        let fixture = TrackedTransport::new(4, 0);
        fixture.transport.pause().unwrap();
        let mut access = fixture.transport.acquire_pool().unwrap().unwrap();
        let mut remaining = 3;
        access
            .discard(|| {
                assert!(fixture.transport.shared.queue.try_lock().is_ok());
                if remaining == 0 {
                    return Ok(None);
                }
                remaining -= 1;
                Ok(Some(fixture.frame()))
            })
            .unwrap();
        fixture.assert_released(3);
        access.resume().unwrap();
        let mut frames = Some(fixture.frame());
        assert!(
            access
                .drain(|| {
                    assert!(fixture.transport.shared.queue.try_lock().is_ok());
                    Ok(frames.take())
                })
                .unwrap()
        );
        assert!(!fixture.transport.shared.queue.lock().unwrap().pool_busy);
        drop(access);
        drop(fixture.transport.drain(DrainPolicy::Ordered).unwrap());
        fixture.assert_released(4);
    }

    #[test]
    fn busy_pool_callbacks_return_and_the_owner_drains_missed_arrivals_in_order() {
        let (notifications, _) = crossbeam_channel::bounded(1);
        let transport = FrameTransport::new(4, notifications);
        let mut access = transport.try_acquire_pool().unwrap();
        let mut calls = 0;
        assert!(
            access
                .drain(|| {
                    assert!(transport.shared.queue.try_lock().is_ok());
                    calls += 1;
                    match calls {
                        1 => Ok(Some(1)),
                        2 => {
                            // Simulate a FrameArrived between an empty pool
                            // result and the owner releasing its acquisition.
                            assert!(transport.try_acquire_pool().is_none());
                            Ok(None)
                        }
                        3 => Ok(Some(2)),
                        4 => Ok(None),
                        _ => panic!("missed arrival should require only one additional drain"),
                    }
                })
                .unwrap()
        );
        assert_eq!(calls, 4);
        assert_eq!(
            transport.drain(DrainPolicy::Ordered).unwrap().frames,
            vec![1, 2]
        );
        // A completed lease must not release a subsequent owner's admission.
        let next = transport.try_acquire_pool().unwrap();
        drop(access);
        assert!(transport.try_acquire_pool().is_none());
        drop(next);
        assert!(transport.try_acquire_pool().is_some());
    }

    #[test]
    fn acquisition_errors_and_unwinding_release_the_lifecycle_barrier() {
        let (notifications, _) = crossbeam_channel::bounded(1);
        let transport = FrameTransport::<i32>::new(4, notifications);
        let mut access = transport.try_acquire_pool().unwrap();
        assert!(matches!(
            access.drain(|| Err(CaptureError::Timeout)),
            Err(CaptureError::Timeout)
        ));
        drop(access);
        let unwind = std::panic::catch_unwind(std::panic::AssertUnwindSafe(|| {
            let mut access = transport.try_acquire_pool().unwrap();
            let _ = access.drain(|| panic!("native acquisition panicked"));
        }));
        assert!(unwind.is_err());
        assert!(transport.try_acquire_pool().is_some());
        transport.shutdown();
        assert!(transport.acquire_pool().unwrap().is_none());
    }

    #[test]
    fn complete_drain_releases_discarded_frames_outside_callback_lock() {
        let fixture = TrackedTransport::new(4, 3);
        let batch = fixture
            .transport
            .drain(DrainPolicy::CompleteLatest)
            .unwrap();

        assert_eq!(batch.frames.len(), 1);
        assert_eq!(batch.discarded, 2);
        fixture.assert_released(2);
        drop(batch);
        fixture.assert_released(3);
    }

    #[test]
    fn shutdown_releases_every_queued_frame_outside_callback_lock() {
        let fixture = TrackedTransport::new(4, 3);
        fixture.transport.shutdown();

        fixture.assert_released(3);
        let batch = fixture.transport.drain(DrainPolicy::Ordered).unwrap();
        assert!(batch.frames.is_empty());
        assert!(batch.closed);
        fixture.transport.shutdown();
        fixture.assert_released(3);
    }

    #[test]
    fn shutdown_waits_for_in_flight_frame_admission() {
        let (notifications, _) = crossbeam_channel::bounded(1);
        let transport = FrameTransport::new(4, notifications);
        let released = Arc::new(AtomicUsize::new(0));
        let released_without_lock = Arc::new(AtomicBool::new(true));
        let frame = TrackedFrame::new(&transport, &released, &released_without_lock);
        let callback_transport = transport.clone();
        let (entered, entry) = crossbeam_channel::bounded(1);
        let (proceed, admission) = crossbeam_channel::bounded(1);
        let callback = std::thread::spawn(move || {
            let access = callback_transport.try_acquire_pool().unwrap();
            entered.send(()).unwrap();
            admission.recv_timeout(Duration::from_secs(5)).unwrap();
            access.admit(frame).unwrap();
        });
        entry.recv_timeout(Duration::from_secs(5)).unwrap();
        let observer = transport.clone();
        let (finished, completion) = crossbeam_channel::bounded(1);
        let shutdown = std::thread::spawn(move || {
            transport.shutdown();
            finished.send(()).unwrap();
        });
        let deadline = Instant::now() + Duration::from_secs(5);
        while !observer.lock_queue().unwrap().closed {
            assert!(Instant::now() < deadline, "shutdown must seal admission");
            std::thread::yield_now();
        }
        assert!(observer.try_acquire_pool().is_none());
        assert!(completion.try_recv().is_err());
        proceed.send(()).unwrap();
        completion.recv_timeout(Duration::from_secs(5)).unwrap();
        callback.join().unwrap();
        shutdown.join().unwrap();

        assert_eq!(released.load(Ordering::SeqCst), 1);
        assert!(released_without_lock.load(Ordering::SeqCst));
    }

    #[test]
    fn complete_drain_coalesces_to_latest_entry() {
        let (notifications, _) = crossbeam_channel::bounded(1);
        let transport = FrameTransport::new(4, notifications);
        let access = transport.acquire_pool().unwrap().unwrap();
        access.admit(1).unwrap();
        access.admit(2).unwrap();
        access.admit(3).unwrap();
        drop(access);

        let drained = transport.drain(DrainPolicy::CompleteLatest).unwrap();
        assert_eq!(drained.frames, vec![3]);
        assert_eq!(drained.discarded, 2);
        assert!(!drained.overflowed);
    }

    #[test]
    fn ordered_drain_preserves_every_entry() {
        let mut queue = BoundedFrameQueue::new(4);
        assert_eq!(queue.push(1), (true, None));
        assert_eq!(queue.push(2), (true, None));
        assert_eq!(queue.push(3), (true, None));

        let drained = queue.drain();
        assert_eq!(drained.entries, vec![1, 2, 3]);
        assert!(!drained.overflowed);
    }

    #[test]
    fn overflow_is_reported_for_ordered_resynchronization() {
        let mut queue = BoundedFrameQueue::new(2);
        assert_eq!(queue.push(1), (true, None));
        assert_eq!(queue.push(2), (true, None));
        assert_eq!(queue.push(3), (true, Some(1)));

        let drained = queue.drain();
        assert_eq!(drained.entries, vec![2, 3]);
        assert!(drained.overflowed);
    }

    #[test]
    fn paused_queue_rejects_new_frames() {
        let mut queue = BoundedFrameQueue::new(4);
        queue.pause();
        drop(queue.clear());
        assert!(!queue.push(1).0);
        let drained = queue.drain();
        assert!(drained.entries.is_empty());
        queue.resume();
        assert!(queue.push(2).0);
    }

    #[test]
    fn closed_queue_cannot_resume_frame_admission() {
        let mut queue = BoundedFrameQueue::new(4);
        queue.closed = true;
        queue.resume();

        assert!(
            !queue.push(1).0,
            "late callbacks must not retain frames after shutdown"
        );
        assert!(queue.entries.is_empty());
    }

    #[test]
    fn pause_preserves_frames_for_an_ordered_contract_transition() {
        let mut queue = BoundedFrameQueue::new(4);
        assert_eq!(queue.push(1), (true, None));
        assert_eq!(queue.push(2), (true, None));
        queue.pause();
        assert!(!queue.push(3).0);
        queue.resume();

        let drained = queue.drain();
        assert_eq!(drained.entries, vec![1, 2]);
        assert!(!drained.overflowed);
    }

    #[test]
    fn recorded_failure_discards_queued_frames() {
        let mut queue = BoundedFrameQueue::new(4);
        assert_eq!(queue.push(1), (true, None));
        drop(queue.record_failure(CaptureError::Timeout));

        assert!(queue.entries.is_empty());
        assert!(matches!(queue.failure, Some(CaptureError::Timeout)));
    }

    #[test]
    fn empty_pool_accepts_null_interface_sentinels() {
        assert!(is_empty_pool_result(&windows::core::Error::from_hresult(
            HRESULT(0)
        )));
        assert!(is_empty_pool_result(&windows::core::Error::from_hresult(
            E_POINTER
        )));
        assert!(!is_empty_pool_result(&windows::core::Error::from_hresult(
            windows::Win32::Graphics::Dxgi::DXGI_ERROR_DEVICE_REMOVED
        )));
    }
}
