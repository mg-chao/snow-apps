use std::collections::HashMap;
use std::ptr;
use std::thread;
use std::time::Duration;

use crossbeam_channel::{Receiver, Sender, bounded, unbounded};
use windows::Win32::Foundation::{HWND, POINT, RECT};
use windows::Win32::System::Com::IDispatch;
use windows::Win32::System::Variant::{VARIANT, VT_DISPATCH, VT_EMPTY, VT_I4, VT_UNKNOWN};
use windows::Win32::UI::Accessibility::{AccessibleObjectFromWindow, IAccessible};
use windows::Win32::UI::WindowsAndMessaging::{CHILDID_SELF, IsHungAppWindow, OBJID_WINDOW};
use windows::core::{IUnknown, Interface, Result};

use crate::ElementRect;
use crate::com::ComApartment;
use crate::geometry::*;
use crate::spatial::*;
use crate::window;

const MSAA_SELF_CHILD_ID: i32 = CHILDID_SELF as i32;
const MSAA_MAX_HIT_PATH_STEPS: usize = 64;
const MSAA_MAX_HIT_PATH_RECTS: usize = 100;
const STATE_SYSTEM_INVISIBLE: i32 = 0x0000_8000;
const STATE_SYSTEM_OFFSCREEN: i32 = 0x0001_0000;
const MSAA_REQUEST_TIMEOUT: Duration = Duration::from_millis(168);

struct MsaaWindow {
    hwnd: HWND,
    bounds: RECT,
    fallback_rects: Option<Vec<RECT>>,
    msaa_quarantined: bool,
}

pub(crate) struct MsaaBackend {
    windows: Vec<MsaaWindow>,
    window_index: WindowSpatialIndex,
    worker: Option<MsaaWorker>,
}

struct MsaaWorker {
    requests: Option<Sender<MsaaRequest>>,
    join: Option<thread::JoinHandle<()>>,
}

enum MsaaRequest {
    HitTest {
        hwnd: isize,
        point: POINT,
        window_bounds: RECT,
        response: Sender<Result<Vec<RECT>>>,
    },
}

#[derive(Default)]
struct MsaaWorkerState {
    roots: HashMap<isize, Option<IAccessible>>,
}

impl MsaaWorker {
    fn spawn() -> Option<Self> {
        let (requests, receiver) = unbounded();
        let join = thread::Builder::new()
            .name("snow-msaa-hit-test".into())
            .spawn(move || run_msaa_worker(receiver))
            .ok()?;
        Some(Self {
            requests: Some(requests),
            join: Some(join),
        })
    }

    fn hit_test(
        &self,
        hwnd: HWND,
        point: POINT,
        window_bounds: RECT,
    ) -> std::result::Result<Result<Vec<RECT>>, crossbeam_channel::RecvTimeoutError> {
        let (response, receiver) = bounded(1);
        if self
            .requests
            .as_ref()
            .ok_or(crossbeam_channel::RecvTimeoutError::Disconnected)?
            .send(MsaaRequest::HitTest {
                hwnd: hwnd.0 as isize,
                point,
                window_bounds,
                response,
            })
            .is_err()
        {
            return Err(crossbeam_channel::RecvTimeoutError::Disconnected);
        }
        receiver.recv_timeout(MSAA_REQUEST_TIMEOUT)
    }
}

impl Drop for MsaaWorker {
    fn drop(&mut self) {
        // Dropping the last sender lets the worker leave its receive loop and
        // uninitialize its COM apartment. Keep the handle so service teardown
        // does not leave an accessibility thread (and its COM heap) detached.
        self.requests.take();
        if let Some(join) = self.join.take() {
            let _ = join.join();
        }
    }
}

fn run_msaa_worker(requests: Receiver<MsaaRequest>) {
    let Ok(_com) = ComApartment::new() else {
        return;
    };
    let mut state = MsaaWorkerState::default();
    while let Ok(request) = requests.recv() {
        match request {
            MsaaRequest::HitTest {
                hwnd,
                point,
                window_bounds,
                response,
            } => {
                let result = state.hit_test(hwnd, point, window_bounds);
                let _ = response.send(result);
            }
        }
    }
}

