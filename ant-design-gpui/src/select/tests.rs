// Copyright (C) 2026 mg-chao
// SPDX-License-Identifier: Apache-2.0

use super::*;
use std::collections::HashSet;

fn options() -> SelectOptions<&'static str> {
    SelectOptions::from_entries([
        SelectOption::new("solo", "id-solo", "Solo")
            .search_field("description", "Independent")
            .into(),
        SelectGroup::new(
            "cities",
            "Cities",
            [
                SelectOption::new("london", "lon", "London").search_field("country", "UK"),
                SelectOption::new("paris", "par", "Paris")
                    .search_field("country", "France")
                    .disabled(true),
            ],
        )
        .into(),
        SelectGroup::new(
            "animals",
            "Animals",
            [SelectOption::new("tiger", "tig", "Tiger")],
        )
        .into(),
    ])
    .unwrap()
}
#[test]
fn immutable_options_share_the_index_and_unfiltered_row_snapshot() {
    let options = options();
    let cloned = options.clone();
    assert!(options.same_collection(&cloned));
    assert!(
        options
            .all()
            .same_rows(&options.search("", &SelectSearch::default()))
    );
    assert_eq!(options.option_count(), 4);
    assert_eq!(options.rows().len(), 6);
    assert_eq!(options.option(&"par").unwrap().label, "Paris");
    assert!(options.option(&"par").unwrap().disabled);
}

#[test]
fn accessibility_positions_exclude_group_headers_and_follow_filtered_order() {
    let data = options();
    let all = data.all();
    assert_eq!(
        (0..6).map(|i| all.option_position(i)).collect::<Vec<_>>(),
        [Some(1), None, Some(2), Some(3), None, Some(4)]
    );
    let filtered = data.search(
        "cities",
        &SelectSearch::default().sort(|a, b, _| b.label.cmp(&a.label)),
    );
    assert_eq!(filtered.rows(), &[1, 3, 2]);
    assert_eq!(filtered.option_count(), 2);
    assert_eq!(filtered.option_position(0), None);
    assert_eq!(filtered.option_position(1), Some(1));
    assert_eq!(filtered.option_position(2), Some(2));
    assert_eq!(filtered.row_position(3), Some(1));
    assert_eq!(filtered.row_position(2), Some(2));
    assert_eq!(filtered.row_position(5), None);
    let mut model = model::SelectModel::new(data);
    assert_eq!(model.option_position(5), Some((4, 4)));
    model.set_query("cities".into());
    assert_eq!(model.option_position(1), Some((1, 2)));
    assert_eq!(model.option_position(2), Some((2, 2)));
    assert_eq!(model.option_position(0), None);
    assert_eq!(model.index_of_value(&"lon"), Some(1));
    assert_eq!(model.index_of_value(&"tig"), None);
}
#[test]
fn default_search_matches_values_and_group_labels_as_upstream_does() {
    let options = options();
    assert!(
        options
            .search("London", &SelectSearch::default())
            .is_empty()
    );
    assert_eq!(
        options.search("LON", &SelectSearch::default()).rows(),
        &[1, 2]
    );
    assert_eq!(
        options.search("cities", &SelectSearch::default()).rows(),
        &[1, 2, 3]
    );
    assert_eq!(
        options.search("London", &SelectSearch::label()).rows(),
        &[1, 2]
    );
}
#[test]
fn multi_field_search_is_or_matching_and_preserves_group_headers() {
    let options = options();
    let policy = SelectSearch::fields([
        SelectSearchField::Label,
        SelectSearchField::Field("country".into()),
    ]);
    assert_eq!(options.search("france", &policy).rows(), &[1, 3]);
    assert_eq!(options.search("tiger", &policy).rows(), &[4, 5]);
    assert!(options.search("missing", &policy).rows().is_empty());
}
#[test]
fn hidden_selections_remove_empty_groups_and_custom_sort_is_stable() {
    let options = options();
    let policy = SelectSearch::default().sort(|a, b, _| b.label.cmp(&a.label));
    assert_eq!(options.search("", &policy).rows(), &[0, 1, 3, 2, 4, 5]);
    let excluded = HashSet::from(["lon", "par"]);
    assert_eq!(
        options.search_excluding("", &policy, &excluded).rows(),
        &[0, 4, 5]
    );
    assert_eq!(
        options
            .search_excluding("Cities", &SelectSearch::default(), &excluded)
            .option_count(),
        0
    );
}
#[test]
fn custom_filter_can_use_domain_data_and_remote_filtering_can_be_disabled() {
    let options = options();
    let policy = SelectSearch::default().filter(|_, option| option.disabled);
    assert_eq!(options.search("anything", &policy).rows(), &[1, 3]);
    let remote = SelectSearch {
        filter_enabled: false,
        ..SelectSearch::default()
    };
    assert!(
        options
            .search("remote query", &remote)
            .same_rows(&options.all())
    );
    assert!(policy.equivalent(&policy.clone()));
    assert!(!policy.equivalent(&SelectSearch::default().filter(|_, option| option.disabled)));
}

