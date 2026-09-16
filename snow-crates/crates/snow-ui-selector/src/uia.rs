mod cache;

use std::time::Duration;

use windows::Win32::Foundation::{E_POINTER, HWND, POINT, RECT};
use windows::Win32::System::Com::{CLSCTX_INPROC_SERVER, CoCreateInstance};
use windows::Win32::UI::Accessibility::{
    AutomationElementMode_Full, CUIAutomation8, IUIAutomation2, IUIAutomationCacheRequest,
    IUIAutomationElement, IUIAutomationElementArray, TreeScope, TreeScope_Children,
    TreeScope_Element, UIA_BoundingRectanglePropertyId, UIA_IsOffscreenPropertyId,
};
use windows::core::Result;

use crate::geometry::*;
use crate::spatial::*;
use crate::window;
use crate::{ElementRect, QueryControl, QueryResult, StopReason, WindowSnapshot};
use cache::{Batch, Candidate, Provider, QueryClock, WindowTree};

struct UiaWindow {
    hwnd: isize,
    bounds: RECT,
    tree: WindowTree<NativeProvider>,
}

pub(crate) struct UiaBackend {
    windows: Vec<UiaWindow>,
    window_index: WindowSpatialIndex,
    provider: NativeProvider,
}

impl UiaBackend {
    pub(crate) fn new_excluding_hwnds(excluded_hwnds: &[HWND]) -> Result<Self> {
        let provider = NativeProvider::new()?;
        let (windows, window_index) = build_uia_window_cache(excluded_hwnds)?;
        Ok(Self {
            windows,
            window_index,
            provider,
        })
    }

    pub(crate) fn refresh(&mut self, excluded_hwnds: &[HWND]) -> Result<()> {
        self.release_cache();
        let (windows, window_index) = build_uia_window_cache(excluded_hwnds)?;
        self.windows = windows;
        self.window_index = window_index;
        Ok(())
    }

    pub(crate) fn release_cache(&mut self) {
        self.windows = Vec::new();
        self.window_index.release_cache();
    }

    pub(crate) fn snapshot(&self) -> WindowSnapshot {
        WindowSnapshot(self.windows.iter().map(|w| (w.hwnd, w.bounds)).collect())
    }

    pub(crate) fn from_snapshot(snapshot: &WindowSnapshot) -> Result<Self> {
        let mut entries = Vec::new();
        let windows = snapshot
            .0
            .iter()
            .enumerate()
            .map(|(i, &(hwnd, bounds))| {
                entries.push(IndexedWindow {
                    envelope: rect_to_aabb(bounds),
                    cache_index: i,
                    z_order: i,
                });
                UiaWindow {
                    hwnd,
                    bounds,
                    tree: WindowTree::new(HWND(hwnd as *mut _), bounds),
                }
            })
            .collect();
        Ok(Self {
            windows,
            window_index: WindowSpatialIndex::build(entries),
            provider: NativeProvider::new()?,
        })
    }

    pub(crate) fn query(
        &mut self,
        point: POINT,
        mode: crate::HitTestMode,
        control: &QueryControl<'_>,
        progress: &mut dyn FnMut(&[ElementRect]),
    ) -> Result<QueryResult> {
        let Some(index) = self
            .window_index
            .window_at_point([point.x, point.y])
            .map(|e| e.cache_index)
        else {
            return Ok(QueryResult {
                path: None,
                reason: StopReason::Complete,
            });
        };
        let window = &mut self.windows[index];
        let (path, reason) = match mode {
            crate::HitTestMode::Window => (vec![window.bounds], StopReason::Complete),
            crate::HitTestMode::UiElement => window.tree.query(
                &mut self.provider,
                point,
                &QueryClock::new(),
                control,
                &mut |path| progress(&path.into_iter().map(ElementRect::new).collect::<Vec<_>>()),
            ),
        };
        Ok(QueryResult {
            path: Some(path.into_iter().map(ElementRect::new).collect()),
            reason,
        })
    }
}

struct NativeProvider {
    request: IUIAutomationCacheRequest,
    automation: IUIAutomation2,
}

