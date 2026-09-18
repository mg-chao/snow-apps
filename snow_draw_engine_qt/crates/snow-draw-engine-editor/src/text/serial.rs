use snow_draw_engine_core::ErrorCode;
use snow_draw_engine_document::{
    ElementId, ElementMeta, TextData, TextLayoutSize, Transaction, serial_number_bound_text_rect,
    text_with_measured_layout, validate_serial_number, validate_text, validate_text_layout_size,
};
use snow_draw_engine_model::DocumentModel;

pub(crate) struct SerialNumberTextCreationRequest<'a> {
    pub(crate) selected_ids: &'a [ElementId],
    pub(crate) default_text: &'a TextData,
    pub(crate) measured_layout: TextLayoutSize,
    pub(crate) next_text_id: ElementId,
}

#[derive(Clone, Debug, Default, PartialEq)]
pub(crate) struct SerialNumberTextCreationPlan {
    pub(crate) transaction: Transaction,
    pub(crate) single_text_id: Option<ElementId>,
}

pub(crate) fn create_serial_number_text_creation_plan(
    document: &DocumentModel,
    request: SerialNumberTextCreationRequest<'_>,
) -> Result<SerialNumberTextCreationPlan, ErrorCode> {
    let measured_layout = validate_text_layout_size(request.measured_layout)?;
    let target_ids = request
        .selected_ids
        .iter()
        .copied()
        .filter(|id| document.serial_number(*id).is_ok())
        .collect::<Vec<_>>();
    if target_ids.is_empty() {
        return Ok(SerialNumberTextCreationPlan::default());
    }
    let single_target = target_ids.len() == 1;

    let mut transaction = Transaction::new("create serial number text");
    let mut next_text_id = request.next_text_id;
    let mut bound_text_ids = Vec::new();
    for serial_id in target_ids {
        let serial = document.serial_number(serial_id)?.clone();
        if let Some(text_id) = document.bound_text_id_for_serial_number(serial_id) {
            bound_text_ids.push(text_id);
            continue;
        }

        let text_id = next_text_id;
        next_text_id.index = next_text_id.index.saturating_add(1);
        let mut text = request.default_text.clone();
        text.font_size = serial.font_size;
        let layout = serial_number_bound_text_rect(&serial, &text, measured_layout)?;
        text.center = layout.center;
        text.rotation = layout.rotation;
        // `serial_number_bound_text_rect` passes the measured size through, so
        // the shared helper stores the wrap rectangle and the ink together.
        text = text_with_measured_layout(&text, measured_layout)?;
        validate_text(&text)?;
        let mut updated_serial = serial;
        updated_serial.text_element_id = Some(text_id);
        validate_serial_number(&updated_serial)?;
        transaction.insert_text(text_id, ElementMeta::default(), text);
        transaction.update_serial_number(serial_id, updated_serial);
        bound_text_ids.push(text_id);
    }

    Ok(SerialNumberTextCreationPlan {
        transaction,
        single_text_id: single_target
            .then(|| bound_text_ids.first().copied())
            .flatten(),
    })
}

#[cfg(test)]
mod tests {
    use super::*;
    use snow_draw_engine_core::Point;
    use snow_draw_engine_document::{ElementData, Operation, SerialNumberData};

    fn insert_serial_number(document: &mut DocumentModel, serial: SerialNumberData) -> ElementId {
        let id = document.peek_next_element_id();
        let mut transaction = Transaction::new("insert serial");
        transaction.insert_serial_number(id, ElementMeta::default(), serial);
        document.apply_transaction(transaction).unwrap();
        id
    }

    fn insert_text(document: &mut DocumentModel, text: TextData) -> ElementId {
        let id = document.peek_next_element_id();
        let mut transaction = Transaction::new("insert text");
        transaction.insert_text(id, ElementMeta::default(), text);
        document.apply_transaction(transaction).unwrap();
        id
    }

