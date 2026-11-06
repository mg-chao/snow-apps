// Copyright (C) 2026 mg-chao
// SPDX-License-Identifier: Apache-2.0

//! Resources belong to native scenes, including cached scene replays. Registries
//! contain weak lookup metadata only; they must not extend a hidden surface's life.

use gpui::{App, BorrowAppContext, Window};
use gpui::{Global, RenderImage};
use std::{collections::HashMap, sync::Arc};
use std::{
    hash::Hash,
    rc::{Rc, Weak},
};

pub(crate) fn next_resource_id() -> u64 {
    static NEXT: std::sync::atomic::AtomicU64 = std::sync::atomic::AtomicU64::new(1);
    NEXT.fetch_add(1, std::sync::atomic::Ordering::Relaxed)
}

pub(crate) fn resource_fingerprint(key: &impl Hash) -> u64 {
    use std::hash::Hasher;
    let mut hash = std::collections::hash_map::DefaultHasher::new();
    key.hash(&mut hash);
    hash.finish()
}

#[derive(Default)]
struct HiddenCleanup(std::collections::HashSet<gpui::WindowId>);
impl Global for HiddenCleanup {}

/// Native hidden windows have no frame source. Retire their old cached scene in
/// one deferred, unpresented draw shared by every overlay host in the window.
pub(crate) fn schedule_hidden_cleanup(window: &mut Window, cx: &mut App) {
    if !cx.has_global::<HiddenCleanup>() {
        cx.set_global(HiddenCleanup::default());
    }
    let target = window.window_handle();
    let id = target.window_id();
    if !cx.global_mut::<HiddenCleanup>().0.insert(id) {
        return;
    }
    cx.defer(move |cx| {
        if !cx.has_global::<HiddenCleanup>() {
            return;
        }
        cx.update_global::<HiddenCleanup, _>(|pending, _| {
            pending.0.remove(&id);
            if pending.0.is_empty() {
                pending.0 = Default::default();
            }
        });
        let _ = target.update(cx, |_, window, cx| {
            if !window.is_visible() {
                window.refresh();
                window.draw(cx).clear(cx);
            }
        });
    });
}

/// Lightweight owners of native title hints also work outside an overlay host.
pub(crate) fn observe_native_hint_visibility(window: &mut Window) -> gpui::Subscription {
    window.observe_window_visibility(|visible, window, cx| {
        if visible.is_visible() {
            window.refresh();
        } else {
            schedule_hidden_cleanup(window, cx);
        }
    })
}

/// Active collections may grow with their live users, but keep no peak-sized
/// bucket allocation after most users have retired.
pub(crate) fn trim_sparse_map<K: Eq + Hash, V>(map: &mut HashMap<K, V>) {
    if map.is_empty() {
        *map = HashMap::new();
    } else if map.capacity() > map.len().saturating_mul(4).max(16) {
        map.shrink_to(map.len().saturating_mul(2).max(16));
    }
}

/// LRU eviction drops lookup metadata only. Active scenes keep their records
/// alive even when there are more consumers than lookup slots.
pub(crate) struct WeakRegistry<K, V> {
    entries: HashMap<K, (Weak<V>, u64)>,
    tick: u64,
}
impl<K, V> Default for WeakRegistry<K, V> {
    fn default() -> Self {
        Self {
            entries: HashMap::new(),
            tick: 0,
        }
    }
}
impl<K: Clone + Eq + Hash, V> WeakRegistry<K, V> {
    pub(crate) fn get_or_insert_with(
        &mut self,
        key: K,
        capacity: usize,
        create: impl FnOnce() -> V,
    ) -> Rc<V> {
        self.tick = self.tick.wrapping_add(1);
        let value = self
            .entries
            .get(&key)
            .and_then(|(value, _)| value.upgrade())
            .unwrap_or_else(|| Rc::new(create()));
        self.prune();
        self.entries.insert(key, (Rc::downgrade(&value), self.tick));
        while self.entries.len() > capacity {
            let key = self
                .entries
                .iter()
                .min_by_key(|(_, (_, used))| used)
                .unwrap()
                .0
                .clone();
            self.entries.remove(&key);
        }
        value
    }
    pub(crate) fn prune(&mut self) {
        self.entries
            .retain(|_, (value, _)| value.strong_count() > 0);
        if self.entries.is_empty() {
            self.entries = HashMap::new();
        }
    }
    #[cfg(test)]
    pub(crate) fn len(&self) -> usize {
        self.entries.len()
    }
}

#[derive(Default)]
struct NativeImageUsers(HashMap<usize, usize>);
impl Global for NativeImageUsers {}

/// Export caches and different native components can share a RenderImage even
/// when their cache-record identities differ. Recheck actual scene users after
/// the current frame/effect has released or installed all of its leases.
pub(crate) fn retire_native_image(image: Arc<RenderImage>, cx: &mut App) {
    let id = image.id.0;
    cx.defer(move |cx| {
        if cx
            .try_global::<NativeImageUsers>()
            .is_none_or(|users| !users.0.contains_key(&id))
        {
            cx.drop_image(image, None);
        }
    });
}

