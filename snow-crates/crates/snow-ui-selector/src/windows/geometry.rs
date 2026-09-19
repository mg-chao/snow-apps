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

impl MonitorCache {
    /// Enumerate all active monitors and cache their work-area rects.
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

    /// Clip a rect to the portion that can actually be displayed on active
    /// monitors. The selector API returns rectangles, not polygons, so a
    /// window spanning multiple monitors is represented by the bounding rect
    /// of its per-monitor intersections.
    pub(crate) fn clip_rect_to_visible_area(&self, rect: RECT) -> Option<RECT> {
        let mut clipped = None;
        for monitor in &self.rects {
            let Some(intersection) = intersect_rect(rect, *monitor) else {
                continue;
            };
            clipped = Some(match clipped {
                Some(current) => union_rect(current, intersection),
                None => intersection,
            });
        }
        clipped
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
            monitors.clip_rect_to_visible_area(rect(-2000, -400, 3000, 2000)),
            Some(rect(-1920, -200, 2560, 1440))
        );
        assert_eq!(
            monitors.clip_rect_to_visible_area(rect(-2000, 100, -1800, 200)),
            Some(rect(-1920, 100, -1800, 200))
        );
        assert!(
            monitors
                .clip_rect_to_visible_area(rect(2560, 0, 2700, 200))
                .is_none()
        );
        assert!(
            monitors
                .clip_rect_to_visible_area(rect(10, 0, 10, 100))
                .is_none()
        );
    }
}
