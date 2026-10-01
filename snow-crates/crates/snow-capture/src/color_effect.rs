//! Best-effort reversal of the Windows full-screen Magnifier color matrix.
//!
//! DXGI HDR sources are corrected in linear scRGB before clamping, SDR white-level
//! normalization, and tone mapping. This restores SDR content on HDR displays;
//! it does not require reversing the tone mapper. GPU conversion consumes the
//! correction in its existing shader pass, while CPU conversion fuses it into
//! the existing HDR kernels. Neither path reapplies it to GPU-converted pixels.
//! SDR sources use the corresponding encoded RGB matrix. WGC already returns
//! original pixels and does not opt into either transform.

use nalgebra::{Matrix3, Vector3};
use std::sync::{Arc, OnceLock};
use std::thread::JoinHandle;

/// One request's color matrix, shared by all of its capture workers.
/// Backends wait for it only after acquiring pixels, before color conversion.
#[derive(Clone, Debug)]
pub struct PendingScreenColorTransform {
    result: Arc<OnceLock<Option<ScreenColorTransform>>>,
}

impl PendingScreenColorTransform {
    pub(crate) fn resolve(&self) -> Option<ScreenColorTransform> {
        *self.result.wait()
    }
}

/// Owns the query worker so cancellation and failed captures cannot leave it running.
pub struct ScreenColorQuery {
    snapshot: PendingScreenColorTransform,
    worker: Option<JoinHandle<()>>,
}

impl ScreenColorQuery {
    pub fn start_current() -> std::io::Result<Self> {
        Self::start_with(current_transform)
    }

    pub fn snapshot(&self) -> PendingScreenColorTransform {
        self.snapshot.clone()
    }

    pub(crate) fn start_with(
        query: impl FnOnce() -> Option<ScreenColorTransform> + Send + 'static,
    ) -> std::io::Result<Self> {
        let snapshot = PendingScreenColorTransform {
            result: Arc::new(OnceLock::new()),
        };
        let result = snapshot.clone();
        let worker = std::thread::Builder::new()
            .name("snow-capture-color-query".to_owned())
            .spawn(move || {
                // Publish a best-effort absence even if the native query unwinds,
                // so capture workers never wait for an abandoned result.
                struct PublishOnDrop(PendingScreenColorTransform);
                impl Drop for PublishOnDrop {
                    fn drop(&mut self) {
                        let _ = self.0.result.set(None);
                    }
                }
                let publish = PublishOnDrop(result);
                snow_core::qos::apply_current_thread();
                let _ = publish.0.result.set(query());
            })?;
        Ok(Self {
            snapshot,
            worker: Some(worker),
        })
    }
}

impl Drop for ScreenColorQuery {
    fn drop(&mut self) {
        if let Some(worker) = self.worker.take() {
            let _ = worker.join();
        }
    }
}

/// Policy for capture paths that contain a full-screen Magnifier effect.
#[derive(Clone, Copy, Debug, Default, PartialEq)]
pub enum ColorCorrection {
    #[default]
    Disabled,
    /// Sample the effect immediately before each capture operation.
    CurrentMagnifier,
    /// Share one sampled effect across all workers in a desktop snapshot.
    Snapshot(Option<ScreenColorTransform>),
}

impl ColorCorrection {
    pub fn snapshot_current() -> Self {
        Self::Snapshot(current_transform())
    }

    pub(crate) fn resolve(self) -> Option<ScreenColorTransform> {
        match self {
            Self::Disabled => None,
            Self::CurrentMagnifier => current_transform(),
            Self::Snapshot(transform) => transform,
        }
    }
}

/// Validated inverse RGB affine transform. Alpha is never modified.
#[derive(Clone, Copy, Debug, PartialEq)]
pub struct ScreenColorTransform {
    pub(crate) rows: [[f32; 4]; 3],
    pub(crate) inverted: bool,
}

impl ScreenColorTransform {
    /// Floating-point DXGI surfaces carry the effect in linear scRGB units.
    pub(crate) fn linear_rows(self) -> [[f32; 4]; 3] {
        self.rows.map(|mut row| {
            row[3] /= 255.0;
            row
        })
    }

