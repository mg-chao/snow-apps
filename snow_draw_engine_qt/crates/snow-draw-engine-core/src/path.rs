use std::sync::Arc;

use serde::{Deserialize, Serialize};

use crate::{DrawRect, Point};

pub const PATH_CHUNK_COMMAND_CAPACITY: usize = 64;

#[derive(Clone, Copy, Debug, Default, PartialEq, Eq, Serialize, Deserialize)]
#[serde(rename_all = "lowercase")]
pub enum PathSegmentMode {
    #[default]
    Curve,
    Straight,
}

#[derive(Clone, Copy, Debug, PartialEq, Serialize, Deserialize)]
#[serde(tag = "type", rename_all = "camelCase")]
pub enum PathCommand {
    MoveTo {
        point: [f64; 2],
    },
    LineTo {
        point: [f64; 2],
    },
    QuadTo {
        control: [f64; 2],
        end: [f64; 2],
    },
    CubicTo {
        control_1: [f64; 2],
        control_2: [f64; 2],
        end: [f64; 2],
    },
}

#[derive(Clone, Debug, PartialEq)]
pub struct PathChunk {
    pub stable_id: u64,
    pub command_start: u32,
    pub commands: Arc<[PathCommand]>,
    pub start_point: [f64; 2],
    pub canvas_bounds: [f64; 4],
    pub cumulative_start_length: f64,
    pub cumulative_end_length: f64,
    pub cumulative_bounds: [f64; 4],
}

#[derive(Clone, Debug, PartialEq)]
pub struct PathGeometry {
    pub revision: u64,
    pub chunks: Arc<[PathChunk]>,
    pub canvas_bounds: [f64; 4],
    pub closed: bool,
}

impl Default for PathGeometry {
    fn default() -> Self {
        Self {
            revision: 0,
            chunks: Arc::default(),
            canvas_bounds: [0.0; 4],
            closed: false,
        }
    }
}

impl PathGeometry {
    /// Contour chunks contain complete subpaths and may have different command counts.
    /// Their lengths describe the underlying centerline, preserving dash phase at tail edits.
    pub fn replace_contour_tail(
        previous: Option<&Self>,
        revision: u64,
        retain_chunks: usize,
        tail: Vec<(Vec<PathCommand>, f64, f64)>,
    ) -> Self {
        let mut chunks = previous
            .map(|geometry| geometry.chunks[..retain_chunks.min(geometry.chunks.len())].to_vec())
            .unwrap_or_default();
        let mut command_start = chunks
            .last()
            .map_or(0, |chunk| chunk.command_start + chunk.commands.len() as u32);
        let mut bounds = chunks.last().map(|chunk| {
            DrawRect::new(
                chunk.cumulative_bounds[0],
                chunk.cumulative_bounds[1],
                chunk.cumulative_bounds[2],
                chunk.cumulative_bounds[3],
            )
        });
        for (commands, start_length, end_length) in tail {
            let mut current = None;
            let mut local_bounds = None;
            for command in &commands {
                let (command_bounds, next, _) = command_metrics(*command, current);
                if let Some(command_bounds) = command_bounds {
                    local_bounds = Some(union_optional(local_bounds, command_bounds));
                    bounds = Some(union_optional(bounds, command_bounds));
                }
                current = next;
            }
            chunks.push(PathChunk {
                stable_id: stable_chunk_id(command_start, &commands),
                command_start,
                commands: Arc::from(commands.as_slice()),
                start_point: [0.0; 2],
                canvas_bounds: bounds_array(local_bounds),
                cumulative_start_length: start_length,
                cumulative_end_length: end_length,
                cumulative_bounds: bounds_array(bounds),
            });
            command_start += commands.len() as u32;
        }
        Self {
            revision,
            chunks: chunks.into(),
            canvas_bounds: bounds_array(bounds),
            closed: false,
        }
    }

    pub fn from_commands(revision: u64, commands: Vec<PathCommand>, closed: bool) -> Self {
        Self::build_tail(revision, None, 0, [0.0; 2], commands, closed)
    }

    pub fn replace_tail(
        &self,
        revision: u64,
        command_start: u32,
        start_point: [f64; 2],
        commands: Vec<PathCommand>,
        closed: bool,
    ) -> Self {
        Self::build_tail(
            revision,
            Some(self),
            command_start,
            start_point,
            commands,
            closed,
        )
    }

