// Copyright (C) 2026 mg-chao
// SPDX-License-Identifier: Apache-2.0

/// Logical leading-edge coordinates, independent of horizontal RTL painting.
#[derive(Clone, Copy, Debug, Default, PartialEq)]
pub(super) struct Span {
    pub start: f32,
    pub length: f32,
}
impl Span {
    pub fn end(self) -> f32 {
        self.start + self.length
    }
    pub fn visible(self, offset: f32, viewport: f32) -> bool {
        self.start >= offset - 0.5 && self.end() <= offset + viewport + 0.5
    }
}
pub(super) fn reveal(span: Span, offset: f32, viewport: f32, total: f32) -> f32 {
    let offset = if span.start < offset || span.length > viewport {
        span.start
    } else if span.end() > offset + viewport {
        span.end() - viewport
    } else {
        offset
    };
    offset.clamp(0., (total - viewport).max(0.))
}
