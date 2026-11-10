//! Backend-independent vector lowering. The same shaped outlines feed preview and export.
use std::{
    collections::{HashMap, VecDeque},
    fmt::Write,
    sync::{Arc, Mutex, MutexGuard, OnceLock},
};
#[cfg(test)]
#[path = "vector_tests.rs"]
mod tests;

use snow_draw_engine::*;
use unicode_segmentation::UnicodeSegmentation;

use crate::CanvasError;

fn font_families(value: &str) -> Vec<String> {
    let mut families = Vec::new();
    let mut name = String::new();
    let mut quote = None;
    let mut escaped = false;
    for character in value.chars() {
        if escaped {
            name.push(character);
            escaped = false;
        } else if character == '\\' {
            escaped = true;
        } else if quote == Some(character) {
            quote = None;
        } else if quote.is_none() && matches!(character, '\'' | '"') {
            quote = Some(character);
        } else if quote.is_none() && character == ',' {
            if !name.trim().is_empty() {
                families.push(name.trim().to_owned());
            }
            name.clear();
        } else {
            name.push(character);
        }
    }
    if !name.trim().is_empty() {
        families.push(name.trim().to_owned());
    }
    families
}

struct GlyphPath(lyon::path::Builder);

impl Default for GlyphPath {
    fn default() -> Self {
        Self(lyon::path::Path::builder())
    }
}

impl rustybuzz::ttf_parser::OutlineBuilder for GlyphPath {
    fn move_to(&mut self, x: f32, y: f32) {
        self.0.begin(lyon::math::point(x, y));
    }
    fn line_to(&mut self, x: f32, y: f32) {
        self.0.line_to(lyon::math::point(x, y));
    }
    fn quad_to(&mut self, x1: f32, y1: f32, x: f32, y: f32) {
        self.0
            .quadratic_bezier_to(lyon::math::point(x1, y1), lyon::math::point(x, y));
    }
    fn curve_to(&mut self, x1: f32, y1: f32, x2: f32, y2: f32, x: f32, y: f32) {
        self.0.cubic_bezier_to(
            lyon::math::point(x1, y1),
            lyon::math::point(x2, y2),
            lyon::math::point(x, y),
        );
    }
    fn close(&mut self) {
        self.0.end(true);
    }
}

pub(crate) fn color(value: ColorRgba8) -> String {
    format!("#{:02x}{:02x}{:02x}", value.r, value.g, value.b)
}

pub(crate) fn path_commands(commands: impl IntoIterator<Item = PathCommand>) -> String {
    let mut path = String::new();
    for command in commands {
        match command {
            PathCommand::MoveTo { point: [x, y] } => write!(path, "M{x} {y} "),
            PathCommand::LineTo { point: [x, y] } => write!(path, "L{x} {y} "),
            PathCommand::QuadTo {
                control: [x, y],
                end: [ex, ey],
            } => {
                write!(path, "Q{x} {y} {ex} {ey} ")
            }
            PathCommand::CubicTo {
                control_1: [x, y],
                control_2: [x2, y2],
                end: [ex, ey],
            } => {
                write!(path, "C{x} {y} {x2} {y2} {ex} {ey} ")
            }
        }
        .expect("writing to String");
    }
    path
}

pub(crate) fn rectangle_path(item: &RectangleDisplayItem) -> String {
    let w = item.width / 2.0;
    let h = item.height / 2.0;
    match item.shape {
        DisplayRectangleShape::Ellipse => {
            format!("M{} 0 A{w} {h} 0 1 0 {w} 0 A{w} {h} 0 1 0 {} 0 Z", -w, -w)
        }
        DisplayRectangleShape::Diamond => {
            let mut path = path_commands(
                snow_draw_engine_document::RoundedDiamond::new(
                    item.width,
                    item.height,
                    item.corner_radii,
                )
                .path_commands(Point::new(0.0, 0.0), 0.0),
            );
            path.push('Z');
            path
        }
        DisplayRectangleShape::Rectangle => {
            let limit = w.min(h).max(0.0);
            let r = item.corner_radii;
            let [tl, tr, br, bl] = [r.top_left, r.top_right, r.bottom_right, r.bottom_left]
                .map(|value| value.clamp(0.0, limit));
            format!(
                "M{} {} H{} Q{w} {} {w} {} V{} Q{w} {h} {} {h} H{} Q{} {h} {} {} V{} Q{} {} {} {} Z",
                -w + tl,
                -h,
                w - tr,
                -h,
                -h + tr,
                h - br,
                w - br,
                -w + bl,
                -w,
                -w,
                h - bl,
                -h + tl,
                -w,
                -h,
                -w + tl,
                -h,
            )
        }
    }
}