impl MsaaWorkerState {
    fn hit_test(&mut self, hwnd: isize, point: POINT, window_bounds: RECT) -> Result<Vec<RECT>> {
        let root = self
            .roots
            .entry(hwnd)
            .or_insert_with(|| accessible_root_from_window(hwnd_from_raw(hwnd)).ok())
            .clone();
        let Some(root) = root else {
            return Ok(Vec::new());
        };
        let result = build_msaa_hit_path(root, MSAA_SELF_CHILD_ID, point, window_bounds);
        if result.is_err() {
            self.roots.remove(&hwnd);
        }
        result
    }
}

fn hwnd_from_raw(hwnd: isize) -> HWND {
    HWND(hwnd as *mut _)
}

impl MsaaBackend {
    pub(crate) fn new_excluding_hwnds(excluded_hwnds: &[HWND]) -> Result<Self> {
        let (windows, window_index) = build_msaa_window_cache(excluded_hwnds)?;
        Ok(Self {
            windows,
            window_index,
            worker: MsaaWorker::spawn(),
        })
    }

    pub(crate) fn refresh(&mut self, excluded_hwnds: &[HWND]) -> Result<()> {
        let (windows, window_index) = build_msaa_window_cache(excluded_hwnds)?;
        self.windows = windows;
        self.window_index = window_index;
        self.worker = MsaaWorker::spawn();
        Ok(())
    }

    pub(crate) fn release_cache(&mut self) {
        self.windows = Vec::new();
        self.window_index.release_cache();
        self.worker = None;
    }

    pub(crate) fn hit_test_point(
        &mut self,
        point: POINT,
        mode: crate::HitTestMode,
    ) -> Result<Option<Vec<ElementRect>>> {
        let Some(window_idx) = self.window_at_point(point) else {
            return Ok(None);
        };

        let window_bounds = self.windows[window_idx].bounds;
        match mode {
            crate::HitTestMode::Window => Ok(Some(vec![ElementRect::new(window_bounds)])),
            crate::HitTestMode::UiElement => {
                let msaa_rects = if self.windows[window_idx].msaa_quarantined {
                    Vec::new()
                } else if unsafe { IsHungAppWindow(self.windows[window_idx].hwnd).as_bool() } {
                    self.mark_unresponsive(window_idx);
                    Vec::new()
                } else if let Some(worker) = &self.worker {
                    match worker.hit_test(self.windows[window_idx].hwnd, point, window_bounds) {
                        Ok(Ok(rects)) => rects,
                        Ok(Err(_)) => Vec::new(),
                        Err(_) => {
                            self.mark_unresponsive(window_idx);
                            self.worker = MsaaWorker::spawn();
                            Vec::new()
                        }
                    }
                } else {
                    Vec::new()
                };

                let fallback_rects = self.fallback_hit_path(window_idx, point);
                let rects = merge_hit_paths(msaa_rects, fallback_rects, window_bounds, point);
                Ok(Some(rects.into_iter().map(ElementRect::new).collect()))
            }
        }
    }

    fn window_at_point(&self, point: POINT) -> Option<usize> {
        self.window_index
            .window_at_point([point.x, point.y])
            .map(|window| window.cache_index)
    }

    fn mark_unresponsive(&mut self, window_idx: usize) {
        // A blocked COM call cannot be forcibly terminated. Quarantine its
        // HWND until refresh so repeated pointer samples cannot leak workers.
        self.windows[window_idx].msaa_quarantined = true;
    }

    fn fallback_hit_path(&mut self, window_idx: usize, point: POINT) -> Vec<RECT> {
        let window = &mut self.windows[window_idx];
        let candidates = window
            .fallback_rects
            .get_or_insert_with(|| window::visible_child_window_rects(window.hwnd, window.bounds));
        let mut containing = candidates
            .iter()
            .copied()
            .filter(|rect| contains_point(*rect, point))
            .collect::<Vec<_>>();
        push_rect_if_useful(&mut containing, window.bounds, point);
        containing
    }
}