impl NativeProvider {
    fn new() -> Result<Self> {
        let automation: IUIAutomation2 =
            unsafe { CoCreateInstance(&CUIAutomation8, None, CLSCTX_INPROC_SERVER)? };
        let request = unsafe { automation.CreateCacheRequest()? };
        unsafe {
            request.SetTreeScope(TreeScope(TreeScope_Element.0 | TreeScope_Children.0))?;
            // Control view removes layout-only panes that can obscure the content hit path.
            request.SetTreeFilter(&automation.ControlViewCondition()?)?;
            // Full references are needed only until each lazily loaded node has been expanded.
            request.SetAutomationElementMode(AutomationElementMode_Full)?;
            request.AddProperty(UIA_BoundingRectanglePropertyId)?;
            request.AddProperty(UIA_IsOffscreenPropertyId)?;
        }
        Ok(Self {
            request,
            automation,
        })
    }

    fn set_timeouts(&self, remaining: Duration) -> Result<()> {
        let milliseconds = remaining.as_millis().clamp(1, u128::from(u32::MAX)) as u32;
        unsafe {
            self.automation.SetConnectionTimeout(milliseconds)?;
            self.automation.SetTransactionTimeout(milliseconds)?;
        }
        Ok(())
    }
}

impl Provider for NativeProvider {
    type Element = IUIAutomationElement;
    type Batch = NativeBatch;

    fn root(&mut self, hwnd: HWND, remaining: Duration) -> Result<NativeBatch> {
        self.set_timeouts(remaining)?;
        NativeBatch::new(unsafe {
            self.automation
                .ElementFromHandleBuildCache(hwnd, &self.request)?
        })
    }

    fn children(
        &mut self,
        element: &IUIAutomationElement,
        remaining: Duration,
    ) -> Result<NativeBatch> {
        self.set_timeouts(remaining)?;
        NativeBatch::new(unsafe { element.BuildUpdatedCache(&self.request)? })
    }
}

struct NativeBatch {
    children: Option<IUIAutomationElementArray>,
    count: usize,
}

impl NativeBatch {
    fn new(element: IUIAutomationElement) -> Result<Self> {
        let children = match unsafe { element.GetCachedChildren() } {
            Ok(children) => Some(children),
            // UIA returns S_OK + null for an empty cached collection; windows-rs maps it to E_POINTER.
            Err(error) if error.code() == E_POINTER => None,
            Err(error) => return Err(error),
        };
        let count = match &children {
            Some(children) => unsafe { children.Length()? }.max(0) as usize,
            None => 0,
        };
        Ok(Self { children, count })
    }
}

impl Batch for NativeBatch {
    type Element = IUIAutomationElement;

    fn len(&self) -> usize {
        self.count
    }

    fn get(&self, index: usize) -> Result<Candidate<IUIAutomationElement>> {
        let element = unsafe {
            self.children
                .as_ref()
                .expect("nonempty collection")
                .GetElement(index as i32)?
        };
        let offscreen = unsafe { element.CachedIsOffscreen()?.as_bool() };
        let bounds = if offscreen {
            RECT::default()
        } else {
            unsafe { element.CachedBoundingRectangle()? }
        };
        Ok(Candidate {
            element,
            bounds,
            offscreen,
        })
    }
}

fn build_uia_window_cache(excluded_hwnds: &[HWND]) -> Result<(Vec<UiaWindow>, WindowSpatialIndex)> {
    let hwnds = window::enumerate_top_windows()?;
    let monitors = MonitorCache::new();
    Ok(collect_window_snapshot(hwnds, excluded_hwnds, |hwnd| {
        if window::is_window_cloaked(hwnd) {
            return None;
        }
        monitors.clip_rect_to_visible_area(window::visible_window_rect(hwnd)?)
    }))
}

