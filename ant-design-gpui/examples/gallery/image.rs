// Copyright (C) 2026 mg-chao
// SPDX-License-Identifier: Apache-2.0
// Examples adapted from Ant Design 6.6.7 (MIT).

use super::shell::{page_padding, sidebar_width};
use ant_design_gpui::{
    Button, ConfigScope, Direction, Divider, Image, ImageState, InputNumber, InputNumberEvent,
    InputNumberState, Modal, ModalState, NumberOptions, PreviewGroup, PreviewState,
    button::ButtonType,
    icons::IconName,
    image::{
        ImagePlaceholder, ImageProgress, ImageStyleResolver, ImageStyles, PreviewAction,
        PreviewEvent, PreviewItem, PreviewOptions, PreviewRenderInfo,
    },
    modal::{ModalAction, ModalContent, ModalOptions},
};
use gpui::{
    AnyElement, AppContext, Context, Entity, FocusHandle, FontWeight, ImageSource, IntoElement,
    Render, Role, ScrollAnchor, StyleRefinement, Subscription, Task, Window, div, prelude::*, px,
    relative,
};
use std::{
    rc::Rc,
    sync::{Arc, OnceLock},
    time::Duration,
};

pub(super) struct Demo {
    pub id: &'static str,
    pub title: &'static str,
    pub description: &'static str,
}
pub(super) const DEMOS: &[Demo] = &[
    Demo {
        id: "image-basic",
        title: "Basic Usage",
        description: "Display an image. Click it to open the native preview.",
    },
    Demo {
        id: "image-placeholder",
        title: "Progressive Loading",
        description: "Animated loading, progress, custom content, and image reloading.",
    },
    Demo {
        id: "image-fallback",
        title: "Fault tolerant",
        description: "Display a fallback image when the original source cannot load.",
    },
    Demo {
        id: "image-preview-group",
        title: "Multiple image preview",
        description: "Preview several images together and navigate between them.",
    },
    Demo {
        id: "image-preview-group-visible",
        title: "Preview from one image",
        description: "Open a larger collection from a single visible thumbnail.",
    },
    Demo {
        id: "image-previewSrc",
        title: "Custom preview image",
        description: "Use an independent image source in the fullscreen preview.",
    },
    Demo {
        id: "image-controlled-preview",
        title: "Controlled Preview",
        description: "Open the preview programmatically and configure its zoom step.",
    },
    Demo {
        id: "image-toolbarRender",
        title: "Custom toolbar render",
        description: "Compose native controls with the preview's action handles.",
    },
    Demo {
        id: "image-imageRender",
        title: "Custom preview render",
        description: "Replace the image with interactive native content.",
    },
    Demo {
        id: "image-mask",
        title: "preview mask",
        description: "Dimmed and mask-free previews. Optional blur uses a dimmed mask with this GPUI revision.",
    },
    Demo {
        id: "image-style-class",
        title: "Custom semantic dom styling",
        description: "Refine native image, cover, and preview semantic slots.",
    },
    Demo {
        id: "image-nested",
        title: "nested",
        description: "Open an image preview inside three nested dialogs.",
    },
];

pub(super) fn source(index: usize) -> ImageSource {
    static IMAGES: OnceLock<[Arc<gpui::Image>; 4]> = OnceLock::new();
    let images = IMAGES.get_or_init(|| {
        [
            include_bytes!("../../resources/image/landscape.png").as_slice(),
            include_bytes!("../../resources/image/portrait.png").as_slice(),
            include_bytes!("../../resources/image/transparent.png").as_slice(),
            include_bytes!("../../resources/image/fallback.png").as_slice(),
        ]
        .map(|bytes| {
            Arc::new(gpui::Image::from_bytes(
                gpui::ImageFormat::Png,
                bytes.to_vec(),
            ))
        })
    });
    images[index % 4].clone().into()
}
fn items() -> Vec<PreviewItem> {
    (0..3)
        .map(|i| {
            PreviewItem::new(format!("image-{i}"), source(i))
                .alt(format!("Example image {}", i + 1))
        })
        .collect()
}

