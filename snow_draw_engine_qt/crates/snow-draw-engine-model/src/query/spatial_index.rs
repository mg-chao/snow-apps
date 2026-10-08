use std::cell::RefCell;
use std::collections::HashMap;

use snow_draw_engine_core::{DrawRect, Point};
use snow_draw_engine_document::{Document, DocumentDelta, ElementId};

#[derive(Clone, Copy, Debug, PartialEq, Eq, Hash)]
struct CellKey {
    x: i32,
    y: i32,
}

#[derive(Clone, Debug)]
pub struct SpatialIndex {
    cell_size: f64,
    cells: HashMap<CellKey, Vec<ElementId>>,
    element_cells: Vec<Option<Vec<CellKey>>>,
    bounds: Vec<Option<DrawRect>>,
    scratch: RefCell<SpatialQueryScratch>,
}

#[derive(Clone, Debug, Default)]
struct SpatialQueryScratch {
    marks: Vec<u32>,
    stamp: u32,
    candidates: Vec<ElementId>,
}

impl Default for SpatialIndex {
    fn default() -> Self {
        Self::new(256.0)
    }
}

impl SpatialIndex {
    pub fn new(cell_size: f64) -> Self {
        Self {
            cell_size: cell_size.max(1.0),
            cells: HashMap::new(),
            element_cells: Vec::new(),
            bounds: Vec::new(),
            scratch: RefCell::new(SpatialQueryScratch::default()),
        }
    }

    pub fn rebuild(&mut self, document: &Document) {
        self.cells.clear();
        self.element_cells.clear();
        self.bounds.clear();

        for state in document.element_states() {
            if !state.visible {
                continue;
            }
            let Ok(bounds) = document.element_bounds(state.id) else {
                continue;
            };
            self.insert(state.id, bounds);
        }
    }

    pub fn refresh(&mut self, document: &Document, delta: &DocumentDelta) {
        let mut ids = delta.touched.clone();
        for id in &delta.removed {
            if !ids.contains(id) {
                ids.push(*id);
            }
        }

        for id in ids {
            self.remove(id);
            let Ok(element) = document.element(id) else {
                continue;
            };
            if !element.meta.visible {
                continue;
            }
            let Ok(bounds) = document.element_bounds(id) else {
                continue;
            };
            self.insert(id, bounds);
        }
    }

    pub fn with_viewport_candidate_marks<T>(
        &self,
        viewport: (f64, f64, f64, f64),
        f: impl FnOnce(&[u32], u32) -> T,
    ) -> T {
        self.with_candidate_marks(
            DrawRect::new(viewport.0, viewport.1, viewport.2, viewport.3),
            f,
        )
    }

    pub fn with_viewport_candidate_ids<T>(
        &self,
        viewport: (f64, f64, f64, f64),
        f: impl FnOnce(&[ElementId]) -> T,
    ) -> T {
        self.with_candidate_ids(
            DrawRect::new(viewport.0, viewport.1, viewport.2, viewport.3),
            f,
        )
    }

    pub fn with_point_candidate_marks<T>(
        &self,
        point: Point<f64>,
        tolerance: f64,
        f: impl FnOnce(&[u32], u32) -> T,
    ) -> T {
        let tolerance = tolerance.max(0.0);
        self.with_candidate_marks(
            DrawRect::new(
                point.x - tolerance,
                point.y - tolerance,
                point.x + tolerance,
                point.y + tolerance,
            ),
            f,
        )
    }

    pub fn with_point_candidate_ids<T>(
        &self,
        point: Point<f64>,
        tolerance: f64,
        f: impl FnOnce(&[ElementId]) -> T,
    ) -> T {
        let tolerance = tolerance.max(0.0);
        self.with_candidate_ids(
            DrawRect::new(
                point.x - tolerance,
                point.y - tolerance,
                point.x + tolerance,
                point.y + tolerance,
            ),
            f,
        )
    }

    pub fn bounds(&self, id: ElementId) -> Option<DrawRect> {
        self.bounds
            .get(id.index as usize)
            .and_then(|bounds| bounds.as_ref().copied())
    }