fn transform(item: &RectangleDisplayItem) -> String {
    format!(
        "translate({} {}) rotate({})",
        item.center_x,
        item.center_y,
        item.rotation.to_degrees()
    )
}

struct SvgIds {
    item: usize,
    next: usize,
}

impl SvgIds {
    fn allocate(&mut self) -> String {
        let id = format!("item-{}-{}", self.item, self.next);
        self.next += 1;
        id
    }
}

fn fill_attributes(fill: ColorRgba8, style: DisplayFillStyle, id: &str) -> String {
    if !style.is_visible(fill) {
        return "fill=\"none\"".into();
    }
    let paint = match style {
        DisplayFillStyle::Line | DisplayFillStyle::CrossLine => format!("url(#{id})"),
        _ => color(fill),
    };
    format!(
        "fill=\"{paint}\" fill-opacity=\"{}\"",
        f64::from(fill.a) / 255.0
    )
}

fn pattern(fill: ColorRgba8, style: DisplayFillStyle, id: &str) -> String {
    if !matches!(style, DisplayFillStyle::Line | DisplayFillStyle::CrossLine)
        || !style.is_visible(fill)
    {
        return String::new();
    }
    let c = color(fill);
    let path = if style == DisplayFillStyle::Line {
        "M-2 2 L2 -2 M0 8 L8 0 M6 10 L10 6"
    } else {
        "M0 0 L8 8 M0 8 L8 0"
    };
    format!(
        r#"<defs><pattern id="{id}" width="8" height="8" patternUnits="userSpaceOnUse"><path d="{path}" stroke="{c}" stroke-width="1"/></pattern></defs>"#
    )
}

fn stroke_attributes(stroke: ColorRgba8, width: f64, style: StrokeStyle) -> String {
    let dash = match style {
        StrokeStyle::Solid => String::new(),
        StrokeStyle::Dashed => format!("stroke-dasharray=\"{} {}\"", width * 5.0, width * 3.0),
        StrokeStyle::Dotted => format!("stroke-dasharray=\"0 {}\"", width * 2.5),
    };
    format!(
        "stroke=\"{}\" stroke-opacity=\"{}\" stroke-width=\"{width}\" stroke-linecap=\"round\" stroke-linejoin=\"round\" {dash}",
        color(stroke),
        f64::from(stroke.a) / 255.0,
    )
}

fn rectangle(item: &RectangleDisplayItem, ids: &mut SvgIds) -> String {
    let id = ids.allocate();
    let path = rectangle_path(item);
    let fill = fill_attributes(item.fill, item.fill_style, &id);
    let stroke = stroke_attributes(item.stroke, item.stroke_width, item.stroke_style);
    format!(
        "{}<g transform=\"{}\" opacity=\"{}\"><path d=\"{path}\" {fill} {stroke}/></g>",
        pattern(item.fill, item.fill_style, &id),
        transform(item),
        item.opacity,
    )
}