fn semantic_styles() -> ImageStyleResolver {
    static STYLES: OnceLock<ImageStyleResolver> = OnceLock::new();
    STYLES
        .get_or_init(|| {
            ImageStyleResolver::new(|context, _, _| {
                if context.preview {
                    ImageStyles {
                        root: StyleRefinement::default()
                            .border_2()
                            .border_color(gpui::rgb(0xa594f9))
                            .p(px(4.))
                            .rounded(px(8.)),
                        image: StyleRefinement::default().rounded(px(4.)),
                        ..Default::default()
                    }
                } else {
                    ImageStyles::default()
                }
            })
        })
        .clone()
}

fn custom_toolbar(
    info: &PreviewRenderInfo,
    scope: &ConfigScope,
    _: &mut Window,
    _: &mut gpui::App,
) -> AnyElement {
    let actions = [
        (
            "previous",
            Some(PreviewAction::Previous),
            IconName::LeftOutlined,
            "Previous image",
            info.current == 0,
        ),
        (
            "next",
            Some(PreviewAction::Next),
            IconName::RightOutlined,
            "Next image",
            info.current + 1 >= info.total,
        ),
        (
            "download",
            None,
            IconName::DownloadOutlined,
            "Save image",
            false,
        ),
        (
            "flip-y",
            Some(PreviewAction::FlipY),
            IconName::SwapOutlined,
            "Flip vertically",
            false,
        ),
        (
            "flip-x",
            Some(PreviewAction::FlipX),
            IconName::SwapOutlined,
            "Flip horizontally",
            false,
        ),
        (
            "rotate-left",
            Some(PreviewAction::RotateLeft),
            IconName::RotateLeftOutlined,
            "Rotate left",
            false,
        ),
        (
            "rotate-right",
            Some(PreviewAction::RotateRight),
            IconName::RotateRightOutlined,
            "Rotate right",
            false,
        ),
        (
            "zoom-out",
            Some(PreviewAction::ZoomOut),
            IconName::ZoomOutOutlined,
            "Zoom out",
            info.transform.scale <= 1.,
        ),
        (
            "zoom-in",
            Some(PreviewAction::ZoomIn),
            IconName::ZoomInOutlined,
            "Zoom in",
            info.transform.scale >= 50.,
        ),
        (
            "reset",
            Some(PreviewAction::Reset),
            IconName::UndoOutlined,
            "Reset",
            false,
        ),
    ];
    div()
        .flex()
        .items_center()
        .gap(px(12.))
        .px(px(24.))
        .bg(gpui::rgba(0x0000001a))
        .rounded(px(100.))
        .children(
            actions
                .into_iter()
                .map(|(id, action, icon, label, disabled)| {
                    let state = info.state.clone();
                    let current = info.current;
                    let button = Button::new(format!("image.custom.{id}.button"))
                        .scope(scope.clone())
                        .button_type(ButtonType::Text)
                        .accessibility_label(label)
                        .automation_id(format!("image.custom.{id}"))
                        .disabled(disabled)
                        .wave(false)
                        .bg(gpui::rgba(0))
                        .w(px(44.))
                        .h(px(44.))
                        .p_0()
                        .on_click(move |_, window, cx| {
                            if let Some(action) = action {
                                state
                                    .update(cx, |state, cx| state.apply_action(action, window, cx));
                            } else {
                                let prompt = cx.prompt_for_new_path(
                                    std::path::Path::new("."),
                                    Some("image.png"),
                                );
                                let bytes = if current == 0 {
                                    include_bytes!("../../resources/image/landscape.png").as_slice()
                                } else {
                                    include_bytes!("../../resources/image/portrait.png").as_slice()
                                };
                                cx.spawn(async move |cx| {
                                    if let Ok(Ok(Some(path))) = prompt.await {
                                        let _ = cx
                                            .background_executor()
                                            .spawn(async move { std::fs::write(path, bytes) })
                                            .await;
                                    }
                                })
                                .detach();
                            }
                        });
                    // The native button owns focus, activation and semantics. A
                    // decorative icon shares its slot and can rotate independently.
                    let artwork = ant_design_gpui::icons::icon(icon)
                        .inherit_color()
                        .size(px(20.))
                        .rotation(gpui::radians(if action == Some(PreviewAction::FlipY) {
                            std::f32::consts::FRAC_PI_2
                        } else {
                            0.
                        }));
                    div()
                        .id(format!("image.custom.{id}.slot"))
                        .debug_selector(move || format!("image.custom.{id}"))
                        .relative()
                        .flex_none()
                        .text_color(gpui::rgb(0xffffff))
                        .opacity(if disabled { 0.3 } else { 1. })
                        .hover(move |style| style.opacity(0.3))
                        .child(button)
                        .child(
                            div()
                                .absolute()
                                .inset_0()
                                .flex()
                                .items_center()
                                .justify_center()
                                .child(artwork),
                        )
                }),
        )
        .into_any_element()
}