    fn build_tail(
        revision: u64,
        previous: Option<&Self>,
        command_start: u32,
        start_point: [f64; 2],
        commands: Vec<PathCommand>,
        closed: bool,
    ) -> Self {
        debug_assert_eq!(command_start as usize % PATH_CHUNK_COMMAND_CAPACITY, 0);
        let prefix_count = previous.map_or(0, |previous| {
            (command_start as usize / PATH_CHUNK_COMMAND_CAPACITY).min(previous.chunks.len())
        });
        if commands.is_empty() {
            if let Some(previous) = previous {
                let chunks = previous.chunks[..prefix_count].to_vec();
                return Self {
                    revision,
                    canvas_bounds: chunks
                        .last()
                        .map_or([0.0; 4], |chunk| chunk.cumulative_bounds),
                    chunks: chunks.into(),
                    closed,
                };
            }
            return Self {
                revision,
                closed,
                ..Self::default()
            };
        }
        let mut chunks = previous
            .map(|previous| previous.chunks[..prefix_count].to_vec())
            .unwrap_or_default();
        chunks.reserve(commands.len().div_ceil(PATH_CHUNK_COMMAND_CAPACITY));
        let mut cumulative_length = chunks
            .last()
            .map_or(0.0, |chunk| chunk.cumulative_end_length);
        let mut geometry_bounds = chunks.last().map(|chunk| {
            DrawRect::new(
                chunk.cumulative_bounds[0],
                chunk.cumulative_bounds[1],
                chunk.cumulative_bounds[2],
                chunk.cumulative_bounds[3],
            )
        });
        let mut current = (command_start != 0).then(|| array_point(start_point));
        for (chunk_index, command_slice) in commands.chunks(PATH_CHUNK_COMMAND_CAPACITY).enumerate()
        {
            let chunk_start_length = cumulative_length;
            let chunk_start_point = current.map_or([0.0, 0.0], point_array);
            let mut chunk_bounds = None;
            for command in command_slice {
                let (bounds, next, length) = command_metrics(*command, current);
                if let Some(bounds) = bounds {
                    chunk_bounds = Some(union_optional(chunk_bounds, bounds));
                    geometry_bounds = Some(union_optional(geometry_bounds, bounds));
                }
                current = next;
                cumulative_length += length;
            }
            let chunk_command_start =
                command_start + (chunk_index * PATH_CHUNK_COMMAND_CAPACITY) as u32;
            chunks.push(PathChunk {
                stable_id: stable_chunk_id(chunk_command_start, command_slice),
                command_start: chunk_command_start,
                commands: Arc::from(command_slice),
                start_point: chunk_start_point,
                canvas_bounds: bounds_array(chunk_bounds),
                cumulative_start_length: chunk_start_length,
                cumulative_end_length: cumulative_length,
                cumulative_bounds: bounds_array(geometry_bounds),
            });
        }
        Self {
            revision,
            chunks: chunks.into(),
            canvas_bounds: bounds_array(geometry_bounds),
            closed,
        }
    }

    pub fn flattened_commands(&self) -> Vec<PathCommand> {
        self.chunks
            .iter()
            .flat_map(|chunk| chunk.commands.iter().copied())
            .collect()
    }
}

pub fn catmull_rom_path_commands(
    vertices: &[Point<f64>],
    segment_modes: &[PathSegmentMode],
    closed: bool,
) -> Vec<PathCommand> {
    if vertices.is_empty() {
        return Vec::new();
    }
    let edge_count = if closed {
        vertices.len()
    } else {
        vertices.len().saturating_sub(1)
    };
    let mut commands = Vec::with_capacity(edge_count + 1);
    commands.push(PathCommand::MoveTo {
        point: point_array(vertices[0]),
    });
    for edge in 0..edge_count {
        let start = vertices[edge];
        let end = vertices[(edge + 1) % vertices.len()];
        if segment_modes.get(edge).copied().unwrap_or_default() == PathSegmentMode::Straight {
            commands.push(PathCommand::LineTo {
                point: point_array(end),
            });
            continue;
        }
        let previous = neighbor_before(vertices, edge, closed);
        let next = neighbor_after(vertices, edge, closed);
        if let Some((control_1, control_2)) = centripetal_controls(previous, start, end, next) {
            commands.push(PathCommand::CubicTo {
                control_1: point_array(control_1),
                control_2: point_array(control_2),
                end: point_array(end),
            });
        } else {
            commands.push(PathCommand::LineTo {
                point: point_array(end),
            });
        }
    }
    commands
}