#[test]
fn entry_callbacks_filter_and_sort_whole_groups_without_losing_option_identity() {
    let data = options();
    let label = |row: &SelectRow<&str>| match row {
        SelectRow::Group { label, .. } => label.to_string(),
        SelectRow::Option { option, .. } => option.label.to_string(),
    };
    let policy = SelectSearch::default()
        .filter_entries(|query, row| match row {
            SelectRow::Group { label, .. } => label.as_ref() == query,
            SelectRow::Option { option, .. } => option.label.as_ref() == query,
        })
        .sort_entries(move |a, b, _| label(a).cmp(&label(b)));
    assert_eq!(data.search("Cities", &policy).rows(), &[1, 2, 3]);
    assert_eq!(data.search("Tiger", &policy).rows(), &[4, 5]);
    let sorted = data.search("", &policy);
    assert_eq!(sorted.rows(), &[4, 5, 1, 2, 3, 0]);
    assert_eq!(sorted.row_position(0), Some(5));
    assert_eq!(sorted.option_position(5), Some(4));
    assert!(policy.equivalent(&policy.clone()));
    assert!(!policy.equivalent(&SelectSearch::default()));
    let excluded = HashSet::from(["lon", "par"]);
    assert_eq!(
        data.search_excluding("", &policy, &excluded).rows(),
        &[4, 5, 0]
    );
}
#[test]
fn duplicate_values_and_native_ids_are_rejected() {
    assert!(matches!(
        SelectOptions::new([
            SelectOption::new("a", 1, "One"),
            SelectOption::new("b", 1, "Duplicate")
        ]),
        Err(SelectOptionsError::DuplicateValue)
    ));
    assert!(matches!(
        SelectOptions::new([
            SelectOption::new("a", 1, "One"),
            SelectOption::new("a", 2, "Two")
        ]),
        Err(SelectOptionsError::DuplicateId(_))
    ));
}

#[test]
fn a_standalone_option_after_a_group_does_not_inherit_its_search_match() {
    let options = SelectOptions::from_entries([
        SelectGroup::new("group", "Fruit", [SelectOption::new("apple", 1, "Apple")]).into(),
        SelectOption::new("solo", 2, "Carrot").into(),
    ])
    .unwrap();
    assert_eq!(
        options.search("fruit", &SelectSearch::default()).rows(),
        &[0, 1]
    );
    assert_eq!(options.search("2", &SelectSearch::default()).rows(), &[2]);
}