fn merge_hit_paths(
    msaa_rects: Vec<RECT>,
    fallback_rects: Vec<RECT>,
    window_bounds: RECT,
    point: POINT,
) -> Vec<RECT> {
    let msaa_seed = msaa_rects.first().copied();
    let fallback_seed = fallback_rects.first().copied();
    let seed = match (msaa_seed, fallback_seed) {
        (Some(msaa), Some(fallback)) if contains_rect(msaa, fallback) => fallback,
        (Some(msaa), _) => msaa,
        (None, Some(fallback)) => fallback,
        (None, None) => window_bounds,
    };

    let mut candidates = msaa_rects;
    candidates.extend(fallback_rects);
    push_rect_if_useful(&mut candidates, window_bounds, point);
    candidates.sort_unstable_by_key(rect_sort_key);
    candidates.dedup_by(|left, right| same_rect(*left, *right));

    let mut path = vec![seed];
    for candidate in candidates {
        let current = *path.last().expect("path always contains its seed");
        if !same_rect(candidate, current) && contains_rect(candidate, current) {
            path.push(candidate);
        }
    }
    push_rect_if_useful(&mut path, window_bounds, point);
    path
}

fn contains_rect(outer: RECT, inner: RECT) -> bool {
    !is_empty(outer)
        && !is_empty(inner)
        && inner.left >= outer.left
        && inner.top >= outer.top
        && inner.right <= outer.right
        && inner.bottom <= outer.bottom
}

fn rect_sort_key(rect: &RECT) -> (i64, i32, i32, i32, i32) {
    let width = i64::from(rect.right) - i64::from(rect.left);
    let height = i64::from(rect.bottom) - i64::from(rect.top);
    (width * height, rect.left, rect.top, rect.right, rect.bottom)
}

fn build_msaa_window_cache(
    excluded_hwnds: &[HWND],
) -> Result<(Vec<MsaaWindow>, WindowSpatialIndex)> {
    let hwnds = window::enumerate_top_windows()?;
    let monitors = MonitorCache::new();
    let mut excluded_raw = excluded_hwnds
        .iter()
        .map(|hwnd| hwnd.0 as isize)
        .collect::<Vec<_>>();
    excluded_raw.sort_unstable();
    excluded_raw.dedup();

    let mut windows = Vec::with_capacity(hwnds.len());
    let mut entries = Vec::with_capacity(hwnds.len());

    for (z_order, hwnd) in hwnds.into_iter().enumerate() {
        if excluded_raw.binary_search(&(hwnd.0 as isize)).is_ok() {
            continue;
        }
        if window::is_window_cloaked(hwnd) {
            continue;
        }
        let Some(rect) = window::visible_window_rect(hwnd) else {
            continue;
        };
        let Some(bounds) = monitors.clip_rect_to_visible_area(rect) else {
            continue;
        };

        let cache_index = windows.len();
        windows.push(MsaaWindow {
            hwnd,
            bounds,
            fallback_rects: None,
            msaa_quarantined: false,
        });
        entries.push(IndexedWindow {
            envelope: rect_to_aabb(bounds),
            cache_index,
            z_order,
        });
    }

    Ok((windows, WindowSpatialIndex::build(entries)))
}

fn accessible_root_from_window(hwnd: HWND) -> Result<IAccessible> {
    let mut raw = ptr::null_mut();
    unsafe {
        AccessibleObjectFromWindow(hwnd, OBJID_WINDOW.0 as u32, &IAccessible::IID, &mut raw)?;
        Ok(IAccessible::from_raw(raw))
    }
}