    /// Updates one retained geometry without rebuilding unrelated index entries.
    pub fn update_bounds(&mut self, id: ElementId, bounds: Option<DrawRect>) {
        self.remove(id);
        if let Some(bounds) = bounds {
            self.insert(id, bounds);
        }
    }

    fn insert(&mut self, id: ElementId, bounds: DrawRect) {
        let keys = self.keys_for_bounds(bounds);
        self.ensure_capacity(id.index as usize);
        for key in &keys {
            self.cells.entry(*key).or_default().push(id);
        }
        self.element_cells[id.index as usize] = Some(keys);
        self.bounds[id.index as usize] = Some(bounds);
    }

    fn remove(&mut self, id: ElementId) {
        let index = id.index as usize;
        if let Some(Some(keys)) = self.element_cells.get(index) {
            for key in keys {
                if let Some(ids) = self.cells.get_mut(key) {
                    ids.retain(|candidate| *candidate != id);
                    if ids.is_empty() {
                        self.cells.remove(key);
                    }
                }
            }
        }

        if index < self.element_cells.len() {
            self.element_cells[index] = None;
        }
        if index < self.bounds.len() {
            self.bounds[index] = None;
        }
    }

    fn ensure_capacity(&mut self, index: usize) {
        if self.element_cells.len() <= index {
            self.element_cells.resize(index + 1, None);
        }
        if self.bounds.len() <= index {
            self.bounds.resize(index + 1, None);
        }
    }

    fn with_candidate_marks<T>(&self, bounds: DrawRect, f: impl FnOnce(&[u32], u32) -> T) -> T {
        self.with_candidates(bounds, |scratch| f(&scratch.marks, scratch.stamp))
    }

    fn with_candidate_ids<T>(&self, bounds: DrawRect, f: impl FnOnce(&[ElementId]) -> T) -> T {
        self.with_candidates(bounds, |scratch| f(&scratch.candidates))
    }

    fn with_candidates<T>(&self, bounds: DrawRect, f: impl FnOnce(&SpatialQueryScratch) -> T) -> T {
        let mut scratch = self.scratch.borrow_mut();
        scratch.advance();
        self.for_each_key_in_bounds(bounds, |key| {
            if let Some(ids) = self.cells.get(&key) {
                for id in ids {
                    scratch.mark(*id);
                }
            }
        });
        f(&scratch)
    }

    fn keys_for_bounds(&self, bounds: DrawRect) -> Vec<CellKey> {
        let min_x = (bounds.min_x / self.cell_size).floor() as i32;
        let min_y = (bounds.min_y / self.cell_size).floor() as i32;
        let max_x = (bounds.max_x / self.cell_size).floor() as i32;
        let max_y = (bounds.max_y / self.cell_size).floor() as i32;

        let mut keys = Vec::new();
        for y in min_y..=max_y {
            for x in min_x..=max_x {
                keys.push(CellKey { x, y });
            }
        }
        keys
    }

    fn for_each_key_in_bounds(&self, bounds: DrawRect, mut f: impl FnMut(CellKey)) {
        let min_x = (bounds.min_x / self.cell_size).floor() as i32;
        let min_y = (bounds.min_y / self.cell_size).floor() as i32;
        let max_x = (bounds.max_x / self.cell_size).floor() as i32;
        let max_y = (bounds.max_y / self.cell_size).floor() as i32;

        for y in min_y..=max_y {
            for x in min_x..=max_x {
                f(CellKey { x, y });
            }
        }
    }
}

impl SpatialQueryScratch {
    fn advance(&mut self) {
        self.stamp = self.stamp.wrapping_add(1);
        self.candidates.clear();
        if self.stamp == 0 {
            self.marks.fill(0);
            self.stamp = 1;
        }
    }

    fn mark(&mut self, id: ElementId) {
        let index = id.index as usize;
        if self.marks.len() <= index {
            self.marks.resize(index + 1, 0);
        }
        if self.marks[index] == self.stamp {
            return;
        }
        self.marks[index] = self.stamp;
        self.candidates.push(id);
    }
}
