//! Validated automation transactions, separate from internal document recovery JSON.
use super::*;
use serde::Deserialize;
use snow_draw_engine_core::{ColorRgba8, CornerRadii};
use snow_draw_engine_document::{
    ArrowData, ElementMeta, FillStyle, FilterData, FreeDrawData, FreeDrawStyle, HighlightShape,
    LinearElementKind, PenFilterData, RectangleData, RectangleElementKind, SerialNumberData,
    TextData, TextLayoutSize, Transaction,
};
const MAX_BYTES: usize = 1024 * 1024;
const MAX_OPERATIONS: usize = 256;
const MAX_POINTS: usize = 8192;
#[derive(Deserialize)]
#[serde(deny_unknown_fields)]
struct Batch {
    version: u32,
    #[serde(default = "default_label")]
    label: String,
    operations: Vec<Annotation>,
}
fn default_label() -> String {
    "MCP annotation".into()
}
#[derive(Deserialize)]
#[serde(tag = "type", rename_all = "snake_case", deny_unknown_fields)]
enum Annotation {
    Rectangle {
        bounds: [f64; 4],
        #[serde(default)]
        style: Style,
    },
    RoundedRectangle {
        bounds: [f64; 4],
        #[serde(default)]
        style: Style,
    },
    RectangleHighlight {
        bounds: [f64; 4],
        #[serde(default)]
        style: Style,
    },
    Arrow {
        points: Vec<[f64; 2]>,
        #[serde(default)]
        style: Style,
    },
    Line {
        points: Vec<[f64; 2]>,
        #[serde(default)]
        style: Style,
    },
    Freehand {
        points: Vec<[f64; 2]>,
        #[serde(default)]
        closed: bool,
        #[serde(default)]
        style: Style,
    },
    PenHighlight {
        points: Vec<[f64; 2]>,
        #[serde(default)]
        style: Style,
    },
    RectangleFilter {
        bounds: [f64; 4],
        #[serde(default)]
        style: Style,
    },
    PenFilter {
        points: Vec<[f64; 2]>,
        #[serde(default)]
        style: Style,
    },
    Text {
        bounds: [f64; 4],
        text: String,
        #[serde(default)]
        style: Style,
    },
    SerialNumber {
        center: [f64; 2],
        number: i64,
        #[serde(default)]
        style: Style,
    },
    Watermark {
        text: String,
        #[serde(default)]
        style: Style,
    },
    Spotlight {
        bounds: [f64; 4],
        #[serde(default)]
        style: Style,
    },
    Select {
        id: ElementId,
    },
    Delete {
        ids: Vec<ElementId>,
    },
}