fn arrow(item: &ArrowDisplayItem, background: ColorRgba8, ids: &mut SvgIds) -> String {
    let id = ids.allocate();
    let mut path = if item.geometry.chunks.is_empty() {
        path_commands(item.path_commands.iter().copied())
    } else {
        path_commands(
            item.geometry
                .chunks
                .iter()
                .flat_map(|chunk| chunk.commands.iter().copied()),
        )
    };
    if item.geometry.closed {
        path.push('Z');
    }
    let tapered = item.arrow_shaft_type == snow_draw_engine_core::arrow::ArrowShaftType::Tapered;
    let fill = if tapered {
        fill_attributes(item.stroke, DisplayFillStyle::Solid, &id)
    } else {
        fill_attributes(item.fill, item.fill_style, &id)
    };
    let stroke = stroke_attributes(
        item.stroke,
        if tapered { 0.0 } else { item.stroke_width },
        item.stroke_style,
    );
    let mut output = if tapered {
        String::new()
    } else {
        pattern(item.fill, item.fill_style, &id)
    };
    write!(output, "<g opacity=\"{}\">", item.opacity).unwrap();
    let clipped =
        item.bound_text_id.is_some() && !item.bound_text_overlay && item.label_bounds.is_some();
    if clipped {
        let clip_id = ids.allocate();
        let bounds = item.label_bounds.unwrap();
        let left = bounds.min_x - 5.0;
        let top = bounds.min_y - 5.0;
        let right = bounds.max_x + 5.0;
        let bottom = bounds.max_y + 5.0;
        write!(output,r#"<defs><clipPath id="{clip_id}"><path clip-rule="evenodd" d="M-100000000 -100000000 H100000000 V100000000 H-100000000 Z M{left} {top} H{right} V{bottom} H{left} Z"/></clipPath></defs><g clip-path="url(#{clip_id})">"#).unwrap();
    }
    if let Some(pressure) = &item.pressure_geometry {
        let outline = path_commands(
            pressure
                .chunks
                .iter()
                .flat_map(|chunk| chunk.commands.iter().copied()),
        );
        write!(
            output,
            "<path d=\"{outline}\" {}/>",
            fill_attributes(item.stroke, DisplayFillStyle::Solid, &id)
        )
        .unwrap();
    } else {
        write!(output, "<path d=\"{path}\" {fill} {stroke}/>").unwrap();
    }
    for head in &item.arrowhead_primitives {
        let fill_color = match head.fill_mode {
            ArrowheadDisplayFillMode::Stroke => item.stroke,
            ArrowheadDisplayFillMode::Background => background,
        };
        let fill = color(fill_color);
        let fill_alpha = f64::from(fill_color.a) / 255.0;
        let head_style = match head.dash_mode {
            ArrowheadDisplayDashMode::Inherit => item.stroke_style,
            ArrowheadDisplayDashMode::Solid => StrokeStyle::Solid,
            ArrowheadDisplayDashMode::DottedCap => StrokeStyle::Dotted,
        };
        let head_stroke = stroke_attributes(item.stroke, item.stroke_width, head_style);
        match head.kind {
            ArrowheadDisplayPrimitiveKind::Circle => {
                write!(
                    output,
                    "<circle cx=\"{}\" cy=\"{}\" r=\"{}\" fill=\"{fill}\" fill-opacity=\"{fill_alpha}\" {head_stroke}/>",
                    head.center[0],
                    head.center[1],
                    head.diameter / 2.0
                )
                .unwrap();
            }
            ArrowheadDisplayPrimitiveKind::Line | ArrowheadDisplayPrimitiveKind::Polygon => {
                let mut polygon = String::new();
                for (index, [x, y]) in head.points.iter().enumerate() {
                    write!(polygon, "{}{x} {y} ", if index == 0 { "M" } else { "L" }).unwrap();
                }
                let fill = if head.kind == ArrowheadDisplayPrimitiveKind::Polygon {
                    polygon.push('Z');
                    fill
                } else {
                    "none".into()
                };
                write!(
                    output,
                    "<path d=\"{polygon}\" fill=\"{fill}\" fill-opacity=\"{fill_alpha}\" {head_stroke}/>"
                )
                .unwrap();
            }
        }
    }
    if clipped {
        output.push_str("</g>");
    }
    output.push_str("</g>");
    output
}

#[derive(Clone)]
pub(crate) struct ShapedGlyph {
    pub font: usvg::fontdb::ID,
    pub id: u16,
    pub x: f64,
    pub y: f64,
    pub scale: f64,
}

/// Immutable font asset and native color-glyph geometry. Positions use font
/// units with y upwards, matching ordinary outline meshes.
pub(crate) struct ColorGlyph {
    pub font_id: usvg::fontdb::ID,
    pub glyph_id: u16,
    pub face_index: u32,
    pub source: usvg::fontdb::Source,
    pub units_per_em: f64,
    pub bounds: [f64; 4],
}

#[derive(Clone)]
pub(crate) struct TextLine {
    pub text: Arc<str>,
    pub width: f64,
    pub start: usize,
    pub carets: Arc<[(usize, f64)]>,
    pub clusters: Arc<[(std::ops::Range<usize>, f64, f64)]>,
    pub ink_reserve: f64,
    pub glyphs: Arc<[ShapedGlyph]>,
}

impl TextLine {
    pub fn caret_x(&self, byte: usize) -> f64 {
        self.carets
            .iter()
            .find(|(index, _)| *index == byte)
            .map_or(self.width, |(_, x)| *x)
    }
    pub fn byte_at_x(&self, x: f64) -> usize {
        self.carets
            .iter()
            .min_by(|(_, a), (_, b)| (a - x).abs().total_cmp(&(b - x).abs()))
            .map_or(0, |(index, _)| *index)
    }
    pub fn selection_segments(&self, range: std::ops::Range<usize>) -> Vec<(f64, f64)> {
        let mut segments: Vec<_> = self
            .clusters
            .iter()
            .filter(|(cluster, _, _)| cluster.start < range.end && cluster.end > range.start)
            .map(|(_, a, b)| (a.min(*b), a.max(*b)))
            .collect();
        segments.sort_by(|a, b| a.0.total_cmp(&b.0));
        let mut merged: Vec<(f64, f64)> = Vec::new();
        for (left, right) in segments {
            if let Some(last) = merged.last_mut()
                && left <= last.1 + 0.01
            {
                last.1 = last.1.max(right);
            } else {
                merged.push((left, right));
            }
        }
        merged
    }
}

#[derive(Clone)]
pub(crate) struct TextLayout {
    pub lines: Vec<TextLine>,
    pub width: f64,
    pub height: f64,
    pub line_height: f64,
    pub ink_reserve: f64,
}

/// Immutable shaped runs are shared by interaction, GPU geometry and exports.
/// Font discovery is lazy and occurs once for the process, including when new
/// canvases and export workers are created.
struct SharedTextCache {
    fontdb: Arc<usvg::fontdb::Database>,
    select_fallback: usvg::FallbackSelectionFn<'static>,
    font_bytes: crate::font_cache::Cache,
    fallbacks: crate::font_cache::FallbackSelections,
    lines: HashMap<LineKey, (TextLine, usize)>,
    order: VecDeque<LineKey>,
    bytes: usize,
    color_glyphs: HashMap<(usvg::fontdb::ID, u16), Option<Arc<ColorGlyph>>>,
}

#[derive(Clone, PartialEq, Eq, Hash)]
struct LineKey {
    family: Arc<str>,
    text: Arc<str>,
    size: u64,
    bold: bool,
    italic: bool,
}

fn shared_text() -> MutexGuard<'static, SharedTextCache> {
    static CACHE: OnceLock<Mutex<SharedTextCache>> = OnceLock::new();
    CACHE
        .get_or_init(|| {
            let (select_fallback, font_bytes) = crate::font_cache::fallback_resolver();
            let mut fontdb = usvg::fontdb::Database::new();
            fontdb.load_system_fonts();
            Mutex::new(SharedTextCache {
                fontdb: Arc::new(fontdb),
                select_fallback,
                font_bytes,
                fallbacks: Default::default(),
                lines: HashMap::new(),
                order: VecDeque::new(),
                bytes: 0,
                color_glyphs: HashMap::new(),
            })
        })
        .lock()
        .unwrap_or_else(std::sync::PoisonError::into_inner)
}

