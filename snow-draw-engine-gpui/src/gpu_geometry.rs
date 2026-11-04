//! Typed, retained geometry preparation. The CPU only shapes/tessellates changed
//! geometry; all coverage, paint, blending and effects are evaluated on the GPU.

use std::{
    collections::HashMap,
    sync::{
        Arc,
        atomic::{AtomicU64, Ordering},
    },
};

use lyon::{
    algorithms::measure::{PathMeasurements, SampleType},
    math::{Angle, point, vector},
    path::{Event, Path, Winding, iterator::PathIterator},
    tessellation::{
        FillOptions, FillTessellator, FillVertex, GeometryBuilderError, LineCap, LineJoin,
        StrokeOptions, StrokeTessellator, StrokeVertex, VertexBuffers, VertexId,
        geometry_builder::{FillGeometryBuilder, GeometryBuilder, StrokeGeometryBuilder},
    },
};
use snow_draw_engine::*;

use crate::{CanvasError, vector::VectorContext};

#[path = "gpu_arrow_outline.rs"]
mod arrow_outline;

static NEXT_MESH: AtomicU64 = AtomicU64::new(1);
const CACHE_BUDGET: usize = 64 * 1024 * 1024;
const MAX_MESH_VERTICES: usize = 1_048_576;

/// Indexed local-coordinate geometry. `identity` never aliases a retired mesh.
pub(crate) struct Mesh {
    pub identity: u64,
    pub vertices: Vec<[f32; 2]>,
    pub indices: Vec<u32>,
    pub bounds: [f32; 4],
}

impl Mesh {
    pub fn from_vertices(vertices: Vec<[f32; 2]>, indices: Vec<u32>) -> Result<Self, CanvasError> {
        if vertices.len() >= MAX_MESH_VERTICES || indices.len() >= MAX_MESH_VERTICES * 6 {
            return Err(CanvasError::ResourceLimit);
        }
        let mut bounds = [
            f32::INFINITY,
            f32::INFINITY,
            f32::NEG_INFINITY,
            f32::NEG_INFINITY,
        ];
        for [x, y] in &vertices {
            if !x.is_finite() || !y.is_finite() {
                return Err(ErrorCode::InvalidArgument.into());
            }
            bounds[0] = bounds[0].min(*x);
            bounds[1] = bounds[1].min(*y);
            bounds[2] = bounds[2].max(*x);
            bounds[3] = bounds[3].max(*y);
        }
        if vertices.is_empty() {
            bounds = [0.0; 4];
        }
        Ok(Self {
            identity: NEXT_MESH.fetch_add(1, Ordering::Relaxed),
            vertices,
            indices,
            bounds,
        })
    }

    pub fn retained_bytes(&self) -> usize {
        self.vertices.len() * std::mem::size_of::<[f32; 2]>()
            + self.indices.len() * std::mem::size_of::<u32>()
    }
}

/// Affine transforms use [xx, yx, xy, yy, tx, ty], applied in f64 before
/// camera-relative device conversion. Color is straight RGBA; GPU blending uses
/// premultiplied channels. Opacity belongs to the containing item, not each part.
#[derive(Clone)]
pub(crate) struct Paint {
    pub mesh: Arc<Mesh>,
    pub color: ColorRgba8,
    pub fill_style: DisplayFillStyle,
    pub transform: [f64; 6],
    pub rough_style: StrokeRenderingStyle,
    pub rough_seed: u32,
    pub rough_scale: f64,
    pub is_text: bool,
    pub color_glyph: Option<Arc<crate::vector::ColorGlyph>>,
    pub glyph_tint: bool,
    /// Consecutive parts with the same nonzero id contribute union coverage.
    /// The GPU applies their shared color/rough effect once after that union.
    pub coverage_group: u32,
}

pub(crate) struct ItemGeometry {
    pub parts: Vec<Paint>,
    pub opacity: f64,
    pub blend: DisplayBlendMode,
    /// World-space exclusion under an attached arrow label.
    pub label_gap: Option<DrawRect>,
}

impl Default for ItemGeometry {
    fn default() -> Self {
        Self {
            parts: Vec::new(),
            opacity: 1.0,
            blend: DisplayBlendMode::Normal,
            label_gap: None,
        }
    }
}

#[derive(Clone, Hash, PartialEq, Eq)]
struct MeshKey {
    path: Vec<u32>,
    tolerance: u32,
    width: u32,
    style: u8,
}

#[derive(Clone, Copy, Hash, PartialEq, Eq)]
struct GlyphKey {
    font: usvg::fontdb::ID,
    id: u16,
    tolerance: u32,
    width: u32,
}

#[derive(Clone, Copy, Hash, PartialEq, Eq)]
struct ChunkKey {
    commands: usize,
    origin: [u64; 2],
    tolerance: u32,
    width: u32,
    style: u8,
    phase: u64,
    closed: bool,
}

struct ChunkMesh {
    // Retaining this Arc makes pointer identity collision-free, including after
    // a source node retires and the allocator would otherwise reuse its address.
    _commands: Arc<[PathCommand]>,
    mesh: Arc<Mesh>,
}

#[derive(Hash, PartialEq, Eq)]
struct ArrowOutlineKey {
    parts: Vec<u64>,
    width: u64,
    tolerance: u32,
}

/// Mesh keys omit pose, color, fill pattern, opacity and blend mode. A style-only
/// edit reuses the geometry uploaded for its previous frame.
#[derive(Default)]
pub(crate) struct GpuGeometryCache {
    meshes: HashMap<MeshKey, Arc<Mesh>>,
    glyphs: HashMap<GlyphKey, Arc<Mesh>>,
    chunks: HashMap<ChunkKey, ChunkMesh>,
    arrow_outlines: HashMap<ArrowOutlineKey, Arc<Mesh>>,
    retained_bytes: usize,
    pub builds: u64,
    pub text: VectorContext,
}

