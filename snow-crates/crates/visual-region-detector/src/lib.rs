//! Source-only screenshot region detection, ported from `python/pipeline.py`.
//!
//! The public facade is backend-independent. Images enter through [`BgrImage`]
//! and [`detect_regions`]; exporting results is a separate operation. Pure Rust
//! processing is the default. Enable the `opencv` feature to select the
//! OpenCV primitive backend and use [`opencv_backend`] for `Mat` interop.
pub use error::{Error, Result};
pub use geometry::{Rect, iou};
pub use grid::BgrImage;
pub use model::Region;
pub use output::{draw_regions, regions_to_jsonable, write_crops};
pub use pipeline::detect_regions;

#[cfg(feature = "opencv")]
pub mod opencv_backend;

/// Name of the compile-time-selected processing backend.
pub fn backend_name() -> &'static str {
    backend::name()
}

mod backend;
mod color;
mod error;
mod geometry;
mod grid;
mod icons;
mod mask;
mod media;
mod model;
mod output;
mod pipeline;
mod selection;
mod surfaces;
mod text;