fn point_array(point: Point<f64>) -> [f64; 2] {
    [point.x, point.y]
}
fn array_point(point: [f64; 2]) -> Point<f64> {
    Point::new(point[0], point[1])
}

fn neighbor_before(vertices: &[Point<f64>], edge: usize, closed: bool) -> Point<f64> {
    if edge > 0 {
        vertices[edge - 1]
    } else if closed {
        vertices[vertices.len() - 1]
    } else {
        reflect(vertices[0], vertices.get(1).copied().unwrap_or(vertices[0]))
    }
}

fn neighbor_after(vertices: &[Point<f64>], edge: usize, closed: bool) -> Point<f64> {
    let end_index = (edge + 1) % vertices.len();
    if edge + 2 < vertices.len() {
        vertices[edge + 2]
    } else if closed {
        vertices[(end_index + 1) % vertices.len()]
    } else {
        reflect(vertices[end_index], vertices[edge])
    }
}

fn reflect(anchor: Point<f64>, neighbor: Point<f64>) -> Point<f64> {
    Point::new(anchor.x * 2.0 - neighbor.x, anchor.y * 2.0 - neighbor.y)
}

fn centripetal_controls(
    p0: Point<f64>,
    p1: Point<f64>,
    p2: Point<f64>,
    p3: Point<f64>,
) -> Option<(Point<f64>, Point<f64>)> {
    let t0 = 0.0;
    let t1 = t0 + chord_parameter(p0, p1);
    let t2 = t1 + chord_parameter(p1, p2);
    let t3 = t2 + chord_parameter(p2, p3);
    if t1 <= t0 || t2 <= t1 || t3 <= t2 {
        return None;
    }
    let segment = t2 - t1;
    let tangent_1 = weighted_tangent(p0, p1, p2, t0, t1, t2)?;
    let tangent_2 = weighted_tangent(p1, p2, p3, t1, t2, t3)?;
    Some((
        Point::new(
            p1.x + tangent_1.x * segment / 3.0,
            p1.y + tangent_1.y * segment / 3.0,
        ),
        Point::new(
            p2.x - tangent_2.x * segment / 3.0,
            p2.y - tangent_2.y * segment / 3.0,
        ),
    ))
}

fn chord_parameter(a: Point<f64>, b: Point<f64>) -> f64 {
    ((b.x - a.x).hypot(b.y - a.y)).sqrt()
}

fn weighted_tangent(
    p0: Point<f64>,
    p1: Point<f64>,
    p2: Point<f64>,
    t0: f64,
    t1: f64,
    t2: f64,
) -> Option<Point<f64>> {
    let d10 = t1 - t0;
    let d21 = t2 - t1;
    let d20 = t2 - t0;
    if d10 <= 0.0 || d21 <= 0.0 || d20 <= 0.0 {
        return None;
    }
    Some(Point::new(
        (p1.x - p0.x) / d10 - (p2.x - p0.x) / d20 + (p2.x - p1.x) / d21,
        (p1.y - p0.y) / d10 - (p2.y - p0.y) / d20 + (p2.y - p1.y) / d21,
    ))
}

