// Copyright (C) 2026 mg-chao
// SPDX-License-Identifier: Apache-2.0

//! A native inline pair's rounded max-content contribution must agree with
//! the width advertised by its table. A raw-width correction at only the table
//! level used to wrap a phone number inside an otherwise unconstrained table.

use gpui::{
    AppContext, Background, Context, Render, StyleRefinement, Styled, TestAppContext,
    VisualTestContext, Window, div, prelude::*, px, rgb,
};

use super::{DescriptionItem, DescriptionItemStyles, Descriptions, DescriptionsStyles};
use crate::{ConfigPatch, ConfigScope, Direction, UiRoot};

const VALUES: [u32; 3] = [0x126001, 0x126002, 0x126003];

struct InlineProbe {
    scope: ConfigScope,
}

impl Render for InlineProbe {
    fn render(&mut self, _: &mut Window, _: &mut Context<Self>) -> impl IntoElement {
        let mut details = Descriptions::new("intrinsic-inline")
            .scope(self.scope.clone())
            .columns(3)
            .styles(DescriptionsStyles {
                root: StyleRefinement::default()
                    .text_size(px(13.7))
                    .line_height(px(22.)),
                ..Default::default()
            });
        for (index, (label, value)) in [
            ("UserName", "ZhouMaomao"),
            ("Telephone", "18100000000"),
            ("Billing Mode", "AnnualSubscription"),
        ]
        .into_iter()
        .enumerate()
        {
            details = details.item(
                DescriptionItem::new(index)
                    .label(label)
                    .content(value)
                    .styles(DescriptionItemStyles {
                        content: StyleRefinement::default().bg(rgb(VALUES[index])),
                        ..Default::default()
                    }),
            );
        }
        // No explicit child width: the complete subtree determines max-content.
        div()
            .w(px(1000.))
            .flex()
            .flex_col()
            .items_start()
            .child(div().flex_none().max_w_full().child(details))
    }
}

#[test]
fn borderless_inline_max_content_preserves_single_line_values_at_fractional_advances() {
    for direction in [Direction::Ltr, Direction::Rtl] {
        for scale in [1., 1.5, 2.] {
            let mut cx = TestAppContext::single();
            let window = cx.add_window(|window, cx| {
                window.set_scale_factor(scale);
                let scope = ConfigScope::new_window(cx);
                scope
                    .update_patch(cx, |patch| {
                        *patch = ConfigPatch {
                            direction: Some(direction),
                            ..Default::default()
                        };
                    })
                    .unwrap();
                let probe = cx.new(|_| InlineProbe {
                    scope: scope.clone(),
                });
                UiRoot::new(scope, probe, window, cx)
            });
            let mut visual = VisualTestContext::from_window(window.into(), &cx);
            visual.update(|window, cx| window.draw(cx).clear(cx));
            visual.update(|window, _| {
                let scene = window.rendered_scene();
                for color in VALUES {
                    let expected: Background = rgb(color).into();
                    let bounds = scene.quads.iter()
                        .find(|quad| quad.background == expected)
                        .unwrap_or_else(|| panic!("missing content {color:06x}"))
                        .bounds;
                    assert_eq!(bounds.size.height.0 / scale, 22.,
                        "max-content must fit the native inline subtree without emergency wrapping: {direction:?}, scale {scale}");
                }
            });
        }
    }
}
