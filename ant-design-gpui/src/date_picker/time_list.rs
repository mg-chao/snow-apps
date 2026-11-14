// Copyright (C) 2026 mg-chao
// SPDX-License-Identifier: Apache-2.0

//! Restrict a native list's fractional tail without replacing its scroll engine.

use gpui::{
    A11ySubtreeBuilder, AnyElement, App, Bounds, Element, ElementId, GlobalElementId,
    InspectorElementId, IntoElement, LayoutId, Pixels, ScrollHandle, Window, accesskit, px,
};

/// A transparent adapter: the wrapped list owns its layout, identity, hitbox,
/// focus, accessibility and virtualization. Only its shared vertical offset is
/// bounded before prepaint, so wheel overshoot cannot paint an extra blank gap.
pub(super) struct TimeList<E: Element> {
    child: E,
    handle: ScrollHandle,
    maximum: Pixels,
}

impl TimeList<AnyElement> {
    pub(super) fn new<T: IntoElement>(
        child: T,
        handle: ScrollHandle,
        maximum: Pixels,
    ) -> TimeList<T::Element> {
        TimeList {
            child: child.into_element(),
            handle,
            maximum: if f32::from(maximum).is_finite() {
                maximum.max(px(0.))
            } else {
                px(0.)
            },
        }
    }
}

impl<E: Element> TimeList<E> {
    fn clamp_offset(&self) {
        let mut offset = self.handle.offset();
        let y = offset.y.clamp(-self.maximum, px(0.));
        if y != offset.y {
            offset.y = y;
            self.handle.set_offset(offset);
        }
    }
}

impl<E: Element> IntoElement for TimeList<E> {
    type Element = Self;

    fn into_element(self) -> Self {
        self
    }
}

impl<E: Element> Element for TimeList<E> {
    type RequestLayoutState = E::RequestLayoutState;
    type PrepaintState = E::PrepaintState;

    fn id(&self) -> Option<ElementId> {
        self.child.id()
    }

    fn source_location(&self) -> Option<&'static std::panic::Location<'static>> {
        self.child.source_location()
    }

    fn a11y_role(&self) -> Option<accesskit::Role> {
        self.child.a11y_role()
    }

    fn write_a11y_info(&self, node: &mut accesskit::Node) {
        self.child.write_a11y_info(node);
    }

    fn a11y_synthetic_children(
        &mut self,
        prepaint: &mut Self::PrepaintState,
        builder: &mut A11ySubtreeBuilder,
    ) {
        self.child.a11y_synthetic_children(prepaint, builder);
    }

    fn request_layout(
        &mut self,
        id: Option<&GlobalElementId>,
        inspector_id: Option<&InspectorElementId>,
        window: &mut Window,
        cx: &mut App,
    ) -> (LayoutId, Self::RequestLayoutState) {
        self.child.request_layout(id, inspector_id, window, cx)
    }

    fn prepaint(
        &mut self,
        id: Option<&GlobalElementId>,
        inspector_id: Option<&InspectorElementId>,
        bounds: Bounds<Pixels>,
        state: &mut Self::RequestLayoutState,
        window: &mut Window,
        cx: &mut App,
    ) -> Self::PrepaintState {
        self.clamp_offset();
        self.child
            .prepaint(id, inspector_id, bounds, state, window, cx)
    }