struct NativeImageLease(Arc<RenderImage>);

/// Count actual native-scene users, rather than cache-record identities. The
/// same pixels can be reached through a decoded source and a direct Render source.
pub(crate) fn lease_native_image(image: &Arc<RenderImage>, window: &mut Window, cx: &mut App) {
    if !cx.has_global::<NativeImageUsers>() {
        cx.set_global(NativeImageUsers::default());
    }
    window.with_id(("native-image-users", window.current_view()), |window| {
        window.use_keyed_state(("native-image-user", image.id.0 as u64), cx, |_, cx| {
            cx.update_global::<NativeImageUsers, _>(|users, _| {
                *users.0.entry(image.id.0).or_default() += 1;
            });
            cx.on_release(|lease: &mut NativeImageLease, cx| {
                if !cx.has_global::<NativeImageUsers>() {
                    return;
                }
                let id = lease.0.id.0;
                let last = cx.update_global::<NativeImageUsers, _>(|users, _| {
                    let Some(count) = users.0.get_mut(&id) else {
                        return false;
                    };
                    *count -= 1;
                    if *count == 0 {
                        users.0.remove(&id);
                        if users.0.is_empty() {
                            users.0 = HashMap::new();
                        }
                        true
                    } else {
                        false
                    }
                });
                if last {
                    retire_native_image(lease.0.clone(), cx);
                }
            })
            .detach();
            NativeImageLease(image.clone())
        });
    });
}

struct SceneLease<R> {
    resource: Option<R>,
    is_last: fn(&R) -> bool,
    retire: fn(&R, &mut App),
}

/// Recover this consumer's active record before consulting bounded weak lookup
/// metadata. Evicting that metadata must never restart an in-flight preparation
/// or replace ready content still owned by the scene. The caller validates the
/// full request key because the native state key contains only its fingerprint.
pub(crate) fn scene_resource<R: Clone + 'static>(
    key: (&'static str, u64),
    matches: impl FnOnce(&R) -> bool,
    create: impl FnOnce(&mut Window, &mut App) -> R,
    is_last: fn(&R) -> bool,
    retire: fn(&R, &mut App),
    window: &mut Window,
    cx: &mut App,
) -> R {
    try_scene_resource(
        key,
        matches,
        |window, cx| Ok::<_, std::convert::Infallible>(create(window, cx)),
        is_last,
        retire,
        window,
        cx,
    )
    .unwrap_or_else(|never| match never {})
}

/// Fallible preparations use the same scene ownership as decoded resources.
/// A failed replacement releases the obsolete record and leaves an empty slot,
/// so later frames can retry without retaining pixels for an unrelated key.
pub(crate) fn try_scene_resource<R: Clone + 'static, E>(
    key: (&'static str, u64),
    matches: impl FnOnce(&R) -> bool,
    create: impl FnOnce(&mut Window, &mut App) -> Result<R, E>,
    is_last: fn(&R) -> bool,
    retire: fn(&R, &mut App),
    window: &mut Window,
    cx: &mut App,
) -> Result<R, E> {
    window.with_id((key.0, window.current_view()), |window| {
        let slot = window.use_keyed_state(key, cx, |_, cx| {
            cx.on_release(|lease: &mut SceneLease<R>, cx| {
                if let Some(resource) = lease.resource.take()
                    && (lease.is_last)(&resource)
                {
                    (lease.retire)(&resource, cx);
                }
            })
            .detach();
            SceneLease {
                resource: None,
                is_last,
                retire,
            }
        });
        if let Some(resource) = slot
            .read(cx)
            .resource
            .as_ref()
            .filter(|value| matches(value))
        {
            return Ok(resource.clone());
        }
        // Creation can borrow the registry currently calling us. Do not hold a
        // state borrow across it, nor retire a replaced record synchronously.
        let resource = create(window, cx);
        let old = slot.update(cx, |slot, _| {
            std::mem::replace(&mut slot.resource, resource.as_ref().ok().cloned())
        });
        if let Some(old) = old {
            cx.defer(move |cx| {
                if is_last(&old) {
                    retire(&old, cx);
                }
            });
        }
        resource
    })
}

#[cfg(test)]
mod tests {
    use super::*;
    use gpui::AppContext;
    use std::cell::Cell;

