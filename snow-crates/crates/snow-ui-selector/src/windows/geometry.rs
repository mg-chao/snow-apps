use rstar::AABB;
use windows::Win32::Foundation::{LPARAM, POINT, RECT};
use windows::Win32::Graphics::Gdi::{
    EnumDisplayMonitors, GetMonitorInfoW, HDC, HMONITOR, MONITORINFO,
};
use windows::core::BOOL;

// ---------------------------------------------------------------------------
// Monitor geometry cache
// ---------------------------------------------------------------------------

/// Cached snapshot of all monitor rects.  Queried once per refresh cycle via
/// `EnumDisplayMonitors` so that per-window `MonitorFromRect` syscalls are
/// replaced by a fast in-process AABB overlap test.
pub(crate) struct MonitorCache {
    rects: Vec<RECT>,
}

pub(crate) struct SelectionRects<'a> {
    first: Option<RECT>,
    second: Option<RECT>,
    remaining: std::slice::Iter<'a, RECT>,
    window_rect: RECT,
}

impl Iterator for SelectionRects<'_> {
    type Item = RECT;

    fn next(&mut self) -> Option<RECT> {
        self.first
            .take()
            .or_else(|| self.second.take())
            .or_else(|| {
                self.remaining
                    .find_map(|monitor| intersect_rect(self.window_rect, *monitor))
            })
    }
}

impl MonitorCache {
    pub(crate) fn from_displays(displays: Option<&[crate::DisplayGeometry]>) -> Self {
        Self::from_displays_or(displays, Self::new)
    }

    fn from_displays_or(
        displays: Option<&[crate::DisplayGeometry]>,
        enumerate: impl FnOnce() -> Self,
    ) -> Self {
        match displays {
            None => enumerate(),
            Some(displays) => Self {
                rects: displays
                    .iter()
                    .map(|d| RECT {
                        left: d.x.round() as i32,
                        top: d.y.round() as i32,
                        right: (d.x + f64::from(d.pixel_width)).round() as i32,
                        bottom: (d.y + f64::from(d.pixel_height)).round() as i32,
                    })
                    .collect(),
            },
        }
    }

    /// Enumerate all active monitors and cache their full bounds.
    pub(crate) fn new() -> Self {
        let mut rects: Vec<RECT> = Vec::with_capacity(4);
        unsafe {
            let _ = EnumDisplayMonitors(
                None,
                None,
                Some(monitor_enum_proc),
                LPARAM(&mut rects as *mut Vec<RECT> as isize),
            );
        }
        Self { rects }
    }

    /// Desktop hosts span the virtual screen. Index each visible monitor portion
    /// separately so both window and element paths stay on the hit monitor.
    /// Ordinary windows retain their complete visible frame across monitors.
    /// Classification is needed only when at least two monitors intersect.
    pub(crate) fn selection_rects(
        &self,
        rect: RECT,
        desktop: impl FnOnce() -> bool,
    ) -> SelectionRects<'_> {
        let mut remaining = self.rects.iter();
        let mut first = remaining.find_map(|monitor| intersect_rect(rect, *monitor));
        let mut second = remaining.find_map(|monitor| intersect_rect(rect, *monitor));
        if second.is_some() && !desktop() {
            let bounds = first.as_mut().expect("second intersection requires first");
            for intersection in second.take().into_iter().chain(
                remaining
                    .by_ref()
                    .filter_map(|monitor| intersect_rect(rect, *monitor)),
            ) {
                *bounds = union_rect(*bounds, intersection);
            }
        }
        SelectionRects {
            first,
            second,
            remaining,
            window_rect: rect,
        }
    }
}

unsafe extern "system" fn monitor_enum_proc(
    hmonitor: HMONITOR,
    _hdc: HDC,
    _lprect: *mut RECT,
    lparam: LPARAM,
) -> BOOL {
    let rects = unsafe { &mut *(lparam.0 as *mut Vec<RECT>) };
    let mut info = MONITORINFO {
        cbSize: std::mem::size_of::<MONITORINFO>() as u32,
        ..Default::default()
    };
    if unsafe { GetMonitorInfoW(hmonitor, &mut info) }.as_bool() {
        rects.push(info.rcMonitor);
    }
    true.into()
}

// ---------------------------------------------------------------------------
// Rect utilities
// ---------------------------------------------------------------------------

pub(crate) fn rect_to_aabb(rect: RECT) -> AABB<[i32; 2]> {
    AABB::from_corners([rect.left, rect.top], [rect.right, rect.bottom])
}

pub(crate) fn contains_point(rect: RECT, point: POINT) -> bool {
    point.x >= rect.left && point.x < rect.right && point.y >= rect.top && point.y < rect.bottom
}

pub(crate) fn same_rect(left: RECT, right: RECT) -> bool {
    left.left == right.left
        && left.top == right.top
        && left.right == right.right
        && left.bottom == right.bottom
}

pub(crate) fn intersect_rect(left: RECT, right: RECT) -> Option<RECT> {
    let rect = RECT {
        left: left.left.max(right.left),
        top: left.top.max(right.top),
        right: left.right.min(right.right),
        bottom: left.bottom.min(right.bottom),
    };
    if is_empty(rect) { None } else { Some(rect) }
}

