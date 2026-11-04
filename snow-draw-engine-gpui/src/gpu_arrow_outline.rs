//! Resolve the boundary of actual arrow ink before stroking its outline. Fill
//! triangles describe coverage, not a manifold: overlapping/near-coincident
//! triangles must be unioned geometrically, never joined by cancelling mesh edges.
use super::*;
use i_overlay::{
    core::{fill_rule::FillRule, overlay_rule::OverlayRule},
    float::overlay::FloatOverlay,
};

type Contour = Vec<[f64; 2]>;

// Bound both the accumulated input and each Boolean result. Keep coordinates in
// f64 until the final Lyon path; the i64 overlay engine resolves intersections
// without depending on f32 triangle adjacency or a canvas-space snapping epsilon.
#[derive(Default)]
struct Ink {
    contours: Vec<Contour>,
    vertices: usize,
}

impl Ink {
    fn push(&mut self, contour: Contour) -> Result<(), CanvasError> {
        if contour.len() < 3 {
            return Ok(());
        }
        if contour.len() >= MAX_MESH_VERTICES - self.vertices {
            return Err(CanvasError::ResourceLimit);
        }
        self.vertices += contour.len();
        self.contours.push(contour);
        Ok(())
    }

    fn add_coverage(&mut self, parts: &[Paint], origin: [f64; 2]) -> Result<(), CanvasError> {
        for part in parts {
            let [xx, yx, xy, yy, tx, ty] = part.transform;
            for triangle in part.mesh.indices.as_chunks::<3>().0 {
                let mut points = triangle.map(|index| {
                    let [x, y] = part.mesh.vertices[index as usize].map(f64::from);
                    [
                        xx * x + xy * y + (tx - origin[0]),
                        yx * x + yy * y + (ty - origin[1]),
                    ]
                });
                if points
                    .iter()
                    .flatten()
                    .any(|value| !(*value as f32).is_finite())
                {
                    return Err(ErrorCode::InvalidArgument.into());
                }
                let [a, b, c] = points;
                let area = (b[0] - a[0]) * (c[1] - a[1]) - (b[1] - a[1]) * (c[0] - a[0]);
                if area == 0.0 {
                    continue;
                }
                // NonZero unions all triangles, including overlapping paints.
                // This also matches the overlay's CCW outer / CW hole output.
                if area < 0.0 {
                    points.swap(1, 2);
                }
                self.push(points.to_vec())?;
            }
        }
        Ok(())
    }

    fn resolve(&mut self, clip: &[Contour], rule: OverlayRule) -> Result<(), CanvasError> {
        let clip_vertices = clip.iter().try_fold(0usize, |total, contour| {
            total
                .checked_add(contour.len())
                .ok_or(CanvasError::ResourceLimit)
        })?;
        if clip_vertices >= MAX_MESH_VERTICES - self.vertices {
            return Err(CanvasError::ResourceLimit);
        }
        if self.contours.is_empty() {
            return Ok(());
        }
        let shapes = FloatOverlay::<[f64; 2], i64>::from_subj_and_clip(&self.contours, clip)
            .overlay(rule, FillRule::NonZero);
        self.contours.clear();
        self.vertices = 0;
        for contour in shapes.into_iter().flatten() {
            self.push(contour)?;
        }
        Ok(())
    }

    fn into_path(self) -> Result<Path, CanvasError> {
        let mut builder = Path::builder();
        for contour in self.contours {
            builder.begin(local(contour[0], [0.0; 2])?);
            for p in contour.iter().skip(1) {
                builder.line_to(local(*p, [0.0; 2])?);
            }
            builder.end(true);
        }
        Ok(builder.build())
    }
}

pub(super) fn ink_contour(
    shaft: &[Paint],
    heads: &[(&ArrowheadDisplayPrimitive, Vec<Paint>)],
    origin: [f64; 2],
    tolerance: f32,
) -> Result<Path, CanvasError> {
    let mut ink = Ink::default();
    ink.add_coverage(shaft, origin)?;
    for (head, parts) in heads {
        if head.fill_mode == ArrowheadDisplayFillMode::Background {
            let path = bounded_flatten(&arrowhead_path(head, origin)?, tolerance)?;
            let hole: Contour = path
                .iter()
                .filter_map(|event| match event {
                    Event::Begin { at } => Some(at.to_array().map(f64::from)),
                    Event::Line { to, .. } => Some(to.to_array().map(f64::from)),
                    _ => None,
                })
                .collect();
            // Preserve paint order: a hollow head erases earlier ink, and its
            // border and subsequent heads add ink back. Difference handles holes
            // extending beyond the shaft without manufacturing negative coverage.
            ink.resolve(&[hole], OverlayRule::Difference)?;
        }
        ink.add_coverage(parts, origin)?;
    }
    // Solid heads share one union, avoiding repeated tessellation and reconstruction.
    ink.resolve(&[], OverlayRule::Subject)?;
    ink.into_path()
}

#[cfg(test)]
#[path = "gpu_arrow_outline_tests.rs"]
mod tests;
