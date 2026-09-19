use super::geometry::*;
use super::traversal::{self, AxProvider, Budget, MAX_DEPTH, MAX_RECTS};
use super::*;
use std::time::Duration;

fn rect(x: f64, y: f64, w: f64, h: f64) -> Rect {
    Rect {
        x,
        y,
        width: w,
        height: h,
    }
}
fn display() -> DisplayInfo {
    DisplayInfo {
        id: 1,
        bounds: rect(0., 0., 100., 100.),
        width: 200,
        height: 200,
    }
}
fn window() -> WindowInfo {
    WindowInfo {
        id: 7,
        pid: 42,
        bounds: rect(0., 0., 100., 100.),
    }
}
#[derive(Clone)]
struct Node {
    bounds: Option<Rect>,
    parent: Option<usize>,
    window: bool,
}
struct Fake {
    nodes: Vec<Node>,
    trusted: bool,
    error: Option<StopReason>,
    pid: i32,
    root: usize,
    calls: usize,
}
impl Fake {
    fn new() -> Self {
        Self {
            nodes: vec![
                Node {
                    bounds: Some(rect(10.1, 10.1, 20.2, 20.2)),
                    parent: Some(1),
                    window: false,
                },
                Node {
                    bounds: Some(window().bounds),
                    parent: None,
                    window: true,
                },
            ],
            trusted: true,
            error: None,
            pid: 42,
            root: 1,
            calls: 0,
        }
    }
}
impl AxProvider for Fake {
    type Element = usize;
    fn trusted(&self) -> bool {
        self.trusted
    }
    fn hit(&mut self, _: &WindowInfo, _: (f64, f64), _: &Budget<'_>) -> Result<usize, StopReason> {
        self.calls += 1;
        if let Some(e) = self.error {
            Err(e)
        } else {
            Ok(0)
        }
    }
    fn window(&mut self, _: &usize, _: &Budget<'_>) -> Result<usize, StopReason> {
        Ok(self.root)
    }
    fn pid(&mut self, _: &usize, _: &Budget<'_>) -> Result<i32, StopReason> {
        Ok(self.pid)
    }
    fn bounds(&mut self, e: &usize, _: &Budget<'_>) -> Result<Option<Rect>, StopReason> {
        Ok(self.nodes[*e].bounds)
    }
    fn is_window(&mut self, e: &usize, _: &Budget<'_>) -> Result<bool, StopReason> {
        Ok(self.nodes[*e].window)
    }
    fn parent(&mut self, e: &usize, _: &Budget<'_>) -> Result<Option<usize>, StopReason> {
        Ok(self.nodes[*e].parent)
    }
}
fn run(p: &mut Fake, c: &QueryControl<'_>) -> QueryResult {
    traversal::query(p, &window(), &display(), (15., 15.), c, &mut |_| {})
}
#[test]
fn retina_mapping_rounds_outward_and_preserves_secondary_origins() {
    let d = DisplayInfo {
        id: 2,
        bounds: rect(-100., -50., 100., 100.),
        width: 200,
        height: 200,
    };
    let p = d.to_pixels(rect(-99.9, -49.9, 10.2, 10.2)).unwrap();
    assert_eq!((p.left, p.top, p.right, p.bottom), (-100, -50, -79, -29));
    assert_eq!(
        d.to_points(Point {
            x: -80,
            y: -30,
            display_id: 2
        }),
        (-90., -40.)
    );
    assert!(d.to_pixels(rect(f64::NAN, 0., 1., 1.)).is_none());
    assert!(d.to_pixels(rect(0., 0., -1., 1.)).is_none());
}
#[test]
fn display_identity_disambiguates_overlapping_physical_spaces() {
    let displays = vec![
        display(),
        DisplayInfo {
            id: 2,
            bounds: rect(100., 0., 100., 100.),
            width: 100,
            height: 100,
        },
    ];
    assert_eq!(
        query_position(
            &displays,
            Point {
                x: 150,
                y: 50,
                display_id: 1
            }
        )
        .unwrap()
        .1,
        (75., 25.)
    );
    assert_eq!(
        query_position(
            &displays,
            Point {
                x: 150,
                y: 50,
                display_id: 2
            }
        )
        .unwrap()
        .1,
        (150., 50.)
    );
    assert!(
        query_position(
            &displays,
            Point {
                x: 150,
                y: 50,
                display_id: 3
            }
        )
        .is_none()
    );
}
#[test]
fn filters_preserve_translucent_windows_and_exclude_desktop_invalid_and_overlays() {
    let r = window().bounds;
    assert!(visible_window(1, 42, r, 0.25, 0, &[]).is_some());
    for (id, pid, alpha, layer) in [
        (0, 42, 1., 0),
        (1, 0, 1., 0),
        (1, 42, 0., 0),
        (1, 42, f64::NAN, 0),
        (1, 42, 1., -1),
    ] {
        assert!(visible_window(id, pid, r, alpha, layer, &[]).is_none());
    }
    assert!(visible_window(1, 42, r, 1., 0, &[1]).is_none());
    assert!(visible_window(1, 42, rect(0., 0., f64::INFINITY, 1.), 1., 0, &[]).is_none());
}
#[test]
fn window_mode_uses_z_order_and_snapshot_without_accessibility() {
    let mut snapshot = WindowSnapshot {
        windows: vec![
            WindowInfo {
                id: 8,
                pid: 43,
                bounds: rect(10., 10., 30., 30.),
            },
            window(),
        ],
        displays: vec![display()],
    };
    let mut service = ElementRegionService::from_snapshot(&snapshot).unwrap();
    snapshot.windows.clear(); // independent plain snapshot ownership
    let result = service
        .query(
            Point {
                x: 30,
                y: 30,
                display_id: 1,
            },
            HitTestMode::Window,
            &QueryControl::foreground(),
            &mut |_| panic!(),
        )
        .unwrap();
    let path = result.path.unwrap();
    assert_eq!(path[0].width(), 60);
    service.release_cache();
    assert!(service.window_snapshot().unwrap().windows.is_empty());
}
#[test]
fn traversal_clips_deduplicates_and_terminates_at_window() {
    let mut p = Fake::new();
    p.nodes[0].bounds = Some(rect(-10., 10., 40., 30.));
    p.nodes.insert(
        1,
        Node {
            bounds: p.nodes[0].bounds,
            parent: Some(2),
            window: false,
        },
    );
    p.root = 2;
    let result = run(&mut p, &QueryControl::foreground());
    assert_eq!(result.reason, StopReason::Complete);
    let path = result.path.unwrap();
    assert_eq!(path.len(), 2);
    assert_eq!(path[0].left(), 0);
    assert_eq!(path[0].right(), 60);
    assert_eq!(path.last().unwrap().width(), 200);
}
#[test]
fn permission_and_provider_failures_always_keep_window_fallback() {
    for reason in [
        StopReason::PermissionRequired,
        StopReason::ProviderTimeout,
        StopReason::ProviderFailure,
    ] {
        let mut p = Fake::new();
        p.error = Some(reason);
        let r = run(&mut p, &QueryControl::foreground());
        assert_eq!(r.reason, reason);
        assert_eq!(r.path.unwrap().len(), 1);
    }
    let mut p = Fake::new();
    p.trusted = false;
    let r = run(&mut p, &QueryControl::foreground());
    assert_eq!(r.reason, StopReason::PermissionRequired);
    assert_eq!(p.calls, 0);
}
#[test]
fn identity_mismatch_rejects_same_process_different_window() {
    let mut p = Fake::new();
    p.nodes[1].bounds = Some(rect(2.5, 0., 100., 100.));
    assert_eq!(
        run(&mut p, &QueryControl::foreground()).reason,
        StopReason::ProviderFailure
    );
    p.nodes[1].bounds = Some(window().bounds);
    p.pid = 9;
    assert_eq!(
        run(&mut p, &QueryControl::foreground()).reason,
        StopReason::ProviderFailure
    );
}
#[test]
fn cancellation_and_budget_do_not_enter_provider() {
    let mut p = Fake::new();
    let c = QueryControl::refinement(&|| true);
    assert_eq!(run(&mut p, &c).reason, StopReason::Cancelled);
    let mut c = QueryControl::foreground();
    c.budget = Duration::ZERO;
    assert_eq!(run(&mut p, &c).reason, StopReason::BudgetExhausted);
    assert_eq!(p.calls, 0);
}
#[test]
fn cycles_depth_and_rectangle_limits_are_bounded() {
    let mut p = Fake::new();
    p.nodes[0].parent = Some(0);
    assert_eq!(
        run(&mut p, &QueryControl::foreground()).reason,
        StopReason::TraversalLimit
    );
    for unique in [false, true] {
        let mut p = Fake::new();
        p.nodes = (0..MAX_DEPTH + 2)
            .map(|i| Node {
                bounds: Some(rect(0., 0., if unique { 20. + i as f64 } else { 30. }, 90.)),
                parent: Some(i + 1),
                window: false,
            })
            .collect();
        p.root = p.nodes.len() - 1;
        p.nodes[p.root] = Node {
            bounds: Some(window().bounds),
            parent: None,
            window: true,
        };
        let result = run(&mut p, &QueryControl::foreground());
        assert_eq!(result.reason, StopReason::TraversalLimit);
        assert!(result.path.unwrap().len() <= MAX_RECTS);
    }
}
#[test]
fn unsupported_bounds_are_skipped_and_unrelated_bounds_are_not_selected() {
    let mut p = Fake::new();
    p.nodes[0].bounds = None;
    assert_eq!(
        run(&mut p, &QueryControl::foreground()).path.unwrap().len(),
        1
    );
    p.nodes[0].bounds = Some(rect(80., 80., 10., 10.));
    assert_eq!(
        run(&mut p, &QueryControl::foreground()).path.unwrap().len(),
        1
    );
}

#[test]
fn rotated_and_scaled_display_modes_use_oriented_bounds() {
    let d = DisplayInfo::from_mode(1, rect(-900., 0., 900., 1440.), 1440, 900, 2880, 1800).unwrap();
    assert_eq!((d.width, d.height), (1800, 2880));
    assert_eq!(
        d.to_points(Point {
            x: 0,
            y: 1440,
            display_id: 1
        }),
        (-450., 720.)
    );
    assert!(DisplayInfo::from_mode(1, display().bounds, 0, 100, 200, 200).is_none());
}

#[test]
fn cancellation_during_ancestry_preserves_window_and_stops_publication() {
    let calls = std::cell::Cell::new(0);
    let cancelled = || {
        calls.set(calls.get() + 1);
        calls.get() > 2
    };
    let control = QueryControl::refinement(&cancelled);
    let result = run(&mut Fake::new(), &control);
    assert_eq!(result.reason, StopReason::Cancelled);
    assert_eq!(result.path.unwrap().last().unwrap().width(), 200);
}