    #[test]
    fn serial_number_text_plan_creates_and_binds_missing_text() {
        let mut document = DocumentModel::new();
        let serial_id = insert_serial_number(
            &mut document,
            SerialNumberData {
                center: Point::new(100.0, 50.0),
                diameter: 40.0,
                font_size: 42.0,
                ..SerialNumberData::default()
            },
        );
        let next_text_id = document.peek_next_element_id();
        let plan = create_serial_number_text_creation_plan(
            &document,
            SerialNumberTextCreationRequest {
                selected_ids: &[serial_id],
                default_text: &TextData {
                    text: "default".to_owned(),
                    font_size: 21.0,
                    ..TextData::default()
                },
                measured_layout: TextLayoutSize::new(120.0, 32.0),
                next_text_id,
            },
        )
        .unwrap();

        assert_eq!(plan.single_text_id, Some(next_text_id));
        let operations = plan.transaction.operations();
        assert_eq!(operations.len(), 2);
        let Operation::InsertElement { id, data, .. } = &operations[0] else {
            panic!("expected text insert");
        };
        assert_eq!(*id, next_text_id);
        let ElementData::Text(text) = data else {
            panic!("expected text data");
        };
        assert_eq!(text.text, "default");
        assert_eq!(text.font_size, 42.0);
        assert_eq!(text.width(), 120.0);
        assert_eq!(text.height(), 32.0);
        assert_eq!(text.center, Point::new(216.0, 50.0));
        let Operation::UpdateElementData { id, data } = &operations[1] else {
            panic!("expected serial update");
        };
        assert_eq!(*id, serial_id);
        let ElementData::SerialNumber(serial) = data else {
            panic!("expected serial data");
        };
        assert_eq!(serial.text_element_id, Some(next_text_id));
    }

    #[test]
    fn serial_number_text_plan_reuses_existing_bound_text_for_single_target() {
        let mut document = DocumentModel::new();
        let text_id = insert_text(
            &mut document,
            TextData {
                text: "existing".to_owned(),
                layout: TextLayoutSize::new(80.0, 24.0),
                ..TextData::default()
            },
        );
        let serial_id = insert_serial_number(
            &mut document,
            SerialNumberData {
                text_element_id: Some(text_id),
                ..SerialNumberData::default()
            },
        );

        let plan = create_serial_number_text_creation_plan(
            &document,
            SerialNumberTextCreationRequest {
                selected_ids: &[serial_id],
                default_text: &TextData::default(),
                measured_layout: TextLayoutSize::new(120.0, 32.0),
                next_text_id: document.peek_next_element_id(),
            },
        )
        .unwrap();

        assert!(plan.transaction.is_empty());
        assert_eq!(plan.single_text_id, Some(text_id));
    }

    #[test]
    fn serial_number_text_plan_creates_text_for_each_selected_serial_number() {
        let mut document = DocumentModel::new();
        let first_serial_id = insert_serial_number(
            &mut document,
            SerialNumberData {
                center: Point::new(0.0, 0.0),
                diameter: 40.0,
                ..SerialNumberData::default()
            },
        );
        let second_serial_id = insert_serial_number(
            &mut document,
            SerialNumberData {
                center: Point::new(200.0, 0.0),
                diameter: 40.0,
                ..SerialNumberData::default()
            },
        );
        let next_text_id = document.peek_next_element_id();
        let following_text_id = ElementId {
            index: next_text_id.index + 1,
            generation: next_text_id.generation,
        };

        let plan = create_serial_number_text_creation_plan(
            &document,
            SerialNumberTextCreationRequest {
                selected_ids: &[first_serial_id, second_serial_id],
                default_text: &TextData {
                    text: "note".to_owned(),
                    ..TextData::default()
                },
                measured_layout: TextLayoutSize::new(80.0, 24.0),
                next_text_id,
            },
        )
        .unwrap();

        assert_eq!(plan.single_text_id, None);
        let operations = plan.transaction.operations();
        assert_eq!(operations.len(), 4);

        let Operation::InsertElement {
            id: first_text_id,
            data: first_text_data,
            ..
        } = &operations[0]
        else {
            panic!("expected first text insert");
        };
        assert_eq!(*first_text_id, next_text_id);
        let ElementData::Text(first_text) = first_text_data else {
            panic!("expected first inserted text data");
        };
        assert_eq!(first_text.text, "note");

        let Operation::UpdateElementData {
            id: first_updated_id,
            data: first_serial_data,
        } = &operations[1]
        else {
            panic!("expected first serial update");
        };
        assert_eq!(*first_updated_id, first_serial_id);
        let ElementData::SerialNumber(first_serial) = first_serial_data else {
            panic!("expected first serial data");
        };
        assert_eq!(first_serial.text_element_id, Some(next_text_id));

        let Operation::InsertElement {
            id: second_text_id,
            data: second_text_data,
            ..
        } = &operations[2]
        else {
            panic!("expected second text insert");
        };
        assert_eq!(*second_text_id, following_text_id);
        let ElementData::Text(second_text) = second_text_data else {
            panic!("expected second inserted text data");
        };
        assert_eq!(second_text.text, "note");

        let Operation::UpdateElementData {
            id: second_updated_id,
            data: second_serial_data,
        } = &operations[3]
        else {
            panic!("expected second serial update");
        };
        assert_eq!(*second_updated_id, second_serial_id);
        let ElementData::SerialNumber(second_serial) = second_serial_data else {
            panic!("expected second serial data");
        };
        assert_eq!(second_serial.text_element_id, Some(following_text_id));
    }
}
