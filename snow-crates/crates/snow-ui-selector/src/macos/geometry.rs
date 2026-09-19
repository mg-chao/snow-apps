use crate::{PixelRect, Point};

#[derive(Clone, Copy, Debug, Default, PartialEq)]
pub(crate) struct Rect {
    pub x: f64,
    pub y: f64,
    pub width: f64,
    pub height: f64,
}
impl Rect {
    pub fn valid(self) -> bool {
        [
            self.x,
            self.y,
            self.width,
            self.height,
            self.right(),
            self.bottom(),
        ]
        .iter()
        .all(|v| v.is_finite())
            && self.width > 0.0
            && self.height > 0.0
    }
    pub fn right(self) -> f64 {
        self.x + self.width
    }
    pub fn bottom(self) -> f64 {
        self.y + self.height
    }
    pub fn contains(self, (x, y): (f64, f64)) -> bool {
        self.valid() && x >= self.x && y >= self.y && x < self.right() && y < self.bottom()
    }
    pub fn intersect(self, other: Self) -> Option<Self> {
        if !self.valid() || !other.valid() {
            return None;
        }
        let x = self.x.max(other.x);
        let y = self.y.max(other.y);
        let r = Self {
            x,
            y,
            width: self.right().min(other.right()) - x,
            height: self.bottom().min(other.bottom()) - y,
        };
        r.valid().then_some(r)
    }
    pub fn matches(self, other: Self) -> bool {
        self.valid()
            && other.valid()
            && [
                (self.x - other.x).abs(),
                (self.y - other.y).abs(),
                (self.right() - other.right()).abs(),
                (self.bottom() - other.bottom()).abs(),
            ]
            .iter()
            .all(|v| *v <= 2.0)
    }
}
#[derive(Clone, Debug, PartialEq)]
pub(crate) struct DisplayInfo {
    pub id: u32,
    pub bounds: Rect,
    pub width: u32,
    pub height: u32,
}
impl DisplayInfo {
    pub fn from_mode(
        id: u32,
        bounds: Rect,
        mode_width: usize,
        mode_height: usize,
        pixel_width: usize,
        pixel_height: usize,
    ) -> Option<Self> {
        if mode_width == 0 || mode_height == 0 || !bounds.valid() {
            return None;
        }
        // Mode dimensions may be unrotated. Derive backing scale first and apply
        // it to Quartz's oriented desktop bounds, including scaled Retina modes.
        let width = (bounds.width * pixel_width as f64 / mode_width as f64).round();
        let height = (bounds.height * pixel_height as f64 / mode_height as f64).round();
        if !(1.0..=f64::from(i32::MAX)).contains(&width)
            || !(1.0..=f64::from(i32::MAX)).contains(&height)
        {
            return None;
        }
        let display = Self {
            id,
            bounds,
            width: width as u32,
            height: height as u32,
        };
        display.valid().then_some(display)
    }

    pub fn valid(&self) -> bool {
        self.id != 0 && self.bounds.valid() && self.width > 0 && self.height > 0
    }
    // Capture frames retain Quartz desktop origins and use pixel-sized dimensions.
    // Scaling desktop origins would disagree with SnowCaptureFrameInfo on secondary screens.
    fn origin(&self) -> (f64, f64) {
        (self.bounds.x.round(), self.bounds.y.round())
    }
    pub fn physical_bounds(&self) -> Rect {
        let (x, y) = self.origin();
        Rect {
            x,
            y,
            width: f64::from(self.width),
            height: f64::from(self.height),
        }
    }
    pub fn to_points(&self, point: Point) -> (f64, f64) {
        let (x, y) = self.origin();
        (
            self.bounds.x + (f64::from(point.x) - x) * self.bounds.width / f64::from(self.width),
            self.bounds.y + (f64::from(point.y) - y) * self.bounds.height / f64::from(self.height),
        )
    }
    pub fn to_pixels(&self, rect: Rect) -> Option<PixelRect> {
        if !self.valid() || !rect.valid() {
            return None;
        }
        let (x, y) = self.origin();
        let sx = f64::from(self.width) / self.bounds.width;
        let sy = f64::from(self.height) / self.bounds.height;
        let edges = [
            (x + (rect.x - self.bounds.x) * sx).floor(),
            (y + (rect.y - self.bounds.y) * sy).floor(),
            (x + (rect.right() - self.bounds.x) * sx).ceil(),
            (y + (rect.bottom() - self.bounds.y) * sy).ceil(),
        ];
        if edges
            .iter()
            .any(|v| !v.is_finite() || *v < f64::from(i32::MIN) || *v > f64::from(i32::MAX))
        {
            return None;
        }
        Some(PixelRect {
            left: edges[0] as i32,
            top: edges[1] as i32,
            right: edges[2] as i32,
            bottom: edges[3] as i32,
        })
    }
}
pub(crate) fn query_position(
    displays: &[DisplayInfo],
    point: Point,
) -> Option<(&DisplayInfo, (f64, f64))> {
    let d = displays.iter().filter(|d| d.valid()).find(|d| {
        (point.display_id == 0 || point.display_id == d.id)
            && d.physical_bounds()
                .contains((f64::from(point.x), f64::from(point.y)))
    })?;
    Some((d, d.to_points(point)))
}
#[derive(Clone, Debug)]
pub(crate) struct WindowInfo {
    pub id: usize,
    pub pid: i32,
    pub bounds: Rect,
}
pub(crate) fn visible_window(
    id: usize,
    pid: i32,
    bounds: Rect,
    alpha: f64,
    layer: i32,
    excluded: &[usize],
) -> Option<WindowInfo> {
    // CGWindowListExcludeDesktopElements excludes wallpaper and desktop icons. Keep
    // positive layers: menus, sheets and floating palettes are selectable windows.
    (id != 0
        && pid > 0
        && bounds.valid()
        && alpha.is_finite()
        && alpha > 0.0
        && layer >= 0
        && !excluded.contains(&id))
    .then_some(WindowInfo { id, pid, bounds })
}