impl GpuGeometryCache {
    pub fn clear(&mut self) {
        self.meshes.clear();
        self.glyphs.clear();
        self.chunks.clear();
        self.arrow_outlines.clear();
        self.retained_bytes = 0;
    }

    pub fn retained_bytes(&self) -> usize {
        self.retained_bytes
    }

    pub fn item(
        &mut self,
        item: &SceneDisplayItem,
        pixels_per_unit: f64,
        clear_color: ColorRgba8,
    ) -> Result<Arc<ItemGeometry>, CanvasError> {
        let tolerance = curve_tolerance(pixels_per_unit)?;
        let mut output = ItemGeometry::default();
        match item {
            SceneDisplayItem::Rectangle(item) => {
                output.opacity = item.opacity;
                output.blend = item.blend_mode;
                self.rectangle(item, tolerance, &mut output.parts)?;
            }
            SceneDisplayItem::Arrow(item) => {
                self.arrow(item, tolerance, clear_color, &mut output)?
            }
            SceneDisplayItem::SpeechBubble(item) => {
                self.arrow(&item.path, tolerance, clear_color, &mut output)?
            }
            SceneDisplayItem::Text(item) => {
                output.opacity = item.opacity;
                self.text_item(item, pixels_per_unit, tolerance, &mut output.parts)?;
            }
            SceneDisplayItem::SerialNumber(item) => {
                output.opacity = item.opacity;
                self.serial(item, pixels_per_unit, tolerance, &mut output.parts)?;
            }
            SceneDisplayItem::SerialNumberConnector(item) => {
                output.opacity = item.opacity;
                let origin = [item.start_x, item.start_y];
                let mut builder = Path::builder();
                builder.begin(point(0.0, 0.0));
                builder.line_to(local([item.end_x, item.end_y], origin)?);
                builder.end(false);
                if item.has_baseline {
                    builder.begin(local(
                        [item.baseline_start_x, item.baseline_start_y],
                        origin,
                    )?);
                    builder.line_to(local([item.baseline_end_x, item.baseline_end_y], origin)?);
                    builder.end(false);
                }
                let path = builder.build();
                self.push(
                    &mut output.parts,
                    &path,
                    Some((item.stroke_width, StrokeStyle::Solid)),
                    item.stroke,
                    DisplayFillStyle::Solid,
                    translation(origin),
                    tolerance,
                    None,
                    false,
                )?;
            }
            SceneDisplayItem::Filter(_) | SceneDisplayItem::Magnifier(_) => {
                return Err(CanvasError::Unsupported(
                    "effect items in the ordinary geometry lowerer",
                ));
            }
            SceneDisplayItem::Image | SceneDisplayItem::Stroke => {
                return Err(CanvasError::Unsupported(
                    "scene items without rendering payloads",
                ));
            }
        }
        Ok(Arc::new(output))
    }

    pub fn rectangle_mask(
        &mut self,
        item: &RectangleDisplayItem,
        pixels_per_unit: f64,
    ) -> Result<Paint, CanvasError> {
        let path = rectangle_path(item)?;
        Ok(Paint {
            mesh: self.mesh(&path, None, curve_tolerance(pixels_per_unit)?)?,
            color: white(),
            fill_style: DisplayFillStyle::Solid,
            transform: pose(item.center_x, item.center_y, item.rotation),
            rough_style: StrokeRenderingStyle::Architect,
            rough_seed: 0,
            rough_scale: 0.0,
            is_text: false,
            color_glyph: None,
            glyph_tint: false,
            coverage_group: 0,
        })
    }

    pub fn filter_mask(
        &mut self,
        item: &FilterDisplayItem,
        pixels_per_unit: f64,
    ) -> Result<Paint, CanvasError> {
        if !item.is_pen_filter {
            return self.rectangle_mask(
                &RectangleDisplayItem {
                    center_x: item.center_x,
                    center_y: item.center_y,
                    width: item.width,
                    height: item.height,
                    rotation: item.rotation,
                    ..Default::default()
                },
                pixels_per_unit,
            );
        }
        let origin = item
            .points
            .first()
            .copied()
            .unwrap_or([item.center_x, item.center_y]);
        let mut builder = Path::builder();
        let single = item.points.len() == 1;
        if single {
            builder.add_circle(
                point(0.0, 0.0),
                item.stroke_width as f32 / 2.0,
                Winding::Positive,
            );
        } else if !item.points.is_empty() {
            builder.begin(point(0.0, 0.0));
            for point in item.points.iter().skip(1) {
                builder.line_to(local(*point, origin)?);
            }
            builder.end(false);
        }
        Ok(Paint {
            mesh: self.mesh(
                &builder.build(),
                (!single).then_some((item.stroke_width, StrokeStyle::Solid)),
                curve_tolerance(pixels_per_unit)?,
            )?,
            color: white(),
            fill_style: DisplayFillStyle::Solid,
            transform: translation(origin),
            rough_style: StrokeRenderingStyle::Architect,
            rough_seed: 0,
            rough_scale: 0.0,
            is_text: false,
            color_glyph: None,
            glyph_tint: false,
            coverage_group: 0,
        })
    }

    fn reserve(&mut self, bytes: usize) {
        if self.retained_bytes.saturating_add(bytes) > CACHE_BUDGET {
            // Cache eviction never invalidates meshes retained by a prepared frame.
            self.meshes.clear();
            self.glyphs.clear();
            self.chunks.clear();
            self.arrow_outlines.clear();
            self.retained_bytes = 0;
        }
    }

    fn mesh(
        &mut self,
        path: &Path,
        stroke: Option<(f64, StrokeStyle)>,
        tolerance: f32,
    ) -> Result<Arc<Mesh>, CanvasError> {
        let key = MeshKey {
            path: path_key(path),
            tolerance: tolerance.to_bits(),
            width: stroke.map_or(0, |(width, _)| (width as f32).to_bits()),
            style: stroke.map_or(0, |(_, style)| stroke_code(style)),
        };
        if let Some(mesh) = self.meshes.get(&key) {
            return Ok(mesh.clone());
        }
        let mesh = Arc::new(tessellate(path, stroke, tolerance)?);
        self.builds += 1;
        let bytes = mesh.retained_bytes() + key.path.len() * 4;
        if bytes <= CACHE_BUDGET {
            self.reserve(bytes);
            self.retained_bytes += bytes;
            self.meshes.insert(key, mesh.clone());
        }
        Ok(mesh)
    }

