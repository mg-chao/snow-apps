use std::sync::Arc;

#[derive(Clone, Copy, Debug, PartialEq, Eq, Hash, Default)]
pub enum CursorCompositionMode {
    #[default]
    AlphaBlend,
    MaskedColor,
}

#[derive(Clone, Copy, Debug, PartialEq, Eq, Hash)]
pub struct CursorShapeId(u64);

impl CursorShapeId {
    pub const fn from_raw(raw: u64) -> Self {
        Self(raw)
    }

    pub const fn get(self) -> u64 {
        self.0
    }
}

impl From<u64> for CursorShapeId {
    fn from(value: u64) -> Self {
        Self::from_raw(value)
    }
}

impl From<CursorShapeId> for u64 {
    fn from(value: CursorShapeId) -> Self {
        value.get()
    }
}

#[derive(Clone, Debug, PartialEq, Eq)]
pub struct CursorShape {
    pub shape_id: CursorShapeId,
    pub hotspot_x: u32,
    pub hotspot_y: u32,
    pub width: u32,
    pub height: u32,
    pub composition_mode: CursorCompositionMode,
    pub shape_rgba: Arc<[u8]>,
}

impl CursorShape {
    pub fn from_rgba(
        hotspot_x: u32,
        hotspot_y: u32,
        width: u32,
        height: u32,
        composition_mode: CursorCompositionMode,
        shape_rgba: impl Into<Arc<[u8]>>,
    ) -> Self {
        let shape_rgba = shape_rgba.into();
        Self {
            shape_id: hash_shape_fields(
                hotspot_x,
                hotspot_y,
                width,
                height,
                composition_mode,
                shape_rgba.as_ref(),
            ),
            hotspot_x,
            hotspot_y,
            width,
            height,
            composition_mode,
            shape_rgba,
        }
    }
}

#[derive(Clone, Debug, PartialEq, Eq)]
pub enum CursorShapeCapture {
    Captured(CursorShape),
    Unavailable,
}

impl CursorShapeCapture {
    pub fn shape(&self) -> Option<&CursorShape> {
        match self {
            Self::Captured(shape) => Some(shape),
            Self::Unavailable => None,
        }
    }

    pub fn into_shape(self) -> Option<CursorShape> {
        match self {
            Self::Captured(shape) => Some(shape),
            Self::Unavailable => None,
        }
    }

    pub fn shape_id(&self) -> Option<CursorShapeId> {
        self.shape().map(|shape| shape.shape_id)
    }
}

#[derive(Clone, Debug, PartialEq, Eq)]
pub struct CursorSnapshot {
    pub absolute_x: i32,
    pub absolute_y: i32,
    pub visible: bool,
    pub shape: CursorShapeCapture,
}

#[derive(Clone, Debug, PartialEq, Eq)]
pub enum CursorShapeState {
    /// Authentic pixels for the current shape. A stream may embed the same ID
    /// again after another shape, or on every frame to tolerate dropped packets.
    Embedded(CursorShape),
    /// Reuses the most recent embedded shape. Projected streams embed each
    /// transition, so consumers can release the previous shape's pixels.
    Cached(CursorShapeId),
    Unavailable,
}

impl CursorShapeState {
    pub fn shape_id(&self) -> Option<CursorShapeId> {
        match self {
            Self::Embedded(shape) => Some(shape.shape_id),
            Self::Cached(shape_id) => Some(*shape_id),
            Self::Unavailable => None,
        }
    }

    pub fn embedded_shape(&self) -> Option<&CursorShape> {
        match self {
            Self::Embedded(shape) => Some(shape),
            Self::Cached(_) | Self::Unavailable => None,
        }
    }
}

#[derive(Clone, Debug, PartialEq, Eq)]
pub struct AttachedCursorSample {
    pub x: i32,
    pub y: i32,
    pub visible: bool,
    pub shape: CursorShapeState,
}

impl AttachedCursorSample {
    pub fn shape_id(&self) -> Option<CursorShapeId> {
        self.shape.shape_id()
    }
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct CursorTargetInfo {
    pub origin_x: i32,
    pub origin_y: i32,
    pub width: u32,
    pub height: u32,
}

impl CursorTargetInfo {
    pub fn contains_relative(&self, x: i32, y: i32) -> bool {
        x >= 0 && y >= 0 && (x as u32) < self.width && (y as u32) < self.height
    }
}

fn hash_shape_fields(
    hotspot_x: u32,
    hotspot_y: u32,
    width: u32,
    height: u32,
    composition_mode: CursorCompositionMode,
    shape_rgba: &[u8],
) -> CursorShapeId {
    const FNV_OFFSET: u64 = 0xcbf29ce484222325;
    const FNV_PRIME: u64 = 0x100000001b3;

    fn hash_bytes(mut hash: u64, bytes: &[u8]) -> u64 {
        for byte in bytes {
            hash ^= u64::from(*byte);
            hash = hash.wrapping_mul(FNV_PRIME);
        }
        hash
    }

    let mut hash = FNV_OFFSET;
    hash = hash_bytes(hash, &hotspot_x.to_le_bytes());
    hash = hash_bytes(hash, &hotspot_y.to_le_bytes());
    hash = hash_bytes(hash, &width.to_le_bytes());
    hash = hash_bytes(hash, &height.to_le_bytes());
    hash = hash_bytes(
        hash,
        &[match composition_mode {
            CursorCompositionMode::AlphaBlend => 0,
            CursorCompositionMode::MaskedColor => 1,
        }],
    );
    CursorShapeId::from_raw(hash_bytes(hash, shape_rgba))
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn computed_shape_id_changes_when_pixels_change() {
        let left =
            CursorShape::from_rgba(1, 2, 2, 2, CursorCompositionMode::AlphaBlend, vec![1; 16]);
        let right =
            CursorShape::from_rgba(1, 2, 2, 2, CursorCompositionMode::AlphaBlend, vec![2; 16]);

        assert_ne!(left.shape_id, right.shape_id);
    }

    #[test]
    fn target_contains_relative_coordinates() {
        let target = CursorTargetInfo {
            origin_x: 100,
            origin_y: 200,
            width: 400,
            height: 300,
        };

        assert!(target.contains_relative(0, 0));
        assert!(target.contains_relative(399, 299));
        assert!(!target.contains_relative(-1, 0));
        assert!(!target.contains_relative(400, 0));
    }
}