pub(crate) fn resolve_fallback(
    character: char,
    used: &[usvg::fontdb::ID],
    database: &mut Arc<usvg::fontdb::Database>,
) -> Option<usvg::fontdb::ID> {
    let mut shared = shared_text();
    let SharedTextCache {
        fontdb,
        select_fallback,
        fallbacks,
        ..
    } = &mut *shared;
    // The inventory is fixed after discovery. Later additions own the bytes of
    // an existing face and introduce no new glyph repertoire or style choice.
    let selected = fallbacks.get_or_resolve(character, used, || {
        (select_fallback)(character, used, fontdb)
    });
    *database = fontdb.clone();
    selected
}

/// Shared text shaping and portable outline serialization.
pub(crate) struct VectorContext {
    clear_color: ColorRgba8,
}

impl Default for VectorContext {
    fn default() -> Self {
        Self {
            clear_color: ColorRgba8 {
                r: 255,
                g: 255,
                b: 255,
                a: 255,
            },
        }
    }
}

impl VectorContext {
    pub fn set_clear_color(&mut self, color: ColorRgba8) {
        self.clear_color = color;
    }

    /// Serialize an outlined fragment with document-unique definition IDs.
    pub fn export_svg_content(
        &mut self,
        item: &SceneDisplayItem,
        index: usize,
    ) -> Result<String, CanvasError> {
        self.content(
            item,
            &mut SvgIds {
                item: index,
                next: 0,
            },
        )
    }

    pub fn text_width(&mut self, text: &str, style: &TextStyle) -> f64 {
        self.shape_line(text, style).width
    }

    fn shape_line(&mut self, text: &str, style: &TextStyle) -> TextLine {
        let key = LineKey {
            family: style.font_family.as_deref().unwrap_or("Segoe UI").into(),
            text: text.into(),
            size: style.font_size.to_bits(),
            bold: style.bold,
            italic: style.italic,
        };
        let mut shared = shared_text();
        if let Some((line, _)) = shared.lines.get(&key) {
            return line.clone();
        }
        let font = shared.resolve_font(style);
        let database = shared.fontdb.clone();
        drop(shared);
        // Shaping can be lengthy on an export worker. Only
        // cache lookup and additive font resolution hold the process lock.
        let line = font
            .and_then(|font| crate::text_shaping::shape(database, text, style, font))
            .unwrap_or_else(|| TextLine {
                text: text.into(),
                width: 0.0,
                start: 0,
                carets: vec![(0, 0.0)].into(),
                clusters: Default::default(),
                ink_reserve: 0.0,
                glyphs: Default::default(),
            });
        let mut shared = shared_text();
        if let Some((existing, _)) = shared.lines.get(&key) {
            return existing.clone();
        }
        shared.store_line(key, line)
    }