#[derive(Deserialize)]
#[serde(default, deny_unknown_fields)]
struct Style {
    stroke: [u8; 4],
    fill: [u8; 4],
    color: [u8; 4],
    stroke_width: f64,
    opacity: f64,
    corner_radius: f64,
    rotation: f64,
    font_size: f64,
    font_family: String,
    diameter: f64,
    strength: f64,
    gap: f64,
    stroke_style: crate::StrokeStyle,
    arrow_type: crate::ArrowType,
    filter: Filter,
}
#[derive(Default, Deserialize)]
#[serde(rename_all = "snake_case")]
enum Filter {
    #[default]
    Mosaic,
    GaussianBlur,
    Grayscale,
    Inversion,
    Emboss,
}
impl Default for Style {
    fn default() -> Self {
        Self {
            stroke: [244, 33, 44, 255],
            fill: [0, 0, 0, 0],
            color: [244, 33, 44, 255],
            stroke_width: 3.0,
            opacity: 1.0,
            corner_radius: 12.0,
            rotation: 0.0,
            font_size: 24.0,
            font_family: String::new(),
            diameter: 32.0,
            strength: 0.5,
            gap: 56.0,
            stroke_style: Default::default(),
            arrow_type: Default::default(),
            filter: Default::default(),
        }
    }
}
impl Style {
    fn validate(&self) -> Result<(), ErrorCode> {
        let valid = [
            self.stroke_width,
            self.opacity,
            self.corner_radius,
            self.rotation,
            self.font_size,
            self.diameter,
            self.strength,
            self.gap,
        ]
        .iter()
        .all(|v| v.is_finite())
            && (0.0..=1024.0).contains(&self.stroke_width)
            && (0.0..=1.0).contains(&self.opacity)
            && (0.0..=1.0).contains(&self.strength)
            && (0.0..=10000.0).contains(&self.corner_radius)
            && (1.0..=1024.0).contains(&self.font_size)
            && (1.0..=10000.0).contains(&self.diameter)
            && (10.0..=200.0).contains(&self.gap)
            && self.font_family.len() <= 256;
        if valid {
            Ok(())
        } else {
            Err(ErrorCode::InvalidArgument)
        }
    }
    fn filter_type(&self) -> crate::CanvasFilterType {
        match self.filter {
            Filter::Mosaic => crate::CanvasFilterType::Mosaic,
            Filter::GaussianBlur => crate::CanvasFilterType::GaussianBlur,
            Filter::Grayscale => crate::CanvasFilterType::Grayscale,
            Filter::Inversion => crate::CanvasFilterType::Inversion,
            Filter::Emboss => crate::CanvasFilterType::Emboss,
        }
    }
}
fn rgba([r, g, b, a]: [u8; 4]) -> ColorRgba8 {
    ColorRgba8 { r, g, b, a }
}
fn bounds(b: [f64; 4]) -> Result<(Point<f64>, f64, f64), ErrorCode> {
    if b.iter().any(|v| !v.is_finite() || v.abs() > 1e6) || b[2] <= 0.0 || b[3] <= 0.0 {
        return Err(ErrorCode::InvalidArgument);
    }
    Ok((Point::new(b[0] + b[2] / 2.0, b[1] + b[3] / 2.0), b[2], b[3]))
}
fn points(p: Vec<[f64; 2]>) -> Result<Vec<Point<f64>>, ErrorCode> {
    if !(2..=MAX_POINTS).contains(&p.len())
        || p.iter().flatten().any(|v| !v.is_finite() || v.abs() > 1e6)
    {
        return Err(ErrorCode::InvalidArgument);
    }
    Ok(p.into_iter().map(|[x, y]| Point::new(x, y)).collect())
}
fn text_valid(text: &str) -> Result<(), ErrorCode> {
    if text.len() > 65536 || text.contains('\0') {
        Err(ErrorCode::InvalidArgument)
    } else {
        Ok(())
    }
}
fn rectangle(b: [f64; 4], s: &Style) -> Result<RectangleData, ErrorCode> {
    s.validate()?;
    let (center, width, height) = bounds(b)?;
    Ok(RectangleData {
        center,
        width,
        height,
        rotation: s.rotation,
        rectangle_kind: RectangleElementKind::Rectangle,
        highlight_shape: HighlightShape::Rectangle,
        fill: rgba(s.fill),
        fill_style: FillStyle::Solid,
        stroke: rgba(s.stroke),
        stroke_width: s.stroke_width,
        stroke_style: s.stroke_style,
        corner_radii: CornerRadii::default(),
        opacity: s.opacity,
    })
}
impl Engine {
    pub fn document_revision(&self) -> u64 {
        self.model.document_revision().0
    }
    /// Validates a complete batch before committing one normal history transaction.
    pub fn apply_annotation_json(
        &mut self,
        bytes: &[u8],
    ) -> Result<(MutationResult, Vec<u8>), ErrorCode> {
        if bytes.is_empty() || bytes.len() > MAX_BYTES {
            return Err(ErrorCode::InvalidArgument);
        }
        let batch: Batch = serde_json::from_slice(bytes).map_err(|_| ErrorCode::InvalidArgument)?;
        if batch.version != 1 {
            return Err(ErrorCode::Unsupported);
        }
        if batch.operations.is_empty()
            || batch.operations.len() > MAX_OPERATIONS
            || batch.label.len() > 128
        {
            return Err(ErrorCode::InvalidArgument);
        }
        let mut tx = Transaction::new(batch.label);
        let mut next = self.model.peek_next_element_id();
        let mut created = Vec::new();
        let mut selection = None;
        for op in batch.operations {
            let id = next;
            let meta = ElementMeta::default();
            let linear_kind = match &op {
                Annotation::Line { .. } => LinearElementKind::Line,
                Annotation::PenHighlight { .. } => LinearElementKind::PenHighlight,
                _ => LinearElementKind::Arrow,
            };
            match op {
                Annotation::Select { id } => {
                    let record = self.model.element(id)?;
                    tx.update_element_meta(id, record.meta);
                    selection = Some(id);
                    continue;
                }
                Annotation::Delete { ids } => {
                    if ids.is_empty() || ids.len() > MAX_OPERATIONS {
                        return Err(ErrorCode::InvalidArgument);
                    }
                    for id in ids {
                        self.model.element(id)?;
                        tx.remove_element(id);
                    }
                    continue;
                }
                Annotation::Watermark { text, style: s } => {
                    s.validate()?;
                    text_valid(&text)?;
                    tx.update_watermark(WatermarkConfig {
                        text,
                        color: rgba(s.color),
                        font_size: s.font_size,
                        font_family: s.font_family,
                        angle: s.rotation,
                        gap: s.gap,
                        opacity: s.opacity,
                        ..Default::default()
                    });
                    continue;
                }
                Annotation::Rectangle {
                    bounds: b,
                    style: s,
                } => {
                    tx.insert_rectangle(id, meta, rectangle(b, &s)?);
                }
                Annotation::RoundedRectangle {
                    bounds: b,
                    style: s,
                } => {
                    let mut r = rectangle(b, &s)?;
                    r.corner_radii = CornerRadii::splat(s.corner_radius);
                    tx.insert_rectangle(id, meta, r);
                }
                Annotation::RectangleHighlight {
                    bounds: b,
                    style: s,
                } => {
                    tx.insert_rectangle(
                        id,
                        meta,
                        rectangle(b, &s)?.into_highlight(HighlightShape::Rectangle),
                    );
                }
                Annotation::Spotlight {
                    bounds: b,
                    style: s,
                } => {
                    tx.insert_rectangle(id, meta, rectangle(b, &s)?.into_spotlight());
                    tx.update_spotlight(SpotlightConfig {
                        color: rgba(s.color),
                        opacity: s.opacity,
                    });
                }
                Annotation::RectangleFilter {
                    bounds: b,
                    style: s,
                } => {
                    s.validate()?;
                    let (center, width, height) = bounds(b)?;
                    tx.insert_filter(
                        id,
                        meta,
                        FilterData {
                            center,
                            width,
                            height,
                            rotation: s.rotation,
                            filter_type: s.filter_type(),
                            strength: s.strength,
                            opacity: s.opacity,
                            ..Default::default()
                        },
                    );
                }
                Annotation::PenFilter {
                    points: p,
                    style: s,
                } => {
                    s.validate()?;
                    let data = PenFilterData::from_global_points(
                        &points(p)?,
                        s.filter_type(),
                        s.strength,
                        s.stroke_width,
                        s.opacity,
                    )
                    .ok_or(ErrorCode::InvalidArgument)?;
                    tx.insert_pen_filter(id, meta, data);
                }
                Annotation::Freehand {
                    points: p,
                    closed,
                    style: s,
                } => {
                    s.validate()?;
                    let data = FreeDrawData::from_global_vertices(
                        &points(p)?,
                        Vec::new(),
                        closed,
                        FreeDrawStyle {
                            stroke: rgba(s.stroke),
                            stroke_width: s.stroke_width,
                            stroke_style: s.stroke_style,
                            fill: rgba(s.fill),
                            fill_style: FillStyle::Solid,
                            opacity: s.opacity,
                        },
                    )
                    .ok_or(ErrorCode::InvalidArgument)?;
                    tx.insert_free_draw(id, meta, data);
                }
                Annotation::Arrow {
                    points: p,
                    style: s,
                }
                | Annotation::Line {
                    points: p,
                    style: s,
                }
                | Annotation::PenHighlight {
                    points: p,
                    style: s,
                } => {
                    s.validate()?;
                    let mut data = ArrowData::from_global_points(
                        &points(p)?,
                        rgba(s.stroke),
                        s.stroke_width,
                        s.stroke_style,
                        s.arrow_type,
                        None,
                        Some(crate::Arrowhead::Arrow),
                    )
                    .ok_or(ErrorCode::InvalidArgument)?;
                    data.opacity = s.opacity;
                    data = match linear_kind {
                        LinearElementKind::Line => data.into_line(rgba(s.fill), FillStyle::Solid),
                        LinearElementKind::PenHighlight => data.into_pen_highlight(),
                        LinearElementKind::Arrow => data,
                    };
                    tx.insert_arrow(id, meta, data);
                }
                Annotation::Text {
                    bounds: b,
                    text,
                    style: s,
                } => {
                    s.validate()?;
                    text_valid(&text)?;
                    let (center, width, height) = bounds(b)?;
                    tx.insert_text(
                        id,
                        meta,
                        TextData {
                            center,
                            layout: TextLayoutSize::new(width, height),
                            text,
                            rotation: s.rotation,
                            color: rgba(s.color),
                            font_size: s.font_size,
                            font_family: snow_draw_engine_document::normalize_font_family(Some(
                                s.font_family,
                            )),
                            fill: rgba(s.fill),
                            opacity: s.opacity,
                            auto_resize: false,
                            ..Default::default()
                        },
                    );
                }
                Annotation::SerialNumber {
                    center: [x, y],
                    number,
                    style: s,
                } => {
                    s.validate()?;
                    if !x.is_finite() || !y.is_finite() || x.abs() > 1e6 || y.abs() > 1e6 {
                        return Err(ErrorCode::InvalidArgument);
                    }
                    tx.insert_serial_number(
                        id,
                        meta,
                        SerialNumberData {
                            center: Point::new(x, y),
                            number,
                            diameter: s.diameter,
                            color: rgba(s.color),
                            fill: rgba(s.fill),
                            font_size: s.font_size,
                            font_family: snow_draw_engine_document::normalize_font_family(Some(
                                s.font_family,
                            )),
                            stroke_width: s.stroke_width,
                            opacity: s.opacity,
                            ..Default::default()
                        },
                    );
                }
            }
            created.push(id);
            next.index = next.index.checked_add(1).ok_or(ErrorCode::InvalidState)?;
        }
        if let Some(id) = selection
            && tx.operations().iter().any(|op|matches!(op,snow_draw_engine_document::Operation::RemoveElement{id:removed} if *removed==id)) {return Err(ErrorCode::InvalidArgument);}
        let before = self.capture_session_snapshot();
        let previous_editor = self.editor.clone();
        if let Some(id) = selection {
            self.editor.select_element(&self.model, id)?;
        }
        let mutation = match self.commit_transaction(
            ViewportId(0),
            ApplyTransactionCommand {
                transaction: tx,
                history_undo_snapshot: Some(before),
            },
        ) {
            Ok(result) => result,
            Err(error) => {
                self.editor = previous_editor;
                return Err(error);
            }
        };
        let history = self.history_state();
        let result = serde_json::json!({"created_element_ids":created,"selected_element_ids":self.selected_ids(),"changed_viewport_ids":mutation.changed_viewports.iter().map(|id|id.0).collect::<Vec<_>>(),"can_undo":history.can_undo,"can_redo":history.can_redo});
        Ok((
            mutation,
            serde_json::to_vec(&result).map_err(|_| ErrorCode::Internal)?,
        ))
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use serde_json::json;
    fn apply(
        engine: &mut Engine,
        operations: serde_json::Value,
    ) -> Result<serde_json::Value, ErrorCode> {
        let (_, result) = engine.apply_annotation_json(
            &serde_json::to_vec(&json!({"version":1,"operations":operations})).unwrap(),
        )?;
        Ok(serde_json::from_slice(&result).unwrap())
    }
    #[test]
    fn annotation_families_form_one_undoable_transaction() {
        let mut engine = Engine::new(EngineConfig::default());
        let viewport = engine.create_viewport(ViewportConfig::default()).unwrap();
        let mut operations = Vec::new();
        for kind in [
            "rectangle",
            "rounded_rectangle",
            "rectangle_highlight",
            "rectangle_filter",
            "spotlight",
        ] {
            operations.push(json!({"type":kind,"bounds":[10,20,40,60]}));
        }
        for kind in ["arrow", "line", "freehand", "pen_highlight", "pen_filter"] {
            operations.push(json!({"type":kind,"points":[[0,0],[20,30],[40,10]]}));
        }
        operations.push(json!({"type":"text","bounds":[10,20,40,60],"text":"hello"}));
        operations.push(json!({"type":"serial_number","center":[20,30],"number":7}));
        operations.push(json!({"type":"watermark","text":"sample"}));
        for operation in &operations {
            let mut isolated = Engine::new(EngineConfig::default());
            assert!(
                apply(&mut isolated, json!([operation])).is_ok(),
                "failed operation: {operation}"
            );
        }
        let result = apply(&mut engine, json!(operations)).unwrap();
        assert_eq!(result["created_element_ids"].as_array().unwrap().len(), 12);
        assert!(
            result["changed_viewport_ids"]
                .as_array()
                .unwrap()
                .contains(&json!(viewport.0))
        );
        assert!(engine.history_state().can_undo);
        engine.undo().unwrap();
        assert!(!engine.history_state().can_undo);
        engine.redo().unwrap();
        assert!(engine.history_state().can_undo);
        let id = result["created_element_ids"][0].clone();
        let selected = apply(&mut engine, json!([{"type":"select","id":id}])).unwrap();
        assert_eq!(selected["selected_element_ids"], json!([id]));
        apply(&mut engine, json!([{"type":"delete","ids":[id]}])).unwrap();
        engine.undo().unwrap();
        assert!(apply(&mut engine, json!([{"type":"select","id":id}])).is_ok());
    }
    #[test]
    fn annotation_invalid_batches_leave_document_and_history_unchanged() {
        let invalid = vec![
            json!({"type":"rectangle","bounds":[0,0,-1,10]}),
            json!({"type":"text","bounds":[0,0,10,10],"text":"x".repeat(65537)}),
            json!({"type":"freehand","points":vec![[0,0];8193]}),
            json!({"type":"rectangle","bounds":[0,0,10,10],"style":{"stroke":[256,0,0,255]}}),
            json!({"type":"rectangle","bounds":[0,0,10,10],"style":{"opacity":1.1}}),
            json!({"type":"arrow","points":[[0,0],[1,1]],"style":{"arrow_type":"invalid"}}),
            json!({"type":"rectangle_filter","bounds":[0,0,10,10],"style":{"filter":"invalid"}}),
            json!({"type":"execute","code":"no"}),
            json!({"type":"rectangle","bounds":[0,0,10,10],"unknown":true}),
        ];
        for invalid in invalid {
            let mut engine = Engine::new(EngineConfig::default());
            let revision = engine.document_revision();
            assert!(
                apply(
                    &mut engine,
                    json!([{"type":"rectangle","bounds":[0,0,20,20]},invalid])
                )
                .is_err()
            );
            assert_eq!(engine.document_revision(), revision);
            assert!(!engine.history_state().can_undo);
        }
        let mut engine = Engine::new(EngineConfig::default());
        for input in [
            br#"{"version":2,"operations":[]}"#.as_slice(),
            br#"{"version":1,"operations":[{"type":"rectangle","bounds":[NaN,0,1,1]}]}"#,
            br#"{"version":1,"operations":[{"type":"rectangle","bounds":[1e999,0,1,1]}]}"#,
        ] {
            assert!(engine.apply_annotation_json(input).is_err());
        }
        assert!(
            engine
                .apply_annotation_json(&vec![b' '; 1024 * 1024 + 1])
                .is_err()
        );
        assert!(
            apply(
                &mut engine,
                json!(vec![json!({"type":"rectangle","bounds":[0,0,1,1]}); 257])
            )
            .is_err()
        );
    }
}