fn command_metrics(
    command: PathCommand,
    current: Option<Point<f64>>,
) -> (Option<DrawRect>, Option<Point<f64>>, f64) {
    match command {
        PathCommand::MoveTo { point } => (None, Some(array_point(point)), 0.0),
        PathCommand::LineTo { point } => {
            let end = array_point(point);
            let Some(start) = current else {
                return (None, Some(end), 0.0);
            };
            (
                Some(line_bounds(start, end)),
                Some(end),
                distance(start, end),
            )
        }
        PathCommand::QuadTo { control, end } => {
            let end = array_point(end);
            let Some(start) = current else {
                return (None, Some(end), 0.0);
            };
            let control = array_point(control);
            let c1 = Point::new(
                start.x + (control.x - start.x) * 2.0 / 3.0,
                start.y + (control.y - start.y) * 2.0 / 3.0,
            );
            let c2 = Point::new(
                end.x + (control.x - end.x) * 2.0 / 3.0,
                end.y + (control.y - end.y) * 2.0 / 3.0,
            );
            (
                Some(cubic_bounds(start, c1, c2, end)),
                Some(end),
                cubic_length(start, c1, c2, end),
            )
        }
        PathCommand::CubicTo {
            control_1,
            control_2,
            end,
        } => {
            let end = array_point(end);
            let Some(start) = current else {
                return (None, Some(end), 0.0);
            };
            let c1 = array_point(control_1);
            let c2 = array_point(control_2);
            (
                Some(cubic_bounds(start, c1, c2, end)),
                Some(end),
                cubic_length(start, c1, c2, end),
            )
        }
    }
}

fn line_bounds(a: Point<f64>, b: Point<f64>) -> DrawRect {
    DrawRect::new(a.x.min(b.x), a.y.min(b.y), a.x.max(b.x), a.y.max(b.y))
}

/// Tight axis-aligned bounds, including all interior derivative extrema.
pub fn cubic_bounds(p0: Point<f64>, p1: Point<f64>, p2: Point<f64>, p3: Point<f64>) -> DrawRect {
    let mut min_x = p0.x.min(p3.x);
    let mut min_y = p0.y.min(p3.y);
    let mut max_x = p0.x.max(p3.x);
    let mut max_y = p0.y.max(p3.y);
    for t in cubic_extrema(p0.x, p1.x, p2.x, p3.x) {
        let value = cubic_value(p0.x, p1.x, p2.x, p3.x, t);
        min_x = min_x.min(value);
        max_x = max_x.max(value);
    }
    for t in cubic_extrema(p0.y, p1.y, p2.y, p3.y) {
        let value = cubic_value(p0.y, p1.y, p2.y, p3.y, t);
        min_y = min_y.min(value);
        max_y = max_y.max(value);
    }
    DrawRect::new(min_x, min_y, max_x, max_y)
}

fn cubic_extrema(p0: f64, p1: f64, p2: f64, p3: f64) -> impl Iterator<Item = f64> {
    let a = -p0 + 3.0 * p1 - 3.0 * p2 + p3;
    let b = 2.0 * (p0 - 2.0 * p1 + p2);
    let c = p1 - p0;
    let roots = if a.abs() <= 1e-12 {
        [if b.abs() <= 1e-12 { None } else { Some(-c / b) }, None]
    } else {
        let discriminant = b * b - 4.0 * a * c;
        if discriminant < 0.0 {
            [None, None]
        } else {
            let root = discriminant.sqrt();
            [Some((-b + root) / (2.0 * a)), Some((-b - root) / (2.0 * a))]
        }
    };
    roots.into_iter().flatten().filter(|t| *t > 0.0 && *t < 1.0)
}

fn cubic_value(p0: f64, p1: f64, p2: f64, p3: f64, t: f64) -> f64 {
    let mt = 1.0 - t;
    mt * mt * mt * p0 + 3.0 * mt * mt * t * p1 + 3.0 * mt * t * t * p2 + t * t * t * p3
}

fn cubic_length(p0: Point<f64>, p1: Point<f64>, p2: Point<f64>, p3: Point<f64>) -> f64 {
    const NODES: [f64; 5] = [
        -0.906_179_845_938_664,
        -0.538_469_310_105_683,
        0.0,
        0.538_469_310_105_683,
        0.906_179_845_938_664,
    ];
    const WEIGHTS: [f64; 5] = [
        0.236_926_885_056_189,
        0.478_628_670_499_366,
        0.568_888_888_888_889,
        0.478_628_670_499_366,
        0.236_926_885_056_189,
    ];
    NODES
        .iter()
        .zip(WEIGHTS)
        .map(|(node, weight)| {
            let t = (node + 1.0) * 0.5;
            let mt = 1.0 - t;
            let dx = 3.0 * mt * mt * (p1.x - p0.x)
                + 6.0 * mt * t * (p2.x - p1.x)
                + 3.0 * t * t * (p3.x - p2.x);
            let dy = 3.0 * mt * mt * (p1.y - p0.y)
                + 6.0 * mt * t * (p2.y - p1.y)
                + 3.0 * t * t * (p3.y - p2.y);
            weight * dx.hypot(dy) * 0.5
        })
        .sum()
}