    /// Font-unit paths shared by GPU tessellation and portable vector export.
    pub fn glyph_path(&self, font: usvg::fontdb::ID, glyph: u16) -> Option<lyon::path::Path> {
        let database = shared_text().fontdb.clone();
        database.with_face_data(font, |bytes, index| {
            let face = rustybuzz::ttf_parser::Face::parse(bytes, index).ok()?;
            let mut outline = GlyphPath::default();
            face.outline_glyph(rustybuzz::ttf_parser::GlyphId(glyph), &mut outline)?;
            Some(outline.0.build())
        })?
    }

    pub fn color_glyph(&self, font: usvg::fontdb::ID, glyph: u16) -> Option<Arc<ColorGlyph>> {
        let key = (font, glyph);
        let shared = shared_text();
        if let Some(cached) = shared.color_glyphs.get(&key) {
            return cached.clone();
        }
        let database = shared.fontdb.clone();
        drop(shared);
        let info = database.face(font)?;
        let glyph = database.with_face_data(font, |bytes, index| {
            let face = rustybuzz::ttf_parser::Face::parse(bytes, index).ok()?;
            let id = rustybuzz::ttf_parser::GlyphId(glyph);
            if !face.is_color_glyph(id)
                && face.glyph_svg_image(id).is_none()
                && face.glyph_raster_image(id, u16::MAX).is_none()
            {
                return None;
            }
            // COLR v0 layers may extend outside the monochrome base glyph.
            // The font-wide bounds include every layer outline; v1 clip boxes
            // below provide a tighter, authoritative rectangle when available.
            let bbox = if face.is_color_glyph(id) {
                face.global_bounding_box()
            } else {
                face.glyph_bounding_box(id)
                    .unwrap_or_else(|| face.global_bounding_box())
            };
            let mut bounds = [bbox.x_min, bbox.y_min, bbox.x_max, bbox.y_max].map(f64::from);
            if let Some(clip) = face
                .tables()
                .colr
                .and_then(|colr| colr.clip_box(id, face.variation_coordinates()))
            {
                bounds = [clip.x_min, clip.y_min, clip.x_max, clip.y_max].map(f64::from);
            }
            Some(Arc::new(ColorGlyph {
                font_id: font,
                glyph_id: glyph,
                face_index: index,
                source: info.source.clone(),
                units_per_em: f64::from(face.units_per_em()),
                bounds,
            }))
        })?;
        let mut shared = shared_text();
        if shared.color_glyphs.len() >= 8192 {
            shared.color_glyphs.clear();
        }
        shared.color_glyphs.insert(key, glyph.clone());
        glyph
    }

    pub fn item_uses_color_fonts(&mut self, item: &SceneDisplayItem) -> bool {
        let item = match item {
            SceneDisplayItem::Text(item) => item.clone(),
            SceneDisplayItem::SerialNumber(item)
                if item.serial_number_type != DisplaySerialNumberType::Circle =>
            {
                TextDisplayItem {
                    text: item.label.clone(),
                    font_family: item.font_family.clone(),
                    font_size: item.font_size,
                    ..Default::default()
                }
            }
            _ => return false,
        };
        let style = TextStyle {
            color: item.color,
            font_size: item.font_size.max(1.0),
            bold: item.bold,
            italic: item.italic,
            font_family: item.font_family,
            fill: item.fill,
            fill_style: FillStyle::Solid,
            stroke: item.stroke,
            stroke_width: item.stroke_width,
            corner_radii: item.corner_radii,
            horizontal_align: TextHorizontalAlign::Left,
            vertical_align: TextVerticalAlign::Center,
            opacity: item.opacity,
        };
        item.text.split('\n').any(|line| {
            self.shape_line(line, &style)
                .glyphs
                .iter()
                .any(|glyph| self.color_glyph(glyph.font, glyph.id).is_some())
        })
    }
}

impl SharedTextCache {
    fn resolve_font(&mut self, style: &TextStyle) -> Option<usvg::fontdb::ID> {
        use usvg::fontdb::{Family, Query, Stretch, Style, Weight};
        let names = font_families(style.font_family.as_deref().unwrap_or("Segoe UI"));
        let families: Vec<_> = names
            .iter()
            .map(|family| match family.as_str() {
                "serif" => Family::Serif,
                "sans-serif" => Family::SansSerif,
                "monospace" => Family::Monospace,
                "cursive" => Family::Cursive,
                "fantasy" => Family::Fantasy,
                name => Family::Name(name),
            })
            .chain([Family::SansSerif])
            .collect();
        let font = self.fontdb.query(&Query {
            families: &families,
            weight: if style.bold {
                Weight::BOLD
            } else {
                Weight::NORMAL
            },
            stretch: Stretch::Normal,
            style: if style.italic {
                Style::Italic
            } else {
                Style::Normal
            },
        })?;
        crate::font_cache::owned_font(font, &mut self.fontdb, &self.font_bytes)
    }