    /// Accepts the row-major MAGCOLOREFFECT matrix (row-vector convention).
    /// Identity, unsupported alpha effects, and unstable inverses return None.
    pub fn from_magnifier_matrix(matrix: &[f32; 25]) -> Option<Self> {
        if matrix.iter().any(|value| !value.is_finite()) {
            return None;
        }
        for row in 0..5 {
            for column in 0..5 {
                if row == 3 || column >= 3 {
                    let expected = if row == column { 1.0 } else { 0.0 };
                    if matrix[row * 5 + column] != expected {
                        return None;
                    }
                }
            }
        }
        let linear = Matrix3::<f64>::from_fn(|r, c| f64::from(matrix[c * 5 + r]));
        let offset = Vector3::new(matrix[20] as f64, matrix[21] as f64, matrix[22] as f64);
        if linear == Matrix3::identity() && offset == Vector3::zeros() {
            return None;
        }
        let inverse = linear.try_inverse()?;
        let norm = |m: &Matrix3<f64>| (0..3).map(|r| m.row(r).abs().sum()).fold(0.0, f64::max);
        if norm(&linear) * norm(&inverse) > 10_000.0
            || norm(&(linear * inverse - Matrix3::identity())) > 1e-6
        {
            return None;
        }
        let translation = -(inverse * offset);
        let rows = std::array::from_fn(|r| {
            [
                inverse[(r, 0)] as f32,
                inverse[(r, 1)] as f32,
                inverse[(r, 2)] as f32,
                (translation[r] * 255.0) as f32,
            ]
        });
        if rows.iter().flatten().any(|v| !v.is_finite()) {
            return None;
        }
        Some(Self {
            inverted: rows
                == [
                    [-1., 0., 0., 255.],
                    [0., -1., 0., 255.],
                    [0., 0., -1., 255.],
                ],
            rows,
        })
    }
}

#[cfg(any(windows, test))]
fn with_magnifier<T>(
    initialize: impl FnOnce() -> bool,
    query: impl FnOnce() -> T,
    uninitialize: impl FnOnce(),
) -> Option<T> {
    if !initialize() {
        return None;
    }
    struct Guard<F: FnOnce()>(Option<F>);
    impl<F: FnOnce()> Drop for Guard<F> {
        fn drop(&mut self) {
            if let Some(uninitialize) = self.0.take() {
                uninitialize();
            }
        }
    }
    let _guard = Guard(Some(uninitialize));
    Some(query())
}

#[cfg(windows)]
fn current_transform() -> Option<ScreenColorTransform> {
    use std::cell::RefCell;
    use std::sync::Mutex;
    use windows::Win32::UI::Magnification::{
        MAGCOLOREFFECT, MagGetFullscreenColorEffect, MagInitialize, MagUninitialize,
    };

    // MagInitialize/MagUninitialize affect process-wide state. Serialize the
    // entire query so another capture cannot uninitialize a concurrent reader.
    // Only the sampled matrix cache survives the query, never native resources.
    static QUERY: Mutex<()> = Mutex::new(());
    struct Reader {
        matrix: Option<[f32; 25]>,
        transform: Option<ScreenColorTransform>,
    }
    impl Reader {
        fn read(&mut self) -> Option<ScreenColorTransform> {
            let matrix = {
                let _query = QUERY.lock().ok()?;
                with_magnifier(
                    || unsafe { MagInitialize() }.as_bool(),
                    || {
                        let mut effect = MAGCOLOREFFECT::default();
                        unsafe { MagGetFullscreenColorEffect(&mut effect) }
                            .as_bool()
                            .then_some(effect.transform)
                    },
                    || unsafe {
                        let _ = MagUninitialize();
                    },
                )
                .flatten()
            };
            let Some(matrix) = matrix else {
                self.matrix = None;
                self.transform = None;
                return None;
            };
            if self.matrix != Some(matrix) {
                self.transform = ScreenColorTransform::from_magnifier_matrix(&matrix);
                self.matrix = Some(matrix);
            }
            self.transform
        }
    }
    thread_local! {
        static READER: RefCell<Reader> = const { RefCell::new(Reader {
            matrix: None, transform: None,
        }) };
    }
    READER.with(|reader| reader.borrow_mut().read())
}

