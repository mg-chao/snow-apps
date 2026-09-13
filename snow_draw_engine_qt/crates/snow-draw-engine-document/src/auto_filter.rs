use crate::{
    CanvasFilterType, Document, DocumentDelta, ElementData, ElementId, ElementKind, ElementMeta,
    FilterData, Operation, Transaction,
};
use serde::{Deserialize, Serialize};
use snow_draw_engine_core::{DrawRect, ErrorCode, Point};
use std::collections::HashSet;

#[derive(Clone, Debug, PartialEq, Serialize, Deserialize)]
pub struct AutoFilterRegion {
    pub id: u64,
    pub bounds: DrawRect,
    pub category: String,
}

#[derive(Clone, Debug, PartialEq, Serialize, Deserialize)]
pub struct AutoFilterRegionRecord {
    pub source_bounds: DrawRect,
    pub regions: Vec<AutoFilterRegion>,
}

impl AutoFilterRegionRecord {
    pub fn validate(&self) -> Result<(), ErrorCode> {
        let valid = |r: DrawRect| {
            [r.min_x, r.min_y, r.max_x, r.max_y]
                .iter()
                .all(|v| v.is_finite())
                && r.width() > 0.0
                && r.height() > 0.0
        };
        let mut ids = HashSet::new();
        if !valid(self.source_bounds)
            || self.regions.len() > 100_000
            || self.regions.iter().any(|r| {
                !valid(r.bounds)
                    || !ids.insert(r.id)
                    || r.category.is_empty()
                    || r.category.len() > 128
            })
        {
            return Err(ErrorCode::InvalidArgument);
        }
        Ok(())
    }

    pub fn region_at(&self, point: Point<f64>) -> Option<&AutoFilterRegion> {
        self.regions
            .iter()
            .filter(|r| {
                point.x >= r.bounds.min_x
                    && point.x < r.bounds.max_x
                    && point.y >= r.bounds.min_y
                    && point.y < r.bounds.max_y
            })
            .min_by(|a, b| {
                (a.bounds.width() * a.bounds.height())
                    .total_cmp(&(b.bounds.width() * b.bounds.height()))
                    .then(a.id.cmp(&b.id))
            })
    }
}

impl Document {
    pub fn auto_filter_regions(&self) -> Option<&AutoFilterRegionRecord> {
        self.auto_filter_regions.as_ref()
    }

    pub fn auto_filter_fill(&self, region: u64) -> Option<(ElementId, &FilterData)> {
        self.paint_order.iter().find_map(|id| {
            self.filter(*id)
                .ok()
                .filter(|f| f.auto_region_id == Some(region))
                .map(|f| (*id, f))
        })
    }

    pub fn auto_filter_record_transaction(
        &self,
        record: Option<AutoFilterRegionRecord>,
    ) -> Result<Transaction, ErrorCode> {
        if let Some(value) = &record {
            value.validate()?;
        }
        let mut transaction = Transaction::new(if record.is_some() {
            "identify filter regions"
        } else {
            "reset filter regions"
        });
        for id in &self.paint_order {
            if self.element(*id)?.data.kind() == ElementKind::AutoFilter {
                transaction.remove_element(*id);
            }
        }
        transaction.push(Operation::UpdateAutoFilterRegions { record });
        Ok(transaction)
    }

    pub fn auto_filter_fill_transaction(
        &self,
        ids: &[u64],
        filter_type: CanvasFilterType,
        strength: f64,
        toggle: bool,
    ) -> Transaction {
        let mut transaction = Transaction::new("fill filter regions");
        let Some(record) = &self.auto_filter_regions else {
            return transaction;
        };
        let mut next = self.peek_next_element_id();
        let mut visited = HashSet::new();
        for region in record.regions.iter().filter(|r| ids.contains(&r.id)) {
            if !visited.insert(region.id) {
                continue;
            }
            let current = self.auto_filter_fill(region.id);
            if let Some((id, fill)) = current
                && toggle
                && fill.filter_type == filter_type
            {
                transaction.remove_element(id);
                continue;
            }
            let fill = FilterData {
                auto_region_id: Some(region.id),
                center: region.bounds.center(),
                width: region.bounds.width(),
                height: region.bounds.height(),
                filter_type,
                strength: FilterData::normalized_strength(strength),
                ..FilterData::default()
            };
            if let Some((id, previous)) = current {
                if previous != &fill {
                    transaction.update_filter(id, fill);
                }
            } else {
                transaction.insert_filter(next, ElementMeta::default(), fill);
                next.index += 1;
            }
        }
        transaction
    }

    pub(crate) fn normalize_auto_filter_order(&mut self, changes: &mut DocumentDelta) {
        let mut fills: Vec<_> = self
            .paint_order
            .iter()
            .filter_map(|id| {
                self.filter(*id)
                    .ok()
                    .and_then(|f| f.auto_region_id.map(|region| (region, *id)))
            })
            .collect();
        if fills.is_empty() {
            return;
        }
        fills.sort_by_key(|(region, _)| *region);
        let ids: HashSet<_> = fills.iter().map(|(_, id)| *id).collect();
        let order: Vec<_> = fills
            .into_iter()
            .map(|(_, id)| id)
            .chain(
                self.paint_order
                    .iter()
                    .copied()
                    .filter(|id| !ids.contains(id)),
            )
            .collect();
        if self.paint_order != order {
            self.paint_order = order;
            changes.z_order_changed = true;
        }
    }

    pub(crate) fn validate_auto_filters(&self) -> Result<(), ErrorCode> {
        if let Some(record) = &self.auto_filter_regions {
            record.validate()?;
        }
        let mut filled = HashSet::new();
        for element in self.slots.iter().flatten() {
            if let ElementData::Filter(fill) = &element.data
                && let Some(region) = fill.auto_region_id
                && (fill.opacity != 1.0
                    || fill.rotation != 0.0
                    || !filled.insert(region)
                    || !self
                        .auto_filter_regions
                        .as_ref()
                        .is_some_and(|r| r.regions.iter().any(|r| r.id == region)))
            {
                return Err(ErrorCode::InvalidArgument);
            }
        }
        Ok(())
    }
}
