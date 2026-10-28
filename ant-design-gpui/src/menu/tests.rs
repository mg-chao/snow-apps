// Copyright (C) 2026 mg-chao
// SPDX-License-Identifier: Apache-2.0

use super::*;
use gpui::{AppContext, Entity, TestAppContext};
use std::{cell::RefCell, rc::Rc};

fn items(entries: impl IntoIterator<Item = MenuEntry>) -> MenuItems {
    MenuItems::new(entries).unwrap()
}
fn tree() -> MenuItems {
    items([
        MenuItem::new("home", "Home").into(),
        SubMenu::new(
            "tools",
            "Tools",
            items([
                MenuGroup::new(
                    "group",
                    "Group",
                    items([
                        MenuItem::new("disabled", "Disabled").disabled(true).into(),
                        MenuItem::new("edit", "Edit").into(),
                    ]),
                )
                .into(),
                SubMenu::new(
                    "more",
                    "More",
                    items([MenuItem::new("export", "Export").into()]),
                )
                .into(),
            ]),
        )
        .into(),
        MenuDivider::new("separator").into(),
        MenuItem::new("end", "End").into(),
    ])
}
#[test]
fn menu_model_indexes_paths_groups_and_unique_keys() {
    let tree = tree();
    assert_eq!(tree.path("edit"), vec!["edit", "tools"]);
    assert_eq!(tree.path("export"), vec!["export", "more", "tools"]);
    assert_eq!(tree.parent("edit").unwrap().as_ref(), "tools");
    assert_eq!(tree.depth("export"), 2);
    assert!(!tree.enabled("disabled"));
    assert!(!tree.enabled("group"));
    assert!(tree.descendant_of("export", "tools"));
    assert_eq!(
        tree.visible_keys(&[], false, None),
        vec!["home", "tools", "end"]
    );
    assert_eq!(
        tree.visible_keys(&["tools".into(), "more".into()], true, None),
        vec!["home", "tools", "edit", "more", "export", "end"]
    );
    assert_eq!(
        tree.visible_keys(&[], false, Some("tools")),
        vec!["edit", "more"]
    );
    assert!(
        MenuItems::new([
            SubMenu::new("root", "Root", items([MenuItem::new("same", "A").into()])).into(),
            MenuItem::new("same", "B").into()
        ])
        .is_err()
    );
    let cloned = tree.clone();
    assert!(tree.same_collection(&cloned));
}
#[test]
fn menu_model_disabled_ancestors_exclude_every_descendant() {
    let data = items([SubMenu::new(
        "disabled",
        "Disabled",
        items([MenuItem::new("child", "Child").into()]),
    )
    .disabled(true)
    .into()]);
    assert!(!data.enabled("child"));
    assert!(
        data.visible_keys(&["disabled".into()], true, None)
            .is_empty()
    );
}