#[cfg(not(windows))]
fn current_transform() -> Option<ScreenColorTransform> {
    None
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn parallel_query_shares_one_sample_and_joins_on_drop() {
        use std::cell::RefCell;
        use std::sync::mpsc;
        use std::time::Duration;

        struct ExitNotice(mpsc::Sender<()>);
        impl Drop for ExitNotice {
            fn drop(&mut self) {
                let _ = self.0.send(());
            }
        }
        thread_local! {
            static EXIT: RefCell<Option<ExitNotice>> = const { RefCell::new(None) };
        }
        let (release, wait) = mpsc::channel();
        let (exited, exit) = mpsc::channel();
        let query = ScreenColorQuery::start_with(move || {
            EXIT.with(|notice| *notice.borrow_mut() = Some(ExitNotice(exited)));
            wait.recv_timeout(Duration::from_secs(5)).unwrap();
            ScreenColorTransform::from_magnifier_matrix(&{
                let mut matrix = identity();
                matrix[0] = 0.5;
                matrix
            })
        })
        .unwrap();
        let snapshot = query.snapshot();
        let consumers = (0..3)
            .map(|_| {
                let snapshot = snapshot.clone();
                std::thread::spawn(move || snapshot.resolve())
            })
            .collect::<Vec<_>>();
        release.send(()).unwrap();
        let expected = snapshot.resolve();
        assert!(expected.is_some());
        for consumer in consumers {
            assert_eq!(consumer.join().unwrap(), expected);
        }
        drop(query);
        // A thread-local destructor runs at actual worker exit, not when its
        // result is published. The request owner must finish that exit too.
        exit.try_recv().unwrap();
    }

    #[test]
    fn parallel_query_failure_and_unwind_release_waiting_consumers() {
        for panics in [false, true] {
            let query = ScreenColorQuery::start_with(move || {
                assert!(!panics, "controlled query failure");
                None
            })
            .unwrap();
            assert_eq!(query.snapshot().resolve(), None);
            drop(query);
        }
    }

    #[test]
    fn magnifier_query_releases_initialization_on_success_and_failure() {
        use std::cell::Cell;

        for query_succeeds in [true, false] {
            let initialized = Cell::new(false);
            let result = with_magnifier(
                || {
                    initialized.set(true);
                    true
                },
                || {
                    assert!(initialized.get());
                    query_succeeds.then_some(42)
                },
                || initialized.set(false),
            );
            assert_eq!(result, Some(query_succeeds.then_some(42)));
            assert!(!initialized.get());
        }
    }

    #[test]
    fn magnifier_initialization_failure_skips_query_and_cleanup() {
        assert_eq!(
            with_magnifier(
                || false,
                || panic!("query requires successful initialization"),
                || panic!("failed initialization must not be uninitialized"),
            ),
            None::<()>,
        );
    }

    #[test]
    fn magnifier_query_releases_initialization_during_unwind() {
        use std::cell::Cell;

        let released = Cell::new(false);
        let result = std::panic::catch_unwind(std::panic::AssertUnwindSafe(|| {
            with_magnifier(|| true, || panic!("query failed"), || released.set(true));
        }));
        assert!(result.is_err());
        assert!(released.get());
    }
    fn identity() -> [f32; 25] {
        std::array::from_fn(|i| if i / 5 == i % 5 { 1.0 } else { 0.0 })
    }
    #[test]
    fn rejects_identity_singular_unstable_and_nonfinite() {
        assert!(ScreenColorTransform::from_magnifier_matrix(&identity()).is_none());
        for value in [0.0, 1e-8, f32::NAN, f32::INFINITY] {
            let mut m = identity();
            m[0] = value;
            assert!(ScreenColorTransform::from_magnifier_matrix(&m).is_none());
        }
        let mut m = identity();
        m[18] = 0.5;
        assert!(ScreenColorTransform::from_magnifier_matrix(&m).is_none());
    }
    #[test]
    fn prepares_exact_inversion() {
        let mut m = identity();
        for c in 0..3 {
            m[c * 6] = -1.0;
            m[20 + c] = 1.0;
        }
        assert!(
            ScreenColorTransform::from_magnifier_matrix(&m)
                .unwrap()
                .inverted
        );
    }
    #[test]
    fn reverses_channel_mixing_and_translation() {
        let mut m = identity();
        m[0] = 0.8;
        m[5] = 0.1;
        m[20] = 0.05;
        let inverse = ScreenColorTransform::from_magnifier_matrix(&m).unwrap();
        let original = [40., 80., 120.];
        let filtered = [40. * 0.8 + 80. * 0.1 + 0.05 * 255., 80., 120.];
        for (row, expected) in inverse.rows.iter().zip(original) {
            let actual =
                row[0] * filtered[0] + row[1] * filtered[1] + row[2] * filtered[2] + row[3];
            assert!((actual - expected).abs() < 0.001);
        }
    }
}