    fn chunk_mesh(
        &mut self,
        chunk: &snow_draw_engine_core::PathChunk,
        stroke: Option<(f64, StrokeStyle)>,
        tolerance: f32,
    ) -> Result<(Arc<Mesh>, [f64; 2]), CanvasError> {
        let closed = stroke.is_none();
        let origin = if closed {
            [chunk.canvas_bounds[0], chunk.canvas_bounds[1]]
        } else if let Some(PathCommand::MoveTo { point }) = chunk.commands.first() {
            *point
        } else {
            chunk.start_point
        };
        let phase = stroke.map_or(0.0, |(width, style)| {
            let period = match style {
                StrokeStyle::Solid => return 0.0,
                StrokeStyle::Dashed => width * 8.0,
                StrokeStyle::Dotted => width * 2.5,
            };
            chunk.cumulative_start_length.rem_euclid(period)
        });
        let key = ChunkKey {
            commands: chunk.commands.as_ptr() as usize,
            origin: origin.map(f64::to_bits),
            tolerance: tolerance.to_bits(),
            width: stroke.map_or(0, |(width, _)| (width as f32).to_bits()),
            style: stroke.map_or(0, |(_, style)| stroke_code(style)),
            phase: phase.to_bits(),
            closed,
        };
        if let Some(cached) = self.chunks.get(&key) {
            return Ok((cached.mesh.clone(), origin));
        }
        let initial = (!matches!(chunk.commands.first(), Some(PathCommand::MoveTo { .. })))
            .then_some(PathCommand::MoveTo {
                point: chunk.start_point,
            });
        let path = command_path(
            initial.into_iter().chain(chunk.commands.iter().copied()),
            origin,
            closed,
        )?;
        let mesh = Arc::new(tessellate_with_phase(&path, stroke, tolerance, phase)?);
        self.builds += 1;
        let bytes = mesh.retained_bytes() + std::mem::size_of_val(chunk.commands.as_ref());
        if bytes <= CACHE_BUDGET {
            self.reserve(bytes);
            self.retained_bytes += bytes;
            self.chunks.insert(
                key,
                ChunkMesh {
                    _commands: chunk.commands.clone(),
                    mesh: mesh.clone(),
                },
            );
        }
        Ok((mesh, origin))
    }

    fn arrow_chunks(
        &mut self,
        geometry: &snow_draw_engine_core::PathGeometry,
        item: &ArrowDisplayItem,
        stroke: Option<(f64, StrokeStyle)>,
        tolerance: f32,
        parts: &mut Vec<Paint>,
    ) -> Result<(), CanvasError> {
        if item.stroke.a == 0 || stroke.is_some_and(|(width, _)| width <= 0.0) {
            return Ok(());
        }
        for chunk in geometry.chunks.iter() {
            let (mesh, origin) = self.chunk_mesh(chunk, stroke, tolerance)?;
            if mesh.indices.is_empty() {
                continue;
            }
            parts.push(Paint {
                mesh,
                color: item.stroke,
                fill_style: DisplayFillStyle::Solid,
                transform: translation(origin),
                rough_style: item.stroke_rendering_style,
                rough_seed: item.stroke_rendering_seed,
                rough_scale: item.stroke_width * 10.0,
                is_text: false,
                color_glyph: None,
                glyph_tint: false,
                coverage_group: 1,
            });
        }
        Ok(())
    }

    #[allow(clippy::too_many_arguments)]
    fn push(
        &mut self,
        parts: &mut Vec<Paint>,
        path: &Path,
        stroke: Option<(f64, StrokeStyle)>,
        color: ColorRgba8,
        fill_style: DisplayFillStyle,
        transform: [f64; 6],
        tolerance: f32,
        rough: Option<(StrokeRenderingStyle, u32, f64)>,
        is_text: bool,
    ) -> Result<(), CanvasError> {
        if color.a == 0
            || fill_style == DisplayFillStyle::Border
            || stroke.is_some_and(|(width, _)| width <= 0.0)
        {
            return Ok(());
        }
        let mesh = self.mesh(path, stroke, tolerance)?;
        if mesh.indices.is_empty() {
            return Ok(());
        }
        let (rough_style, rough_seed, rough_scale) =
            rough.unwrap_or((StrokeRenderingStyle::Architect, 0, 0.0));
        parts.push(Paint {
            mesh,
            color,
            fill_style,
            transform,
            rough_style,
            rough_seed,
            rough_scale,
            is_text,
            color_glyph: None,
            glyph_tint: false,
            coverage_group: 0,
        });
        Ok(())
    }

    fn rectangle(
        &mut self,
        item: &RectangleDisplayItem,
        tolerance: f32,
        parts: &mut Vec<Paint>,
    ) -> Result<(), CanvasError> {
        let path = rectangle_path(item)?;
        let transform = pose(item.center_x, item.center_y, item.rotation);
        self.push(
            parts,
            &path,
            None,
            item.fill,
            item.fill_style,
            transform,
            tolerance,
            None,
            false,
        )?;
        self.push(
            parts,
            &path,
            Some((item.stroke_width, item.stroke_style)),
            item.stroke,
            DisplayFillStyle::Solid,
            transform,
            tolerance,
            Some((
                item.stroke_rendering_style,
                item.stroke_rendering_seed,
                item.width.min(item.height),
            )),
            false,
        )
    }