#[test]
fn selected_labels_survive_remote_replacement_and_active_identity_survives_reordering() {
    let mut model = model::SelectModel::new(options());
    model.set_values(vec![SelectValue::new("lon", "London")]);
    model.active = Some(2);
    model.set_options(
        SelectOptions::new([
            SelectOption::new("london", "lon", "Updated London"),
            SelectOption::new("solo", "id-solo", "Solo"),
        ])
        .unwrap(),
    );
    assert_eq!(model.active_id(), Some(&gpui::ElementId::from("london")));
    assert_eq!(model.values[0].label, "Updated London");
    model.set_options(SelectOptions::default());
    assert_eq!(model.values[0].label, "Updated London");
    assert_eq!(model.row_count(), 0);
}

#[test]
fn created_tags_reappear_as_selected_options_and_search_fields_prevent_duplicate_suggestions() {
    let mut model = model::SelectModel::new(
        SelectOptions::new([SelectOption::new("london", "lon".to_owned(), "London")]).unwrap(),
    );
    model.tag_factory = Some(std::rc::Rc::new(|s| {
        Some(SelectOption::new(
            gpui::SharedString::from(s.to_owned()),
            s.to_owned(),
            s.to_owned(),
        ))
    }));
    model.set_settings(SelectSettings {
        mode: SelectMode::Tags,
        ..Default::default()
    });
    model.set_values(vec![
        SelectValue::new("zebra".to_owned(), "Zebra"),
        SelectValue::new("apple".to_owned(), "Apple"),
    ]);
    assert_eq!(model.row_count(), 3);
    assert_eq!(model.option(1).unwrap().label, "Apple");
    assert_eq!(model.option(2).unwrap().label, "Zebra");
    model.set_search(SelectSearch::label());
    model.set_query("London".into());
    assert!(model.suggestion.is_none());
    assert_eq!(model.row_count(), 1);
    model.set_values(vec![]);
    model.set_query("".into());
    assert_eq!(
        model.row_count(),
        1,
        "removed tags must not leak into a growing cache"
    );
}

#[test]
fn created_tags_share_custom_sort_order_and_accessibility_ordinals_with_options() {
    let data = SelectOptions::new([
        SelectOption::new("pear", "pear".to_owned(), "Pear"),
        SelectOption::new("banana", "banana".to_owned(), "Banana"),
    ])
    .unwrap();
    let mut model = model::SelectModel::new(data);
    model.set_settings(SelectSettings {
        mode: SelectMode::Tags,
        ..Default::default()
    });
    model.set_values(vec![
        SelectValue::new("apple".to_owned(), "Apple"),
        SelectValue::new("zebra".to_owned(), "Zebra"),
    ]);
    model.set_search(SelectSearch::label().sort(|a, b, _| a.label.cmp(&b.label)));
    assert_eq!(
        (0..4)
            .map(|i| model.option(i).unwrap().label.as_ref())
            .collect::<Vec<_>>(),
        ["Apple", "Banana", "Pear", "Zebra"]
    );
    assert_eq!(model.index_of_value(&"apple".to_owned()), Some(0));
    assert_eq!(model.index_of_value(&"pear".to_owned()), Some(2));
    assert_eq!(model.option_position(2), Some((3, 4)));
    model.set_query("a".into());
    assert_eq!(
        (0..4)
            .map(|i| model.option(i).unwrap().label.as_ref())
            .collect::<Vec<_>>(),
        ["Apple", "Banana", "Pear", "Zebra"]
    );
    model.set_values(vec![]);
    assert_eq!(model.row_count(), 2);
    assert_eq!(model.index_of_value(&"pear".to_owned()), Some(1));
}

