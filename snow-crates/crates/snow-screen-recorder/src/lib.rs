pub use snow_recording_export::{
    EditingSession, ExportAudioOutputConfig, ExportAudioTrackRequest, ExportExecutionMode,
    ExportFormat, ExportPathKind, ExportPerformanceConfig, ExportProgress, ExportRequest,
    ExportResult, ExportRuntimeReport, ExportStage, ExportStageDurationsMs, ExportTask,
    MouseEditConfig, RecordingExportError, SoftwareH264Priority, StreamingAudioConfig,
    StreamingEncoder, StreamingEncoderConfig, StreamingEncoderReport, VideoCodec,
    scaled_output_dimensions,
};
pub use snow_recording_model::{
    AudioSampleFormat, AudioTrackManifest, AudioTrackRole, ClickEventRecord, CursorFrameRecord,
    CursorShapeCompositionMode, CursorShapeRecord, EffectsConfig, FinalizedTimeline,
    IntermediateRecordingProfile, LocalRecordingPaths, MouseButton, MouseStore, PauseInterval,
    PlaybackOverlay, RecordingArtifact, RecordingModelError, RenderConfig, RenderMetadata,
    SessionManifest, StoredFrame, VideoEncodeConfig, VideoEncodingSpeed, read_mouse_records,
    write_mouse_records,
};
pub use snow_recording_runtime::{
    AudioChannels, CaptureBackendKind, DeferredRecordingOptions, DeferredRecordingSession,
    DeferredRecordingSource, DeferredRenderProgress, DeferredRenderState, DeferredRenderTask,
    DirectRecordingConfig, DirectRecordingReport, DirectRecordingSession, KeyboardOverlayConfig,
    KeyboardOverlayFont, MediaPermission, MonitorSelector, RecordingAudioConfig,
    RecordingAudioMode, RecordingAudioTrackConfig, RecordingAudioTrackSource, RecordingConfig,
    RecordingRegion, RecordingSession, RecordingState, RecordingTarget, ScreenRecorderError,
    WindowId,
};

#[cfg(target_os = "macos")]
pub use snow_recording_runtime::macos;
