// Copyright (C) 2026 mg-chao
// SPDX-License-Identifier: Apache-2.0

use crate::theme::AliasTokens;

/// Viewport-based values, using the theme's cumulative Ant Design breakpoints.
/// `xs` applies only at or below `screen_xs_max`.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct DescriptionsBreakpoints<T> {
    pub xs: Option<T>,
    pub sm: Option<T>,
    pub md: Option<T>,
    pub lg: Option<T>,
    pub xl: Option<T>,
    pub xxl: Option<T>,
    pub xxxl: Option<T>,
}

impl<T> Default for DescriptionsBreakpoints<T> {
    fn default() -> Self {
        Self {
            xs: None,
            sm: None,
            md: None,
            lg: None,
            xl: None,
            xxl: None,
            xxxl: None,
        }
    }
}

impl<T: Copy> DescriptionsBreakpoints<T> {
    /// Choose the largest active supplied breakpoint. A smaller supplied
    /// breakpoint remains active at larger viewports, except for `xs`.
    pub fn resolve(&self, width: f64, tokens: &AliasTokens) -> Option<T> {
        [
            (width >= tokens.screen_xxxl, self.xxxl),
            (width >= tokens.screen_xxl, self.xxl),
            (width >= tokens.screen_xl, self.xl),
            (width >= tokens.screen_lg, self.lg),
            (width >= tokens.screen_md, self.md),
            (width >= tokens.screen_sm, self.sm),
            (width <= tokens.screen_xs_max, self.xs),
        ]
        .into_iter()
        .find_map(|(active, value)| if active { value } else { None })
    }
}

/// Logical item columns, independent of the bordered label/value cell count.
#[derive(Clone, Debug, PartialEq, Eq)]
pub enum DescriptionsColumns {
    Fixed(usize),
    Responsive(DescriptionsBreakpoints<usize>),
}

impl Default for DescriptionsColumns {
    fn default() -> Self {
        Self::Responsive(DescriptionsBreakpoints::default())
    }
}

impl From<usize> for DescriptionsColumns {
    fn from(value: usize) -> Self {
        Self::Fixed(value)
    }
}

impl From<DescriptionsBreakpoints<usize>> for DescriptionsColumns {
    fn from(value: DescriptionsBreakpoints<usize>) -> Self {
        Self::Responsive(value)
    }
}

impl DescriptionsColumns {
    pub fn resolve(&self, width: f64, tokens: &AliasTokens) -> usize {
        let value = match self {
            Self::Fixed(value) => Some(*value),
            Self::Responsive(values) => values.resolve(width, tokens),
        };
        value
            .or_else(|| {
                DescriptionsBreakpoints {
                    xs: Some(1),
                    sm: Some(2),
                    md: Some(3),
                    lg: Some(3),
                    xl: Some(3),
                    xxl: Some(3),
                    xxxl: Some(4),
                }
                .resolve(width, tokens)
            })
            .unwrap_or(3)
            .max(1)
    }
}

/// An item's logical width. `Filled` closes its row using the remaining space.
#[derive(Clone, Debug, PartialEq, Eq)]
pub enum DescriptionsSpan {
    Fixed(usize),
    Responsive(DescriptionsBreakpoints<usize>),
    Filled,
}

impl Default for DescriptionsSpan {
    fn default() -> Self {
        Self::Fixed(1)
    }
}

impl From<usize> for DescriptionsSpan {
    fn from(value: usize) -> Self {
        Self::Fixed(value)
    }
}

impl From<DescriptionsBreakpoints<usize>> for DescriptionsSpan {
    fn from(value: DescriptionsBreakpoints<usize>) -> Self {
        Self::Responsive(value)
    }
}

impl DescriptionsSpan {
    /// `None` denotes a filled item; missing responsive values use one column.
    pub fn resolve(&self, width: f64, tokens: &AliasTokens) -> Option<usize> {
        match self {
            Self::Fixed(value) => Some((*value).max(1)),
            Self::Responsive(values) => Some(values.resolve(width, tokens).unwrap_or(1).max(1)),
            Self::Filled => None,
        }
    }
}

#[derive(Clone, Copy, Debug, Default, PartialEq, Eq, Hash)]
pub enum DescriptionsLayout {
    #[default]
    Horizontal,
    Vertical,
}
