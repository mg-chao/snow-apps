// Copyright (C) 2026 mg-chao
// SPDX-License-Identifier: Apache-2.0
// Token defaults adapted from Ant Design 6.6.7 (MIT).

use crate::{
    Direction,
    theme::{AliasTokens, ComponentTheme},
};
use gpui::{Refineable, StyleRefinement};
use std::sync::Arc;

pub struct CarouselTheme;

macro_rules! tokens {
    ($($field:ident = $value:expr;)+) => {
        #[derive(Clone, Debug, PartialEq)]
        pub struct CarouselTokens {
            $(pub $field: f64,)+
            pub(crate) globals: Arc<AliasTokens>,
        }
        #[derive(Clone, Debug, Default, PartialEq)]
        pub struct CarouselTokenPatch { $(pub $field: Option<f64>,)+ }
        impl ComponentTheme for CarouselTheme {
            type Tokens = CarouselTokens;
            type Patch = CarouselTokenPatch;
            fn default_tokens(g: &AliasTokens) -> CarouselTokens {
                CarouselTokens { $($field: ($value)(g),)+ globals: Arc::new(g.clone()) }
            }
            fn merge(parent: &CarouselTokenPatch, local: &CarouselTokenPatch) -> CarouselTokenPatch {
                CarouselTokenPatch { $($field: local.$field.or(parent.$field),)+ }
            }
            fn apply(tokens: &mut CarouselTokens, patch: &CarouselTokenPatch) {
                let defaults = Self::default_tokens(&tokens.globals);
                $(if let Some(value) = patch.$field {
                    tokens.$field = if value.is_finite() && (value as f32).is_finite() {
                        value.max(0.)
                    } else { defaults.$field };
                })+
            }
        }
    };
}
tokens! {
    dot_width = |_: &AliasTokens| 16.;
    dot_height = |_: &AliasTokens| 3.;
    dot_active_width = |_: &AliasTokens| 24.;
    dot_gap = |g: &AliasTokens| g.margin_xxs;
    dot_offset = |_: &AliasTokens| 12.;
    arrow_size = |_: &AliasTokens| 16.;
    arrow_offset = |g: &AliasTokens| g.margin_xs;
}
impl CarouselTokens {
    pub fn globals(&self) -> &AliasTokens {
        &self.globals
    }
}

/// Native style slots; unset properties inherit the preceding configuration layer.
#[derive(Clone, Debug, Default, PartialEq)]
pub struct CarouselStyles {
    pub root: StyleRefinement,
    pub viewport: StyleRefinement,
    pub slide: StyleRefinement,
    pub dots: StyleRefinement,
    pub dot: StyleRefinement,
    pub arrow: StyleRefinement,
}
impl CarouselStyles {
    pub(crate) fn refine(&mut self, other: &Self) {
        self.root.refine(&other.root);
        self.viewport.refine(&other.viewport);
        self.slide.refine(&other.slide);
        self.dots.refine(&other.dots);
        self.dot.refine(&other.dot);
        self.arrow.refine(&other.arrow);
    }
}

#[derive(Clone, Copy, Debug, PartialEq)]
pub struct CarouselStyleContext {
    pub current: Option<usize>,
    pub count: usize,
    pub vertical: bool,
    pub direction: Direction,
    pub autoplay: bool,
    pub paused: bool,
}