    #[test]
    fn active_scene_slots_survive_lookup_pressure_collisions_and_failed_replacements() {
        struct Resource {
            key: u64,
            retirements: Rc<Cell<usize>>,
        }
        struct Consumer {
            registry: WeakRegistry<u64, Resource>,
            probes: Vec<Weak<Resource>>,
            creations: usize,
            retirements: Rc<Cell<usize>>,
            revision: u64,
            fail_preparation: bool,
            shown: bool,
        }
        impl gpui::Render for Consumer {
            fn render(
                &mut self,
                window: &mut Window,
                cx: &mut gpui::Context<Self>,
            ) -> impl gpui::IntoElement {
                if self.shown {
                    for slot in 0..3 {
                        let key = slot + self.revision * 3;
                        let result = try_scene_resource(
                            ("scene-pressure-test", slot),
                            |record: &Rc<Resource>| record.key == key,
                            |_, _| {
                                if self.fail_preparation {
                                    return Err(());
                                }
                                Ok(self.registry.get_or_insert_with(key, 2, || {
                                    self.creations += 1;
                                    Resource {
                                        key,
                                        retirements: self.retirements.clone(),
                                    }
                                }))
                            },
                            |record| Rc::strong_count(record) == 1,
                            |record, _| {
                                record.retirements.set(record.retirements.get() + 1);
                            },
                            window,
                            cx,
                        );
                        assert_eq!(result.is_err(), self.fail_preparation);
                        // Capture the newly indexed record without retaining it.
                        if let Some((record, _)) = self.registry.entries.get(&key)
                            && !self.probes.iter().any(|probe| probe.ptr_eq(record))
                        {
                            self.probes.push(record.clone());
                        }
                    }
                }
                gpui::div()
            }
        }
        let mut cx = gpui::TestAppContext::single();
        let retirements = Rc::new(Cell::new(0));
        let window = cx.add_window(|_, _| Consumer {
            registry: WeakRegistry::default(),
            probes: Vec::new(),
            creations: 0,
            retirements: retirements.clone(),
            revision: 0,
            fail_preparation: false,
            shown: true,
        });
        let draw = |cx: &mut gpui::TestAppContext| {
            cx.update_window(window.into(), |_, window, cx| window.draw(cx).clear(cx))
                .unwrap();
            cx.run_until_parked();
        };
        for _ in 0..4 {
            window.update(&mut cx, |_, _, cx| cx.notify()).unwrap();
            draw(&mut cx);
        }
        window
            .read_with(&cx, |consumer, _| {
                assert_eq!(consumer.creations, 3);
                assert_eq!(consumer.registry.len(), 2);
                assert!(
                    consumer
                        .probes
                        .iter()
                        .all(|probe| probe.upgrade().is_some())
                );
            })
            .unwrap();
        window
            .update(&mut cx, |consumer, _, cx| {
                consumer.revision = 1;
                cx.notify();
            })
            .unwrap();
        draw(&mut cx);
        assert_eq!(
            retirements.get(),
            3,
            "collisions retire the replaced records"
        );
        window
            .update(&mut cx, |consumer, _, cx| {
                assert_eq!(consumer.creations, 6);
                assert!(
                    consumer.probes[..3]
                        .iter()
                        .all(|probe| probe.upgrade().is_none())
                );
                consumer.revision = 2;
                consumer.fail_preparation = true;
                cx.notify();
            })
            .unwrap();
        draw(&mut cx);
        assert_eq!(
            retirements.get(),
            6,
            "failed replacements must release obsolete scene resources"
        );
        window
            .update(&mut cx, |consumer, _, cx| {
                assert_eq!(consumer.creations, 6);
                assert!(
                    consumer
                        .probes
                        .iter()
                        .all(|probe| probe.upgrade().is_none())
                );
                consumer.fail_preparation = false;
                cx.notify();
            })
            .unwrap();
        draw(&mut cx);
        window
            .update(&mut cx, |consumer, _, cx| {
                assert_eq!(consumer.creations, 9, "a failed preparation can be retried");
                consumer.shown = false;
                cx.notify();
            })
            .unwrap();
        draw(&mut cx);
        assert_eq!(retirements.get(), 9);
        window
            .update(&mut cx, |consumer, _, _| {
                assert!(
                    consumer
                        .probes
                        .iter()
                        .all(|probe| probe.upgrade().is_none())
                );
                consumer.registry.prune();
                assert_eq!(consumer.registry.len(), 0);
            })
            .unwrap();
    }

    #[test]
    fn weak_registry_is_bounded_without_owning_live_or_idle_payloads() {
        let mut registry = WeakRegistry::default();
        let first = registry.get_or_insert_with(0, 2, || vec![0; 4096]);
        let shared = registry.get_or_insert_with(0, 2, || unreachable!());
        assert!(Rc::ptr_eq(&first, &shared));
        let second = registry.get_or_insert_with(1, 2, || vec![1; 4096]);
        let third = registry.get_or_insert_with(2, 2, || vec![2; 4096]);
        assert_eq!(registry.len(), 2);
        assert_eq!(first.len(), 4096, "eviction leaves active leases intact");
        let weak = Rc::downgrade(&first);
        drop((first, shared, second, third));
        registry.prune();
        assert!(weak.upgrade().is_none());
        assert_eq!(registry.len(), 0);
    }
}
