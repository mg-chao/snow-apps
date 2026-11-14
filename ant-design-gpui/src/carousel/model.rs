// Copyright (C) 2026 mg-chao
// SPDX-License-Identifier: Apache-2.0

use crate::{Direction, theme::CubicBezier};
use gpui::{
    AnyElement, AnyView, App, AppContext, Context, Entity, IntoElement, ParentElement, Render,
    SharedString, Subscription, Window,
};
use std::{collections::HashMap, fmt, rc::Rc, time::Duration};

#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
pub enum CarouselEffect {
    #[default]
    Scroll,
    Fade,
}

#[cfg(test)]
#[path = "model_tests.rs"]
mod tests;

/// Logical placement. Start and End automatically select a vertical carousel
/// unless `CarouselOptions::vertical` explicitly overrides the orientation.
#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
pub enum DotPlacement {
    Top,
    #[default]
    Bottom,
    Start,
    End,
}
impl DotPlacement {
    pub fn vertical(self) -> bool {
        matches!(self, Self::Start | Self::End)
    }
}

/// Native equivalents of the documented Ant Design Carousel options.
#[derive(Clone, Debug, PartialEq)]
pub struct CarouselOptions {
    pub effect: CarouselEffect,
    pub dot_placement: DotPlacement,
    pub vertical: Option<bool>,
    pub dots: bool,
    pub arrows: bool,
    pub draggable: bool,
    pub infinite: bool,
    pub adaptive_height: bool,
    pub initial_slide: usize,
    pub speed: Duration,
    pub easing: CubicBezier,
    pub autoplay: bool,
    pub autoplay_speed: Duration,
    pub dot_duration: bool,
    pub wait_for_animate: bool,
}
impl Default for CarouselOptions {
    fn default() -> Self {
        Self {
            effect: CarouselEffect::Scroll,
            dot_placement: DotPlacement::Bottom,
            vertical: None,
            dots: true,
            arrows: false,
            draggable: false,
            infinite: true,
            adaptive_height: false,
            initial_slide: 0,
            speed: Duration::from_millis(500),
            easing: CubicBezier::new(0.25, 0.1, 0.25, 1.),
            autoplay: false,
            autoplay_speed: Duration::from_millis(3000),
            dot_duration: false,
            wait_for_animate: false,
        }
    }
}
impl CarouselOptions {
    pub fn is_vertical(&self) -> bool {
        self.vertical
            .unwrap_or_else(|| self.dot_placement.vertical())
    }

    /// Sign of the physical displacement for increasing logical slide indices.
    pub(super) fn axis_direction(&self, direction: Direction) -> f64 {
        if !self.is_vertical() && direction == Direction::Rtl {
            -1.
        } else {
            1.
        }
    }

    pub(super) fn normalize(&self, index: usize, count: usize) -> Option<usize> {
        (count > 0).then(|| {
            if self.infinite {
                index % count
            } else {
                index.min(count - 1)
            }
        })
    }
}

pub(super) type ContentRenderer = Rc<dyn Fn(&mut Window, &mut App) -> AnyElement>;
pub(super) type ViewObserver =
    Box<dyn FnOnce(SharedString, &mut Context<super::CarouselState>) -> Subscription>;
pub(super) struct SlideContent {
    pub view: AnyView,
    pub observer: Option<ViewObserver>,
}
pub(super) type ViewFactory = Rc<dyn Fn(&mut Window, &mut App) -> SlideContent>;

struct ContentView(ContentRenderer);
impl Render for ContentView {
    fn render(&mut self, window: &mut Window, cx: &mut Context<Self>) -> impl IntoElement {
        (self.0)(window, cx)
    }
}

