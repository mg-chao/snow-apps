use snow_draw_engine_document::{Document, DocumentDelta, ElementData, ElementId};
use std::collections::HashMap;

#[derive(Clone, Debug, Default)]
pub struct RelationIndex {
    arrow_labels: Vec<(ElementId, ElementId)>,
    label_owners: HashMap<ElementId, ElementId>,
    serials_by_text: HashMap<ElementId, Vec<ElementId>>,
    text_links: HashMap<ElementId, (ElementId, bool)>,
    rebuild_count: u64,
    bound_arrows_by_bindable: Vec<Vec<ElementId>>,
    bound_bindables_by_arrow: Vec<Vec<ElementId>>,
}

impl RelationIndex {
    pub fn rebuild(&mut self, document: &Document) {
        self.rebuild_count += 1;
        self.arrow_labels.clear();
        self.label_owners.clear();
        self.serials_by_text.clear();
        self.text_links.clear();
        for id in document.paint_order().iter().copied() {
            if let Some((text, is_arrow)) = Self::text_link(document, id) {
                self.text_links.insert(id, (text, is_arrow));
                if document.text(text).is_ok() {
                    if is_arrow {
                        self.arrow_labels.push((id, text));
                        self.label_owners.insert(text, id);
                    } else {
                        self.serials_by_text.entry(text).or_default().push(id);
                    }
                }
            }
        }
        self.bound_arrows_by_bindable.clear();
        self.bound_bindables_by_arrow.clear();

        for arrow_id in document.paint_order().iter().copied() {
            let Ok(arrow) = document.arrow(arrow_id) else {
                continue;
            };

            for bindable_id in arrow.bound_element_ids() {
                if document.element(bindable_id).is_err() {
                    continue;
                }
                self.ensure_bindable_capacity(bindable_id.index as usize);
                self.ensure_arrow_capacity(arrow_id.index as usize);

                let bound_arrows = &mut self.bound_arrows_by_bindable[bindable_id.index as usize];
                if !bound_arrows.contains(&arrow_id) {
                    bound_arrows.push(arrow_id);
                }

                let bound_bindables = &mut self.bound_bindables_by_arrow[arrow_id.index as usize];
                if !bound_bindables.contains(&bindable_id) {
                    bound_bindables.push(bindable_id);
                }
            }
        }
    }

    pub fn refresh(&mut self, document: &Document, delta: &DocumentDelta) {
        if delta.relations_changed
            || delta.z_order_changed
            || !delta.created.is_empty()
            || !delta.removed.is_empty()
            || delta
                .touched
                .iter()
                .any(|id| Self::text_link(document, *id) != self.text_links.get(id).copied())
        {
            self.rebuild(document);
        }
    }

    fn text_link(document: &Document, id: ElementId) -> Option<(ElementId, bool)> {
        match &document.element(id).ok()?.data {
            ElementData::Arrow(arrow) => arrow.text_element_id.map(|text| (text, true)),
            ElementData::SerialNumber(serial) => serial.text_element_id.map(|text| (text, false)),
            _ => None,
        }
    }
    pub fn rebuild_count(&self) -> u64 {
        self.rebuild_count
    }
    pub fn arrow_label_bindings(&self) -> &[(ElementId, ElementId)] {
        &self.arrow_labels
    }
    pub fn arrow_label_owner(&self, text: ElementId) -> Option<ElementId> {
        self.label_owners.get(&text).copied()
    }
    pub fn serials_for_text(&self, text: ElementId) -> &[ElementId] {
        self.serials_by_text
            .get(&text)
            .map(Vec::as_slice)
            .unwrap_or_default()
    }

    pub fn bound_arrow_ids(&self, bindable_id: ElementId) -> &[ElementId] {
        self.bound_arrows_by_bindable
            .get(bindable_id.index as usize)
            .map(Vec::as_slice)
            .unwrap_or(&[])
    }

    pub fn bound_bindable_ids(&self, arrow_id: ElementId) -> &[ElementId] {
        self.bound_bindables_by_arrow
            .get(arrow_id.index as usize)
            .map(Vec::as_slice)
            .unwrap_or(&[])
    }

    fn ensure_bindable_capacity(&mut self, index: usize) {
        if self.bound_arrows_by_bindable.len() <= index {
            self.bound_arrows_by_bindable
                .resize_with(index + 1, Vec::new);
        }
    }

    fn ensure_arrow_capacity(&mut self, index: usize) {
        if self.bound_bindables_by_arrow.len() <= index {
            self.bound_bindables_by_arrow
                .resize_with(index + 1, Vec::new);
        }
    }
}