fn build_msaa_hit_path(
    root_object: IAccessible,
    root_child_id: i32,
    point: POINT,
    window_bounds: RECT,
) -> Result<Vec<RECT>> {
    let mut rects = Vec::with_capacity(8);
    let mut object = root_object;
    let mut child_id = root_child_id;

    for _ in 0..MSAA_MAX_HIT_PATH_STEPS {
        push_msaa_candidate_rect(&mut rects, &object, child_id, point, window_bounds)?;

        let hit_variant = match unsafe { object.accHitTest(point.x, point.y) } {
            Ok(value) => value,
            Err(_) => break,
        };

        match resolve_msaa_hit(&hit_variant) {
            MsaaHit::None | MsaaHit::SelfObject => break,
            MsaaHit::ChildId(hit_child_id) => {
                if hit_child_id == child_id {
                    break;
                }

                let child_variant = msaa_child_variant(hit_child_id);
                if let Ok(dispatch) = unsafe { object.get_accChild(&child_variant) }
                    && let Ok(child_object) = dispatch.cast::<IAccessible>()
                {
                    object = child_object;
                    child_id = MSAA_SELF_CHILD_ID;
                    continue;
                }

                child_id = hit_child_id;
                push_msaa_candidate_rect(&mut rects, &object, child_id, point, window_bounds)?;
                break;
            }
            MsaaHit::Object(hit_object) => {
                if same_accessible_object(&object, &hit_object) {
                    break;
                }
                object = hit_object;
                child_id = MSAA_SELF_CHILD_ID;
            }
        }

        if rects.len() >= MSAA_MAX_HIT_PATH_RECTS {
            break;
        }
    }

    rects.reverse();
    push_rect_if_useful(&mut rects, window_bounds, point);
    Ok(rects)
}

fn push_msaa_candidate_rect(
    rects: &mut Vec<RECT>,
    object: &IAccessible,
    child_id: i32,
    point: POINT,
    window_bounds: RECT,
) -> Result<()> {
    if msaa_is_invisible_or_offscreen(object, child_id) {
        return Ok(());
    }

    let Some(rect) = msaa_location_rect(object, child_id)? else {
        return Ok(());
    };
    let rect = intersect_rect(rect, window_bounds).unwrap_or(rect);
    push_rect_if_useful(rects, rect, point);
    Ok(())
}

fn push_rect_if_useful(rects: &mut Vec<RECT>, rect: RECT, point: POINT) {
    if rects.len() >= MSAA_MAX_HIT_PATH_RECTS || is_empty(rect) || !contains_point(rect, point) {
        return;
    }
    if rects.iter().any(|existing| same_rect(*existing, rect)) {
        return;
    }
    rects.push(rect);
}

fn msaa_location_rect(object: &IAccessible, child_id: i32) -> Result<Option<RECT>> {
    let mut left = 0i32;
    let mut top = 0i32;
    let mut width = 0i32;
    let mut height = 0i32;
    let child_variant = msaa_child_variant(child_id);

    if unsafe { object.accLocation(&mut left, &mut top, &mut width, &mut height, &child_variant) }
        .is_err()
    {
        return Ok(None);
    }
    if width <= 0 || height <= 0 {
        return Ok(None);
    }

    let rect = RECT {
        left,
        top,
        right: left.saturating_add(width),
        bottom: top.saturating_add(height),
    };
    Ok((!is_empty(rect)).then_some(rect))
}

fn msaa_is_invisible_or_offscreen(object: &IAccessible, child_id: i32) -> bool {
    let child_variant = msaa_child_variant(child_id);
    let Ok(state_variant) = (unsafe { object.get_accState(&child_variant) }) else {
        return false;
    };
    let Ok(state) = i32::try_from(&state_variant) else {
        return false;
    };
    state & (STATE_SYSTEM_INVISIBLE | STATE_SYSTEM_OFFSCREEN) != 0
}

fn msaa_child_variant(child_id: i32) -> VARIANT {
    child_id.into()
}

enum MsaaHit {
    None,
    SelfObject,
    ChildId(i32),
    Object(IAccessible),
}

fn resolve_msaa_hit(value: &VARIANT) -> MsaaHit {
    let variant_type = unsafe { value.Anonymous.Anonymous.vt };

    if variant_type == VT_EMPTY {
        return MsaaHit::None;
    }
    if variant_type == VT_I4 {
        let child_id = unsafe { value.Anonymous.Anonymous.Anonymous.lVal };
        if child_id == MSAA_SELF_CHILD_ID {
            return MsaaHit::SelfObject;
        }
        return if child_id > 0 {
            MsaaHit::ChildId(child_id)
        } else {
            MsaaHit::None
        };
    }
    variant_to_accessible(value)
        .map(MsaaHit::Object)
        .unwrap_or(MsaaHit::None)
}

