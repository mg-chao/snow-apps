// Copyright (C) 2026 mg-chao
// SPDX-License-Identifier: Apache-2.0

//! Native component gallery. Run with `--capture <path.png>` for a rendered frame.

mod alert;
mod button;
mod carousel;
mod catalog;
mod checkbox;
mod context_menu;
mod descriptions;
mod divider;
mod image;
mod input;
mod input_examples;
mod input_number;
mod menu;
mod message;
mod modal;
mod notification;
mod pagination;
mod popup;
mod progress;
mod radio;
mod result;
mod scroll_area;
mod segmented;
mod select;
mod shell;
mod slider;
mod spin;
mod switch;
mod tabs;
mod tag;

#[cfg(test)]
mod tests;

use std::{path::PathBuf, time::Duration};

use ant_design_gpui::{ConfigScope, Direction, UiRoot, theme::AlgorithmChain};
use gpui::{App, AppContext, Bounds, Focusable, WindowBounds, WindowOptions, px, size};

#[derive(Default)]
struct Options {
    component: catalog::ComponentId,
    capture: Option<PathBuf>,
    capture_scroll_y: Option<f32>,
    headless: bool,
    reduced_motion: bool,
    dark: bool,
    compact: bool,
    rtl: bool,
    window_size: Option<(u32, u32)>,
}

impl Options {
    fn interactive(&self) -> bool {
        // Captures stay hidden; --headless also exits after saving the image.
        self.capture.is_none()
    }

    fn parse(arguments: impl IntoIterator<Item = String>) -> Result<Self, String> {
        let mut options = Self::default();
        let mut arguments = arguments.into_iter();
        while let Some(argument) = arguments.next() {
            match argument.as_str() {
                "--component" => {
                    options.component =
                        match arguments.next().as_deref() {
                            Some("carousel") => catalog::ComponentId::Carousel,
                            Some("button") => catalog::ComponentId::Button,
                            Some("divider") => catalog::ComponentId::Divider,
                            Some("checkbox") => catalog::ComponentId::Checkbox,
                            Some("input") => catalog::ComponentId::Input,
                            Some("input-number") => catalog::ComponentId::InputNumber,
                            Some("radio") => catalog::ComponentId::Radio,
                            Some("select") => catalog::ComponentId::Select,
                            Some("slider") => catalog::ComponentId::Slider,
                            Some("segmented") => catalog::ComponentId::Segmented,
                            Some("switch") => catalog::ComponentId::Switch,
                            Some("tag") => catalog::ComponentId::Tag,
                            Some("tooltip") => catalog::ComponentId::Tooltip,
                            Some("popover") => catalog::ComponentId::Popover,
                            Some("popconfirm") => catalog::ComponentId::Popconfirm,
                            Some("context-menu") => catalog::ComponentId::ContextMenu,
                            Some("menu") => catalog::ComponentId::Menu,
                            Some("pagination") => catalog::ComponentId::Pagination,
                            Some("scroll-area") => catalog::ComponentId::ScrollArea,
                            Some("tabs") => catalog::ComponentId::Tabs,
                            Some("modal") => catalog::ComponentId::Modal,
                            Some("alert") => catalog::ComponentId::Alert,
                            Some("result") => catalog::ComponentId::Result,
                            Some("image") => catalog::ComponentId::Image,
                            Some("descriptions") => catalog::ComponentId::Descriptions,
                            Some("spin") => catalog::ComponentId::Spin,
                            Some("progress") => catalog::ComponentId::Progress,
                            Some("message") => catalog::ComponentId::Message,
                            Some("notification") => catalog::ComponentId::Notification,

                            _ => return Err(
                                "--component requires button, divider, checkbox, input, input-number, radio, select, slider, segmented, switch, tag, tooltip, popover, popconfirm, context-menu, menu, pagination, scroll-area, tabs, alert, result, descriptions, spin, progress, message, notification, carousel, or modal"
                                    .into(),
                            ),
                        };
                }
                "--capture" => {
                    options.capture = Some(
                        arguments
                            .next()
                            .filter(|path| !path.starts_with("--"))
                            .ok_or("--capture requires an output path")?
                            .into(),
                    );
                }
                "--capture-scroll-y" => {
                    options.capture_scroll_y = Some(
                        arguments
                            .next()
                            .and_then(|value| value.parse::<f32>().ok())
                            .filter(|value| value.is_finite() && *value >= 0.)
                            .ok_or("--capture-scroll-y requires a finite nonnegative offset")?,
                    );
                }
                "--reduced-motion" => options.reduced_motion = true,
                "--headless" => options.headless = true,
                "--dark" => options.dark = true,
                "--compact" => options.compact = true,
                "--rtl" => options.rtl = true,
                "--window-size" => {
                    let value = arguments
                        .next()
                        .ok_or("--window-size requires WIDTHxHEIGHT")?;
                    let (width, height) = value
                        .split_once('x')
                        .ok_or("--window-size requires WIDTHxHEIGHT")?;
                    let dimensions = (width.parse::<u32>(), height.parse::<u32>());
                    let (Ok(width), Ok(height)) = dimensions else {
                        return Err("--window-size requires integer dimensions".into());
                    };
                    if width < 640 || height < 480 {
                        return Err("The minimum gallery window size is 640x480".into());
                    }
                    options.window_size = Some((width, height));
                }
                _ => return Err(format!("unknown argument: {argument}")),
            }
        }
        if options.headless && options.capture.is_none() {
            return Err("--headless requires --capture".into());
        }
        if options.capture_scroll_y.is_some() && options.capture.is_none() {
            return Err("--capture-scroll-y requires --capture".into());
        }
        Ok(options)
    }
}