pub(super) struct ImagePage {
    pub scope: ConfigScope,
    pub images: Vec<Entity<ImageState>>,
    pub groups: Vec<Entity<PreviewState>>,
    pub modals: Vec<Entity<ModalState>>,
    pub nested_focus: Vec<FocusHandle>,
    pub percent: f32,
    pub generating: bool,
    pub scale_step: f32,
    pub zoom_step: Entity<InputNumberState>,
    options: Vec<PreviewOptions>,
    anchors: Vec<ScrollAnchor>,
    generation_task: Option<Task<()>>,
    reload_task: Option<Task<()>>,
    _controlled: Subscription,
    _zoom_subscription: Subscription,
    _config: Subscription,
}
impl ImagePage {
    pub fn new(
        scope: ConfigScope,
        anchors: Vec<ScrollAnchor>,
        window: &mut Window,
        cx: &mut Context<Self>,
    ) -> Self {
        let images: Vec<_> = (0..28)
            .map(|i| {
                cx.new(|cx| {
                    let mut image = if (2..6).contains(&i) || i == 7 {
                        ImageState::empty(window, cx)
                    } else {
                        ImageState::new(
                            source(match i {
                                10..=12 => i - 10,
                                13 | 14 | 16 | 22 | 23 | 25 | 26 => 0,
                                17 | 27 => 1,
                                _ => i % 3,
                            }),
                            window,
                            cx,
                        )
                    };
                    image.set_scope(scope.clone(), cx);
                    image.set_alt(format!("Example image {}", i + 1), cx);
                    if i == 8 || i == 9 {
                        image.set_source(
                            Some(ImageSource::Custom(Arc::new(|_, _| {
                                Some(Err(gpui::ImageCacheError::Asset(
                                    "Missing example image".into(),
                                )))
                            }))),
                            cx,
                        );
                    }
                    if i == 8 {
                        image.set_fallback(Some(source(3)), cx);
                    }
                    if i == 14 {
                        image.set_preview_source(Some(source(2)), cx);
                    }
                    image
                })
            })
            .collect();
        let groups: Vec<_> = (0..5)
            .map(|_| {
                cx.new(|cx| {
                    let mut s = PreviewState::new(items(), window, cx);
                    s.set_scope(scope.clone(), cx);
                    s
                })
            })
            .collect();
        let mut options = vec![PreviewOptions::default(); 28];
        let zoom_step = cx.new(|cx| {
            let mut s = InputNumberState::with_options(
                NumberOptions {
                    minimum: Some("0.1".parse().unwrap()),
                    maximum: Some(5.into()),
                    step: "0.1".parse().unwrap(),
                    ..Default::default()
                },
                window,
                cx,
            )
            .unwrap();
            s.set_value(Some(0.5.into()), window, cx).unwrap();
            s
        });
        let zoom_subscription = cx.subscribe(&zoom_step, |this, _, event, cx| {
            if let InputNumberEvent::Change(change) = event
                && let Some(value) = change.value.as_ref().and_then(|v| v.to_number().ok())
            {
                this.scale_step = value as f32;
                this.groups[3].update(cx, |s, cx| {
                    let mut o = s.options().clone();
                    o.scale_step = value as f32;
                    s.set_options(o, cx);
                });
                cx.notify();
            }
        });
        groups[3].update(cx, |s, cx| {
            s.set_options(
                PreviewOptions {
                    controlled_open: Some(false),
                    controlled_current: Some(0),
                    ..Default::default()
                },
                cx,
            )
        });
        let controlled = cx.subscribe_in(&groups[3], window, |_, state, event, window, cx| {
            let mut options = state.read(cx).options().clone();
            match event {
                PreviewEvent::OpenChange { open, current } => {
                    options.controlled_open = Some(*open);
                    options.controlled_current = Some(*current);
                }
                PreviewEvent::Change { current, .. } => options.controlled_current = Some(*current),
                _ => return,
            }
            state.update(cx, |s, cx| {
                s.set_options(options, cx);
                s.set_open(s.options().controlled_open.unwrap_or(false), window, cx);
            });
        });
        options[18].actions_render = Some(Rc::new(|_, _, _, _| div().into_any_element()));
        options[18].image_render = Some(Rc::new(|_, info, _, _| {
            let state = info.state.clone();
            div()
                .w(px(360.))
                .p(px(32.))
                .rounded(px(12.))
                .bg(gpui::rgb(0xffffff))
                .text_color(gpui::rgb(0x141414))
                .flex()
                .flex_col()
                .gap(px(16.))
                .child("Custom native preview")
                .child("Applications can supply any GPUI element or retained view here.")
                .child(
                    Button::new("custom-content-close")
                        .label("Close preview")
                        .on_click(move |_, w, cx| state.update(cx, |s, cx| s.request_close(w, cx))),
                )
                .into_any_element()
        }));
        options[19].blur = true;
        options[21].mask = false;
        options[23].controlled_open = Some(false);
        let toolbar_scope = scope.clone();
        let toolbar = PreviewOptions {
            actions_render: Some(Rc::new(move |_, info, window, cx| {
                custom_toolbar(info, &toolbar_scope, window, cx)
            })),
            ..Default::default()
        };
        groups[2].update(cx, |s, cx| {
            s.set_items(items().into_iter().take(2).collect(), cx);
            s.set_options(toolbar, cx);
        });
        groups[4].update(cx, |state, cx| {
            state.set_items(items().into_iter().take(2).collect(), cx)
        });
        let modals: Vec<_> = (0..3)
            .map(|_| {
                cx.new(|cx| {
                    let mut s = ModalState::new(window, cx);
                    s.set_scope(scope.clone(), cx);
                    s
                })
            })
            .collect();
        let nested_focus = (0..3)
            .map(|_| ant_design_gpui::behavior::focus_handle(cx))
            .collect::<Vec<_>>();
        for index in (0..3).rev() {
            let next = modals.get(index + 1).cloned();
            let image = images[25].clone();
            let nested_images = [images[26].clone(), images[27].clone()];
            let nested_group = groups[4].clone();
            let modal_scope = scope.clone();
            let trigger_focus = nested_focus[(index + 1).min(2)].clone();
            modals[index].update(cx, |s, cx| {
                s.set_options(
                    ModalOptions {
                        title: Some(ModalContent::text(format!("Image dialog {}", index + 1))),
                        on_cancel: Some(Rc::new(|_, _, _| ModalAction::Close)),
                        on_ok: Some(Rc::new(|_, _, _| ModalAction::Close)),
                        content: Some(ModalContent::render(move |_, _, cx| {
                            if let Some(next) = &next {
                                let target = next.clone();
                                div()
                                    .child(
                                        div()
                                            .id(format!("image.nested.next.{index}.wrapper"))
                                            .debug_selector(move || {
                                                format!("image.nested.next.{index}")
                                            })
                                            .child(
                                                Button::new(format!("image.nested.next.{index}"))
                                                    .focus_handle(&trigger_focus)
                                                    .label(format!("Open dialog {}", index + 2))
                                                    .on_click(move |_, w, cx| {
                                                        target.update(cx, |s, cx| {
                                                            s.set_open(true, w, cx)
                                                        })
                                                    }),
                                            ),
                                    )
                                    .child(Modal::new(
                                        format!("image.nested.child.{index}"),
                                        next,
                                        cx,
                                    ))
                                    .into_any_element()
                            } else {
                                div()
                                    .child(
                                        div()
                                            .id("nested-image-wrapper")
                                            .w(px(200.))
                                            .debug_selector(|| "image.nested.thumbnail".into())
                                            .child(
                                                Image::new("image.nested.thumbnail", &image, cx)
                                                    .scope(modal_scope.clone())
                                                    .w(px(200.)),
                                            ),
                                    )
                                    .child(Divider::new("image.nested.divider"))
                                    .child(
                                        PreviewGroup::new("image.nested.group", &nested_group)
                                            .scope(modal_scope.clone())
                                            .child(div().flex().gap(px(8.)).children(
                                                nested_images.iter().enumerate().map(
                                                    |(index, image)| {
                                                        Image::new(
                                                            format!("image.nested.group.{index}"),
                                                            image,
                                                            cx,
                                                        )
                                                        .scope(modal_scope.clone())
                                                        .preview_group(&nested_group)
                                                        .w(px(150.))
                                                    },
                                                ),
                                            )),
                                    )
                                    .into_any_element()
                            }
                        })),
                        ..Default::default()
                    },
                    cx,
                )
            });
        }
        Self {
            _config: scope.observe(cx),
            scope,
            images,
            groups,
            modals,
            nested_focus,
            percent: 0.,
            generating: false,
            scale_step: 0.5,
            zoom_step,
            options,
            anchors,
            generation_task: None,
            reload_task: None,
            _controlled: controlled,
            _zoom_subscription: zoom_subscription,
        }
    }
    fn image(&self, index: usize, cx: &Context<Self>) -> Image {
        Image::new(format!("image.example.{index}"), &self.images[index], cx)
            .scope(self.scope.clone())
            .automation_id(format!("image.thumbnail.{index}"))
            .w(px(200.))
    }
    fn progressive_image(&self, index: usize, cx: &Context<Self>) -> Image {
        let radius = px(self.scope.snapshot(cx).theme.tokens.border_radius_lg as f32);
        self.image(index, cx).h(px(200.)).styles(ImageStyles {
            root: StyleRefinement::default().rounded(radius),
            image: StyleRefinement::default().rounded(radius),
            cover: StyleRefinement::default().rounded(radius),
            progress_root: StyleRefinement::default().rounded(radius),
            ..Default::default()
        })
    }
    fn content(&self, index: usize, cx: &mut Context<Self>) -> AnyElement {
        match index {
            0 => self.basic_content(cx),
            1 => self.progressive_content(cx),
            2 => self.fallback_content(cx),
            3 => self.group_content(cx),
            4 => self.hidden_group_content(cx),
            5 => self.preview_source_content(cx),
            6 => self.controlled_content(cx),
            7 => self.toolbar_content(cx),
            8 => self.image_render_content(cx),
            9 => self.mask_content(cx),
            10 => self.styles_content(cx),
            _ => self.nested_content(cx),
        }
    }

