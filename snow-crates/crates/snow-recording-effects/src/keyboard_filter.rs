//! Capture-time key admission, independent of labels, locale, and rendering policy.
use std::collections::BTreeSet;

use crate::keyboard_overlay::KeyEvent;

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct KeyboardFilterConfig {
    pub record_number_keys: bool,
    pub record_letters_and_symbols: bool,
    pub record_control_keys: bool,
    pub preserve_keyboard_shortcuts: bool,
}

impl Default for KeyboardFilterConfig {
    fn default() -> Self {
        Self {
            record_number_keys: true,
            record_letters_and_symbols: true,
            record_control_keys: true,
            preserve_keyboard_shortcuts: true,
        }
    }
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
enum Category {
    Number,
    Text,
    Control,
}

fn category(key: u16) -> Category {
    if (0x100..0x200).contains(&key) {
        // macOS observations retain their physical hardware code, offset from Windows VKs.
        return match key - 0x100 {
            18..=23 | 25 | 26 | 28 | 29 | 82..=89 | 91 | 92 => Category::Number,
            0..=17
            | 24
            | 27
            | 30..=35
            | 37..=47
            | 49
            | 50
            | 65
            | 67
            | 69
            | 75
            | 78
            | 81
            | 93
            | 94
            | 95 => Category::Text,
            _ => Category::Control,
        };
    }
    match key {
        0x30..=0x39 | 0x60..=0x69 => Category::Number,
        0x20 | 0x41..=0x5a | 0x6a..=0x6f | 0xba..=0xc0 | 0xdb..=0xdf | 0xe2 => Category::Text,
        _ => Category::Control,
    }
}

impl KeyboardFilterConfig {
    fn includes(self, key: u16) -> bool {
        match category(key) {
            Category::Number => self.record_number_keys,
            Category::Text => self.record_letters_and_symbols,
            Category::Control => self.record_control_keys,
        }
    }

    pub fn include_mouse_modifiers(self, show_keyboard: bool) -> bool {
        show_keyboard && self.record_control_keys
    }
}

/// Remembers admitted shortcut presses until release, even if modifiers release first.
#[derive(Default)]
pub struct KeyboardFilter {
    admitted: BTreeSet<u16>,
}

impl KeyboardFilter {
    pub fn reset(&mut self) {
        self.admitted.clear();
    }

