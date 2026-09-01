use crate::{BindableState, ElementId, RectangleData};
use snow_draw_engine_core::arrow::BindableShape;

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum AnchorKind {
    Center,
    Left,
    Right,
    Top,
    Bottom,
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct AnchorRef {
    pub element_id: ElementId,
    pub anchor: AnchorKind,
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum ConnectorEndpoint {
    Start,
    End,
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum Binding {
    GroupMember {
        parent: ElementId,
        child: ElementId,
    },
    ConnectorAnchor {
        connector_id: ElementId,
        endpoint: ConnectorEndpoint,
        target: AnchorRef,
    },
}

#[derive(Clone, Debug, PartialEq)]
pub struct BindableElementState {
    pub(crate) id: ElementId,
    pub(crate) rect: RectangleData,
    pub(crate) paint_index: u32,
    pub(crate) locked: bool,
}

impl BindableElementState {
    pub fn rectangle_proxy(
        id: ElementId,
        rect: RectangleData,
        paint_index: u32,
        locked: bool,
    ) -> Self {
        Self {
            id,
            rect,
            paint_index,
            locked,
        }
    }

    pub fn id(&self) -> ElementId {
        self.id
    }

    pub(crate) fn arrow_bindable_state(&self) -> BindableState {
        BindableState {
            id: self.id,
            shape: BindableShape::Rectangle,
            x: self.rect.center.x - self.rect.width / 2.0,
            y: self.rect.center.y - self.rect.height / 2.0,
            width: self.rect.width,
            height: self.rect.height,
            angle: self.rect.rotation,
            stroke_width: self.rect.stroke_width,
            z_index: Some(self.paint_index as f64),
            background_opaque: Some(self.rect.fill.a != 0),
            binding_enabled: Some(!self.locked),
            interior_hit_enabled: Some(true),
            visibility_bounds: None,
        }
    }

    pub(crate) fn matches_arrow_bindable_id(&self, arrow_bindable_id: ElementId) -> bool {
        self.id == arrow_bindable_id
    }
}