    fn store_line(&mut self, key: LineKey, line: TextLine) -> TextLine {
        let bytes = std::mem::size_of::<LineKey>() * 2
            + key.family.len()
            + key.text.len()
            + line.text.len()
            + line.carets.len() * std::mem::size_of::<(usize, f64)>()
            + line.clusters.len() * std::mem::size_of::<(std::ops::Range<usize>, f64, f64)>()
            + line.glyphs.len() * std::mem::size_of::<ShapedGlyph>();
        const LINE_BUDGET: usize = 16 * 1024 * 1024;
        if bytes > LINE_BUDGET {
            return line;
        }
        while self.lines.len() >= 4096 || self.bytes.saturating_add(bytes) > LINE_BUDGET {
            let Some(oldest) = self.order.pop_front() else {
                break;
            };
            if let Some((_, bytes)) = self.lines.remove(&oldest) {
                self.bytes -= bytes;
            }
        }
        self.order.push_back(key.clone());
        self.lines.insert(key, (line.clone(), bytes));
        self.bytes += bytes;
        line
    }
}

impl VectorContext {
    pub fn layout(&mut self, text: &str, style: &TextStyle, max_width: f64) -> TextLayout {
        let mut lines = Vec::new();
        for paragraph in text.split('\n') {
            // Shape a paragraph once to choose breaks. Shaping every growing word
            // prefix makes a long paste quadratic. Final lines are shaped again
            // because joining and ligatures can change at a line boundary.
            let natural = self.shape_line(paragraph, style);
            if natural.width <= max_width || paragraph.is_empty() {
                lines.push(natural);
                continue;
            }
            let advances: HashMap<_, _> = natural
                .clusters
                .iter()
                .map(|(range, left, right)| (range.start, right - left))
                .collect();
            let mut boundaries = vec![(0, 0.0)];
            let mut advance = 0.0;
            for (index, grapheme) in paragraph.grapheme_indices(true) {
                advance += advances.get(&index).copied().unwrap_or(style.font_size);
                boundaries.push((index + grapheme.len(), advance));
            }
            let word_ends: Vec<_> = paragraph
                .split_word_bound_indices()
                .map(|(index, word)| index + word.len())
                .collect();
            let mut start = 0;
            while start + 1 < boundaries.len() {
                let target = boundaries[start].1 + max_width.max(0.0);
                let mut end = boundaries
                    .partition_point(|(_, width)| *width <= target)
                    .saturating_sub(1)
                    .max(start + 1);
                let word = word_ends.partition_point(|byte| *byte <= boundaries[end].0);
                if word > 0 && word_ends[word - 1] > boundaries[start].0 {
                    end = boundaries
                        .binary_search_by_key(&word_ends[word - 1], |b| b.0)
                        .unwrap_or(end);
                }
                let mut shaped =
                    self.shape_line(&paragraph[boundaries[start].0..boundaries[end].0], style);
                if shaped.width > max_width && end > start + 1 {
                    // Bounded binary correction for context-sensitive shaping.
                    let (mut low, mut high) = (start + 1, end);
                    while low + 1 < high {
                        let middle = (low + high) / 2;
                        let probe = self.shape_line(
                            &paragraph[boundaries[start].0..boundaries[middle].0],
                            style,
                        );
                        if probe.width <= max_width {
                            low = middle;
                        } else {
                            high = middle;
                        }
                    }
                    end = low;
                    shaped =
                        self.shape_line(&paragraph[boundaries[start].0..boundaries[end].0], style);
                }
                lines.push(shaped);
                start = end;
            }
        }
        let mut offset = 0;
        for line in &mut lines {
            line.start = offset;
            offset += line.text.len();
            if text[offset..].starts_with('\n') {
                offset += 1;
            }
        }
        let width = lines.iter().map(|line| line.width).fold(1.0, f64::max);
        let line_height = style.font_size * 1.3;
        let ink_reserve = lines
            .iter()
            .map(|line| line.ink_reserve)
            .fold(0.0, f64::max);
        TextLayout {
            height: lines.len() as f64 * line_height,
            width,
            line_height,
            lines,
            ink_reserve,
        }
    }

