//! Serializable detection results.

use crate::geometry::Rect;
use serde::Serialize;

#[derive(Clone, Debug, PartialEq, Eq, Serialize)]
pub struct Region {
    #[serde(flatten)]
    pub rect: Rect,
    #[serde(rename = "type")]
    pub kind: String,
}
pub(crate) fn region(rect: Rect, kind: &str) -> Region {
    Region {
        rect,
        kind: kind.into(),
    }
}