#[test]
fn custom_tokenizer_can_preserve_quoted_fields_and_unchanged_input() {
    let mut model = model::SelectModel::<String>::new(SelectOptions::default());
    model.set_settings(SelectSettings {
        mode: SelectMode::Tags,
        ..Default::default()
    });
    model.tag_factory = Some(std::rc::Rc::new(|s| {
        Some(SelectOption::new(
            gpui::SharedString::from(s.to_owned()),
            s.to_owned(),
            s.to_owned(),
        ))
    }));
    model.tokenizer = Some(std::rc::Rc::new(|s| {
        if s == "quoted input" {
            vec!["San Francisco, CA".into(), "New York".into()]
        } else {
            vec![s.to_owned().into()]
        }
    }));
    model.settings.token_separators = vec![','];
    let (submitted, _) = model.tokenize("San Francisco, CA", true).unwrap();
    assert_eq!(submitted.values.len(), 1, "Enter submits one complete tag");
    assert_eq!(submitted.values[0].value, "San Francisco, CA");
    assert!(model.tokenize("unchanged", false).is_none());
    let (proposal, tail) = model.tokenize("quoted input", false).unwrap();
    assert!(tail.is_empty());
    assert_eq!(
        proposal
            .values
            .iter()
            .map(|v| v.value.as_str())
            .collect::<Vec<_>>(),
        vec!["San Francisco, CA", "New York"]
    );
}

#[test]
fn token_proposals_match_original_browser_callback_traces() {
    let options = SelectOptions::new(
        [
            ("jack", "Jack"),
            ("lucy", "Lucy"),
            ("disabled", "Disabled"),
            ("yiminghe", "Yiminghe"),
        ]
        .map(|(value, label)| {
            SelectOption::new(value, value.to_owned(), label).disabled(value == "disabled")
        }),
    )
    .unwrap();
    for (mode, input, trace) in [
        (
            SelectMode::Tags,
            "foo;bar",
            include_str!("../../fixtures/select/tags-token-events.json"),
        ),
        (
            SelectMode::Multiple,
            "Disabled;Yiminghe",
            include_str!("../../fixtures/select/multiple-token-events.json"),
        ),
    ] {
        let mut model = model::SelectModel::new(options.clone());
        model.set_settings(SelectSettings {
            mode,
            token_separators: vec![';'],
            ..Default::default()
        });
        model.set_values(vec![
            SelectValue::new("lucy".to_owned(), "Lucy"),
            SelectValue::new("jack".to_owned(), "Jack"),
        ]);
        model.tag_factory = Some(std::rc::Rc::new(|s| {
            Some(SelectOption::new(
                gpui::SharedString::from(s.to_owned()),
                s.to_owned(),
                s.to_owned(),
            ))
        }));
        let (proposal, tail) = model.tokenize(input, false).unwrap();
        assert!(tail.is_empty());
        let trace: Vec<serde_json::Value> = serde_json::from_str(trace).unwrap();
        let values: Vec<_> = proposal.values.iter().map(|v| v.value.as_str()).collect();
        assert_eq!(serde_json::json!(values), trace[0]["args"][0]);
        let selected: Vec<_> = proposal
            .selected
            .iter()
            .map(|o| serde_json::json!(o.value))
            .collect();
        let reference: Vec<_> = trace
            .iter()
            .filter(|e| e["type"] == "select")
            .map(|e| e["args"][0].clone())
            .collect();
        assert_eq!(
            selected, reference,
            "selection callback ordering includes preexisting values"
        );
    }
}

#[test]
fn selected_snapshots_and_accessible_labels_are_reused_until_values_change() {
    let mut model = model::SelectModel::new(options());
    model.set_settings(SelectSettings {
        mode: SelectMode::Multiple,
        ..Default::default()
    });
    model.set_values(vec![
        SelectValue::new("lon", "London"),
        SelectValue::new("par", "Paris"),
    ]);
    let values = model.values.clone();
    assert_eq!(model.accessible_value, "London, Paris");
    model.set_query("p".into());
    assert!(
        std::rc::Rc::ptr_eq(&values, &model.values),
        "query changes must not clone selected values"
    );
    model.set_options(options());
    assert!(
        std::rc::Rc::ptr_eq(&values, &model.values),
        "equal labels preserve the selected snapshot"
    );
    model.set_options(
        SelectOptions::new([SelectOption::new("london", "lon", "New London")]).unwrap(),
    );
    assert_eq!(model.accessible_value, "New London, Paris");
    assert_eq!(
        values[0].label, "London",
        "previous frame keeps its immutable snapshot"
    );
}
