// Copyright (C) 2026 mg-chao
// SPDX-License-Identifier: Apache-2.0

use std::{
    any::{Any, TypeId},
    collections::HashMap,
    fmt,
    marker::PhantomData,
    sync::Arc,
};

use super::{AlgorithmChain, AliasTokens, TokenPatch};

/// Extension implemented by each future control's marker type.
///
/// Patches should represent optional fields; `merge` must preserve inherited
/// fields that are absent locally. Component tokens remain outside the global
/// inventory and are resolved lazily from the effective global aliases.
pub trait ComponentTheme: Send + Sync + 'static {
    type Tokens: Clone + fmt::Debug + PartialEq + Send + Sync + 'static;
    type Patch: Clone + fmt::Debug + Default + PartialEq + Send + Sync + 'static;
    fn default_tokens(global: &AliasTokens) -> Self::Tokens;
    fn merge(parent: &Self::Patch, local: &Self::Patch) -> Self::Patch;
    fn apply(tokens: &mut Self::Tokens, patch: &Self::Patch);
}

pub struct ComponentThemeConfig<C: ComponentTheme> {
    pub patch: C::Patch,
    pub tokens: TokenPatch,
    /// `None` applies global overrides directly; `Some` recomputes their aliases.
    pub algorithms: Option<AlgorithmChain>,
    inherit_algorithms: bool,
    marker: PhantomData<fn() -> C>,
}
impl<C: ComponentTheme> ComponentThemeConfig<C> {
    pub fn new(patch: C::Patch) -> Self {
        Self {
            patch,
            ..Self::default()
        }
    }
    pub fn with_tokens(mut self, tokens: TokenPatch) -> Self {
        self.tokens = tokens;
        self
    }
    pub fn with_algorithms(mut self, algorithms: AlgorithmChain) -> Self {
        self.algorithms = Some(algorithms);
        self
    }
    /// Apply global overrides directly, clearing an inherited component algorithm.
    pub fn without_algorithms(mut self) -> Self {
        self.algorithms = None;
        self.inherit_algorithms = false;
        self
    }
}
impl<C: ComponentTheme> Default for ComponentThemeConfig<C> {
    fn default() -> Self {
        Self {
            patch: Default::default(),
            tokens: Default::default(),
            algorithms: None,
            inherit_algorithms: true,
            marker: PhantomData,
        }
    }
}
impl<C: ComponentTheme> Clone for ComponentThemeConfig<C> {
    fn clone(&self) -> Self {
        Self {
            patch: self.patch.clone(),
            tokens: self.tokens.clone(),
            algorithms: self.algorithms.clone(),
            inherit_algorithms: self.inherit_algorithms,
            marker: PhantomData,
        }
    }
}
impl<C: ComponentTheme> fmt::Debug for ComponentThemeConfig<C> {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        f.debug_struct("ComponentThemeConfig")
            .field("patch", &self.patch)
            .field("tokens", &self.tokens)
            .field("algorithms", &self.algorithms)
            .field("inherit_algorithms", &self.inherit_algorithms)
            .finish()
    }
}
impl<C: ComponentTheme> PartialEq for ComponentThemeConfig<C> {
    fn eq(&self, other: &Self) -> bool {
        self.patch == other.patch
            && self.tokens == other.tokens
            && self.algorithms == other.algorithms
            && self.inherit_algorithms == other.inherit_algorithms
    }
}

trait ErasedComponent: fmt::Debug + Send + Sync {
    fn as_any(&self) -> &dyn Any;
    fn merged(&self, local: &dyn ErasedComponent) -> Arc<dyn ErasedComponent>;
    fn equals(&self, other: &dyn ErasedComponent) -> bool;
    fn validate(&self) -> Result<(), super::ThemeError>;
}
impl<C: ComponentTheme> ErasedComponent for ComponentThemeConfig<C> {
    fn as_any(&self) -> &dyn Any {
        self
    }
    fn merged(&self, local: &dyn ErasedComponent) -> Arc<dyn ErasedComponent> {
        let local = local
            .as_any()
            .downcast_ref::<Self>()
            .expect("component theme type matches registry key");
        Arc::new(Self {
            patch: C::merge(&self.patch, &local.patch),
            tokens: TokenPatch::merged(&self.tokens, &local.tokens),
            algorithms: local.algorithms.clone().or_else(|| {
                if local.inherit_algorithms {
                    self.algorithms.clone()
                } else {
                    None
                }
            }),
            inherit_algorithms: true,
            marker: PhantomData,
        })
    }
    fn equals(&self, other: &dyn ErasedComponent) -> bool {
        other
            .as_any()
            .downcast_ref::<Self>()
            .is_some_and(|other| self == other)
    }
    fn validate(&self) -> Result<(), super::ThemeError> {
        self.tokens.validate()
    }
}

/// Type-indexed component overrides. No component names or string token keys are required.
#[derive(Clone, Debug, Default)]
pub struct ComponentThemes(HashMap<TypeId, Arc<dyn ErasedComponent>>);
impl PartialEq for ComponentThemes {
    fn eq(&self, other: &Self) -> bool {
        self.0.len() == other.0.len()
            && self.0.iter().all(|(key, entry)| {
                other
                    .0
                    .get(key)
                    .is_some_and(|other| Arc::ptr_eq(entry, other) || entry.equals(other.as_ref()))
            })
    }
}
impl ComponentThemes {
    pub fn set<C: ComponentTheme>(&mut self, patch: C::Patch) {
        self.set_theme(ComponentThemeConfig::<C>::new(patch));
    }
    pub fn set_theme<C: ComponentTheme>(&mut self, config: ComponentThemeConfig<C>) {
        self.0.insert(TypeId::of::<C>(), Arc::new(config));
    }
    pub fn get<C: ComponentTheme>(&self) -> Option<&ComponentThemeConfig<C>> {
        self.0
            .get(&TypeId::of::<C>())
            .and_then(|entry| entry.as_any().downcast_ref())
    }
    pub fn remove<C: ComponentTheme>(&mut self) {
        self.0.remove(&TypeId::of::<C>());
    }
    pub fn is_empty(&self) -> bool {
        self.0.is_empty()
    }
    pub fn merged(parent: &Self, local: &Self) -> Self {
        let mut merged = parent.clone();
        for (key, entry) in &local.0 {
            let value = match parent.0.get(key) {
                Some(parent) => parent.merged(entry.as_ref()),
                None => entry.clone(),
            };
            merged.0.insert(*key, value);
        }
        merged
    }
    pub(crate) fn validate(&self) -> Result<(), super::ThemeError> {
        for component in self.0.values() {
            component.validate()?;
        }
        Ok(())
    }
}