pub(crate) fn union_rect(left: RECT, right: RECT) -> RECT {
    RECT {
        left: left.left.min(right.left),
        top: left.top.min(right.top),
        right: left.right.max(right.right),
        bottom: left.bottom.max(right.bottom),
    }
}

pub(crate) fn is_empty(rect: RECT) -> bool {
    rect.right <= rect.left || rect.bottom <= rect.top
}

#[cfg(test)]
mod tests {
    use super::*;

    fn rect(left: i32, top: i32, right: i32, bottom: i32) -> RECT {
        RECT {
            left,
            top,
            right,
            bottom,
        }
    }

    fn tuple(rect: RECT) -> (i32, i32, i32, i32) {
        (rect.left, rect.top, rect.right, rect.bottom)
    }

    #[test]
    fn supplied_layout_skips_enumeration_and_legacy_calls_it_once() {
        let calls = std::cell::Cell::new(0);
        let enumerate = || {
            calls.set(calls.get() + 1);
            MonitorCache { rects: vec![] }
        };
        let geometry = crate::DisplayGeometry {
            display_id: 0,
            x: -100.,
            y: 20.,
            width: 100.,
            height: 80.,
            pixel_width: 100,
            pixel_height: 80,
        };
        let cache = MonitorCache::from_displays_or(Some(&[geometry]), enumerate);
        assert_eq!(calls.get(), 0);
        assert_eq!(
            cache
                .selection_rects(rect(-150, 0, 50, 200), || false)
                .next(),
            Some(rect(-100, 20, 0, 100))
        );
        MonitorCache::from_displays_or(None, enumerate);
        assert_eq!(calls.get(), 1);
    }

    #[test]
    fn intersect_rect_returns_overlap() {
        assert_eq!(
            intersect_rect(rect(0, 0, 100, 100), rect(50, 10, 150, 90)).map(tuple),
            Some((50, 10, 100, 90))
        );
    }

    #[test]
    fn intersect_rect_rejects_edge_touching_rects() {
        assert!(intersect_rect(rect(0, 0, 100, 100), rect(100, 0, 200, 100)).is_none());
    }

    #[test]
    fn union_rect_bounds_both_inputs() {
        assert_eq!(
            tuple(union_rect(rect(10, 20, 30, 40), rect(-5, 25, 20, 60))),
            (-5, 20, 30, 60)
        );
    }

    #[test]
    fn monitor_snapshot_clips_across_displays_and_rejects_invisible_rectangles() {
        let monitors = MonitorCache {
            rects: vec![rect(-1920, 0, 0, 1080), rect(0, -200, 2560, 1440)],
        };
        assert_eq!(
            monitors
                .selection_rects(rect(-2000, -400, 3000, 2000), || false)
                .next(),
            Some(rect(-1920, -200, 2560, 1440))
        );
        assert_eq!(
            monitors
                .selection_rects(rect(-2000, 100, -1800, 200), || false)
                .next(),
            Some(rect(-1920, 100, -1800, 200))
        );
        assert!(
            monitors
                .selection_rects(rect(2560, 0, 2700, 200), || false)
                .next()
                .is_none()
        );
        assert!(
            monitors
                .selection_rects(rect(10, 0, 10, 100), || false)
                .next()
                .is_none()
        );
    }

    #[test]
    fn desktop_selection_is_split_by_monitor_but_spanning_windows_are_preserved() {
        let left = rect(-1920, 0, 0, 1080);
        let right = rect(0, -200, 2560, 1440);
        let monitors = MonitorCache {
            rects: vec![left, right],
        };
        let desktop = rect(-1920, -200, 2560, 1440);
        assert_eq!(
            monitors
                .selection_rects(desktop, || true)
                .collect::<Vec<_>>(),
            [left, right]
        );
        assert_eq!(
            monitors
                .selection_rects(desktop, || false)
                .collect::<Vec<_>>(),
            [desktop]
        );
        assert_eq!(
            monitors
                .selection_rects(rect(-100, 20, 100, 120), || true)
                .collect::<Vec<_>>(),
            [rect(-100, 20, 0, 120), rect(0, 20, 100, 120)]
        );
        assert!(
            monitors
                .selection_rects(rect(2560, 0, 3000, 200), || true)
                .next()
                .is_none()
        );
    }

    #[test]
    fn desktop_detection_is_skipped_for_zero_or_one_intersecting_monitor() {
        let monitors = MonitorCache {
            rects: vec![
                rect(-100, 0, 0, 100),
                rect(0, 0, 100, 100),
                rect(100, 0, 200, 100),
            ],
        };
        for (window, expected) in [
            (rect(-200, 0, -100, 100), None),
            (rect(10, 10, 90, 90), Some(rect(10, 10, 90, 90))),
        ] {
            let mut rects = monitors.selection_rects(window, || panic!("unneeded native lookup"));
            assert_eq!(rects.next(), expected);
            assert_eq!(rects.next(), None);
        }
        let calls = std::cell::Cell::new(0);
        for desktop in [false, true] {
            let rects = monitors
                .selection_rects(rect(-100, 0, 200, 100), || {
                    calls.set(calls.get() + 1);
                    desktop
                })
                .collect::<Vec<_>>();
            assert_eq!(calls.get(), if desktop { 2 } else { 1 });
            if desktop {
                assert_eq!(rects, monitors.rects);
            } else {
                assert_eq!(rects, [rect(-100, 0, 200, 100)]);
            }
        }
    }
}