/// A stable application key and retained native content. Clone slides when
/// reordering them to preserve their factory identity and native view lifetime.
#[derive(Clone)]
pub struct CarouselSlide {
    pub key: SharedString,
    pub label: Option<SharedString>,
    pub(super) factory: Option<ViewFactory>,
}
impl fmt::Debug for CarouselSlide {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        f.debug_struct("CarouselSlide")
            .field("key", &self.key)
            .field("label", &self.label)
            .finish_non_exhaustive()
    }
}
impl CarouselSlide {
    pub fn new(key: impl Into<SharedString>) -> Self {
        Self {
            key: key.into(),
            label: None,
            factory: None,
        }
    }
    pub fn label(mut self, label: impl Into<SharedString>) -> Self {
        self.label = Some(label.into());
        self
    }
    /// Typed retained content. Notifications from this entity invalidate the
    /// intrinsic-height cache as well as its native rendered content.
    pub fn view<V: Render>(
        mut self,
        factory: impl Fn(&mut Window, &mut App) -> Entity<V> + 'static,
    ) -> Self {
        self.factory = Some(Rc::new(move |window, cx| {
            let view = factory(window, cx);
            let observed = view.clone();
            SlideContent {
                view: view.into(),
                observer: Some(Box::new(move |key, cx| {
                    cx.observe(&observed, move |state, _, cx| {
                        state.invalidate_slide_measurement(&key, cx)
                    })
                })),
            }
        }));
        self
    }
    /// Type-erased content. Call `CarouselState::invalidate_measurements` when
    /// a change to this view can affect its intrinsic height.
    pub fn any_view(
        mut self,
        factory: impl Fn(&mut Window, &mut App) -> AnyView + 'static,
    ) -> Self {
        self.factory = Some(Rc::new(move |window, cx| SlideContent {
            view: factory(window, cx),
            observer: None,
        }));
        self
    }
    pub fn content(self, render: impl Fn(&mut Window, &mut App) -> AnyElement + 'static) -> Self {
        let render: ContentRenderer = Rc::new(render);
        self.view(move |_, cx| cx.new(|_| ContentView(render.clone())))
    }
    pub fn text(self, text: impl Into<SharedString>) -> Self {
        let text = text.into();
        self.content(move |_, _| gpui::div().child(text.clone()).into_any_element())
    }
}

#[derive(Clone, Debug, PartialEq, Eq)]
pub enum CarouselItemsError {
    DuplicateKey(SharedString),
}
impl fmt::Display for CarouselItemsError {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        match self {
            Self::DuplicateKey(key) => write!(f, "duplicate carousel slide key: {key}"),
        }
    }
}
impl std::error::Error for CarouselItemsError {}

#[derive(Default, Debug)]
struct Items {
    entries: Vec<CarouselSlide>,
    positions: HashMap<SharedString, usize>,
}

/// Immutable slide snapshot with cheap cloning and constant-time keyed lookup.
#[derive(Clone, Default, Debug)]
pub struct CarouselItems(Rc<Items>);
impl CarouselItems {
    pub fn new(
        slides: impl IntoIterator<Item = CarouselSlide>,
    ) -> Result<Self, CarouselItemsError> {
        let entries: Vec<_> = slides.into_iter().collect();
        let mut positions = HashMap::with_capacity(entries.len());
        for (index, slide) in entries.iter().enumerate() {
            if positions.insert(slide.key.clone(), index).is_some() {
                return Err(CarouselItemsError::DuplicateKey(slide.key.clone()));
            }
        }
        Ok(Self(Rc::new(Items { entries, positions })))
    }
    pub fn entries(&self) -> &[CarouselSlide] {
        &self.0.entries
    }
    pub fn len(&self) -> usize {
        self.0.entries.len()
    }
    pub fn is_empty(&self) -> bool {
        self.0.entries.is_empty()
    }
    pub fn index(&self, key: &str) -> Option<usize> {
        self.0.positions.get(key).copied()
    }
    pub fn get(&self, key: &str) -> Option<&CarouselSlide> {
        self.index(key).map(|index| &self.0.entries[index])
    }
    pub(super) fn same(&self, other: &Self) -> bool {
        Rc::ptr_eq(&self.0, &other.0)
    }
}

pub(super) struct CarouselModel {
    pub items: CarouselItems,
    pub current: Option<usize>,
    pub initialized: bool,
}
impl CarouselModel {
    pub fn new(items: CarouselItems) -> Self {
        Self {
            current: (!items.is_empty()).then_some(0),
            items,
            initialized: false,
        }
    }
    pub fn set_items(&mut self, items: CarouselItems) -> bool {
        if self.items.same(&items) {
            return false;
        }
        let previous = self.current.unwrap_or(0);
        let key = self
            .current
            .and_then(|index| self.items.entries().get(index));
        self.current = key
            .and_then(|slide| items.index(&slide.key))
            .or_else(|| (!items.is_empty()).then(|| previous.min(items.len() - 1)));
        self.items = items;
        true
    }
    pub fn next(&self, step: isize, infinite: bool) -> Option<usize> {
        let current = self.current?;
        let count = self.items.len();
        if count < 2 {
            return None;
        }
        let target = if infinite {
            (current as i128 + step as i128).rem_euclid(count as i128) as usize
        } else {
            (current as i128 + step as i128).clamp(0, (count - 1) as i128) as usize
        };
        (target != current).then_some(target)
    }
}
