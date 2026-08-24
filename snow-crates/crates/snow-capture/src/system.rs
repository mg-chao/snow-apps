use std::sync::Arc;

use crate::CaptureTarget;
use crate::backend::{
    self, AutoBackendPolicy, CaptureBackend, CaptureBackendKind, CaptureWorkload, WgcUpdateMode,
};
use crate::capture_session::{CaptureSession, CaptureTargetInfo, inspect_target_from_backend};
use crate::error::CaptureResult;
use crate::monitor::MonitorId;
use crate::region::MonitorLayout;

#[derive(Clone, Copy, Debug)]
pub struct CaptureOptions {
    pub capture_retry_count: usize,
    pub workload: CaptureWorkload,
    pub gpu_hdr_conversion: bool,
    pub hdr_tonemap_lut: bool,
    /// Controls how Windows Graphics Capture updates its canonical GPU frame.
    ///
    /// This is independent from [`CaptureWorkload`]: workload selects latency
    /// and backpressure behavior, while this option selects the WGC surface
    /// correctness contract.
    pub wgc_update_mode: WgcUpdateMode,
    /// Record a per-stage timing breakdown inside participating backends and
    /// attach it to each frame's metadata (`FrameMetadata::stage_timings`).
    ///
    /// Compile-time gated by the `stage-timing` cargo feature: this field,
    /// the metadata it feeds, and all backend instrumentation only exist in
    /// builds that enable that feature (benchmarks and diagnostics). When
    /// the feature is disabled the capture hot path contains no
    /// instrumentation code at all.
    #[cfg(feature = "stage-timing")]
    pub record_stage_timings: bool,
}

impl Default for CaptureOptions {
    fn default() -> Self {
        Self {
            capture_retry_count: 1,
            workload: CaptureWorkload::Snapshot,
            gpu_hdr_conversion: true,
            hdr_tonemap_lut: true,
            wgc_update_mode: WgcUpdateMode::Auto,
            #[cfg(feature = "stage-timing")]
            record_stage_timings: false,
        }
    }
}

#[derive(Clone)]
pub struct CaptureSystem {
    backend: Arc<dyn CaptureBackend>,
    backend_kind: CaptureBackendKind,
    auto_backend_policy: AutoBackendPolicy,
    auto_backend_policy_is_explicit: bool,
}

impl CaptureSystem {
    pub fn builder() -> CaptureSystemBuilder {
        CaptureSystemBuilder::new()
    }

    pub fn enumerate_monitors(&self) -> CaptureResult<Vec<MonitorId>> {
        self.backend.enumerate_monitors()
    }

    pub fn primary_monitor(&self) -> CaptureResult<MonitorId> {
        self.backend.primary_monitor()
    }

    pub fn monitor_layout(&self) -> CaptureResult<MonitorLayout> {
        self.backend.monitor_layout()
    }

    pub fn refresh_display_configuration(&self) -> CaptureResult<()> {
        self.backend.refresh_display_configuration()
    }

    pub fn backend_kind(&self) -> CaptureBackendKind {
        self.backend_kind
    }

    pub fn inspect_target(&self, target: &CaptureTarget) -> CaptureResult<CaptureTargetInfo> {
        inspect_target_from_backend(&self.backend, target)
    }

    pub fn open_session(
        &self,
        target: CaptureTarget,
        options: CaptureOptions,
    ) -> CaptureResult<CaptureSession> {
        let session_backend = if options.workload == CaptureWorkload::Snapshot {
            backend::backend_for_kind_with_auto_policy(
                self.backend_kind,
                self.auto_backend_policy.clone(),
                self.auto_backend_policy_is_explicit,
            )?
        } else {
            Arc::clone(&self.backend)
        };
        CaptureSession::open_with_backend(target, session_backend, options)
    }
}

pub struct CaptureSystemBuilder {
    backend_kind: CaptureBackendKind,
    auto_backend_policy: AutoBackendPolicy,
    auto_backend_policy_is_explicit: bool,
}

impl CaptureSystemBuilder {
    pub fn new() -> Self {
        Self {
            backend_kind: CaptureBackendKind::Auto,
            auto_backend_policy: AutoBackendPolicy::default(),
            auto_backend_policy_is_explicit: false,
        }
    }

    pub fn with_backend_kind(mut self, kind: CaptureBackendKind) -> Self {
        self.backend_kind = kind;
        self
    }

    pub fn with_auto_backend_policy(mut self, auto_backend_policy: AutoBackendPolicy) -> Self {
        self.auto_backend_policy = auto_backend_policy;
        self.auto_backend_policy_is_explicit = true;
        self
    }

    pub fn build(self) -> CaptureResult<CaptureSystem> {
        let backend = backend::backend_for_kind_with_auto_policy(
            self.backend_kind,
            self.auto_backend_policy.clone(),
            self.auto_backend_policy_is_explicit,
        )?;
        Ok(CaptureSystem {
            backend,
            backend_kind: self.backend_kind,
            auto_backend_policy: self.auto_backend_policy,
            auto_backend_policy_is_explicit: self.auto_backend_policy_is_explicit,
        })
    }
}

impl Default for CaptureSystemBuilder {
    fn default() -> Self {
        Self::new()
    }
}
