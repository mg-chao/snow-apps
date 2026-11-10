//! Wait on recovery work without repeatedly observing a failed native producer.
use crossbeam_channel::{Receiver, Select};
use std::time::Instant;

#[derive(Debug, PartialEq, Eq)]
pub(super) enum WaitOutcome {
    Progress,
    Canceled,
    Deadline,
    Retry,
}

pub(super) fn wait_for_progress<T>(
    progress: Option<&Receiver<T>>,
    cancellation: &Receiver<()>,
    deadline: Instant,
    retry_at: Option<Instant>,
) -> WaitOutcome {
    let (until, timed_out) = recovery_deadline(Instant::now(), deadline, retry_at);
    let mut ready = Select::new();
    let canceled = ready.recv(cancellation);
    if let Some(progress) = progress {
        ready.recv(progress);
    }
    // Readiness does not consume the result. Refresh must revalidate it against
    // the latest pointer, topology and exclusions before committing a source.
    match ready.ready_deadline(until) {
        Ok(index) if index == canceled => WaitOutcome::Canceled,
        Ok(_) => WaitOutcome::Progress,
        Err(_) => timed_out,
    }
}

fn recovery_deadline(
    now: Instant,
    deadline: Instant,
    retry_at: Option<Instant>,
) -> (Instant, WaitOutcome) {
    // An expired retry cannot shorten the wait: preparation was either already
    // attempted by refresh or is blocked on another control change.
    if let Some(retry) = retry_at.filter(|retry| *retry > now && *retry < deadline) {
        (retry, WaitOutcome::Retry)
    } else {
        (deadline, WaitOutcome::Deadline)
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use crossbeam_channel::{RecvTimeoutError, bounded};
    use std::time::Duration;

    #[test]
    fn failed_source_waits_until_capture_deadline_with_or_without_replacement_work() {
        let (_cancel, cancellation) = bounded(1);
        for preparing in [false, true] {
            let (_result, progress) = bounded::<u8>(1);
            let deadline = Instant::now() + Duration::from_millis(5);
            assert_eq!(
                wait_for_progress(
                    preparing.then_some(&progress),
                    &cancellation,
                    deadline,
                    None,
                ),
                WaitOutcome::Deadline,
            );
            assert!(Instant::now() >= deadline);
        }
    }

    #[test]
    fn zero_capture_budget_does_not_wait_for_retry_or_replacement() {
        let (_cancel, cancellation) = bounded(1);
        let (_result, progress) = bounded::<u8>(1);
        let deadline = Instant::now();
        assert_eq!(
            wait_for_progress(
                Some(&progress),
                &cancellation,
                deadline,
                Some(deadline + Duration::from_secs(1)),
            ),
            WaitOutcome::Deadline,
        );
    }

    #[test]
    fn preparation_and_retirement_wake_capture_without_consuming_results() {
        let (_cancel, cancellation) = bounded(1);
        let (result, progress) = bounded(1);
        result.send(42).unwrap();
        assert_eq!(
            wait_for_progress(
                Some(&progress),
                &cancellation,
                Instant::now() + Duration::from_secs(1),
                None,
            ),
            WaitOutcome::Progress,
        );
        assert_eq!(progress.try_recv(), Ok(42));
        let (done, retired) = bounded(1);
        done.send(()).unwrap();
        assert_eq!(
            wait_for_progress(
                Some(&retired),
                &cancellation,
                Instant::now() + Duration::from_secs(1),
                None,
            ),
            WaitOutcome::Progress,
        );
        assert_eq!(retired.try_recv(), Ok(()));
    }

    #[test]
    fn cancellation_wakes_recovery_and_preserves_the_session_signal() {
        let (cancel, cancellation) = bounded(1);
        let (_result, progress) = bounded::<u8>(1);
        cancel.send(()).unwrap();
        assert_eq!(
            wait_for_progress(
                Some(&progress),
                &cancellation,
                Instant::now() + Duration::from_secs(1),
                None,
            ),
            WaitOutcome::Canceled,
        );
        assert_eq!(cancellation.try_recv(), Ok(()));
    }

    #[test]
    fn pending_recovery_wakes_when_progress_or_cancellation_arrives() {
        for cancel in [false, true] {
            let (cancellation_tx, cancellation) = bounded(1);
            let (progress_tx, progress) = bounded(1);
            let (entered_tx, entered) = bounded(1);
            let (completed_tx, completed) = bounded(1);
            let waiter = std::thread::spawn(move || {
                entered_tx.send(()).unwrap();
                let outcome = wait_for_progress(
                    Some(&progress),
                    &cancellation,
                    Instant::now() + Duration::from_secs(10),
                    None,
                );
                completed_tx
                    .send((outcome, progress, cancellation))
                    .unwrap();
            });
            entered.recv_timeout(Duration::from_secs(5)).unwrap();
            // Both senders stay alive with no observations. The recovery wait
            // must remain pending until one receives the asynchronous signal.
            assert!(matches!(
                completed.recv_timeout(Duration::from_millis(5)),
                Err(RecvTimeoutError::Timeout),
            ));
            if cancel {
                cancellation_tx.send(()).unwrap();
            } else {
                progress_tx.send(42).unwrap();
            }
            // A generous watchdog bounds a regressed wait and the subsequent
            // join; no small upper timing limit depends on host scheduling.
            let (outcome, progress, cancellation) =
                completed.recv_timeout(Duration::from_secs(5)).unwrap();
            waiter.join().unwrap();
            if cancel {
                assert_eq!(outcome, WaitOutcome::Canceled);
                assert_eq!(cancellation.try_recv(), Ok(()));
            } else {
                assert_eq!(outcome, WaitOutcome::Progress);
                assert_eq!(progress.try_recv(), Ok(42));
            }
        }
    }

    #[test]
    fn recovery_backoff_bounds_the_wait_and_expired_retry_does_not_spin() {
        let now = Instant::now();
        let retry = now + Duration::from_millis(250);
        let deadline = now + Duration::from_secs(1);
        assert_eq!(
            recovery_deadline(now, deadline, Some(retry)),
            (retry, WaitOutcome::Retry),
        );
        assert_eq!(
            recovery_deadline(now, retry, Some(deadline)),
            (retry, WaitOutcome::Deadline),
        );
        assert_eq!(
            recovery_deadline(retry, deadline, Some(retry)),
            (deadline, WaitOutcome::Deadline),
        );
        let (_cancel, cancellation) = bounded(1);
        let deadline = Instant::now() + Duration::from_millis(5);
        assert_eq!(
            wait_for_progress::<()>(None, &cancellation, deadline, Some(now)),
            WaitOutcome::Deadline,
        );
        assert!(Instant::now() >= deadline);
    }
}