fn collect_window_snapshot(
    hwnds: Vec<HWND>,
    excluded_hwnds: &[HWND],
    mut visible_bounds: impl FnMut(HWND) -> Option<RECT>,
) -> (Vec<UiaWindow>, WindowSpatialIndex) {
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
        let Some(bounds) = visible_bounds(hwnd) else {
            continue;
        };
        let cache_index = windows.len();
        windows.push(UiaWindow {
            hwnd: hwnd.0 as isize,
            bounds,
            tree: WindowTree::new(hwnd, bounds),
        });
        entries.push(IndexedWindow {
            envelope: rect_to_aabb(bounds),
            cache_index,
            z_order,
        });
    }
    (windows, WindowSpatialIndex::build(entries))
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::com::ComApartment;

    static UIA_TEST_LOCK: std::sync::Mutex<()> = std::sync::Mutex::new(());

    fn hwnd(value: usize) -> HWND {
        HWND(value as *mut _)
    }
    fn bounds() -> RECT {
        RECT {
            left: -10,
            top: -10,
            right: 100,
            bottom: 100,
        }
    }

    #[test]
    fn snapshot_excludes_overlays_before_querying_geometry_and_preserves_z_order() {
        for count in [4, 25] {
            let mut queried = Vec::new();
            let (windows, index) = collect_window_snapshot(
                (1..=count).map(hwnd).collect(),
                &[hwnd(2), hwnd(1), hwnd(2)],
                |window| {
                    queried.push(window);
                    (window != hwnd(3)).then_some(bounds())
                },
            );
            assert_eq!(queried[0], hwnd(3));
            assert!(!queried.contains(&hwnd(1)) && !queried.contains(&hwnd(2)));
            assert_eq!(windows.len(), count - 3);
            let front = index.window_at_point([5, 5]).unwrap();
            assert_eq!(front.z_order, 3);
            assert_eq!(front.cache_index, 0);
            assert_eq!(windows[0].bounds, bounds());
        }
    }

    #[test]
    fn refinement_reconstructs_exact_geometry_without_reenumerating_desktop() {
        let _serial = UIA_TEST_LOCK.lock().unwrap();
        let _com = ComApartment::new().unwrap();
        let (windows, window_index) =
            collect_window_snapshot(vec![hwnd(11), hwnd(22), hwnd(33)], &[hwnd(22)], |_| {
                Some(bounds())
            });
        let source = UiaBackend {
            windows,
            window_index,
            provider: NativeProvider::new().unwrap(),
        };
        let snapshot = source.snapshot();
        let mut refinement = UiaBackend::from_snapshot(&snapshot).unwrap();
        assert_eq!(refinement.snapshot().0, snapshot.0);
        assert_eq!(refinement.windows[0].hwnd, 11);
        assert_eq!(refinement.windows[1].hwnd, 33);
        let result = refinement
            .query(
                POINT { x: 5, y: 5 },
                crate::HitTestMode::Window,
                &QueryControl::foreground(),
                &mut |_| panic!("window lookup must not publish progress"),
            )
            .unwrap();
        assert_eq!(result.path.unwrap()[0].rect, bounds());
    }

    #[test]
    fn native_request_keeps_one_level_and_full_expansion_references() {
        let _serial = UIA_TEST_LOCK.lock().unwrap();
        let _com = ComApartment::new().unwrap();
        let provider = NativeProvider::new().unwrap();
        unsafe {
            assert_eq!(
                provider.request.TreeScope().unwrap(),
                TreeScope(TreeScope_Element.0 | TreeScope_Children.0)
            );
            assert_eq!(
                provider.request.AutomationElementMode().unwrap(),
                AutomationElementMode_Full
            );
            assert!(provider.request.TreeFilter().is_ok());
        }
    }

    #[test]
    fn window_queries_and_cache_release_do_not_need_live_elements() {
        let _serial = UIA_TEST_LOCK.lock().unwrap();
        let _com = ComApartment::new().unwrap();
        let provider = NativeProvider::new().unwrap();
        let (windows, window_index) =
            collect_window_snapshot(vec![hwnd(1)], &[], |_| Some(bounds()));
        let mut backend = UiaBackend {
            windows,
            window_index,
            provider,
        };
        let point = POINT { x: 5, y: 5 };
        let result = backend
            .query(
                point,
                crate::HitTestMode::Window,
                &QueryControl::foreground(),
                &mut |_| {},
            )
            .unwrap()
            .path
            .unwrap();
        assert_eq!(result.len(), 1);
        assert_eq!(result[0].left(), -10);
        backend.release_cache();
        assert!(
            backend
                .query(
                    point,
                    crate::HitTestMode::Window,
                    &QueryControl::foreground(),
                    &mut |_| {}
                )
                .unwrap()
                .path
                .is_none()
        );
        assert!(
            backend
                .query(
                    point,
                    crate::HitTestMode::UiElement,
                    &QueryControl::foreground(),
                    &mut |_| {}
                )
                .unwrap()
                .path
                .is_none()
        );
        let snapshot = collect_window_snapshot(vec![hwnd(1)], &[], |_| Some(bounds()));
        backend.windows = snapshot.0;
        backend.window_index = snapshot.1;
        assert!(
            backend
                .query(
                    point,
                    crate::HitTestMode::Window,
                    &QueryControl::foreground(),
                    &mut |_| {}
                )
                .unwrap()
                .path
                .is_some()
        );
    }
}