    fn arrow(
        &mut self,
        item: &ArrowDisplayItem,
        tolerance: f32,
        clear_color: ColorRgba8,
        output: &mut ItemGeometry,
    ) -> Result<(), CanvasError> {
        output.opacity = item.opacity;
        output.blend = item.blend_mode;
        output.label_gap = item
            .label_bounds
            .filter(|_| item.bound_text_id.is_some() && !item.bound_text_overlay)
            .map(|b| DrawRect::new(b.min_x - 5.0, b.min_y - 5.0, b.max_x + 5.0, b.max_y + 5.0));
        let origin = item
            .geometry
            .chunks
            .first()
            .map(|c| match c.commands.first() {
                Some(PathCommand::MoveTo { point }) => *point,
                _ => c.start_point,
            })
            .or_else(|| item.points.first().copied())
            .or_else(|| item.path_commands.first().map(command_start))
            .or_else(|| item.arrowhead_primitives.first().map(|h| h.center))
            .unwrap_or([0.0, 0.0]);
        let transform = translation(origin);
        let outlined_roughness = arrow_outline_visible(item).then_some((
            item.stroke_rendering_style,
            item.stroke_rendering_seed,
            item.stroke_width * 10.0,
        ));
        let full_path =
            || {
                let commands: Box<dyn Iterator<Item = PathCommand> + '_> =
                    if item.geometry.chunks.is_empty() {
                        if item.path_commands.is_empty() {
                            Box::new(item.points.iter().copied().enumerate().map(
                                |(index, point)| {
                                    if index == 0 {
                                        PathCommand::MoveTo { point }
                                    } else {
                                        PathCommand::LineTo { point }
                                    }
                                },
                            ))
                        } else {
                            Box::new(item.path_commands.iter().copied())
                        }
                    } else {
                        Box::new(
                            item.geometry
                                .chunks
                                .iter()
                                .flat_map(|chunk| chunk.commands.iter().copied()),
                        )
                    };
                command_path(commands, origin, item.geometry.closed)
            };
        if let Some(pressure) = &item.pressure_geometry {
            self.arrow_chunks(pressure, item, None, tolerance, &mut output.parts)?;
        } else if item.arrow_shaft_type == snow_draw_engine_core::arrow::ArrowShaftType::Tapered {
            self.push(
                &mut output.parts,
                &full_path()?,
                None,
                item.stroke,
                DisplayFillStyle::Solid,
                transform,
                tolerance,
                outlined_roughness,
                false,
            )?;
        } else {
            // Filled closed contours must retain a single winding domain. Open
            // centerline/pressure chunks have independent union coverage and can
            // retain every unchanged prefix mesh during a tail edit.
            if item.fill_style.is_visible(item.fill) {
                self.push(
                    &mut output.parts,
                    &full_path()?,
                    None,
                    item.fill,
                    item.fill_style,
                    transform,
                    tolerance,
                    None,
                    false,
                )?;
            }
            if !item.geometry.chunks.is_empty() && !item.geometry.closed {
                self.arrow_chunks(
                    &item.geometry,
                    item,
                    Some((item.stroke_width, item.stroke_style)),
                    tolerance,
                    &mut output.parts,
                )?;
            } else {
                self.push(
                    &mut output.parts,
                    &full_path()?,
                    Some((item.stroke_width, item.stroke_style)),
                    item.stroke,
                    DisplayFillStyle::Solid,
                    transform,
                    tolerance,
                    Some((
                        item.stroke_rendering_style,
                        item.stroke_rendering_seed,
                        item.stroke_width * 10.0,
                    )),
                    false,
                )?;
            }
        }
        for head in &item.arrowhead_primitives {
            output
                .parts
                .extend(self.arrow_head(item, head, origin, tolerance, clear_color)?);
        }
        if arrow_outline_visible(item) {
            let outline = self.arrow_outline(item, origin, tolerance)?;
            if !outline.indices.is_empty() {
                output.parts.insert(
                    0,
                    Paint {
                        mesh: outline,
                        color: item.arrow_outline_color,
                        fill_style: DisplayFillStyle::Solid,
                        transform,
                        rough_style: item.stroke_rendering_style,
                        rough_seed: item.stroke_rendering_seed,
                        rough_scale: item.stroke_width * 10.0,
                        is_text: false,
                        color_glyph: None,
                        glyph_tint: false,
                        coverage_group: 2,
                    },
                );
            }
        }
        Ok(())
    }

    fn arrow_head(
        &mut self,
        item: &ArrowDisplayItem,
        head: &ArrowheadDisplayPrimitive,
        origin: [f64; 2],
        tolerance: f32,
        clear_color: ColorRgba8,
    ) -> Result<Vec<Paint>, CanvasError> {
        let mut parts = Vec::new();
        let transform = translation(origin);
        let outlined_roughness = arrow_outline_visible(item).then_some((
            item.stroke_rendering_style,
            item.stroke_rendering_seed,
            item.stroke_width * 10.0,
        ));
        let path = arrowhead_path(head, origin)?;
        let fill = match head.fill_mode {
            ArrowheadDisplayFillMode::Stroke => item.stroke,
            ArrowheadDisplayFillMode::Background => clear_color,
        };
        if head.kind != ArrowheadDisplayPrimitiveKind::Line {
            self.push(
                &mut parts,
                &path,
                None,
                fill,
                DisplayFillStyle::Solid,
                transform,
                tolerance,
                outlined_roughness,
                false,
            )?;
        }
        let style = match head.dash_mode {
            ArrowheadDisplayDashMode::Inherit => item.stroke_style,
            ArrowheadDisplayDashMode::Solid => StrokeStyle::Solid,
            ArrowheadDisplayDashMode::DottedCap => StrokeStyle::Dotted,
        };
        self.push(
            &mut parts,
            &path,
            Some((item.stroke_width, style)),
            item.stroke,
            DisplayFillStyle::Solid,
            transform,
            tolerance,
            outlined_roughness,
            false,
        )?;
        Ok(parts)
    }

