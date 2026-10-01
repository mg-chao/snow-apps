pub mod artifact;
pub mod bundle;
pub mod error;
pub mod input;
pub mod model;
pub mod mouse;
pub mod render;
pub mod shared;
pub mod source;
pub mod video_index;

pub use artifact::{
    AudioSampleFormat, AudioTrackManifest, AudioTrackRole, LocalRecordingPaths, PauseInterval,
    RecordingArtifact, SessionManifest,
};
pub use bundle::{
    BundleAssetKind, BundleAssetRecord, RecordingBundleAsset, RecordingBundleFooter,
    read_recording_bundle_asset, read_recording_bundle_footer, write_recording_bundle,
};
pub use error::{RecordingModelError, Result};
pub use input::{
    InputMouseButton, InputStoreReader, InputStoreWriter, KeyEventRecord, RecordedInput,
    RecordedInputEvent, RecordedMouseClick,
};
pub use model::StoredFrame;
pub use mouse::{
    ClickEventRecord, CursorFrameRecord, CursorShapeCompositionMode, CursorShapeRecord,
    MouseButton, MouseStore, MouseStoreWriter, decode_mouse_records, read_mouse_records,
    write_mouse_records,
};
pub use render::{
    EffectsConfig, FinalizedTimeline, KeyboardOverlayConfig, KeyboardOverlayFont, PlaybackOverlay,
    RenderConfig, RenderFrame, RenderMetadata, read_bundle_render_metadata, read_render_metadata,
    write_render_metadata,
};
pub use shared::{IntermediateRecordingProfile, VideoCodec, VideoEncodeConfig, VideoEncodingSpeed};
pub use source::SourceDescription;
pub use video_index::{
    VIDEO_INDEX_MAGIC, VIDEO_INDEX_RECORD_BYTES, VideoIndexEntry, VideoIndexReader,
};

pub mod media;