    fn basic_content(&self, cx: &mut Context<Self>) -> AnyElement {
        let row = div().flex().flex_wrap().gap(px(16.)).items_start();
        row.child(self.image(0, cx))
            .child(self.image(1, cx).preview(false).w(px(100.)))
            .into_any_element()
    }

    fn progressive_content(&self, cx: &mut Context<Self>) -> AnyElement {
        let row = div().flex().flex_wrap().gap(px(16.)).items_start();
        let reload = self.images[6].clone();
        row.children((2..6).map(|i| {
            self.progressive_image(i, cx).progress(ImageProgress {
                percent: match i {
                    4 => Some(50.),
                    5 => Some(75.),
                    _ => None,
                },
                render: match i {
                    3 => Some(
                        Rc::new(|_: AnyElement, _: u8, _: &mut Window, _: &mut gpui::App| {
                            div().child("loading...").into_any_element()
                        }) as ant_design_gpui::image::ImageProgressRenderer,
                    ),
                    5 => Some(Rc::new(|rail, p, _, _| {
                        div()
                            .w_full()
                            .child(rail)
                            .child(format!("Generating {p}%"))
                            .into_any_element()
                    })),
                    _ => None,
                },
            })
        }))
        .child(
            div()
                .flex()
                .flex_col()
                .gap(px(8.))
                .child(
                    div()
                        .id("image.reload.wrapper")
                        .debug_selector(|| "image.reload".into())
                        .child(Button::new("image.reload").label("Reload Image").on_click(
                            cx.listener(move |this, _, _, cx| {
                                reload.update(cx, |s, cx| s.set_source(None, cx));
                                let target = reload.clone();
                                this.reload_task = Some(cx.spawn(async move |_, cx| {
                                    cx.background_executor()
                                        .timer(Duration::from_millis(600))
                                        .await;
                                    target.update(cx, |s, cx| s.set_source(Some(source(0)), cx));
                                }));
                            }),
                        )),
                )
                .child(
                    self.progressive_image(6, cx)
                        .placeholder(ImagePlaceholder::Content(Rc::new(|_, _| {
                            div()
                                .id("image.reload.placeholder")
                                .debug_selector(|| "image.reload.placeholder".into())
                                .size_full()
                                .bg(gpui::rgba(0xffffff4d))
                                .flex()
                                .items_center()
                                .justify_center()
                                .child("Loading…")
                                .into_any_element()
                        }))),
                ),
        )
        .child(
            div()
                .flex()
                .flex_col()
                .gap(px(8.))
                .child(
                    div()
                        .id("image.generate.wrapper")
                        .debug_selector(|| "image.generate".into())
                        .child(
                            Button::new("image.generate")
                                .label("Generate")
                                .disabled(self.generating)
                                .on_click(cx.listener(|this, _, _, cx| {
                                    this.percent = 0.;
                                    this.generating = true;
                                    this.images[7].update(cx, |s, cx| s.set_source(None, cx));
                                    this.generation_task = Some(cx.spawn(async move |this, cx| {
                                        for step in 1..=20 {
                                            cx.background_executor()
                                                .timer(Duration::from_millis(200))
                                                .await;
                                            if this
                                                .update(cx, |s, cx| {
                                                    s.percent = (step * 5) as f32;
                                                    if step == 20 {
                                                        s.generating = false;
                                                        s.images[7].update(cx, |image, cx| {
                                                            image.set_source(Some(source(0)), cx)
                                                        });
                                                    }
                                                    cx.notify();
                                                })
                                                .is_err()
                                            {
                                                return;
                                            }
                                        }
                                    }));
                                    cx.notify();
                                })),
                        ),
                )
                .child(if self.percent >= 100. {
                    self.progressive_image(7, cx).into_any_element()
                } else {
                    self.progressive_image(7, cx)
                        .progress(ImageProgress {
                            percent: Some(self.percent),
                            render: Some(Rc::new(|rail, p, _, _| {
                                div()
                                    .w_full()
                                    .child(rail)
                                    .child(format!("Generating {p}%"))
                                    .into_any_element()
                            })),
                        })
                        .into_any_element()
                }),
        )
        .into_any_element()
    }