    fn arrow_outline(
        &mut self,
        item: &ArrowDisplayItem,
        origin: [f64; 2],
        tolerance: f32,
    ) -> Result<Arc<Mesh>, CanvasError> {
        // Geometry must survive a transparent foreground. Alpha belongs to its
        // paint, rather than determining which contours have an outline.
        let mut source = item.clone();
        source.stroke = white();
        source.fill.a = 0;
        source.arrow_outline_width = 0.0;
        source.stroke_rendering_style = StrokeRenderingStyle::Architect;
        source.arrowhead_primitives.clear();
        let mut shaft = ItemGeometry::default();
        self.arrow(&source, tolerance, ColorRgba8::default(), &mut shaft)?;
        let mut heads = Vec::new();
        for head in &item.arrowhead_primitives {
            // Use the shaft's local origin for both visible head ink and outline
            // coverage, retaining subpixel alignment even far from canvas zero.
            let parts = self.arrow_head(&source, head, origin, tolerance, ColorRgba8::default())?;
            heads.push((head, parts));
        }
        let mut key = ArrowOutlineKey {
            parts: Vec::new(),
            width: item.arrow_outline_width.to_bits(),
            tolerance: tolerance.to_bits(),
        };
        for parts in std::iter::once(&shaft.parts).chain(heads.iter().map(|(_, parts)| parts)) {
            key.parts.push(parts.len() as u64);
            for part in parts {
                key.parts.push(part.mesh.identity);
                let mut transform = part.transform;
                transform[4] -= origin[0];
                transform[5] -= origin[1];
                key.parts.extend(transform.map(f64::to_bits));
            }
        }
        for (head, _) in &heads {
            key.parts.push(head.fill_mode as u64);
        }
        if let Some(mesh) = self.arrow_outlines.get(&key) {
            return Ok(mesh.clone());
        }
        let path = arrow_outline::ink_contour(&shaft.parts, &heads, origin, tolerance)?;
        let mesh = Arc::new(tessellate(
            &path,
            Some((item.arrow_outline_width, StrokeStyle::Solid)),
            tolerance,
        )?);
        self.builds += 1;
        let bytes = mesh.retained_bytes() + key.parts.len() * std::mem::size_of::<u64>();
        if bytes <= CACHE_BUDGET {
            self.reserve(bytes);
            self.retained_bytes += bytes;
            self.arrow_outlines.insert(key, mesh.clone());
        }
        Ok(mesh)
    }

    fn serial(
        &mut self,
        item: &SerialNumberDisplayItem,
        pixels_per_unit: f64,
        tolerance: f32,
        parts: &mut Vec<Paint>,
    ) -> Result<(), CanvasError> {
        let square = matches!(
            item.serial_number_type,
            DisplaySerialNumberType::OutlinedSquare | DisplaySerialNumberType::SolidSquare
        );
        let solid = matches!(
            item.serial_number_type,
            DisplaySerialNumberType::SolidCircle | DisplaySerialNumberType::SolidSquare
        );
        self.rectangle(
            &RectangleDisplayItem {
                center_x: item.center_x,
                center_y: item.center_y,
                width: item.diameter,
                height: item.diameter,
                rotation: item.rotation,
                corner_radii: item.corner_radii,
                shape: if square {
                    DisplayRectangleShape::Rectangle
                } else {
                    DisplayRectangleShape::Ellipse
                },
                stroke: item.color,
                stroke_width: item.stroke_width,
                stroke_style: item.stroke_style,
                stroke_rendering_style: item.stroke_rendering_style,
                stroke_rendering_seed: item.stroke_rendering_seed,
                fill: if solid { item.color } else { item.fill },
                fill_style: if solid {
                    DisplayFillStyle::Solid
                } else {
                    item.fill_style
                },
                ..Default::default()
            },
            tolerance,
            parts,
        )?;
        if item.serial_number_type != DisplaySerialNumberType::Circle {
            self.text_item(
                &TextDisplayItem {
                    center_x: item.center_x,
                    center_y: item.center_y,
                    width: item.diameter,
                    height: item.diameter,
                    rotation: item.rotation,
                    text: item.label.clone(),
                    color: if solid {
                        crate::vector::serial_text_color(item.color)
                    } else {
                        item.color
                    },
                    font_size: item.font_size,
                    font_family: item.font_family.clone(),
                    horizontal_align: DisplayTextHorizontalAlign::Center,
                    ..Default::default()
                },
                pixels_per_unit,
                tolerance,
                parts,
            )?;
        }
        Ok(())
    }