struct Fixture {
    cx: TestAppContext,
    state: Entity<MenuState>,
    window: gpui::WindowHandle<gpui::Empty>,
    events: Rc<RefCell<Vec<MenuEvent>>>,
}
impl Fixture {
    fn new() -> Self {
        let mut cx = TestAppContext::single();
        let mut state = None;
        let window = cx.add_window(|window, cx| {
            let menu = cx.new(|cx| MenuState::new(tree(), window, cx));
            state = Some(menu);
            gpui::Empty
        });
        let state = state.unwrap();
        let events = Rc::new(RefCell::new(Vec::new()));
        let recorded = events.clone();
        state.update(&mut cx, |s, _| {
            s.mounted = true;
            s.on_event = Some(Rc::new(move |event, _, _| {
                recorded.borrow_mut().push(event.clone())
            }));
        });
        Self {
            cx,
            state,
            window,
            events,
        }
    }
    fn activate(&mut self, key: &str) {
        let state = self.state.clone();
        let key = key.to_owned().into();
        self.window
            .update(&mut self.cx, |_, window, cx| {
                state::activate(&state, &key, MenuSource::Pointer, window, cx)
            })
            .unwrap();
    }
    fn toggle(&mut self, key: &str, open: bool) {
        let state = self.state.clone();
        let key = key.to_owned().into();
        self.window
            .update(&mut self.cx, |_, window, cx| {
                state::toggle(&state, &key, open, MenuSource::Pointer, window, cx)
            })
            .unwrap();
    }
    fn configure(
        &mut self,
        options: MenuOptions,
        selected: Option<Vec<MenuKey>>,
        open: Option<Vec<MenuKey>>,
    ) {
        self.state.update(&mut self.cx, |s, cx| {
            s.configure(options, selected, None, open, None, cx)
        });
    }
    fn selected(&self) -> Vec<MenuKey> {
        self.state.read_with(&self.cx, |s, _| s.selected.clone())
    }
    fn open(&self) -> Vec<MenuKey> {
        self.state.read_with(&self.cx, |s, _| s.open.clone())
    }
    fn revision(&self) -> u64 {
        self.state.read_with(&self.cx, |s, _| s.revision)
    }
}
#[test]
fn menu_selection_retains_state_and_emits_ordered_proposals() {
    let mut f = Fixture::new();
    f.activate("home");
    assert_eq!(f.selected(), vec!["home"]);
    assert!(matches!(
        &f.events.borrow()[..],
        [MenuEvent::Activate(_), MenuEvent::Select(_)]
    ));
    f.events.borrow_mut().clear();
    f.activate("disabled");
    assert!(f.events.borrow().is_empty());
    f.configure(
        MenuOptions {
            multiple: true,
            ..Default::default()
        },
        None,
        None,
    );
    f.activate("edit");
    f.activate("home");
    assert_eq!(f.selected(), vec!["edit"]);
    assert!(matches!(
        f.events.borrow().last(),
        Some(MenuEvent::Deselect(_))
    ));
}
#[test]
fn menu_controlled_values_stay_authoritative_and_setters_are_silent() {
    let mut f = Fixture::new();
    f.configure(
        MenuOptions::default(),
        Some(vec!["home".into()]),
        Some(vec![]),
    );
    f.activate("edit");
    f.toggle("tools", true);
    assert_eq!(f.selected(), vec!["home"]);
    assert!(f.open().is_empty());
    assert!(f.events.borrow().iter().any(|event|matches!(event,MenuEvent::OpenChange {open_keys,..} if open_keys==&vec![MenuKey::from("tools")])));

    let multiple = MenuOptions {
        mode: MenuMode::Inline,
        multiple: true,
        ..Default::default()
    };
    f.configure(
        multiple.clone(),
        Some(vec!["home".into()]),
        Some(vec!["tools".into(), "more".into()]),
    );
    let export = MenuActivation {
        key: "export".into(),
        key_path: vec!["export".into(), "more".into(), "tools".into()],
        label: "Export".into(),
        source: MenuSource::Pointer,
    };
    let select = vec![
        MenuEvent::Activate(export.clone()),
        MenuEvent::Select(MenuSelection {
            activation: export.clone(),
            selected_keys: vec!["home".into(), "export".into()],
        }),
    ];

    f.events.borrow_mut().clear();
    f.activate("export");
    assert_eq!(*f.events.borrow(), select);
    assert_eq!(f.selected(), vec!["home"]);
    f.events.borrow_mut().clear();
    f.activate("export");
    assert_eq!(*f.events.borrow(), select);
    assert_eq!(f.selected(), vec!["home"]);
    f.events.borrow_mut().clear();
    f.configure(
        multiple.clone(),
        Some(vec!["home".into(), "export".into()]),
        Some(vec!["tools".into(), "more".into()]),
    );
    assert_eq!(f.selected(), vec!["home", "export"]);
    assert!(f.events.borrow().is_empty());

    let deselect = vec![
        MenuEvent::Activate(export.clone()),
        MenuEvent::Deselect(MenuSelection {
            activation: export,
            selected_keys: vec!["home".into()],
        }),
    ];
    f.activate("export");
    assert_eq!(*f.events.borrow(), deselect);
    assert_eq!(f.selected(), vec!["home", "export"]);
    f.events.borrow_mut().clear();
    f.activate("export");
    assert_eq!(*f.events.borrow(), deselect);
    assert_eq!(f.selected(), vec!["home", "export"]);
    f.events.borrow_mut().clear();
    f.configure(
        multiple,
        Some(vec!["home".into()]),
        Some(vec!["tools".into(), "more".into()]),
    );
    assert_eq!(f.selected(), vec!["home"]);
    assert!(f.events.borrow().is_empty());

    f.events.borrow_mut().clear();
    f.state.update(&mut f.cx, |s, cx| {
        s.set_selected_keys(["export"], cx);
        s.set_open_keys(["tools", "more"], cx);
    });
    assert!(f.events.borrow().is_empty());
    assert_eq!(f.selected(), vec!["export"]);
}
#[test]
fn menu_inline_collapse_restores_expansion_and_parent_closure_preserves_children() {
    let mut f = Fixture::new();
    let options = MenuOptions {
        mode: MenuMode::Inline,
        ..Default::default()
    };
    f.configure(options.clone(), None, None);
    f.toggle("tools", true);
    f.toggle("more", true);
    f.toggle("tools", false);
    assert_eq!(f.open(), vec!["more"]);
    f.toggle("tools", true);
    f.configure(
        MenuOptions {
            inline_collapsed: true,
            ..options.clone()
        },
        None,
        None,
    );
    assert!(f.open().is_empty());
    f.configure(options, None, None);
    assert_eq!(f.open(), vec!["more", "tools"]);
}
#[test]
fn menu_single_activation_closes_flyouts_even_without_selection() {
    let mut f = Fixture::new();
    f.configure(
        MenuOptions {
            selectable: false,
            ..Default::default()
        },
        None,
        None,
    );
    f.toggle("tools", true);
    f.toggle("more", true);
    f.activate("export");
    assert!(f.open().is_empty());
    assert!(f.selected().is_empty());
    assert!(
        f.events
            .borrow()
            .iter()
            .any(|e| matches!(e, MenuEvent::Activate(_)))
    );
    assert!(
        !f.events
            .borrow()
            .iter()
            .any(|e| matches!(e, MenuEvent::Select(_)))
    );
}
#[test]
fn menu_replacement_preserves_selection_and_prevents_stale_activation() {
    let mut f = Fixture::new();
    f.activate("edit");
    let revision = f.revision();
    f.state.update(&mut f.cx, |s, cx| {
        s.set_items(items([MenuItem::new("new", "New").into()]), cx)
    });
    assert!(f.revision() > revision);
    assert_eq!(f.selected(), vec!["edit"]);
    f.events.borrow_mut().clear();
    f.activate("edit");
    assert!(f.events.borrow().is_empty());
    assert_eq!(
        f.state
            .read_with(&f.cx, |s, _| s.active.clone())
            .unwrap()
            .as_ref(),
        "new"
    );
}
#[test]
fn menu_default_keys_initialize_once_and_controlled_collapse_keeps_owner_keys() {
    let mut f = Fixture::new();
    f.state.update(&mut f.cx, |s, cx| {
        s.configure(
            MenuOptions::default(),
            None,
            Some(vec!["home".into()]),
            None,
            Some(vec!["tools".into()]),
            cx,
        )
    });
    f.activate("edit");
    f.state.update(&mut f.cx, |s, cx| {
        s.configure(
            MenuOptions::default(),
            None,
            Some(vec!["home".into()]),
            None,
            Some(vec![]),
            cx,
        )
    });
    assert_eq!(f.selected(), vec!["edit"]);
    f.configure(
        MenuOptions {
            mode: MenuMode::Inline,
            inline_collapsed: true,
            ..Default::default()
        },
        None,
        Some(vec!["tools".into()]),
    );
    assert_eq!(f.open(), vec!["tools"]);
}
