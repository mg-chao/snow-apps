use std::ffi::c_void;

#[derive(Clone, Copy, PartialEq, Eq)]
pub(crate) enum AccessibilityBackend {
    Uia,
    Msaa,
}

pub(crate) enum HitTestMode {
    UiElement,
    Window,
}

pub(crate) struct WindowHandle(pub(crate) *mut c_void);
pub(crate) struct ScreenPoint {
    pub(crate) x: i32,
    pub(crate) y: i32,
}

pub(crate) struct ElementRect(snow_macos::Window);

impl ElementRect {
    pub(crate) fn left(&self) -> i32 {
        self.0.x
    }
    pub(crate) fn top(&self) -> i32 {
        self.0.y
    }
    pub(crate) fn right(&self) -> i32 {
        self.0.x.saturating_add_unsigned(self.0.width)
    }
    pub(crate) fn bottom(&self) -> i32 {
        self.0.y.saturating_add_unsigned(self.0.height)
    }
}

pub(crate) struct ElementRegionService {
    backend: AccessibilityBackend,
    windows: Vec<snow_macos::Window>,
}

impl ElementRegionService {
    pub(crate) fn with_backend_excluding_hwnds(
        backend: AccessibilityBackend,
        excluded: &[WindowHandle],
    ) -> Result<Self, String> {
        let mut service = Self {
            backend,
            windows: Vec::new(),
        };
        service.refresh_excluding_hwnds(excluded)?;
        Ok(service)
    }

    pub(crate) fn backend(&self) -> AccessibilityBackend {
        self.backend
    }

    pub(crate) fn refresh_excluding_hwnds(
        &mut self,
        excluded: &[WindowHandle],
    ) -> Result<(), String> {
        self.windows = snow_macos::windows()?;
        self.windows.retain(|window| {
            !excluded
                .iter()
                .any(|handle| handle.0 as usize == window.id as usize)
        });
        Ok(())
    }

    pub(crate) fn release_cache(&mut self) {
        self.windows.clear();
    }

    pub(crate) fn hit_test_point(
        &mut self,
        point: ScreenPoint,
        mode: HitTestMode,
    ) -> Result<Option<Vec<ElementRect>>, String> {
        let Some(window) = self
            .windows
            .iter()
            .find(|window| window.contains(point.x, point.y))
            .copied()
        else {
            return Ok(None);
        };
        let child = match mode {
            HitTestMode::UiElement => snow_macos::window_element(window.id, point.x, point.y),
            HitTestMode::Window => None,
        };
        Ok(Some(selection_regions(window, child)))
    }
}

fn selection_regions(
    window: snow_macos::Window,
    child: Option<snow_macos::Window>,
) -> Vec<ElementRect> {
    let mut regions = Vec::new();
    if let Some(child) = child
        && child.width > 0
        && child.height > 0
        && (child.x, child.y, child.width, child.height)
            != (window.x, window.y, window.width, window.height)
    {
        regions.push(ElementRect(child));
    }
    regions.push(ElementRect(window));
    regions
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn accessible_control_precedes_window_and_missing_control_falls_back() {
        let window = snow_macos::Window {
            id: 5,
            x: -100,
            y: 0,
            width: 800,
            height: 600,
        };
        let control = snow_macos::Window {
            x: 30,
            y: 40,
            width: 90,
            height: 30,
            ..window
        };
        let regions = selection_regions(window, Some(control));
        assert_eq!(regions.len(), 2);
        assert_eq!(
            (
                regions[0].left(),
                regions[0].top(),
                regions[0].right(),
                regions[0].bottom()
            ),
            (30, 40, 120, 70)
        );
        assert_eq!(regions[1].left(), -100);
        assert_eq!(selection_regions(window, None).len(), 1);
        assert_eq!(selection_regions(window, Some(window)).len(), 1);
    }

    #[test]
    fn window_selection_preserves_front_to_back_order() {
        let front = snow_macos::Window {
            id: 42,
            x: -100,
            y: 0,
            width: 200,
            height: 100,
        };
        let back = snow_macos::Window {
            id: 43,
            width: 400,
            ..front
        };
        let mut service = ElementRegionService {
            backend: AccessibilityBackend::Uia,
            windows: vec![front, back],
        };
        let hit = service
            .hit_test_point(ScreenPoint { x: 0, y: 50 }, HitTestMode::Window)
            .unwrap()
            .unwrap();
        assert_eq!(hit[0].0.id, 42);
        let hit = service
            .hit_test_point(ScreenPoint { x: 100, y: 50 }, HitTestMode::Window)
            .unwrap()
            .unwrap();
        assert_eq!(hit[0].0.id, 43);
        service.release_cache();
        assert!(
            service
                .hit_test_point(ScreenPoint { x: 0, y: 50 }, HitTestMode::Window)
                .unwrap()
                .is_none()
        );
    }
}
