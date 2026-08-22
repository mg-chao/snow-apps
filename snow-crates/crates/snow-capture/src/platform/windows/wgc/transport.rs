use std::collections::VecDeque;
use std::sync::{Arc, Mutex};
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
        }
    }

    fn push(&mut self, value: T) -> bool {
        if self.paused {
            return false;
        }
        if self.entries.len() == self.capacity {
            self.entries.pop_front();
            self.overflowed = true;
        }
        self.entries.push_back(value);
        true
    }

    fn drain(&mut self, policy: DrainPolicy) -> QueueDrain<T> {
        let overflowed = std::mem::take(&mut self.overflowed);
        let discarded = match policy {
            DrainPolicy::CompleteLatest => self.entries.len().saturating_sub(1),
            DrainPolicy::Ordered => 0,
        };
        let entries = match policy {
            DrainPolicy::CompleteLatest => self.entries.pop_back().into_iter().collect(),
            DrainPolicy::Ordered => self.entries.drain(..).collect(),
        };
        self.entries.clear();
        QueueDrain {
            entries,
            overflowed,
            discarded,
            closed: self.closed,
        }
    }

    fn clear(&mut self) {
        self.entries.clear();
        self.overflowed = false;
    }

    fn pause_and_clear(&mut self) {
        self.paused = true;
        self.clear();
    }

    fn pause(&mut self) {
        self.paused = true;
    }

    fn resume(&mut self) {
        self.paused = false;
    }

    fn record_failure(&mut self, error: CaptureError) {
        self.clear();
        self.failure = Some(error);
    }
}

struct QueueDrain<T> {
    entries: Vec<T>,
    overflowed: bool,
    discarded: usize,
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

pub(super) struct FrameBatch {
    pub frames: Vec<FramePacket>,
    pub overflowed: bool,
    pub discarded: usize,
    pub closed: bool,
}

struct SharedTransport {
    queue: Mutex<BoundedFrameQueue<FramePacket>>,
    notification: Sender<()>,
}

#[derive(Clone)]
pub(super) struct FrameTransport {
    shared: Arc<SharedTransport>,
}

impl FrameTransport {
    pub fn new(capacity: usize, notification: Sender<()>) -> Self {
        Self {
            shared: Arc::new(SharedTransport {
                queue: Mutex::new(BoundedFrameQueue::new(capacity)),
                notification,
            }),
        }
    }

    pub fn drain_frame_pool(&self, frame_pool: &Direct3D11CaptureFramePool) {
        let mut notify = false;
        if let Ok(mut queue) = self.shared.queue.lock() {
            match drain_frame_pool_locked(&mut queue, frame_pool) {
                Ok(queued) => notify = queued,
                Err(error) => {
                    queue.record_failure(error);
                    notify = true;
                }
            }
        }
        if notify {
            let _ = self.shared.notification.try_send(());
        }
    }

    pub fn mark_closed(&self) {
        if let Ok(mut queue) = self.shared.queue.lock() {
            queue.closed = true;
        }
        let _ = self.shared.notification.try_send(());
    }

    pub fn drain(&self, policy: DrainPolicy) -> CaptureResult<FrameBatch> {
        let mut queue = self.shared.queue.lock().map_err(|_| {
            CaptureError::platform(anyhow::anyhow!("WGC frame transport mutex was poisoned"))
        })?;
        if let Some(error) = queue.failure.take() {
            queue.clear();
            return Err(error);
        }
        let drained = queue.drain(policy);
        Ok(FrameBatch {
            frames: drained.entries,
            overflowed: drained.overflowed,
            discarded: drained.discarded,
            closed: drained.closed,
        })
    }

    pub fn pause_and_clear(&self) -> CaptureResult<()> {
        let mut queue = self.shared.queue.lock().map_err(|_| {
            CaptureError::platform(anyhow::anyhow!("WGC frame transport mutex was poisoned"))
        })?;
        queue.pause_and_clear();
        Ok(())
    }

    pub fn pause(&self) -> CaptureResult<()> {
        let mut queue = self.shared.queue.lock().map_err(|_| {
            CaptureError::platform(anyhow::anyhow!("WGC frame transport mutex was poisoned"))
        })?;
        queue.pause();
        Ok(())
    }