fn distance(a: Point<f64>, b: Point<f64>) -> f64 {
    (b.x - a.x).hypot(b.y - a.y)
}

fn union_optional(current: Option<DrawRect>, next: DrawRect) -> DrawRect {
    current.map_or(next, |current| {
        DrawRect::new(
            current.min_x.min(next.min_x),
            current.min_y.min(next.min_y),
            current.max_x.max(next.max_x),
            current.max_y.max(next.max_y),
        )
    })
}

fn bounds_array(bounds: Option<DrawRect>) -> [f64; 4] {
    bounds.map_or([0.0; 4], |bounds| {
        [bounds.min_x, bounds.min_y, bounds.max_x, bounds.max_y]
    })
}

fn stable_chunk_id(command_start: u32, commands: &[PathCommand]) -> u64 {
    let mut hash = 0xcbf2_9ce4_8422_2325u64 ^ u64::from(command_start);
    let mut mix = |value: u64| {
        hash ^= value;
        hash = hash.wrapping_mul(0x100_0000_01b3);
    };
    for command in commands {
        match command {
            PathCommand::MoveTo { point } => {
                mix(1);
                point.iter().for_each(|value| mix(value.to_bits()));
            }
            PathCommand::LineTo { point } => {
                mix(2);
                point.iter().for_each(|value| mix(value.to_bits()));
            }
            PathCommand::QuadTo { control, end } => {
                mix(3);
                control
                    .iter()
                    .chain(end)
                    .for_each(|value| mix(value.to_bits()));
            }
            PathCommand::CubicTo {
                control_1,
                control_2,
                end,
            } => {
                mix(4);
                control_1
                    .iter()
                    .chain(control_2)
                    .chain(end)
                    .for_each(|value| mix(value.to_bits()));
            }
        }
    }
    hash
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn cubic_bounds_include_derivative_extrema() {
        let geometry = PathGeometry::from_commands(
            1,
            vec![
                PathCommand::MoveTo { point: [0.0, 0.0] },
                PathCommand::CubicTo {
                    control_1: [0.0, 100.0],
                    control_2: [100.0, 100.0],
                    end: [100.0, 0.0],
                },
            ],
            false,
        );
        assert!((geometry.canvas_bounds[3] - 75.0).abs() < 1e-9);
    }

    #[test]
    fn paths_are_chunked_at_sixty_four_commands() {
        let mut commands = vec![PathCommand::MoveTo { point: [0.0, 0.0] }];
        commands.extend((1..130).map(|x| PathCommand::LineTo {
            point: [x as f64, 0.0],
        }));
        let geometry = PathGeometry::from_commands(1, commands, false);
        assert_eq!(
            geometry
                .chunks
                .iter()
                .map(|chunk| chunk.commands.len())
                .collect::<Vec<_>>(),
            vec![64, 64, 2]
        );
    }

    #[test]
    fn tail_replacement_reuses_prefix_and_matches_full_geometry() {
        let commands = (0..130)
            .map(|index| {
                if index == 0 {
                    PathCommand::MoveTo { point: [0.0, 0.0] }
                } else {
                    PathCommand::LineTo {
                        point: [index as f64, (index % 7) as f64],
                    }
                }
            })
            .collect::<Vec<_>>();
        let original = PathGeometry::from_commands(1, commands.clone(), false);
        let mut changed = commands;
        changed[129] = PathCommand::LineTo {
            point: [140.0, -10.0],
        };
        let full = PathGeometry::from_commands(2, changed.clone(), false);
        let replaced = original.replace_tail(
            2,
            128,
            [127.0, (127 % 7) as f64],
            changed[128..].to_vec(),
            false,
        );

        assert_eq!(replaced, full);
        assert!(Arc::ptr_eq(
            &replaced.chunks[0].commands,
            &original.chunks[0].commands
        ));
        assert!(Arc::ptr_eq(
            &replaced.chunks[1].commands,
            &original.chunks[1].commands
        ));
    }
}
