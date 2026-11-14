// Copyright (C) 2026 mg-chao
// SPDX-License-Identifier: Apache-2.0

//! Pure row planning and intrinsic track constraints, independent of painting.

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub(super) struct Placement {
    pub item: usize,
    pub start: usize,
    pub span: usize,
}

pub(super) fn plan_rows(
    columns: usize,
    spans: impl IntoIterator<Item = Option<usize>>,
) -> Vec<Vec<Placement>> {
    let columns = columns.max(1);
    let mut rows = Vec::new();
    let mut row = Vec::new();
    let mut used = 0;
    for (item, span) in spans.into_iter().enumerate() {
        let remaining = columns - used;
        let span = span.unwrap_or(remaining).max(1).min(remaining);
        row.push(Placement {
            item,
            start: used,
            span,
        });
        used += span;
        if used == columns {
            rows.push(std::mem::take(&mut row));
            used = 0;
        }
    }
    if let Some(last) = row.last_mut() {
        last.span += columns - used;
        rows.push(row);
    }
    rows
}

#[derive(Clone, Copy, Debug, Default)]
pub(super) struct Intrinsic {
    pub minimum: f32,
    pub preferred: f32,
    pub constrained: bool,
}

#[derive(Clone, Copy, Debug)]
pub(super) struct Constraint {
    pub start: usize,
    pub span: usize,
    pub widths: Intrinsic,
}

/// Non-spanning cells establish columns before spanning cells distribute their
/// remaining constraints. Sorting by span makes nested spans deterministic.
pub(super) fn solve_tracks(count: usize, constraints: &mut [Constraint]) -> Vec<Intrinsic> {
    let mut tracks = vec![Intrinsic::default(); count];
    constraints.sort_by_key(|constraint| (constraint.span, constraint.start));
    for constraint in constraints {
        let covered = &mut tracks[constraint.start..constraint.start + constraint.span];
        if constraint.span == 1 {
            let track = &mut covered[0];
            track.minimum = track.minimum.max(constraint.widths.minimum);
            track.preferred = track
                .preferred
                .max(constraint.widths.preferred)
                .max(track.minimum);
            track.constrained |= constraint.widths.constrained;
            continue;
        }
        let minima = distribute(covered, constraint.widths.minimum);
        for (track, minimum) in covered.iter_mut().zip(minima) {
            track.minimum = track.minimum.max(minimum);
            track.preferred = track.preferred.max(track.minimum);
        }
        let preferred = distribute(covered, constraint.widths.preferred);
        for (track, preferred) in covered.iter_mut().zip(preferred) {
            track.preferred = track.preferred.max(preferred).max(track.minimum);
        }
    }
    tracks
}

/// A spanning cell participates in the same intrinsic interpolation as the
/// complete table. Wide content therefore preserves existing track proportions
/// instead of adding an equal increment to unrelated narrow and wide columns.
fn distribute(tracks: &[Intrinsic], target: f32) -> Vec<f32> {
    let minimum = tracks.iter().map(|track| track.minimum).sum::<f32>();
    let preferred = tracks.iter().map(|track| track.preferred).sum::<f32>();
    if target <= minimum {
        return tracks.iter().map(|track| track.minimum).collect();
    }
    if target < preferred {
        let progress = (target - minimum) / (preferred - minimum);
        tracks
            .iter()
            .map(|track| track.minimum + (track.preferred - track.minimum) * progress)
            .collect()
    } else if preferred > 0. {
        let auto = tracks
            .iter()
            .filter(|track| !track.constrained)
            .map(|track| track.preferred)
            .sum::<f32>();
        tracks
            .iter()
            .map(|track| {
                let weight = if auto > 0. {
                    if track.constrained {
                        0.
                    } else {
                        track.preferred / auto
                    }
                } else {
                    track.preferred / preferred
                };
                track.preferred + (target - preferred) * weight
            })
            .collect()
    } else {
        vec![target / tracks.len() as f32; tracks.len()]
    }
}

pub(super) fn span_widths(tracks: &[Intrinsic], start: usize, span: usize) -> Intrinsic {
    tracks[start..start + span]
        .iter()
        .fold(Intrinsic::default(), |sum, track| Intrinsic {
            minimum: sum.minimum + track.minimum,
            preferred: sum.preferred + track.preferred,
            constrained: false,
        })
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn packing_clamps_overflow_and_fills_the_last_cell() {
        let rows = plan_rows(3, [Some(2), Some(2), Some(1), None, None, Some(1)]);
        assert_eq!(
            rows.iter()
                .map(|row| row.iter().map(|p| p.span).collect::<Vec<_>>())
                .collect::<Vec<_>>(),
            [vec![2, 1], vec![1, 2], vec![3], vec![3]]
        );
        assert_eq!(rows[1][1].start, 1);
        assert_eq!(rows[3][0].item, 5);
        assert!(plan_rows(3, []).is_empty());
        assert_eq!(plan_rows(0, [Some(0)])[0][0].span, 1);
    }

    #[test]
    fn all_rows_contribute_to_shared_spanned_tracks() {
        let tracks = solve_tracks(
            3,
            &mut [
                Constraint {
                    start: 0,
                    span: 3,
                    widths: Intrinsic {
                        minimum: 120.,
                        preferred: 240.,
                        ..Default::default()
                    },
                },
                Constraint {
                    start: 0,
                    span: 1,
                    widths: Intrinsic {
                        minimum: 20.,
                        preferred: 80.,
                        ..Default::default()
                    },
                },
                Constraint {
                    start: 1,
                    span: 1,
                    widths: Intrinsic {
                        minimum: 40.,
                        preferred: 60.,
                        ..Default::default()
                    },
                },
                Constraint {
                    start: 2,
                    span: 1,
                    widths: Intrinsic {
                        minimum: 30.,
                        preferred: 40.,
                        ..Default::default()
                    },
                },
            ],
        );
        let all = span_widths(&tracks, 0, 3);
        assert_eq!(all.minimum, 120.);
        assert!((all.preferred - 240.).abs() < 0.001);
        assert_eq!(tracks[0].minimum, 40.);
    }
}
