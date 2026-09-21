//! Shared abstractions for the snow screen recording workspace.
//!
//! `snow-core` provides common traits, timestamp types, and error foundations
//! consumed by all leaf crates (`snow-capture`, `snow-audio-recorder`,
//! `snow-cursor`) and the compositor crate (`snow-screen-recorder`).

pub mod error;
pub mod event;
pub mod recording_clock;
pub mod stream_queue;
pub mod streaming;
pub mod timestamp;

pub mod cancellation;

pub mod keycap_layout;
