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
        _mode: HitTestMode,
    ) -> Result<Option<Vec<ElementRect>>, String> {
        // This first macOS backend snaps to windows. Element-level AX traversal is a separate
        // capability; UIA/MSAA preferences retain their stored values for Windows portability.
        Ok(self
            .windows
            .iter()
            .find(|window| window.contains(point.x, point.y))
            .copied()
            .map(|window| vec![ElementRect(window)]))
    }
}

#[cfg(test)]
mod tests {
    use super::*;

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
            .hit_test_point(ScreenPoint { x: 100, y: 50 }, HitTestMode::UiElement)
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