    fn fallback_content(&self, cx: &mut Context<Self>) -> AnyElement {
        let row = div().flex().flex_wrap().gap(px(16.)).items_start();
        row.child(self.image(8, cx).h(px(200.)))
            .child(self.image(9, cx).h(px(200.)).alt("Unavailable image"))
            .into_any_element()
    }

    fn group_content(&self, cx: &mut Context<Self>) -> AnyElement {
        let row = div().flex().flex_wrap().gap(px(16.)).items_start();
        PreviewGroup::new("image.group", &self.groups[0])
            .child(row.children(
                (10..13).map(|i| self.image(i, cx).preview_group(&self.groups[0]).w(px(120.))),
            ))
            .into_any_element()
    }

    fn hidden_group_content(&self, cx: &mut Context<Self>) -> AnyElement {
        PreviewGroup::new("image.one", &self.groups[1])
            .child(self.image(13, cx).preview_group(&self.groups[1]))
            .into_any_element()
    }

    fn preview_source_content(&self, cx: &mut Context<Self>) -> AnyElement {
        self.image(14, cx).into_any_element()
    }

    fn controlled_content(&self, cx: &mut Context<Self>) -> AnyElement {
        let state = self.groups[3].clone();
        div()
            .flex()
            .flex_col()
            .gap(px(16.))
            .child(
                div()
                    .flex()
                    .items_center()
                    .gap(px(8.))
                    .child("scaleStep:")
                    .child(
                        InputNumber::new("image.scale-step", &self.zoom_step, cx)
                            .scope(self.scope.clone())
                            .w(px(100.)),
                    ),
            )
            .child(
                div()
                    .id("controlled-open")
                    .debug_selector(|| "image.controlled.open".to_owned())
                    .child(
                        Button::new("image.controlled.open")
                            .label("show image preview")
                            .on_click(move |_, w, cx| {
                                state.update(cx, |s, cx| s.request_open(0, w, cx))
                            }),
                    ),
            )
            .child(PreviewGroup::new("image.controlled", &self.groups[3]))
            .into_any_element()
    }