    fn text_item(
        &mut self,
        item: &TextDisplayItem,
        pixels_per_unit: f64,
        tolerance: f32,
        parts: &mut Vec<Paint>,
    ) -> Result<(), CanvasError> {
        let style = TextStyle {
            color: item.color,
            font_size: item.font_size.max(1.0),
            bold: item.bold,
            italic: item.italic,
            font_family: item.font_family.clone(),
            fill: item.fill,
            fill_style: FillStyle::Solid,
            stroke: item.stroke,
            stroke_width: item.stroke_width,
            corner_radii: item.corner_radii,
            horizontal_align: TextHorizontalAlign::Left,
            vertical_align: TextVerticalAlign::Center,
            opacity: 1.0,
        };
        let layout = self.text.layout(&item.text, &style, item.width.max(1.0));
        let (pad_x, pad_y) = snow_draw_engine_document::text_fill_outset(
            &snow_draw_engine_document::TextPaintGeometry {
                center: Point::new(item.center_x, item.center_y),
                width: item.width,
                height: item.height,
                rotation: item.rotation,
                content_width: layout.width,
                content_height: layout.height,
                horizontal_align: style.horizontal_align,
                vertical_align: style.vertical_align,
                has_text: !item.text.is_empty(),
                font_size: style.font_size,
                fill: item.fill,
                fill_style: match item.fill_style {
                    DisplayFillStyle::Border => FillStyle::Border,
                    DisplayFillStyle::Solid => FillStyle::Solid,
                    DisplayFillStyle::Line => FillStyle::Line,
                    DisplayFillStyle::CrossLine => FillStyle::CrossLine,
                },
                stroke: item.stroke,
                stroke_width: item.stroke_width,
                text_ink_reserve: layout.ink_reserve,
            },
        );
        let top = match item.vertical_align {
            DisplayTextVerticalAlign::Top => -item.height / 2.0,
            DisplayTextVerticalAlign::Center => -layout.height / 2.0,
            DisplayTextVerticalAlign::Bottom => item.height / 2.0 - layout.height,
        };
        let transform = pose(item.center_x, item.center_y, item.rotation);
        for (index, line) in layout.lines.iter().enumerate() {
            let x = match item.horizontal_align {
                DisplayTextHorizontalAlign::Left => -item.width / 2.0,
                DisplayTextHorizontalAlign::Center => -line.width / 2.0,
                DisplayTextHorizontalAlign::Right => item.width / 2.0 - line.width,
            };
            let y = top + index as f64 * layout.line_height;
            if item.fill_style.is_visible(item.fill) {
                let path = rectangle_path(&RectangleDisplayItem {
                    width: line.width + 2.0 * pad_x,
                    height: layout.line_height + 2.0 * pad_y,
                    corner_radii: item.corner_radii,
                    ..Default::default()
                })?;
                self.push(
                    parts,
                    &path,
                    None,
                    item.fill,
                    item.fill_style,
                    compose(
                        transform,
                        translation([x + line.width / 2.0, y + layout.line_height / 2.0]),
                    ),
                    tolerance,
                    None,
                    false,
                )?;
            }
            // Stroke the complete line before filling it, including overlapping marks.
            for (is_stroke, color, width) in [
                (true, item.stroke, item.stroke_width),
                (false, item.color, 0.0),
            ] {
                if color.a == 0 || (is_stroke && width <= 0.0) {
                    continue;
                }
                for glyph in line.glyphs.iter() {
                    let color_glyph = (!is_stroke)
                        .then(|| self.text.color_glyph(glyph.font, glyph.id))
                        .flatten();
                    let glyph_tolerance = curve_tolerance(pixels_per_unit * glyph.scale)?;
                    let key = GlyphKey {
                        font: glyph.font,
                        id: glyph.id,
                        tolerance: if color_glyph.is_some() {
                            0
                        } else {
                            glyph_tolerance.to_bits()
                        },
                        width: ((width / glyph.scale) as f32).to_bits(),
                    };
                    let mesh = if let Some(mesh) = self.glyphs.get(&key) {
                        mesh.clone()
                    } else {
                        let mesh = if let Some(color) = &color_glyph {
                            Arc::new(color_glyph_mesh(color)?)
                        } else {
                            let Some(path) = self.text.glyph_path(glyph.font, glyph.id) else {
                                continue;
                            };
                            Arc::new(tessellate(
                                &path,
                                (width > 0.0).then_some((width / glyph.scale, StrokeStyle::Solid)),
                                glyph_tolerance,
                            )?)
                        };
                        let bytes = mesh.retained_bytes();
                        self.builds += 1;
                        if bytes <= CACHE_BUDGET {
                            self.reserve(bytes);
                            self.retained_bytes += bytes;
                            self.glyphs.insert(key, mesh.clone());
                        }
                        mesh
                    };
                    if mesh.indices.is_empty() {
                        continue;
                    }
                    parts.push(Paint {
                        mesh,
                        color,
                        fill_style: DisplayFillStyle::Solid,
                        transform: compose(
                            transform,
                            [
                                glyph.scale,
                                0.0,
                                0.0,
                                -glyph.scale,
                                x + glyph.x,
                                y + style.font_size + glyph.y,
                            ],
                        ),
                        rough_style: StrokeRenderingStyle::Architect,
                        rough_seed: 0,
                        rough_scale: 0.0,
                        is_text: true,
                        coverage_group: if color_glyph.is_some() {
                            0
                        } else {
                            index as u32 * 2 + if is_stroke { 1 } else { 2 }
                        },
                        color_glyph,
                        glyph_tint: false,
                    });
                }
            }
        }
        Ok(())
    }
}

fn white() -> ColorRgba8 {
    ColorRgba8 {
        r: 255,
        g: 255,
        b: 255,
        a: 255,
    }
}

fn arrowhead_path(head: &ArrowheadDisplayPrimitive, origin: [f64; 2]) -> Result<Path, CanvasError> {
    let mut path = Path::builder();
    if head.kind == ArrowheadDisplayPrimitiveKind::Circle {
        path.add_circle(
            local(head.center, origin)?,
            head.diameter as f32 / 2.0,
            Winding::Positive,
        );
    } else if let Some(first) = head.points.first() {
        path.begin(local(*first, origin)?);
        for p in head.points.iter().skip(1) {
            path.line_to(local(*p, origin)?);
        }
        path.end(head.kind == ArrowheadDisplayPrimitiveKind::Polygon);
    }
    Ok(path.build())
}

pub(crate) fn arrow_outline_visible(item: &ArrowDisplayItem) -> bool {
    !item.is_free_draw
        && item.stroke_width > 0.0
        && item.arrow_outline_width > 0.0
        && item.arrow_outline_color.a != 0
}