fn main() {
    let options = Options::parse(std::env::args().skip(1)).unwrap_or_else(|error| {
        eprintln!("{error}\nUsage: gallery [--component button|divider|checkbox|input|input-number|radio|select|slider|segmented|switch|tag|tooltip|popover|popconfirm|context-menu|menu|pagination|scroll-area|tabs|alert|result|descriptions|spin|progress|message|notification|modal|carousel] [--dark] [--compact] [--rtl] [--reduced-motion] [--window-size WIDTHxHEIGHT] [--capture <path.png>] [--capture-scroll-y OFFSET] [--headless]");
        std::process::exit(2);
    });
    gpui_platform::application().run(move |cx: &mut App| {
        ant_design_gpui::init(cx);
        let scope = ConfigScope::new_window(cx);
        scope
            .update_patch(cx, |patch| {
                patch.motion = Some(!options.reduced_motion);
                patch.direction = Some(if options.rtl {
                    Direction::Rtl
                } else {
                    Direction::Ltr
                });
                patch.theme.get_or_insert_default().algorithms =
                    Some(match (options.dark, options.compact) {
                        (false, false) => AlgorithmChain::default(),
                        (true, false) => AlgorithmChain::dark(),
                        (false, true) => AlgorithmChain::compact(),
                        (true, true) => AlgorithmChain::dark_compact(),
                    });
            })
            .expect("valid gallery configuration");
        let (width, height) = options.window_size.unwrap_or((1280, 900));
        let bounds = Bounds::centered(None, size(px(width as f32), px(height as f32)), cx);
        let interactive = options.interactive();
        cx.open_window(
            WindowOptions {
                window_bounds: Some(WindowBounds::Windowed(bounds)),
                window_min_size: Some(size(px(640.), px(480.))),
                show: interactive,
                focus: interactive,
                ..Default::default()
            },
            |window, cx| {
                if !interactive {
                    window.resize(size(px(width as f32), px(height as f32)));
                }
                window.set_window_title("Ant Design GPUI · Components");
                let gallery = cx.new(|cx| {
                    let mut gallery = shell::Gallery::new(scope.clone(), window, cx);
                    gallery.selected = options.component;
                    gallery
                });
                let root = cx.new(|cx| UiRoot::new(scope, gallery.clone(), window, cx));
                window.focus(&root.focus_handle(cx), cx);
                if interactive {
                    window.activate_window();
                }
                if let Some(path) = options.capture {
                    window
                        .spawn(cx, async move |cx| {
                            cx.background_executor()
                                .timer(Duration::from_millis(300))
                                .await;
                            // Hidden windows do not receive presentation frames.
                            // Flush layout-dependent controls through the event loop:
                            // initial measurement, overflow reservation, selection reveal.
                            for _ in 0..3 {
                                cx.update(|window, cx| window.draw(cx).clear(cx)).ok();
                                cx.background_executor()
                                    .timer(Duration::from_millis(16))
                                    .await;
                            }
                            if let Some(offset) = options.capture_scroll_y {
                                cx.update(|_, cx| {
                                    gallery.update(cx, |gallery, cx| {
                                        gallery.scroll.set_offset(gpui::point(px(0.), px(-offset)));
                                        cx.notify();
                                    });
                                })
                                .ok();
                                for _ in 0..2 {
                                    cx.update(|window, cx| window.draw(cx).clear(cx)).ok();
                                    cx.background_executor()
                                        .timer(Duration::from_millis(16))
                                        .await;
                                }
                            }
                            let result = cx.update(|window, cx| {
                                window.draw(cx).clear(cx);
                                match window.render_to_image() {
                                    Ok(image) => {
                                        if let Err(error) = image.save(&path) {
                                            eprintln!("Could not save gallery capture: {error}");
                                        }
                                    }
                                    Err(error) => {
                                        eprintln!("Could not render gallery capture: {error}");
                                    }
                                }
                                if options.headless {
                                    cx.quit();
                                }
                            });
                            if let Err(error) = result {
                                eprintln!("Gallery closed before capture: {error}");
                            }
                        })
                        .detach();
                }
                root
            },
        )
        .expect("open component gallery");
        if interactive {
            cx.activate(true);
        }
    });
}
