//! Broadcast cancellation. Closing a channel wakes every concurrent waiter;
//! a single queued cancellation message would wake only one cloned receiver.
use crossbeam_channel::{Receiver, Sender};
use std::sync::{
    Arc, Mutex,
    atomic::{AtomicBool, Ordering},
};
use std::time::Duration;
#[derive(Debug)]
struct State {
    canceled: AtomicBool,
    sender: Mutex<Option<Sender<()>>>,
    receiver: Receiver<()>,
}
#[derive(Clone, Debug)]
pub struct CancellationToken(Arc<State>);
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum WaitError {
    Canceled,
    Timeout,
    Disconnected,
}
impl Default for CancellationToken {
    fn default() -> Self {
        let (sender, receiver) = crossbeam_channel::bounded(0);
        Self(Arc::new(State {
            canceled: AtomicBool::new(false),
            sender: Mutex::new(Some(sender)),
            receiver,
        }))
    }
}
impl CancellationToken {
    pub fn cancel(&self) {
        let mut sender = self.0.sender.lock().unwrap_or_else(|e| e.into_inner());
        self.0.canceled.store(true, Ordering::Release);
        sender.take();
    }
    pub fn is_canceled(&self) -> bool {
        self.0.canceled.load(Ordering::Acquire)
    }
    /// Linearize a short final publication against cancellation. If cancel wins,
    /// the closure is never called. If publication wins, cancellation waits until
    /// it returns. Do not block on native work or reenter this token in `publish`.
    pub fn commit<T>(&self, publish: impl FnOnce() -> T) -> Result<T, WaitError> {
        let _guard = self.0.sender.lock().unwrap_or_else(|e| e.into_inner());
        if self.is_canceled() {
            Err(WaitError::Canceled)
        } else {
            Ok(publish())
        }
    }
    /// Becomes disconnected at cancellation; it never receives a value.
    pub fn receiver(&self) -> &Receiver<()> {
        &self.0.receiver
    }
    pub fn wait_for<T>(&self, completion: &Receiver<T>, timeout: Duration) -> Result<T, WaitError> {
        if self.is_canceled() {
            return Err(WaitError::Canceled);
        }
        crossbeam_channel::select_biased! {
            recv(self.receiver()) -> _ => Err(WaitError::Canceled),
            recv(completion) -> result => {
                if self.is_canceled() { Err(WaitError::Canceled) }
                else { result.map_err(|_| WaitError::Disconnected) }
            },
            default(timeout) => Err(WaitError::Timeout),
        }
    }
}
#[cfg(test)]
mod tests {
    use super::*;
    #[test]
    fn commit_and_cancel_have_an_unambiguous_order() {
        let token = CancellationToken::default();
        assert_eq!(token.commit(|| 42), Ok(42));
        let inside = token.clone();
        let worker = token
            .commit(|| {
                let worker = std::thread::spawn(move || inside.cancel());
                assert!(!token.is_canceled());
                worker
            })
            .unwrap();
        worker.join().unwrap();
        assert!(token.is_canceled());
        assert_eq!(
            token.commit(|| panic!("canceled publication must never run")),
            Err(WaitError::Canceled)
        );
    }
    #[test]
    fn cancellation_wakes_every_waiter_and_wins_over_late_delivery() {
        let token = CancellationToken::default();
        let (sender, receiver) = crossbeam_channel::bounded::<u32>(1);
        let barrier = Arc::new(std::sync::Barrier::new(3));
        let workers: Vec<_> = (0..2)
            .map(|_| {
                let token = token.clone();
                let receiver = receiver.clone();
                let barrier = barrier.clone();
                std::thread::spawn(move || {
                    barrier.wait();
                    token.wait_for(&receiver, Duration::from_secs(2))
                })
            })
            .collect();
        barrier.wait();
        token.cancel();
        sender.try_send(7).unwrap();
        for worker in workers {
            assert_eq!(worker.join().unwrap(), Err(WaitError::Canceled));
        }
        assert_eq!(
            token.wait_for(&receiver, Duration::ZERO),
            Err(WaitError::Canceled)
        );
        token.cancel();
        assert!(token.is_canceled());
    }
    #[test]
    fn timeout_and_disconnection_remain_distinct() {
        let token = CancellationToken::default();
        let (sender, receiver) = crossbeam_channel::bounded::<u32>(1);
        assert_eq!(
            token.wait_for(&receiver, Duration::ZERO),
            Err(WaitError::Timeout)
        );
        sender.send(9).unwrap();
        assert_eq!(token.wait_for(&receiver, Duration::ZERO), Ok(9));
        drop(sender);
        assert_eq!(
            token.wait_for(&receiver, Duration::ZERO),
            Err(WaitError::Disconnected)
        );
    }
}