pub(crate) fn color_glyph_mesh(glyph: &crate::vector::ColorGlyph) -> Result<Mesh, CanvasError> {
    let [left, bottom, right, top] = glyph.bounds.map(|value| value as f32);
    Mesh::from_vertices(
        vec![[left, bottom], [right, bottom], [right, top], [left, top]],
        vec![0, 1, 2, 0, 2, 3],
    )
}
fn translation(origin: [f64; 2]) -> [f64; 6] {
    [1.0, 0.0, 0.0, 1.0, origin[0], origin[1]]
}
fn pose(x: f64, y: f64, rotation: f64) -> [f64; 6] {
    let (sin, cos) = rotation.sin_cos();
    [cos, sin, -sin, cos, x, y]
}
pub(crate) fn compose(a: [f64; 6], b: [f64; 6]) -> [f64; 6] {
    [
        a[0] * b[0] + a[2] * b[1],
        a[1] * b[0] + a[3] * b[1],
        a[0] * b[2] + a[2] * b[3],
        a[1] * b[2] + a[3] * b[3],
        a[0] * b[4] + a[2] * b[5] + a[4],
        a[1] * b[4] + a[3] * b[5] + a[5],
    ]
}
fn curve_tolerance(pixels_per_unit: f64) -> Result<f32, CanvasError> {
    if !pixels_per_unit.is_finite() || pixels_per_unit <= 0.0 {
        return Err(ErrorCode::InvalidArgument.into());
    }
    let tolerance = (0.25 / 2.0_f64.powf(pixels_per_unit.log2().ceil())) as f32;
    if !tolerance.is_finite() || tolerance <= 0.0 {
        return Err(CanvasError::ResourceLimit);
    }
    Ok(tolerance)
}
fn local(value: [f64; 2], origin: [f64; 2]) -> Result<lyon::math::Point, CanvasError> {
    let x = (value[0] - origin[0]) as f32;
    let y = (value[1] - origin[1]) as f32;
    if !x.is_finite() || !y.is_finite() {
        return Err(ErrorCode::InvalidArgument.into());
    }
    Ok(point(x, y))
}
fn command_start(command: &PathCommand) -> [f64; 2] {
    match command {
        PathCommand::MoveTo { point } | PathCommand::LineTo { point } => *point,
        PathCommand::QuadTo { end, .. } | PathCommand::CubicTo { end, .. } => *end,
    }
}
fn command_path(
    commands: impl IntoIterator<Item = PathCommand>,
    origin: [f64; 2],
    closed: bool,
) -> Result<Path, CanvasError> {
    let mut path = Path::builder();
    let mut open = false;
    for command in commands {
        match command {
            PathCommand::MoveTo { point } => {
                if open {
                    path.end(false);
                }
                path.begin(local(point, origin)?);
                open = true;
            }
            PathCommand::LineTo { point } => {
                if open {
                    path.line_to(local(point, origin)?);
                } else {
                    path.begin(local(point, origin)?);
                    open = true;
                }
            }
            PathCommand::QuadTo { control, end } => {
                if !open {
                    path.begin(local(end, origin)?);
                    open = true;
                } else {
                    path.quadratic_bezier_to(local(control, origin)?, local(end, origin)?);
                }
            }
            PathCommand::CubicTo {
                control_1,
                control_2,
                end,
            } => {
                if !open {
                    path.begin(local(end, origin)?);
                    open = true;
                } else {
                    path.cubic_bezier_to(
                        local(control_1, origin)?,
                        local(control_2, origin)?,
                        local(end, origin)?,
                    );
                }
            }
        }
    }
    if open {
        path.end(closed);
    }
    Ok(path.build())
}
fn rectangle_path(item: &RectangleDisplayItem) -> Result<Path, CanvasError> {
    if !item.width.is_finite() || !item.height.is_finite() || item.width < 0.0 || item.height < 0.0
    {
        return Err(ErrorCode::InvalidArgument.into());
    }
    if item.width == 0.0 || item.height == 0.0 {
        return Ok(Path::builder().build());
    }
    let w = item.width as f32 / 2.0;
    let h = item.height as f32 / 2.0;
    let mut path = Path::builder();
    match item.shape {
        DisplayRectangleShape::Ellipse => path.add_ellipse(
            point(0.0, 0.0),
            vector(w, h),
            Angle::zero(),
            Winding::Positive,
        ),
        DisplayRectangleShape::Diamond => {
            return command_path(
                snow_draw_engine_document::RoundedDiamond::new(
                    item.width,
                    item.height,
                    item.corner_radii,
                )
                .path_commands(Point::new(0.0, 0.0), 0.0),
                [0.0, 0.0],
                true,
            );
        }
        DisplayRectangleShape::Rectangle => {
            let radii = item.corner_radii;
            let [tl, tr, br, bl] = [
                radii.top_left,
                radii.top_right,
                radii.bottom_right,
                radii.bottom_left,
            ]
            .map(|r| (r as f32).clamp(0.0, w.min(h)));
            path.begin(point(-w + tl, -h));
            path.line_to(point(w - tr, -h));
            path.quadratic_bezier_to(point(w, -h), point(w, -h + tr));
            path.line_to(point(w, h - br));
            path.quadratic_bezier_to(point(w, h), point(w - br, h));
            path.line_to(point(-w + bl, h));
            path.quadratic_bezier_to(point(-w, h), point(-w, h - bl));
            path.line_to(point(-w, -h + tl));
            path.quadratic_bezier_to(point(-w, -h), point(-w + tl, -h));
            path.end(true);
        }
    }
    Ok(path.build())
}
fn stroke_code(style: StrokeStyle) -> u8 {
    match style {
        StrokeStyle::Solid => 1,
        StrokeStyle::Dashed => 2,
        StrokeStyle::Dotted => 3,
    }
}
fn path_key(path: &Path) -> Vec<u32> {
    let mut key = Vec::new();
    let mut p = |tag, points: &[lyon::math::Point]| {
        key.push(tag);
        for point in points {
            key.extend([point.x.to_bits(), point.y.to_bits()]);
        }
    };
    for event in path.iter() {
        match event {
            Event::Begin { at } => p(0, &[at]),
            Event::Line { to, .. } => p(1, &[to]),
            Event::Quadratic { ctrl, to, .. } => p(2, &[ctrl, to]),
            Event::Cubic {
                ctrl1, ctrl2, to, ..
            } => p(3, &[ctrl1, ctrl2, to]),
            Event::End { close, .. } => p(if close { 5 } else { 4 }, &[]),
        }
    }
    key
}
pub(crate) fn glyph_mesh(
    metrics: &VectorContext,
    glyph: &crate::vector::ShapedGlyph,
    width: f64,
    pixels_per_unit: f64,
) -> Result<Option<Mesh>, CanvasError> {
    let Some(path) = metrics.glyph_path(glyph.font, glyph.id) else {
        return Ok(None);
    };
    tessellate(
        &path,
        (width > 0.0).then_some((width / glyph.scale, StrokeStyle::Solid)),
        curve_tolerance(pixels_per_unit * glyph.scale)?,
    )
    .map(Some)
}

