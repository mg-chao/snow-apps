pub mod artifact;
pub mod bundle;
pub mod error;
pub mod model;
pub mod mouse;
pub mod shared;

pub use artifact::{
    AudioSampleFormat, AudioTrackManifest, AudioTrackRole, LocalRecordingPaths, PauseInterval,
    RecordingArtifact, SessionManifest,
};
pub use bundle::{
    BundleAssetKind, BundleAssetRecord, RecordingBundleAsset, RecordingBundleFooter,
    read_recording_bundle_asset, read_recording_bundle_footer, write_recording_bundle,
};
pub use error::{RecordingModelError, Result};
pub use model::StoredFrame;
pub use mouse::{
    ClickEventRecord, CursorFrameRecord, CursorShapeCompositionMode, CursorShapeRecord,
    MouseButton, MouseStore, decode_mouse_records, read_mouse_records, write_mouse_records,
};
pub use shared::{IntermediateRecordingProfile, VideoCodec, VideoEncodeConfig, VideoEncodingSpeed};