    fn content(
        &mut self,
        item: &SceneDisplayItem,
        ids: &mut SvgIds,
    ) -> Result<String, CanvasError> {
        Ok(match item {
            SceneDisplayItem::Rectangle(item) => rectangle(item, ids),
            SceneDisplayItem::Arrow(item) => {
                if crate::gpu_geometry::arrow_outline_visible(item) {
                    return Err(CanvasError::Unsupported(
                        "vector serialization of arrow outlines",
                    ));
                }
                arrow(item, self.clear_color, ids)
            }
            SceneDisplayItem::SpeechBubble(item) => {
                if crate::gpu_geometry::arrow_outline_visible(&item.path) {
                    return Err(CanvasError::Unsupported(
                        "vector serialization of arrow outlines",
                    ));
                }
                arrow(&item.path, self.clear_color, ids)
            }
            SceneDisplayItem::Text(item) => self.text(item, ids),
            SceneDisplayItem::SerialNumber(item) => {
                let square = matches!(
                    item.serial_number_type,
                    DisplaySerialNumberType::OutlinedSquare | DisplaySerialNumberType::SolidSquare
                );
                let solid = matches!(
                    item.serial_number_type,
                    DisplaySerialNumberType::SolidCircle | DisplaySerialNumberType::SolidSquare
                );
                let shape = RectangleDisplayItem {
                    center_x: item.center_x,
                    center_y: item.center_y,
                    width: item.diameter,
                    height: item.diameter,
                    rotation: item.rotation,
                    corner_radii: item.corner_radii,
                    shape: if square {
                        DisplayRectangleShape::Rectangle
                    } else {
                        DisplayRectangleShape::Ellipse
                    },
                    stroke: item.color,
                    stroke_width: item.stroke_width,
                    stroke_style: item.stroke_style,
                    stroke_rendering_style: item.stroke_rendering_style,
                    stroke_rendering_seed: item.stroke_rendering_seed,
                    fill: if solid { item.color } else { item.fill },
                    fill_style: if solid {
                        DisplayFillStyle::Solid
                    } else {
                        item.fill_style
                    },
                    opacity: 1.0,
                    ..Default::default()
                };
                let mut output = format!("<g opacity=\"{}\">", item.opacity);
                output.push_str(&rectangle(&shape, ids));
                if item.serial_number_type != DisplaySerialNumberType::Circle {
                    output.push_str(&self.text(
                        &TextDisplayItem {
                            center_x: item.center_x,
                            center_y: item.center_y,
                            width: item.diameter,
                            height: item.diameter,
                            rotation: item.rotation,
                            text: item.label.clone(),
                            color: if solid {
                                serial_text_color(item.color)
                            } else {
                                item.color
                            },
                            font_size: item.font_size,
                            font_family: item.font_family.clone(),
                            horizontal_align: DisplayTextHorizontalAlign::Center,
                            opacity: 1.0,
                            ..Default::default()
                        },
                        ids,
                    ));
                }
                output.push_str("</g>");
                output
            }
            SceneDisplayItem::SerialNumberConnector(item) => {
                let baseline = if item.has_baseline {
                    format!(
                        "M{} {} L{} {}",
                        item.baseline_start_x,
                        item.baseline_start_y,
                        item.baseline_end_x,
                        item.baseline_end_y
                    )
                } else {
                    String::new()
                };
                format!(
                    "<path d=\"M{} {} L{} {} {baseline}\" fill=\"none\" opacity=\"{}\" {}/>",
                    item.start_x,
                    item.start_y,
                    item.end_x,
                    item.end_y,
                    item.opacity,
                    stroke_attributes(item.stroke, item.stroke_width, StrokeStyle::Solid)
                )
            }
            SceneDisplayItem::Magnifier(_) | SceneDisplayItem::Filter(_) => {
                return Err(CanvasError::Unsupported(
                    "vector serialization of GPU effects",
                ));
            }
            SceneDisplayItem::Image | SceneDisplayItem::Stroke => {
                return Err(CanvasError::Unsupported(
                    "scene items without rendering payloads",
                ));
            }
        })
    }

