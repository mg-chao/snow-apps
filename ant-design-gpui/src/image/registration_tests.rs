// Copyright (C) 2026 mg-chao
// SPDX-License-Identifier: Apache-2.0
use super::*;
use crate::{ConfigScope, UiRoot};
use gpui::{
    AppContext, Context, Entity, IntoElement, Render, TestAppContext, Window, WindowHandle, div,
    prelude::*, px,
};
use std::sync::Arc;

struct Thumbnail {
    image: Entity<ImageState>,
    group: Entity<PreviewState>,
    scope: ConfigScope,
    id: &'static str,
    shown: bool,
    enabled: bool,
}
impl Render for Thumbnail {
    fn render(&mut self, _: &mut Window, cx: &mut Context<Self>) -> impl IntoElement {
        div().when(self.shown, |d| {
            d.child(
                Image::new(self.id, &self.image, cx)
                    .scope(self.scope.clone())
                    .preview_group(&self.group)
                    .preview(self.enabled)
                    .w(px(20.))
                    .h(px(20.)),
            )
        })
    }
}
struct Collection {
    group: Entity<PreviewState>,
    children: Vec<Entity<Thumbnail>>,
}
impl Render for Collection {
    fn render(&mut self, _: &mut Window, _: &mut Context<Self>) -> impl IntoElement {
        PreviewGroup::new("collection", &self.group).children(self.children.iter().cloned())
    }
}
struct Fixture {
    cx: TestAppContext,
    window: WindowHandle<UiRoot>,
    group: Entity<PreviewState>,
    children: Vec<Entity<Thumbnail>>,
}
impl Fixture {
    fn new() -> Self {
        let mut cx = TestAppContext::single();
        let mut data = None;
        let window = cx.add_window(|w, cx| {
            let scope = ConfigScope::new_window(cx);
            scope.update_patch(cx, |p| p.motion = Some(false)).unwrap();
            let group = cx.new(|cx| PreviewState::new(Vec::new(), w, cx));
            group.update(cx, |s, cx| s.set_scope(scope.clone(), cx));
            let children = (0..2)
                .map(|i| {
                    let source = Arc::new(gpui::RenderImage::new(vec![::image::Frame::new(
                        ::image::RgbaImage::from_pixel(2, 2, ::image::Rgba([i, 0, 0, 255])),
                    )]));
                    let image = cx.new(|cx| ImageState::new(source, w, cx));
                    cx.new(|_| Thumbnail {
                        image,
                        group: group.clone(),
                        scope: scope.clone(),
                        id: "same-local-id",
                        shown: true,
                        enabled: true,
                    })
                })
                .collect::<Vec<_>>();
            let content = cx.new(|_| Collection {
                group: group.clone(),
                children: children.clone(),
            });
            data = Some((group, children));
            UiRoot::new(scope, content, w, cx)
        });
        let (group, children) = data.unwrap();
        let mut f = Self {
            cx,
            window,
            group,
            children,
        };
        f.draw();
        f.draw();
        f
    }
    fn draw(&mut self) {
        self.cx.run_until_parked();
        self.cx
            .update_window(self.window.into(), |_, w, cx| w.draw(cx).clear(cx))
            .unwrap();
        self.cx.run_until_parked();
    }
    fn edit(&mut self, index: usize, f: impl FnOnce(&mut Thumbnail)) {
        self.children[index].update(&mut self.cx, |s, cx| {
            f(s);
            cx.notify();
        });
        self.draw();
        self.draw();
    }
}
#[test]
fn repeated_local_ids_in_separate_native_views_register_distinct_images() {
    let mut f = Fixture::new();
    let ids = f.group.read_with(&f.cx, |s, _| {
        s.items().iter().map(|i| i.id.clone()).collect::<Vec<_>>()
    });
    assert_eq!(ids.len(), 2);
    assert_ne!(ids[0], ids[1]);
    f.edit(0, |t| t.id = "new-widget-id");
    assert_eq!(
        f.group.read_with(&f.cx, |s, _| s
            .items()
            .iter()
            .map(|i| i.id.clone())
            .collect::<Vec<_>>()),
        ids,
        "widget reconstruction must preserve logical item identity and registration order"
    );
}
#[test]
fn disabled_and_removed_thumbnails_release_registration_and_preserve_selected_item() {
    let mut f = Fixture::new();
    f.cx.update_window(f.window.into(), |_, w, cx| {
        f.group.update(cx, |s, cx| s.request_open(1, w, cx))
    })
    .unwrap();
    f.draw();
    let selected = f
        .group
        .read_with(&f.cx, |s, _| s.current_item().unwrap().id.clone());
    f.edit(0, |t| t.enabled = false);
    assert_eq!(f.group.read_with(&f.cx, |s, _| s.items().len()), 1);
    assert_eq!(
        f.group
            .read_with(&f.cx, |s, _| s.current_item().unwrap().id.clone()),
        selected
    );
    f.edit(1, |t| t.shown = false);
    assert!(
        f.group
            .read_with(&f.cx, |s, _| s.items().is_empty() && !s.is_open())
    );
}
