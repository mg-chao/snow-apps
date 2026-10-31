// Copyright (C) 2026 mg-chao
// SPDX-License-Identifier: Apache-2.0

use gpui::SharedString;
use std::collections::VecDeque;

const CAPACITY: usize = 128;

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
enum Key {
    Number(i64, usize),
    Decade(i32, i32),
    Quarter(u32),
}

/// Only labels actually requested by a rendered panel are retained. Keys own
/// their complete content, so locale/style changes cannot leave stale strings.
#[derive(Default)]
pub(super) struct LabelCache {
    entries: VecDeque<(Key, SharedString)>,
    #[cfg(test)]
    misses: usize,
}

impl LabelCache {
    pub(super) fn number(&mut self, value: i64, minimum_digits: usize) -> SharedString {
        self.get(Key::Number(value, minimum_digits))
    }

    pub(super) fn decade(&mut self, start: i32, end: i32) -> SharedString {
        self.get(Key::Decade(start, end))
    }

    pub(super) fn quarter(&mut self, value: u32) -> SharedString {
        self.get(Key::Quarter(value))
    }

    fn get(&mut self, key: Key) -> SharedString {
        if let Some(index) = self
            .entries
            .iter()
            .position(|(candidate, _)| *candidate == key)
        {
            let entry = self
                .entries
                .remove(index)
                .expect("the matching label exists");
            let label = entry.1.clone();
            self.entries.push_back(entry);
            return label;
        }
        let label: SharedString = match key {
            Key::Number(value, minimum_digits) => format!("{value:0minimum_digits$}"),
            Key::Decade(start, end) => format!("{start}-{end}"),
            Key::Quarter(value) => format!("Q{value}"),
        }
        .into();
        if self.entries.len() == CAPACITY {
            self.entries.pop_front();
        }
        self.entries.push_back((key, label.clone()));
        #[cfg(test)]
        {
            self.misses += 1;
        }
        label
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn date_picker_label_keys_keep_numeric_precision_and_period_content() {
        let mut cache = LabelCache::default();
        assert_eq!(
            cache.entries.capacity(),
            0,
            "closed pickers allocate no label storage"
        );
        assert_eq!(cache.number(7, 0), "7");
        assert_eq!(cache.number(7, 2), "07");
        assert_eq!(cache.number(7, 3), "007");
        assert_eq!(cache.number(-9, 3), "-09");
        assert_eq!(cache.number(2024, 0), "2024");
        assert_eq!(cache.decade(2020, 2029), "2020-2029");
        assert_eq!(cache.decade(2020, 2030), "2020-2030");
        assert_eq!(cache.quarter(1), "Q1");
        assert_eq!(cache.quarter(4), "Q4");
        assert_eq!(cache.misses, 9);
        assert_eq!(cache.number(7, 2), "07");
        assert_eq!(cache.quarter(1), "Q1");
        assert_eq!(cache.misses, 9, "hits do not format a fresh label");
    }

    #[test]
    fn date_picker_label_hits_reuse_heap_backed_shared_strings() {
        let mut cache = LabelCache::default();
        let first = cache.number(42, 32);
        let reused = cache.number(42, 32);
        assert_eq!(first, reused);
        assert_eq!(first.as_ptr(), reused.as_ptr());
        assert_eq!(cache.misses, 1);
    }

    #[test]
    fn date_picker_label_cache_is_lru_bounded_and_milliseconds_are_lazy() {
        let mut cache = LabelCache::default();
        let first = cache.number(0, 32);
        for value in 1..128 {
            cache.number(value, 32);
        }
        assert_eq!(cache.entries.len(), CAPACITY);
        assert_eq!(cache.entries.capacity(), CAPACITY);
        assert_eq!(cache.number(0, 32).as_ptr(), first.as_ptr());
        cache.number(128, 32);
        assert!(
            cache
                .entries
                .iter()
                .any(|(key, _)| *key == Key::Number(0, 32))
        );
        assert!(
            !cache
                .entries
                .iter()
                .any(|(key, _)| *key == Key::Number(1, 32))
        );
        for value in 0..1000 {
            cache.number(value, 3);
            assert!(cache.entries.len() <= CAPACITY);
            assert!(cache.entries.capacity() <= CAPACITY);
        }
        let misses = cache.misses;
        assert_eq!(cache.number(999, 3), "999");
        assert_eq!(cache.misses, misses);
        let replaced = cache.number(0, 32);
        assert_ne!(first.as_ptr(), replaced.as_ptr());
        assert_eq!(cache.entries.len(), CAPACITY);
    }
}