    fn toolbar_content(&self, cx: &mut Context<Self>) -> AnyElement {
        let row = div().flex().flex_wrap().gap(px(16.)).items_start();
        PreviewGroup::new("image.toolbar", &self.groups[2])
            .child(
                row.child(
                    div()
                        .id("image.toolbar.thumbnail.wrapper")
                        .debug_selector(|| "image.toolbar.thumbnail".into())
                        .child(
                            self.image(16, cx)
                                .preview_group(&self.groups[2])
                                .w(px(150.)),
                        ),
                )
                .child(
                    self.image(17, cx)
                        .preview_group(&self.groups[2])
                        .w(px(150.)),
                ),
            )
            .into_any_element()
    }

    fn image_render_content(&self, cx: &mut Context<Self>) -> AnyElement {
        self.image(18, cx)
            .preview_options(self.options[18].clone())
            .into_any_element()
    }

    fn mask_content(&self, cx: &mut Context<Self>) -> AnyElement {
        let row = div().flex().flex_wrap().gap(px(16.)).items_start();
        row.children((19..22).map(|i| {
            self.image(i, cx)
                .w(px(100.))
                .preview_options(self.options[i].clone())
                .cover(Rc::new(move |_, _| {
                    div()
                        .child(match i {
                            19 => "blur",
                            20 => "Dimmed mask",
                            _ => "No mask",
                        })
                        .into_any_element()
                }))
        }))
        .into_any_element()
    }