fn tessellate(
    path: &Path,
    stroke: Option<(f64, StrokeStyle)>,
    tolerance: f32,
) -> Result<Mesh, CanvasError> {
    tessellate_with_phase(path, stroke, tolerance, 0.0)
}

fn tessellate_with_phase(
    path: &Path,
    stroke: Option<(f64, StrokeStyle)>,
    tolerance: f32,
    phase: f64,
) -> Result<Mesh, CanvasError> {
    // Flatten incrementally before Lyon constructs its sweep queue, bounding
    // pathological large curves at a very small device-space tolerance.
    let path = bounded_flatten(path, tolerance)?;
    let path = &path;
    let mut buffers: VertexBuffers<[f32; 2], u32> = VertexBuffers::new();
    if let Some((width, style)) = stroke {
        if !width.is_finite()
            || !((width as f32).is_finite())
            || width <= 0.0
            || width as f32 <= 0.0
        {
            return Err(ErrorCode::InvalidArgument.into());
        }
        let mut dashed = None;
        if style != StrokeStyle::Solid && width > 0.0 {
            let measured = PathMeasurements::from_path(path, tolerance.min(0.1));
            let mut sampler = measured.create_sampler(path, SampleType::Distance);
            let total = sampler.length();
            let spacing = width as f32
                * if style == StrokeStyle::Dotted {
                    2.5
                } else {
                    8.0
                };
            if total / spacing > MAX_MESH_VERTICES as f32 / 8.0 {
                return Err(CanvasError::ResourceLimit);
            }
            let mut builder = Path::builder();
            let mut position = -(phase as f32).rem_euclid(spacing);
            while position < total {
                if style == StrokeStyle::Dotted {
                    if position >= 0.0 {
                        builder.add_circle(
                            sampler.sample(position).position(),
                            width as f32 / 2.0,
                            Winding::Positive,
                        );
                    }
                } else {
                    let start = position.max(0.0);
                    let end = (position + width as f32 * 5.0).min(total);
                    if end > start {
                        sampler.split_range(start..end, &mut builder);
                    }
                }
                position += spacing;
            }
            dashed = Some(builder.build());
        }
        let path = dashed.as_ref().unwrap_or(path);
        if style == StrokeStyle::Dotted {
            FillTessellator::new()
                .tessellate_path(
                    path,
                    &FillOptions::non_zero().with_tolerance(tolerance),
                    &mut BoundedGeometry(&mut buffers, false),
                )
                .map_err(|_| CanvasError::Unsupported("tessellating invalid fill geometry"))?;
        } else {
            StrokeTessellator::new()
                .tessellate_path(
                    path,
                    &StrokeOptions::default()
                        .with_tolerance(tolerance)
                        .with_line_width(width as f32)
                        .with_line_cap(LineCap::Round)
                        .with_line_join(LineJoin::Round),
                    &mut BoundedGeometry(&mut buffers, false),
                )
                .map_err(|_| CanvasError::Unsupported("tessellating invalid stroke geometry"))?;
        }
    } else {
        FillTessellator::new()
            .tessellate_path(
                path,
                &FillOptions::non_zero().with_tolerance(tolerance),
                &mut BoundedGeometry(&mut buffers, false),
            )
            .map_err(|_| CanvasError::Unsupported("tessellating invalid fill geometry"))?;
    }
    Mesh::from_vertices(buffers.vertices, buffers.indices)
}

fn bounded_flatten(path: &Path, tolerance: f32) -> Result<Path, CanvasError> {
    let mut builder = Path::builder();
    for (count, event) in path.iter().flattened(tolerance).enumerate() {
        if count >= MAX_MESH_VERTICES / 2 {
            return Err(CanvasError::ResourceLimit);
        }
        match event {
            Event::Begin { at } => {
                builder.begin(at);
            }
            Event::Line { to, .. } => {
                builder.line_to(to);
            }
            Event::End { close, .. } => builder.end(close),
            _ => unreachable!("flattening only emits lines"),
        }
    }
    Ok(builder.build())
}

struct BoundedGeometry<'a>(&'a mut VertexBuffers<[f32; 2], u32>, bool);
impl BoundedGeometry<'_> {
    fn vertex(&mut self, point: lyon::math::Point) -> Result<VertexId, GeometryBuilderError> {
        if self.1 || self.0.vertices.len() >= MAX_MESH_VERTICES {
            return Err(GeometryBuilderError::TooManyVertices);
        }
        if !point.x.is_finite() || !point.y.is_finite() {
            return Err(GeometryBuilderError::InvalidVertex);
        }
        let id = VertexId::from_usize(self.0.vertices.len());
        self.0.vertices.push(point.to_array());
        Ok(id)
    }
}
impl GeometryBuilder for BoundedGeometry<'_> {
    fn add_triangle(&mut self, a: VertexId, b: VertexId, c: VertexId) {
        if self.0.indices.len() >= MAX_MESH_VERTICES * 6 {
            self.1 = true;
            return;
        }
        self.0.indices.extend([a.0, b.0, c.0]);
    }
}
impl FillGeometryBuilder for BoundedGeometry<'_> {
    fn add_fill_vertex(&mut self, vertex: FillVertex) -> Result<VertexId, GeometryBuilderError> {
        self.vertex(vertex.position())
    }
}
impl StrokeGeometryBuilder for BoundedGeometry<'_> {
    fn add_stroke_vertex(
        &mut self,
        vertex: StrokeVertex,
    ) -> Result<VertexId, GeometryBuilderError> {
        self.vertex(vertex.position())
    }
}

#[cfg(test)]
#[path = "gpu_geometry_tests.rs"]
mod tests;
