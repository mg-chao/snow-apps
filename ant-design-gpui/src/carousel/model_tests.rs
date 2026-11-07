// Copyright (C) 2026 mg-chao
// SPDX-License-Identifier: Apache-2.0

use super::*;

fn items() -> CarouselItems {
    CarouselItems::new([
        CarouselSlide::new("one").text("One"),
        CarouselSlide::new("two").text("Two"),
        CarouselSlide::new("three").text("Three"),
    ])
    .unwrap()
}

#[test]
fn duplicate_keys_are_rejected_without_restricting_labels() {
    let duplicate = CarouselItems::new([
        CarouselSlide::new("same").label("First"),
        CarouselSlide::new("same").label("Second"),
    ]);
    assert_eq!(
        duplicate.unwrap_err(),
        CarouselItemsError::DuplicateKey("same".into())
    );
    let repeated_labels = CarouselItems::new([
        CarouselSlide::new("first").label("Same"),
        CarouselSlide::new("second").label("Same"),
    ])
    .unwrap();
    assert_eq!(repeated_labels.len(), 2);
}

#[test]
fn selection_survives_reorder_and_recovers_at_removed_index() {
    let mut model = CarouselModel::new(items());
    model.current = Some(1);
    let entries = model.items.entries();
    let changed =
        CarouselItems::new([entries[2].clone(), entries[0].clone(), entries[1].clone()]).unwrap();
    assert!(model.set_items(changed));
    assert_eq!(model.current, Some(2));
    assert!(model.set_items(CarouselItems::new([CarouselSlide::new("replacement")]).unwrap()));
    assert_eq!(model.current, Some(0));
    assert!(model.set_items(CarouselItems::default()));
    assert_eq!(model.current, None);
    assert!(model.set_items(items()));
    assert_eq!(model.current, Some(0));
}

#[test]
fn shared_snapshots_preserve_identity_and_key_lookup() {
    let items = items();
    assert!(items.same(&items.clone()));
    assert_eq!(items.index("two"), Some(1));
    assert_eq!(items.get("three").unwrap().key.as_ref(), "three");
    assert!(items.get("missing").is_none());
    let mut model = CarouselModel::new(items.clone());
    assert!(!model.set_items(items));
}

#[test]
fn finite_navigation_stops_and_infinite_navigation_wraps() {
    let mut model = CarouselModel::new(items());
    assert_eq!(model.next(-1, false), None);
    assert_eq!(model.next(-1, true), Some(2));
    assert_eq!(model.next(1, false), Some(1));
    model.current = Some(2);
    assert_eq!(model.next(1, false), None);
    assert_eq!(model.next(1, true), Some(0));
    assert_eq!(
        model.next(isize::MAX, true),
        Some(((2_i128 + isize::MAX as i128) % 3) as usize)
    );
    assert_eq!(model.next(isize::MIN, false), Some(0));
    assert_eq!(
        CarouselModel::new(CarouselItems::default()).next(1, true),
        None
    );
    let single = CarouselModel::new(CarouselItems::new([CarouselSlide::new("only")]).unwrap());
    assert_eq!(single.next(1, true), None);
}

#[test]
fn logical_placement_selects_orientation_unless_explicitly_overridden() {
    for placement in [
        DotPlacement::Top,
        DotPlacement::Bottom,
        DotPlacement::Start,
        DotPlacement::End,
    ] {
        let mut options = CarouselOptions {
            dot_placement: placement,
            ..Default::default()
        };
        assert_eq!(options.is_vertical(), placement.vertical());
        assert_eq!(options.axis_direction(Direction::Ltr), 1.);
        assert_eq!(
            options.axis_direction(Direction::Rtl),
            if placement.vertical() { 1. } else { -1. }
        );
        options.vertical = Some(false);
        assert!(!options.is_vertical());
        options.vertical = Some(true);
        assert!(options.is_vertical());
        assert_eq!(options.axis_direction(Direction::Rtl), 1.);
    }
}

#[test]
fn requested_indices_handle_empty_and_extreme_inputs() {
    let mut options = CarouselOptions::default();
    assert_eq!(options.normalize(usize::MAX, 0), None);
    assert_eq!(options.normalize(usize::MAX, 1), Some(0));
    assert_eq!(options.normalize(usize::MAX, 3), Some(usize::MAX % 3));
    options.infinite = false;
    assert_eq!(options.normalize(usize::MAX, 3), Some(2));
    assert_eq!(options.normalize(1, 3), Some(1));
}

#[test]
fn defaults_match_the_documented_ant_design_surface() {
    let options = CarouselOptions::default();
    assert_eq!(options.effect, CarouselEffect::Scroll);
    assert_eq!(options.dot_placement, DotPlacement::Bottom);
    assert!(options.dots && options.infinite);
    assert!(!options.arrows && !options.draggable && !options.adaptive_height);
    assert!(!options.autoplay && !options.dot_duration && !options.wait_for_animate);
    assert_eq!(options.initial_slide, 0);
    assert_eq!(options.speed, Duration::from_millis(500));
    assert_eq!(options.autoplay_speed, Duration::from_secs(3));
}
