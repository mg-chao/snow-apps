use super::geometry::*;
use super::native::is_dock_surface;
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
    children: std::collections::HashMap<usize, Vec<usize>>,
    hidden: Vec<usize>,
    hit_unavailable: bool,
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
            children: Default::default(),
            hidden: vec![],
            hit_unavailable: false,
        }
    }
}
impl AxProvider for Fake {
    type Element = usize;
    fn trusted(&self) -> bool {
        self.trusted
    }
    fn hit(
        &mut self,
        _: &WindowInfo,
        _: (f64, f64),
        _: &Budget<'_>,
    ) -> Result<Option<usize>, StopReason> {
        self.calls += 1;
        if let Some(e) = self.error {
            Err(e)
        } else {
            Ok((!self.hit_unavailable).then_some(0))
        }
    }
    fn window_at(&mut self, _: &WindowInfo, _: &Budget<'_>) -> Result<usize, StopReason> {
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
    fn hidden(&mut self, e: &usize, _: &Budget<'_>) -> Result<bool, StopReason> {
        Ok(self.hidden.contains(e))
    }
    fn children(
        &mut self,
        e: &usize,
        offset: usize,
        _: &Budget<'_>,
    ) -> Result<traversal::Children<usize>, StopReason> {
        let children = self.children.get(e).cloned().unwrap_or_default();
        Ok(traversal::Children {
            elements: children
                .iter()
                .skip(offset)
                .take(traversal::CHILD_BATCH)
                .copied()
                .collect(),
            more: offset + traversal::CHILD_BATCH < children.len(),
        })
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
        ..WindowSnapshot::default()
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

#[test]
fn dock_surface_does_not_mask_application_windows_or_their_children() {
    let app = rect(10., 10., 60., 60.);
    let windows = [
        (9, 99, rect(0., 0., 100., 5.), 24, "SystemUIServer"), // menu bar
        (
            8,
            98,
            display().bounds,
            20,
            "/System/Library/CoreServices/Dock.app/Contents/MacOS/Dock",
        ), // Dock's full-display backing surface
        (7, 42, app, 0, "Application"),
    ]
    .into_iter()
    .filter(|(_, _, _, layer, owner)| !is_dock_surface(*layer, 20, Some(owner)))
    .filter_map(|(id, pid, bounds, layer, _)| visible_window(id, pid, bounds, 1., layer, &[]))
    .collect();
    let snapshot = WindowSnapshot {
        windows,
        displays: vec![display()],
        ..WindowSnapshot::default()
    };
    let mut service = ElementRegionService::from_snapshot(&snapshot).unwrap();
    let result = service
        .query(
            Point {
                x: 30,
                y: 30,
                display_id: 1,
            },
            HitTestMode::Window,
            &QueryControl::foreground(),
            &mut |_| {},
        )
        .unwrap();
    assert_eq!(
        result.path.unwrap(),
        vec![ElementRect::new(display().to_pixels(app).unwrap())]
    );
    let selected = snapshot
        .windows
        .iter()
        .find(|w| w.bounds.contains((15., 15.)))
        .unwrap();
    let mut provider = Fake::new();
    provider.nodes[1].bounds = Some(app);
    let result = traversal::query(
        &mut provider,
        selected,
        &display(),
        (15., 15.),
        &QueryControl::foreground(),
        &mut |_| {},
    );
    assert_eq!(result.reason, StopReason::Complete);
    assert_eq!(result.path.unwrap().len(), 2);
    let menu = service
        .query(
            Point {
                x: 30,
                y: 2,
                display_id: 1,
            },
            HitTestMode::Window,
            &QueryControl::foreground(),
            &mut |_| {},
        )
        .unwrap();
    assert_eq!(menu.path.unwrap()[0].height(), 10);
}

#[test]
fn empty_dock_area_selects_only_the_queried_display_in_both_modes() {
    let secondary = DisplayInfo {
        id: 2,
        bounds: rect(100., 0., 100., 100.),
        width: 100,
        height: 100,
    };
    let snapshot = WindowSnapshot {
        windows: vec![],
        displays: vec![display(), secondary.clone()],
        ..WindowSnapshot::default()
    };
    let mut service = ElementRegionService::from_snapshot(&snapshot).unwrap();
    for mode in [HitTestMode::Window, HitTestMode::UiElement] {
        // Physical display rectangles overlap; the request's identity must win.
        for d in [display(), secondary.clone()] {
            let result = service
                .query(
                    Point {
                        x: 150,
                        y: 95,
                        display_id: d.id,
                    },
                    mode,
                    &QueryControl::foreground(),
                    &mut |_| panic!("no AX traversal needed"),
                )
                .unwrap();
            assert_eq!(result.reason, StopReason::Complete);
            assert_eq!(
                result.path.unwrap(),
                vec![ElementRect::new(d.to_pixels(d.bounds).unwrap())]
            );
        }
    }
    let outside = service
        .query(
            Point {
                x: 300,
                y: 300,
                display_id: 2,
            },
            HitTestMode::Window,
            &QueryControl::foreground(),
            &mut |_| {},
        )
        .unwrap();
    assert!(outside.path.is_none());
}

#[test]
fn dock_filter_preserves_other_owners_and_other_dock_levels() {
    assert!(is_dock_surface(
        20,
        20,
        Some("/System/Library/CoreServices/Dock.app/Contents/MacOS/Dock")
    ));
    assert!(!is_dock_surface(20, 20, Some("Application")));
    assert!(!is_dock_surface(20, 20, None));
    // Display names and similarly named third-party executables are not system identity.
    for name in [
        "Dock",
        "程序坞",
        "Dock.app",
        "/Applications/Dock.app/Contents/MacOS/Dock",
    ] {
        assert!(!is_dock_surface(20, 20, Some(name)));
    }
    assert!(!is_dock_surface(
        24,
        20,
        Some("/System/Library/CoreServices/Dock.app/Contents/MacOS/Dock")
    ));
    assert!(!is_dock_surface(
        0,
        20,
        Some("/System/Library/CoreServices/Dock.app/Contents/MacOS/Dock")
    ));
}

fn add_child(p: &mut Fake, parent: usize, bounds: Option<Rect>) -> usize {
    let index = p.nodes.len();
    p.nodes.push(Node {
        bounds,
        parent: Some(parent),
        window: false,
    });
    p.children.entry(parent).or_default().push(index);
    index
}
#[test]
fn coarse_hit_descends_through_geometryless_nodes_and_preserves_actual_ancestors() {
    let mut p = Fake::new();
    p.nodes[0].bounds = Some(rect(0., 0., 90., 90.));
    let group = add_child(&mut p, 0, None);
    let pane = add_child(&mut p, group, Some(rect(5., 5., 60., 60.)));
    add_child(&mut p, pane, Some(rect(10., 10., 20., 20.)));
    let r = run(&mut p, &QueryControl::foreground());
    assert_eq!(r.reason, StopReason::Complete);
    assert_eq!(
        r.path
            .unwrap()
            .iter()
            .map(ElementRect::width)
            .collect::<Vec<_>>(),
        [40, 120, 180, 200]
    );
}
#[test]
fn overlap_ranking_uses_depth_then_area_then_provider_order() {
    let mut p = Fake::new();
    p.nodes[0].bounds = Some(rect(0., 0., 90., 90.));
    add_child(&mut p, 0, Some(rect(0., 0., 40., 40.)));
    let small = add_child(&mut p, 0, Some(rect(10., 10., 20., 20.)));
    add_child(&mut p, 0, Some(rect(5., 5., 20., 20.)));
    assert_eq!(
        run(&mut p, &QueryControl::foreground()).path.unwrap()[0].left(),
        20
    );
    // A deeper exposed node wins even if its clipped area is larger.
    add_child(&mut p, small, Some(rect(0., 0., 30., 30.)));
    assert_eq!(
        run(&mut p, &QueryControl::foreground()).path.unwrap()[0].width(),
        60
    );
}
#[test]
fn hidden_off_pointer_disappeared_and_foreign_children_are_not_selected() {
    let mut p = Fake::new();
    let hidden = add_child(&mut p, 0, Some(rect(14., 14., 2., 2.)));
    p.hidden.push(hidden);
    let outside = add_child(&mut p, 0, Some(rect(70., 70., 10., 10.)));
    add_child(&mut p, outside, Some(rect(14., 14., 2., 2.)));
    let gone = add_child(&mut p, 0, Some(rect(14., 14., 2., 2.)));
    p.nodes[gone].parent = None;
    let foreign = add_child(&mut p, 0, Some(rect(14., 14., 2., 2.)));
    p.nodes[foreign].parent = Some(99);
    let r = run(&mut p, &QueryControl::foreground());
    assert_eq!(r.reason, StopReason::Complete);
    assert_eq!(r.path.unwrap().len(), 2);
}
#[test]
fn native_hit_branch_excludes_smaller_siblings_outside_that_branch() {
    let mut p = Fake::new();
    add_child(&mut p, 1, Some(rect(14., 14., 2., 2.)));
    let r = run(&mut p, &QueryControl::foreground());
    assert_eq!(r.path.unwrap()[0].width(), 41);
}
#[test]
fn child_pages_cycles_duplicates_and_node_limit_are_bounded() {
    let mut p = Fake::new();
    for _ in 0..40 {
        add_child(&mut p, 0, Some(rect(70., 70., 10., 10.)));
    }
    let leaf = add_child(&mut p, 0, Some(rect(14., 14., 2., 2.)));
    p.children.get_mut(&0).unwrap().push(leaf); // duplicate edge, not a cycle
    assert_eq!(
        run(&mut p, &QueryControl::foreground()).path.unwrap()[0].width(),
        4
    );
    p.children.insert(leaf, vec![0]);
    assert_eq!(
        run(&mut p, &QueryControl::foreground()).reason,
        StopReason::TraversalLimit
    );
    p.children.remove(&leaf);
    for _ in 0..600 {
        add_child(&mut p, 0, Some(rect(70., 70., 10., 10.)));
    }
    let mut metrics = traversal::Metrics::default();
    let r = traversal::query_with_metrics(
        &mut p,
        &window(),
        &display(),
        (15., 15.),
        &QueryControl::foreground(),
        &mut |_| {},
        &mut metrics,
    );
    assert_eq!(r.reason, StopReason::TraversalLimit);
    assert_eq!(metrics.visited, traversal::MAX_NODES);
    assert_eq!(r.path.unwrap()[0].width(), 4);
}
#[test]
fn child_depth_limit_keeps_a_valid_bounded_path() {
    let mut p = Fake::new();
    let mut parent = 0;
    for _ in 0..100 {
        parent = add_child(&mut p, parent, Some(rect(10., 10., 20., 20.)));
    }
    let r = run(&mut p, &QueryControl::foreground());
    assert_eq!(r.reason, StopReason::TraversalLimit);
    assert!(r.path.unwrap().len() <= MAX_RECTS);
}

#[test]
fn hidden_hit_ancestors_suppress_all_child_frames() {
    let mut p = Fake::new();
    p.hidden.push(p.root);
    let r = run(&mut p, &QueryControl::foreground());
    assert_eq!(r.reason, StopReason::Complete);
    assert_eq!(r.path.unwrap().len(), 1);
}

#[test]
fn missing_native_hit_uses_validated_window_and_descends_to_the_leaf() {
    let mut p = Fake::new();
    p.hit_unavailable = true;
    p.children.insert(1, vec![0]);
    add_child(&mut p, 0, Some(rect(14., 14., 2., 2.)));
    let r = run(&mut p, &QueryControl::foreground());
    assert_eq!(r.reason, StopReason::Complete);
    assert_eq!(
        r.path
            .unwrap()
            .iter()
            .map(ElementRect::width)
            .collect::<Vec<_>>(),
        [4, 41, 200]
    );
    p.pid = 99;
    let r = run(&mut p, &QueryControl::foreground());
    assert_eq!(r.reason, StopReason::ProviderFailure);
    assert_eq!(r.path.unwrap().len(), 1);
}

#[test]
fn initialization_keeps_valid_partial_frames_but_never_keeps_identity_failures() {
    let path = run(&mut Fake::new(), &QueryControl::foreground());
    for reason in [
        StopReason::BudgetExhausted,
        StopReason::ProviderTimeout,
        StopReason::TraversalLimit,
        StopReason::ProviderFailure,
        StopReason::PermissionRequired,
    ] {
        let fallback = QueryResult {
            path: Some(vec![*path.path.as_ref().unwrap().last().unwrap()]),
            reason,
        };
        let result = retain_partial(path.clone(), fallback);
        let expected = if matches!(
            reason,
            StopReason::ProviderFailure | StopReason::PermissionRequired
        ) {
            1
        } else {
            2
        };
        assert_eq!(result.path.unwrap().len(), expected);
        assert_eq!(result.reason, reason);
    }
}

#[test]
fn valid_parent_chain_does_not_require_a_window_attribute() {
    let mut p = Fake::new();
    // WebKit exposes a valid parent chain even when AXWindow is a stale proxy.
    p.nodes.insert(
        1,
        Node {
            bounds: None,
            parent: Some(2),
            window: false,
        },
    );
    p.root = 2;
    let result = run(&mut p, &QueryControl::foreground());
    assert_eq!(result.reason, StopReason::Complete);
    assert_eq!(result.path.unwrap().len(), 2);
}

#[test]
fn missing_parent_or_wrong_nearest_window_never_publishes_children() {
    for missing_parent in [true, false] {
        let mut p = Fake::new();
        if missing_parent {
            p.nodes[0].parent = None;
        } else {
            // A foreign window cannot be skipped on the way to the expected root.
            p.nodes.insert(
                1,
                Node {
                    bounds: Some(rect(2., 2., 50., 50.)),
                    parent: Some(2),
                    window: true,
                },
            );
            p.root = 2;
        }
        let mut control = QueryControl::refinement(&|| false);
        control.publication_interval = Some(Duration::ZERO);
        let result = traversal::query(
            &mut p,
            &window(),
            &display(),
            (15., 15.),
            &control,
            &mut |_| panic!("unvalidated ancestry published"),
        );
        assert_eq!(result.reason, StopReason::ProviderFailure);
        assert_eq!(result.path.unwrap().len(), 1);
    }
}

#[test]
fn native_control_readiness_is_local_and_requires_a_complete_validated_path() {
    let mut result = run(&mut Fake::new(), &QueryControl::foreground());
    assert!(has_usable_control(&result, true));
    // A different branch containing only containers still needs initialization.
    assert!(!has_usable_control(&result, false));
    for reason in [
        StopReason::ProviderFailure,
        StopReason::PermissionRequired,
        StopReason::Cancelled,
        StopReason::BudgetExhausted,
        StopReason::ProviderTimeout,
        StopReason::TraversalLimit,
    ] {
        result.reason = reason;
        assert!(!has_usable_control(&result, true));
    }
    result.reason = StopReason::Complete;
    result.path.as_mut().unwrap().remove(0);
    assert!(!has_usable_control(&result, true));
}