    fn styles_content(&self, cx: &mut Context<Self>) -> AnyElement {
        let row = div().flex().flex_wrap().gap(px(16.)).items_start();
        row.child(
            self.image(22, cx).w(px(160.)).styles(ImageStyles {
                root: StyleRefinement::default()
                    .p(px(4.))
                    .rounded(px(8.))
                    .overflow_hidden(),
                image: StyleRefinement::default().rounded(px(4.)),
                ..Default::default()
            }),
        )
        .child(
            self.image(23, cx)
                .w(px(160.))
                .grayscale(0.5)
                .preview_options(self.options[23].clone())
                .styles(ImageStyles {
                    root: StyleRefinement::default()
                        .p(px(4.))
                        .rounded(px(8.))
                        .overflow_hidden(),
                    image: StyleRefinement::default().rounded(px(4.)),
                    ..Default::default()
                })
                .styles_fn(semantic_styles()),
        )
        .into_any_element()
    }

    fn nested_content(&self, cx: &mut Context<Self>) -> AnyElement {
        let state = self.modals[0].clone();
        div()
            .child(
                div()
                    .id("image.nested.open.wrapper")
                    .debug_selector(|| "image.nested.open".into())
                    .child(
                        Button::new("image.nested.open")
                            .focus_handle(&self.nested_focus[0])
                            .label("showModal")
                            .on_click(move |_, w, cx| {
                                state.update(cx, |s, cx| s.set_open(true, w, cx))
                            }),
                    ),
            )
            .child(Modal::new("image.nested.root", &self.modals[0], cx))
            .into_any_element()
    }
}
impl Render for ImagePage {
    fn render(&mut self, w: &mut Window, cx: &mut Context<Self>) -> impl IntoElement {
        let config = self.scope.snapshot(cx);
        let g = &config.theme.tokens;
        let width = w.viewport_size().width;
        let two = width - sidebar_width(width) - page_padding(width) * 2. >= px(840.);
        div()
            .id("gallery.image.examples")
            .w_full()
            .grid()
            .grid_cols(if two { 2 } else { 1 })
            .items_start()
            .gap(px(g.margin_lg as f32))
            .when(config.direction == Direction::Rtl, |d| d.text_right())
            .children(DEMOS.iter().enumerate().map(|(i, demo)| {
                div()
                    .id(demo.id)
                    .anchor_scroll(Some(self.anchors[i].clone()))
                    .role(Role::Group)
                    .aria_label(demo.title)
                    .debug_selector(|| format!("gallery.card.{}", demo.id))
                    .min_w(px(0.))
                    .border_1()
                    .border_color(g.color_border_secondary)
                    .rounded(px(g.border_radius_lg as f32))
                    .bg(g.color_bg_container)
                    .child(
                        div()
                            .p(px(g.padding as f32))
                            .font_weight(FontWeight::SEMIBOLD)
                            .child(demo.title),
                    )
                    .child(
                        div()
                            .p(px(g.padding_lg as f32))
                            .min_w(px(0.))
                            .debug_selector(|| format!("gallery.preview.{}", demo.id))
                            .child(self.content(i, cx)),
                    )
                    .child(
                        div()
                            .border_t_1()
                            .border_color(g.color_border_secondary)
                            .p(px(g.padding as f32))
                            .text_size(px(g.font_size_sm as f32))
                            .line_height(relative(g.line_height as f32))
                            .text_color(g.color_text_secondary)
                            .child(demo.description),
                    )
            }))
    }
}