    fn paint(
        &mut self,
        id: Option<&GlobalElementId>,
        inspector_id: Option<&InspectorElementId>,
        bounds: Bounds<Pixels>,
        state: &mut Self::RequestLayoutState,
        prepaint: &mut Self::PrepaintState,
        window: &mut Window,
        cx: &mut App,
    ) {
        self.child
            .paint(id, inspector_id, bounds, state, prepaint, window, cx);
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use gpui::{
        AppContext, Context, Entity, Render, ScrollStrategy, TestAppContext,
        UniformListScrollHandle, WindowHandle, canvas, div, point, prelude::*, uniform_list,
    };
    use std::{cell::RefCell, rc::Rc};

    type RowBounds = Rc<RefCell<Vec<(usize, Bounds<Pixels>)>>>;

    struct Consumer {
        scroll: UniformListScrollHandle,
        rows: RowBounds,
        wrapped: bool,
    }

    impl Render for Consumer {
        fn render(&mut self, _: &mut Window, _: &mut Context<Self>) -> impl IntoElement {
            let measured = self.rows.clone();
            // A 216px viewport needs 188px of tail after the final 28px row.
            // Seven native blank rows supply 196px, hence the 8px overshoot.
            let list = uniform_list("native-time-column", 31, move |range, _, _| {
                range
                    .map(|index| {
                        let measured = measured.clone();
                        div().relative().h(px(28.)).child(
                            canvas(
                                move |bounds, _, _| measured.borrow_mut().push((index, bounds)),
                                |_, (), _, _| (),
                            )
                            .absolute()
                            .inset_0(),
                        )
                    })
                    .collect()
            })
            .track_scroll(&self.scroll)
            .w(px(56.))
            .h(px(216.));
            let child = if self.wrapped {
                let handle = self.scroll.0.borrow().base_handle.clone();
                TimeList::new(list, handle, px(23. * 28.)).into_any_element()
            } else {
                list.into_any_element()
            };
            div().p(px(12.)).child(child)
        }
    }

    struct Fixture {
        cx: TestAppContext,
        window: WindowHandle<Consumer>,
        consumer: Entity<Consumer>,
        handle: ScrollHandle,
        rows: RowBounds,
    }

    impl Fixture {
        fn new() -> Self {
            let mut cx = TestAppContext::single();
            let rows: RowBounds = Rc::default();
            let scroll = UniformListScrollHandle::new();
            let handle = scroll.0.borrow().base_handle.clone();
            let window = cx.add_window(|_, _| Consumer {
                scroll,
                rows: rows.clone(),
                wrapped: false,
            });
            let consumer = window.root(&mut cx).unwrap();
            let mut fixture = Self {
                cx,
                window,
                consumer,
                handle,
                rows,
            };
            fixture.draw();
            fixture
        }

        fn draw(&mut self) {
            self.rows.borrow_mut().clear();
            self.consumer.update(&mut self.cx, |_, cx| cx.notify());
            self.cx.run_until_parked();
            self.cx
                .update_window(self.window.into(), |_, window, cx| {
                    window.draw(cx).clear(cx)
                })
                .unwrap();
            self.cx.run_until_parked();
        }

        fn last_row_top(&self) -> Pixels {
            self.rows
                .borrow()
                .iter()
                .find(|(index, _)| *index == 23)
                .unwrap()
                .1
                .top()
        }
    }

    #[test]
    fn date_picker_time_list_clamps_fractional_tail_before_native_rows_prepaint() {
        let mut fixture = Fixture::new();
        fixture.handle.set_offset(point(px(0.), px(-652.)));
        fixture.draw();
        assert_eq!(fixture.handle.offset().y, px(-652.));
        assert_eq!(
            fixture.last_row_top(),
            fixture.handle.bounds().top() - px(8.)
        );

        fixture
            .consumer
            .update(&mut fixture.cx, |consumer, _| consumer.wrapped = true);
        fixture.draw();
        assert_eq!(fixture.handle.offset().y, px(-644.));
        assert_eq!(fixture.last_row_top(), fixture.handle.bounds().top());
        let visible: std::collections::BTreeSet<_> = fixture
            .rows
            .borrow()
            .iter()
            .map(|(index, _)| *index)
            .collect();
        assert!(
            visible.len() <= 10,
            "native list remains virtualized: {visible:?}"
        );

        fixture.handle.set_offset(point(px(0.), px(40.)));
        fixture.draw();
        assert_eq!(fixture.handle.offset().y, px(0.));

        fixture.consumer.read_with(&fixture.cx, |consumer, _| {
            consumer
                .scroll
                .scroll_to_item_strict(23, ScrollStrategy::Top);
        });
        fixture.draw();
        assert_eq!(fixture.handle.offset().y, px(-644.));
        assert_eq!(fixture.last_row_top(), fixture.handle.bounds().top());
    }

    #[test]
    fn date_picker_time_list_preserves_child_identity_semantics_and_horizontal_offset() {
        let handle = ScrollHandle::new();
        let adapter = TimeList::new(
            div()
                .id("hours")
                .role(gpui::Role::ListBox)
                .aria_label("Hours"),
            handle.clone(),
            px(100.),
        );
        assert_eq!(adapter.id(), Some("hours".into()));
        assert_eq!(adapter.a11y_role(), Some(accesskit::Role::ListBox));
        let mut node = accesskit::Node::new(accesskit::Role::ListBox);
        adapter.write_a11y_info(&mut node);
        assert_eq!(node.label(), Some("Hours"));
        handle.set_offset(point(px(-17.), px(-108.)));
        adapter.clamp_offset();
        assert_eq!(handle.offset(), point(px(-17.), px(-100.)));
    }
}
