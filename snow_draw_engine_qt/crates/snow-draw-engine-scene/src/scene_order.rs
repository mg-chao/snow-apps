use std::collections::HashMap;

use snow_draw_engine_display::{
    DisplayFilterType, DisplayItemId, FilterRenderSpec, SceneDisplayItem, SceneRenderRun,
};
use snow_draw_engine_document::ElementId;

use crate::item_conversions::display_item_id;

/// Geometry-free ordering input. None is a drawable/source boundary (including Smart Erase).
#[derive(Clone, Copy, Debug, PartialEq)]
pub(crate) struct OrderNode {
    pub id: ElementId,
    pub effect: Option<FilterRenderSpec>,
    pub smart_erase: bool,
}

impl OrderNode {
    pub fn new(id: ElementId, item: &SceneDisplayItem) -> Self {
        let effect = match item {
            SceneDisplayItem::Filter(filter)
                if filter.filter.filter_type != DisplayFilterType::SmartErase =>
            {
                let mut spec = filter.filter;
                spec.render_phase = 0;
                Some(spec)
            }
            _ => None,
        };
        Self {
            id,
            effect,
            smart_erase: matches!(item, SceneDisplayItem::Filter(f) if f.filter.filter_type == DisplayFilterType::SmartErase),
        }
    }
}

#[derive(Debug, Default)]
pub(crate) struct SceneOrderPlan {
    pub nodes: Vec<OrderNode>,
    positions: HashMap<ElementId, usize>,
    memberships: HashMap<DisplayItemId, (DisplayItemId, DisplayItemId)>,
}

impl SceneOrderPlan {
    pub fn new(nodes: Vec<OrderNode>) -> Self {
        let mut memberships = HashMap::new();
        let mut source = None;
        let mut effect = None;
        let mut previous_spec = None;
        for node in &nodes {
            let Some(spec) = node.effect else {
                source = None;
                previous_spec = None;
                continue;
            };
            let id = display_item_id(node.id);
            let source = *source.get_or_insert(id);
            if previous_spec != Some(spec) {
                effect = Some(id);
            }
            memberships.insert(id, (source, effect.unwrap_or(id)));
            previous_spec = Some(spec);
        }
        let positions = nodes.iter().enumerate().map(|(i, n)| (n.id, i)).collect();
        Self {
            nodes,
            memberships,
            positions,
        }
    }

    pub fn node(&self, id: ElementId) -> Option<&OrderNode> {
        self.positions.get(&id).map(|i| &self.nodes[*i])
    }

    pub fn project(&self, items: &[SceneDisplayItem]) -> Vec<SceneRenderRun> {
        let mut runs: Vec<SceneRenderRun> = Vec::new();
        for (index, item) in items.iter().enumerate() {
            let SceneDisplayItem::Filter(filter) = item else {
                continue;
            };
            if filter.filter.filter_type == DisplayFilterType::SmartErase {
                continue;
            }
            let &(source_pass, effect_run) = self
                .memberships
                .get(&filter.id)
                .expect("every displayed filter must belong to the uncropped order plan");
            if let Some(last) = runs.last_mut()
                && last.source_pass == source_pass
                && last.effect_run == effect_run
                && last.start + last.count == index as u32
            {
                last.count += 1;
            } else {
                runs.push(SceneRenderRun {
                    source_pass,
                    effect_run,
                    start: index as u32,
                    count: 1,
                });
            }
        }
        runs
    }
}

#[cfg(test)]
pub(crate) fn plan_for_items(items: &[SceneDisplayItem]) -> Vec<SceneRenderRun> {
    let nodes = items
        .iter()
        .enumerate()
        .map(|(index, item)| {
            let id = match item {
                SceneDisplayItem::Filter(filter) => ElementId {
                    index: filter.id.index,
                    generation: filter.id.generation,
                },
                _ => ElementId {
                    index: index as u32,
                    generation: 0,
                },
            };
            OrderNode::new(id, item)
        })
        .collect();
    SceneOrderPlan::new(nodes).project(items)
}