    fn text(&mut self, item: &TextDisplayItem, ids: &mut SvgIds) -> String {
        let style = TextStyle {
            color: item.color,
            font_size: item.font_size.max(1.0),
            bold: item.bold,
            italic: item.italic,
            font_family: item.font_family.clone(),
            fill: item.fill,
            fill_style: FillStyle::Solid,
            stroke: item.stroke,
            stroke_width: item.stroke_width,
            corner_radii: item.corner_radii,
            horizontal_align: TextHorizontalAlign::Left,
            vertical_align: TextVerticalAlign::Center,
            opacity: item.opacity,
        };
        let layout = self.layout(&item.text, &style, item.width.max(1.0));
        let (padding_x, padding_y) = snow_draw_engine_document::text_fill_outset(
            &snow_draw_engine_document::TextPaintGeometry {
                center: Point::new(item.center_x, item.center_y),
                width: item.width,
                height: item.height,
                rotation: item.rotation,
                content_width: layout.width,
                content_height: layout.height,
                horizontal_align: style.horizontal_align,
                vertical_align: style.vertical_align,
                has_text: !item.text.is_empty(),
                font_size: item.font_size,
                fill: item.fill,
                fill_style: match item.fill_style {
                    DisplayFillStyle::Border => FillStyle::Border,
                    DisplayFillStyle::Solid => FillStyle::Solid,
                    DisplayFillStyle::Line => FillStyle::Line,
                    DisplayFillStyle::CrossLine => FillStyle::CrossLine,
                },
                stroke: item.stroke,
                stroke_width: item.stroke_width,
                text_ink_reserve: layout.ink_reserve,
            },
        );
        let top = match item.vertical_align {
            DisplayTextVerticalAlign::Top => -item.height / 2.0,
            DisplayTextVerticalAlign::Center => -layout.height / 2.0,
            DisplayTextVerticalAlign::Bottom => item.height / 2.0 - layout.height,
        };
        let mut output = format!(
            "<g transform=\"translate({} {}) rotate({})\" opacity=\"{}\">",
            item.center_x,
            item.center_y,
            item.rotation.to_degrees(),
            item.opacity
        );
        for (index, line) in layout.lines.iter().enumerate() {
            let x = match item.horizontal_align {
                DisplayTextHorizontalAlign::Left => -item.width / 2.0,
                DisplayTextHorizontalAlign::Center => -line.width / 2.0,
                DisplayTextHorizontalAlign::Right => item.width / 2.0 - line.width,
            };
            let y = top + index as f64 * layout.line_height;
            if item.fill.a != 0 {
                output.push_str(&rectangle(
                    &RectangleDisplayItem {
                        center_x: x + line.width / 2.0,
                        center_y: y + layout.line_height / 2.0,
                        width: line.width + padding_x * 2.0,
                        height: layout.line_height + padding_y * 2.0,
                        fill: item.fill,
                        fill_style: item.fill_style,
                        stroke_width: 0.0,
                        corner_radii: item.corner_radii,
                        ..Default::default()
                    },
                    ids,
                ));
            }
            output.push_str(&self.outlined_text_span(line, &style, x, y + style.font_size));
        }
        output.push_str("</g>");
        output
    }

    fn outlined_text_span(&self, line: &TextLine, style: &TextStyle, x: f64, y: f64) -> String {
        let mut data = String::new();
        for glyph in line.glyphs.iter() {
            let Some(path) = self.glyph_path(glyph.font, glyph.id) else {
                continue;
            };
            let point = |p: lyon::math::Point| {
                (
                    x + glyph.x + f64::from(p.x) * glyph.scale,
                    y + glyph.y - f64::from(p.y) * glyph.scale,
                )
            };
            for event in path.iter() {
                use lyon::path::Event;
                match event {
                    Event::Begin { at } => {
                        let (x, y) = point(at);
                        write!(data, "M{x} {y}").unwrap();
                    }
                    Event::Line { to, .. } => {
                        let (x, y) = point(to);
                        write!(data, "L{x} {y}").unwrap();
                    }
                    Event::Quadratic { ctrl, to, .. } => {
                        let (cx, cy) = point(ctrl);
                        let (x, y) = point(to);
                        write!(data, "Q{cx} {cy} {x} {y}").unwrap();
                    }
                    Event::Cubic {
                        ctrl1, ctrl2, to, ..
                    } => {
                        let (ax, ay) = point(ctrl1);
                        let (bx, by) = point(ctrl2);
                        let (x, y) = point(to);
                        write!(data, "C{ax} {ay} {bx} {by} {x} {y}").unwrap();
                    }
                    Event::End { close, .. } => {
                        if close {
                            data.push('Z');
                        }
                    }
                }
            }
        }
        format!(
            "<path d=\"{data}\" fill=\"{}\" fill-opacity=\"{}\" stroke=\"{}\" stroke-opacity=\"{}\" stroke-width=\"{}\" paint-order=\"stroke fill\"/>",
            color(style.color),
            f64::from(style.color.a) / 255.0,
            color(style.stroke),
            f64::from(style.stroke.a) / 255.0,
            style.stroke_width,
        )
    }
}

pub(crate) fn serial_text_color(fill: ColorRgba8) -> ColorRgba8 {
    let linear = |channel: u8| {
        let value = f64::from(channel) / 255.0;
        if value <= 0.04045 {
            value / 12.92
        } else {
            ((value + 0.055) / 1.055).powf(2.4)
        }
    };
    let luminance = 0.2126 * linear(fill.r) + 0.7152 * linear(fill.g) + 0.0722 * linear(fill.b);
    if 1.05 / (luminance + 0.05) >= (luminance + 0.05) / (31.0 / 255.0 * luminance + 0.05) {
        ColorRgba8 {
            r: 255,
            g: 255,
            b: 255,
            a: 255,
        }
    } else {
        ColorRgba8 {
            r: 0,
            g: 0,
            b: 0,
            a: 224,
        }
    }
}