    pub fn event(&mut self, config: KeyboardFilterConfig, mut event: KeyEvent) -> Option<KeyEvent> {
        // Mouse observations share the keycap renderer but are independently enabled.
        if event.key >= 0x200 {
            if !config.record_control_keys {
                event.modifiers.clear();
            }
            return Some(event);
        }
        let category_enabled = config.includes(event.key);
        let accepted = if event.down {
            let shortcut = config.record_control_keys
                && config.preserve_keyboard_shortcuts
                && event.modifiers.iter().any(|(key, label)| {
                    matches!(*key, 0x11 | 0x12 | 0x5b | 0x5c | 0xa2..=0xa5)
                        && !(*key == 0xa5 && label == "AltGr")
                });
            let accepted = category_enabled || shortcut;
            if accepted {
                self.admitted.insert(event.key);
            }
            accepted
        } else {
            self.admitted.remove(&event.key) || category_enabled
        };
        if !accepted {
            return None;
        }
        if !config.record_control_keys {
            event.modifiers.clear();
        }
        Some(event)
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    fn key(key: u16, down: bool, modifiers: &[(u16, &str)]) -> KeyEvent {
        KeyEvent {
            at_ms: 0,
            key,
            down,
            label: "key".into(),
            modifiers: modifiers
                .iter()
                .map(|(key, label)| (*key, (*label).into()))
                .collect(),
        }
    }

    #[test]
    fn physical_categories_cover_windows_and_macos_keys() {
        for key in [0x30, 0x39, 0x60, 0x69, 0x112, 0x11d, 0x152, 0x15c] {
            assert_eq!(category(key), Category::Number, "{key:x}");
        }
        for key in [
            0x20, 0x41, 0x5a, 0xba, 0xdf, 0xe2, 0x6b, 0x100, 0x12f, 0x131, 0x14b,
        ] {
            assert_eq!(category(key), Category::Text, "{key:x}");
        }
        for key in [0x08, 0x0d, 0x25, 0x70, 0xa2, 0xb3, 0x124, 0x135, 0x17b] {
            assert_eq!(category(key), Category::Control, "{key:x}");
        }
    }

    #[test]
    fn categories_filter_independently_and_defaults_preserve_input() {
        let mut filter = KeyboardFilter::default();
        for code in [0x31, 0x41, 0x0d, 0x112, 0x100, 0x124] {
            let event = key(code, true, &[]);
            assert_eq!(
                filter.event(KeyboardFilterConfig::default(), event.clone()),
                Some(event)
            );
        }
        let config = KeyboardFilterConfig {
            record_number_keys: false,
            record_letters_and_symbols: false,
            ..Default::default()
        };
        filter.reset();
        for code in [0x31, 0x41, 0x112, 0x100] {
            assert!(filter.event(config, key(code, true, &[])).is_none());
            assert!(filter.event(config, key(code, false, &[])).is_none());
        }
        assert!(filter.event(config, key(0x0d, true, &[])).is_some());
    }

    #[test]
    fn every_category_combination_filters_both_platforms_independently() {
        for mask in 0..8 {
            let config = KeyboardFilterConfig {
                record_number_keys: mask & 1 != 0,
                record_letters_and_symbols: mask & 2 != 0,
                record_control_keys: mask & 4 != 0,
                preserve_keyboard_shortcuts: true,
            };
            for (code, enabled) in [
                (0x31, config.record_number_keys),
                (0x112, config.record_number_keys),
                (0x41, config.record_letters_and_symbols),
                (0x100, config.record_letters_and_symbols),
                (0x0d, config.record_control_keys),
                (0x124, config.record_control_keys),
            ] {
                let mut filter = KeyboardFilter::default();
                assert_eq!(
                    filter.event(config, key(code, true, &[])).is_some(),
                    enabled
                );
                assert_eq!(
                    filter.event(config, key(code, false, &[])).is_some(),
                    enabled
                );
            }
        }
    }

    #[test]
    fn shortcuts_preserve_excluded_keys_and_release_after_the_modifier() {
        let mut filter = KeyboardFilter::default();
        let config = KeyboardFilterConfig {
            record_number_keys: false,
            record_letters_and_symbols: false,
            ..Default::default()
        };
        for (code, modifier) in [(0x41, 0xa2), (0x31, 0xa4), (0x100, 0x5b), (0x112, 0xa4)] {
            assert!(
                filter
                    .event(config, key(code, true, &[(modifier, "modifier")]))
                    .is_some()
            );
            assert!(filter.event(config, key(modifier, false, &[])).is_some());
            assert!(filter.event(config, key(code, false, &[])).is_some());
        }
        assert!(
            filter
                .event(config, key(0x41, true, &[(0xa0, "Left Shift")]))
                .is_none()
        );
        assert!(
            filter
                .event(config, key(0x41, true, &[(0xa5, "AltGr")]))
                .is_none()
        );
        assert!(
            filter
                .event(
                    KeyboardFilterConfig {
                        preserve_keyboard_shortcuts: false,
                        ..config
                    },
                    key(0x41, true, &[(0xa2, "Left Ctrl")])
                )
                .is_none()
        );
        filter.event(config, key(0x41, true, &[(0xa2, "Left Ctrl")]));
        filter.reset();
        assert!(filter.event(config, key(0x41, false, &[])).is_none());
    }

    #[test]
    fn disabling_control_keys_strips_keyboard_and_mouse_modifier_snapshots() {
        let mut filter = KeyboardFilter::default();
        let config = KeyboardFilterConfig {
            record_control_keys: false,
            ..Default::default()
        };
        assert!(
            filter
                .event(config, key(0xa2, true, &[(0xa2, "Left Ctrl")]))
                .is_none()
        );
        for code in [0x41, 0x200] {
            assert!(
                filter
                    .event(config, key(code, true, &[(0xa2, "Left Ctrl")]))
                    .unwrap()
                    .modifiers
                    .is_empty()
            );
        }
        assert!(
            filter
                .event(
                    KeyboardFilterConfig {
                        record_letters_and_symbols: false,
                        ..config
                    },
                    key(0x41, true, &[(0xa2, "Left Ctrl")])
                )
                .is_none()
        );
    }
}
