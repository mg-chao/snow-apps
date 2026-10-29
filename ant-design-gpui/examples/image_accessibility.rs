// Copyright (C) 2026 mg-chao
// SPDX-License-Identifier: Apache-2.0

//! Process-owned native Windows UIA fixture. See scripts/test-image-accessibility.ps1.

use ant_design_gpui::{
    Button, ConfigScope, Image, ImageState, Modal, ModalState, PreviewGroup, PreviewState, UiRoot,
    image::PreviewItem,
    modal::{ModalAction, ModalContent, ModalFooter, ModalOptions},
};
use gpui::{
    App, AppContext, Bounds, Context, Entity, IntoElement, Render, Window, WindowBounds,
    WindowOptions, div, prelude::*, px, size,
};
use std::{rc::Rc, sync::Arc};

fn source(index: usize) -> gpui::ImageSource {
    let bytes = match index {
        0 => include_bytes!("../resources/image/landscape.png").as_slice(),
        1 => include_bytes!("../resources/image/portrait.png").as_slice(),
        _ => include_bytes!("../resources/image/transparent.png").as_slice(),
    };
    Arc::new(gpui::Image::from_bytes(
        gpui::ImageFormat::Png,
        bytes.to_vec(),
    ))
    .into()
}

struct Surface {
    scope: ConfigScope,
    images: Vec<Entity<ImageState>>,
    group: Entity<PreviewState>,
    parent: Entity<ModalState>,
}
impl Surface {
    fn new(scope: ConfigScope, window: &mut Window, cx: &mut Context<Self>) -> Self {
        let images = (0..4)
            .map(|index| {
                cx.new(|cx| {
                    let mut state = ImageState::new(source(index % 2), window, cx);
                    state.set_scope(scope.clone(), cx);
                    state
                })
            })
            .collect::<Vec<_>>();
        let group = cx.new(|cx| {
            let mut state = PreviewState::new(
                (0..3)
                    .map(|index| {
                        PreviewItem::new(format!("uia-item-{index}"), source(index))
                            .alt(format!("Gallery image {}", index + 1))
                    })
                    .collect(),
                window,
                cx,
            );
            state.set_scope(scope.clone(), cx);
            state
        });
        let nested_image = images[3].clone();
        let nested_scope = scope.clone();
        let parent = cx.new(|cx| {
            let mut state = ModalState::new(window, cx);
            state.set_scope(scope.clone(), cx);
            state.set_options(
                ModalOptions {
                    title: Some(ModalContent::text("Image parent dialog")),
                    automation_id: Some("image.uia.parent.dialog".into()),
                    footer: ModalFooter::None,
                    content: Some(ModalContent::render(move |_, _, cx| {
                        Image::new("nested-image", &nested_image, cx)
                            .scope(nested_scope.clone())
                            .alt("Nested portrait preview")
                            .automation_id("image.uia.nested.thumbnail")
                            .w(px(200.))
                            .h(px(160.))
                    })),
                    on_cancel: Some(Rc::new(|_, _, _| ModalAction::Close)),
                    ..Default::default()
                },
                cx,
            );
            state
        });
        if std::env::args().any(|argument| argument == "--parent-open") {
            eprintln!(
                "Image={} ImageStyles={} PreviewOptions={} ModalOptions={} ConfigPatch={} ResolvedConfig={}",
                std::mem::size_of::<Image>(),
                std::mem::size_of::<ant_design_gpui::image::ImageStyles>(),
                std::mem::size_of::<ant_design_gpui::image::PreviewOptions>(),
                std::mem::size_of::<ModalOptions>(),
                std::mem::size_of::<ant_design_gpui::ConfigPatch>(),
                std::mem::size_of::<ant_design_gpui::ResolvedConfig>(),
            );
            parent.update(cx, |state, cx| state.set_open(true, window, cx));
        }
        Self {
            scope,
            images,
            group,
            parent,
        }
    }
}
impl Render for Surface {
    fn render(&mut self, _: &mut Window, cx: &mut Context<Self>) -> impl IntoElement {
        let parent = self.parent.clone();
        div()
            .size_full()
            .p(px(24.))
            .flex()
            .flex_col()
            .items_start()
            .gap(px(20.))
            .bg(self.scope.snapshot(cx).theme.tokens.color_bg_container)
            .child("Native Image accessibility verification")
            .child(
                div()
                    .flex()
                    .gap(px(24.))
                    .child(
                        Image::new("single-image", &self.images[0], cx)
                            .scope(self.scope.clone())
                            .alt("Landscape preview")
                            .automation_id("image.uia.thumbnail")
                            .w(px(200.))
                            .h(px(120.)),
                    )
                    .child(
                        Image::new("plain-image", &self.images[1], cx)
                            .scope(self.scope.clone())
                            .preview(false)
                            .alt("Portrait display image")
                            .automation_id("image.uia.plain")
                            .w(px(120.))
                            .h(px(120.)),
                    ),
            )
            .child(
                PreviewGroup::new("gallery-preview", &self.group)
                    .scope(self.scope.clone())
                    .child(
                        Image::new("group-image", &self.images[2], cx)
                            .scope(self.scope.clone())
                            .preview_group(&self.group)
                            .alt("Open three gallery images")
                            .automation_id("image.uia.group")
                            .w(px(200.))
                            .h(px(120.)),
                    ),
            )
            .child(
                Button::new("open-parent")
                    .scope(self.scope.clone())
                    .label("Open nested dialog")
                    .automation_id("image.uia.nested.open")
                    .on_click(move |_, window, cx| {
                        parent.update(cx, |state, cx| state.set_open(true, window, cx))
                    }),
            )
            .child(Modal::new("parent-dialog", &self.parent, cx).scope(self.scope.clone()))
    }
}

fn main() {
    gpui_platform::application().run(|cx: &mut App| {
        ant_design_gpui::init(cx);
        cx.on_window_closed(|cx, _| cx.quit()).detach();
        let scope = ConfigScope::new_window(cx);
        scope
            .update_patch(cx, |patch| patch.motion = Some(false))
            .unwrap();
        cx.open_window(
            WindowOptions {
                window_bounds: Some(WindowBounds::Windowed(Bounds::centered(
                    None,
                    size(px(900.), px(700.)),
                    cx,
                ))),
                focus: false,
                ..Default::default()
            },
            |window, cx| {
                window.set_window_title(&format!(
                    "Ant Design GPUI Image accessibility {}",
                    std::process::id()
                ));
                let surface = cx.new(|cx| Surface::new(scope.clone(), window, cx));
                cx.new(|cx| UiRoot::new(scope, surface, window, cx))
            },
        )
        .expect("native Image accessibility fixture");
    });
}