fn same_accessible_object(left: &IAccessible, right: &IAccessible) -> bool {
    let Ok(left_unknown) = left.cast::<IUnknown>() else {
        return false;
    };
    let Ok(right_unknown) = right.cast::<IUnknown>() else {
        return false;
    };
    left_unknown.as_raw() == right_unknown.as_raw()
}

fn variant_to_accessible(value: &VARIANT) -> Option<IAccessible> {
    let variant_type = unsafe { value.Anonymous.Anonymous.vt };

    if variant_type == VT_DISPATCH {
        let dispatch_opt = unsafe { &value.Anonymous.Anonymous.Anonymous.pdispVal };
        let dispatch: &IDispatch = dispatch_opt.as_ref()?;
        return dispatch.clone().cast::<IAccessible>().ok();
    }
    if variant_type == VT_UNKNOWN {
        let unknown_opt = unsafe { &value.Anonymous.Anonymous.Anonymous.punkVal };
        let unknown: &IUnknown = unknown_opt.as_ref()?;
        return unknown.clone().cast::<IAccessible>().ok();
    }

    None
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

    #[test]
    fn push_rect_if_useful_rejects_empty_outside_and_duplicate_rects() {
        let point = POINT { x: 5, y: 5 };
        let mut rects = Vec::new();

        push_rect_if_useful(&mut rects, rect(0, 0, 0, 10), point);
        push_rect_if_useful(&mut rects, rect(20, 20, 30, 30), point);
        push_rect_if_useful(&mut rects, rect(0, 0, 10, 10), point);
        push_rect_if_useful(&mut rects, rect(0, 0, 10, 10), point);

        assert_eq!(rects.len(), 1);
        assert!(same_rect(rects[0], rect(0, 0, 10, 10)));
    }

    #[test]
    fn hit_path_rects_are_reversed_before_window_fallback_is_appended() {
        let point = POINT { x: 5, y: 5 };
        let mut rects = vec![rect(0, 0, 100, 100), rect(0, 0, 10, 10)];

        rects.reverse();
        push_rect_if_useful(&mut rects, rect(0, 0, 200, 200), point);

        assert!(same_rect(rects[0], rect(0, 0, 10, 10)));
        assert!(same_rect(rects[1], rect(0, 0, 100, 100)));
        assert!(same_rect(rects[2], rect(0, 0, 200, 200)));
    }

    #[test]
    fn hwnd_fallback_refines_a_coarse_msaa_path() {
        let point = POINT { x: 25, y: 25 };
        let window = rect(0, 0, 200, 200);
        let msaa_panel = rect(10, 10, 150, 150);
        let child_hwnd = rect(20, 20, 80, 80);

        let path = merge_hit_paths(
            vec![msaa_panel, window],
            vec![child_hwnd, window],
            window,
            point,
        );

        assert_eq!(path.len(), 3);
        assert!(same_rect(path[0], child_hwnd));
        assert!(same_rect(path[1], msaa_panel));
        assert!(same_rect(path[2], window));
    }

    #[test]
    fn overlapping_fallback_does_not_replace_a_semantic_msaa_hit() {
        let point = POINT { x: 75, y: 75 };
        let window = rect(0, 0, 200, 200);
        let msaa_element = rect(50, 50, 100, 100);
        let overlapping_hwnd = rect(60, 60, 140, 140);

        let path = merge_hit_paths(
            vec![msaa_element, window],
            vec![overlapping_hwnd, window],
            window,
            point,
        );

        assert_eq!(path.len(), 2);
        assert!(same_rect(path[0], msaa_element));
        assert!(same_rect(path[1], window));
    }

    #[test]
    fn missing_msaa_uses_nested_hwnd_fallback_path() {
        let point = POINT { x: 25, y: 25 };
        let window = rect(0, 0, 200, 200);
        let panel = rect(10, 10, 150, 150);
        let child = rect(20, 20, 80, 80);

        let path = merge_hit_paths(Vec::new(), vec![child, panel, window], window, point);

        assert_eq!(path.len(), 3);
        assert!(same_rect(path[0], child));
        assert!(same_rect(path[1], panel));
        assert!(same_rect(path[2], window));
    }
}
