#[path = "arrow/data.rs"]
mod arrow;
#[path = "arrow/binding_core.rs"]
mod arrow_binding_core;
#[path = "arrow/binding_geometry.rs"]
mod arrow_binding_geometry;
#[path = "arrow/elbow_core.rs"]
mod arrow_elbow_core;
#[path = "arrow/elbow_router.rs"]
mod arrow_elbow_router;
#[path = "arrow/elbow_update.rs"]
mod arrow_elbow_update;
#[path = "arrow/engine.rs"]
mod arrow_engine;
#[path = "arrow/focus_core.rs"]
mod arrow_focus_core;
#[path = "arrow/geom.rs"]
mod arrow_geom;
#[path = "arrow/hit_test.rs"]
mod arrow_hit_test;
#[path = "arrow/model.rs"]
mod arrow_model;
#[path = "arrow/operations.rs"]
mod arrow_operations;
#[path = "arrow/render_core.rs"]
mod arrow_render_core;
#[path = "arrow/state_core.rs"]
mod arrow_state_core;
mod bindings;
mod document;
mod document_geometry;
mod free_draw;
mod transaction;

pub use arrow::{
    ArrowData, ArrowEndpointBinding, DEFAULT_ARROW_MAX_COORDINATE, LinearElementKind, arrow_bounds,
    arrow_hit_test, arrow_is_degenerate, arrow_length, arrow_segment_midpoints,
    arrowhead_render_primitives, validate_arrow,
};
pub use arrow_operations::{
    ArrowEndpointDragOptions, ArrowFocusDragOptions, compute_arrow_endpoint_drag,
    compute_arrow_focus_drag, drag_elbow_arrow_segment, preview_elbow_arrow_endpoint_binding,
    recompute_arrow_after_bindable_change, visible_arrow_focus_points,
};
pub use bindings::*;
pub use document::*;
pub use document_geometry::*;
pub use free_draw::*;
pub use transaction::*;

pub use arrow_render_core::ArrowheadRenderPrimitivesInput;

pub(crate) use arrow_model::*;
pub(crate) use snow_draw_engine_core::arrow::{
    ArrowEndpointEdge, ArrowEndpointPosition, ArrowEndpointSelector, ArrowPathCommand, Arrowhead,
    ArrowheadCirclePrimitive, ArrowheadDashMode, ArrowheadFillMode, ArrowheadLinePrimitive,
    ArrowheadPoints, ArrowheadPolygonPrimitive, ArrowheadPrimitiveKind, ArrowheadRenderPrimitive,
    BindMode, BindableShape, CurvePathOp, EngineContext, FixedSegment, FocusPointContext,
    UpdateElbowArrowOptions, normalize_arrow_endpoint_edge,
};