    pub fn discard_and_resume(&self, frame_pool: &Direct3D11CaptureFramePool) -> CaptureResult<()> {
        let mut queue = self.shared.queue.lock().map_err(|_| {
            CaptureError::platform(anyhow::anyhow!("WGC frame transport mutex was poisoned"))
        })?;
        loop {
            match frame_pool.TryGetNextFrame() {
                Ok(frame) => {
                    let _ = frame.Close();
                }
                Err(error) if is_empty_pool_result(&error) => break,
                Err(error) => {
                    queue.resume();
                    return Err(super::map_platform_error(
                        error,
                        "Direct3D11CaptureFramePool::TryGetNextFrame failed while discarding",
                    ));
                }
            }
        }
        queue.resume();
        Ok(())
    }

    pub fn resume_and_drain(&self, frame_pool: &Direct3D11CaptureFramePool) -> CaptureResult<()> {
        let mut queue = self.shared.queue.lock().map_err(|_| {
            CaptureError::platform(anyhow::anyhow!("WGC frame transport mutex was poisoned"))
        })?;
        queue.resume();
        let queued = drain_frame_pool_locked(&mut queue, frame_pool)?;
        drop(queue);
        if queued {
            let _ = self.shared.notification.try_send(());
        }
        Ok(())
    }
}

fn drain_frame_pool_locked(
    queue: &mut BoundedFrameQueue<FramePacket>,
    frame_pool: &Direct3D11CaptureFramePool,
) -> CaptureResult<bool> {
    if queue.paused {
        return Ok(false);
    }
    let mut queued = false;
    loop {
        match frame_pool.TryGetNextFrame() {
            Ok(frame) => {
                let system_relative_time_hns = match frame.SystemRelativeTime() {
                    Ok(time) => time.Duration,
                    Err(error) => {
                        let _ = frame.Close();
                        return Err(super::map_platform_error(
                            error,
                            "Direct3D11CaptureFrame::SystemRelativeTime failed",
                        ));
                    }
                };
                queued |= queue.push(FramePacket {
                    frame,
                    system_relative_time_hns,
                    received_at: Instant::now(),
                });
            }
            Err(error) if is_empty_pool_result(&error) => break,
            Err(error) => {
                return Err(super::map_platform_error(
                    error,
                    "Direct3D11CaptureFramePool::TryGetNextFrame failed",
                ));
            }
        }
    }
    Ok(queued)
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn complete_drain_coalesces_to_latest_entry() {
        let mut queue = BoundedFrameQueue::new(4);
        queue.push(1);
        queue.push(2);
        queue.push(3);

        let drained = queue.drain(DrainPolicy::CompleteLatest);
        assert_eq!(drained.entries, vec![3]);
        assert_eq!(drained.discarded, 2);
        assert!(!drained.overflowed);
    }

    #[test]
    fn ordered_drain_preserves_every_entry() {
        let mut queue = BoundedFrameQueue::new(4);
        queue.push(1);
        queue.push(2);
        queue.push(3);

        let drained = queue.drain(DrainPolicy::Ordered);
        assert_eq!(drained.entries, vec![1, 2, 3]);
        assert_eq!(drained.discarded, 0);
        assert!(!drained.overflowed);
    }

    #[test]
    fn overflow_is_reported_for_ordered_resynchronization() {
        let mut queue = BoundedFrameQueue::new(2);
        queue.push(1);
        queue.push(2);
        queue.push(3);

        let drained = queue.drain(DrainPolicy::Ordered);
        assert_eq!(drained.entries, vec![2, 3]);
        assert!(drained.overflowed);
    }

    #[test]
    fn paused_queue_rejects_new_frames() {
        let mut queue = BoundedFrameQueue::new(4);
        queue.pause_and_clear();
        assert!(!queue.push(1));
        let drained = queue.drain(DrainPolicy::Ordered);
        assert!(drained.entries.is_empty());
        queue.resume();
        assert!(queue.push(2));
    }

    #[test]
    fn pause_preserves_frames_for_an_ordered_contract_transition() {
        let mut queue = BoundedFrameQueue::new(4);
        queue.push(1);
        queue.push(2);
        queue.pause();
        assert!(!queue.push(3));
        queue.resume();

        let drained = queue.drain(DrainPolicy::Ordered);
        assert_eq!(drained.entries, vec![1, 2]);
        assert!(!drained.overflowed);
    }

    #[test]
    fn recorded_failure_discards_queued_frames() {
        let mut queue = BoundedFrameQueue::new(4);
        queue.push(1);
        queue.record_failure(CaptureError::Timeout);

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
