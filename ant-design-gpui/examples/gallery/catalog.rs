// Copyright (C) 2026 mg-chao
// SPDX-License-Identifier: Apache-2.0

//! Navigation metadata stays independent of component page implementations.

#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
pub(super) enum ComponentId {
    #[default]
    Button,
    Divider,
    Checkbox,
    Input,
    InputNumber,
    Radio,
    Segmented,
    Select,
    Slider,
    Switch,
    Tag,
    Tooltip,
    Popover,
    Popconfirm,
    ContextMenu,
    Menu,
    Pagination,
    ScrollArea,
    Tabs,
}

pub(super) struct Component {
    pub id: ComponentId,
    pub navigation_id: &'static str,
    pub group: &'static str,
    pub title: &'static str,
    pub description: &'static str,
}

pub(super) const COMPONENTS: &[Component] = &[
    Component {
        id: ComponentId::Button,
        navigation_id: "gallery.navigation.button",
        group: "General",
        title: "Button",
        description: "To trigger an operation.",
    },
    Component {
        id: ComponentId::Divider,
        navigation_id: "gallery.navigation.divider",
        group: "Layout",
        title: "Divider",
        description: "A divider line separates different content.",
    },
    Component {
        id: ComponentId::ScrollArea,
        navigation_id: "gallery.navigation.scroll-area",
        group: "Layout",
        title: "Scroll Area",
        description: "Native scrolling with precise, themed overlay scrollbars.",
    },
    Component {
        id: ComponentId::ContextMenu,
        navigation_id: "gallery.navigation.context-menu",
        group: "Navigation",
        title: "Context Menu",
        description: "Actions for the content under your pointer or keyboard focus.",
    },
    Component {
        id: ComponentId::Menu,
        navigation_id: "gallery.navigation.menu",
        group: "Navigation",
        title: "Menu",
        description: "A versatile menu for navigation.",
    },
    Component {
        id: ComponentId::Pagination,
        navigation_id: "gallery.navigation.pagination",
        group: "Navigation",
        title: "Pagination",
        description: "A long list can be divided into several pages, and only one page will be loaded at a time.",
    },
    Component {
        id: ComponentId::Tabs,
        navigation_id: "gallery.navigation.tabs",
        group: "Navigation",
        title: "Tabs",
        description: "Tabs make it easy to explore and switch between different views.",
    },
    Component {
        id: ComponentId::Checkbox,
        navigation_id: "gallery.navigation.checkbox",
        group: "Data Entry",
        title: "Checkbox",
        description: "Collect user's choices.",
    },
    Component {
        id: ComponentId::Radio,
        navigation_id: "gallery.navigation.radio",
        group: "Data Entry",
        title: "Radio",
        description: "Used to select a single state from multiple options.",
    },
    Component {
        id: ComponentId::Input,
        navigation_id: "gallery.navigation.input",
        group: "Data Entry",
        title: "Input",
        description: "A basic widget for getting user input.",
    },
    Component {
        id: ComponentId::InputNumber,
        navigation_id: "gallery.navigation.input-number",
        group: "Data Entry",
        title: "InputNumber",
        description: "Enter a number within certain range with the mouse or keyboard.",
    },
    Component {
        id: ComponentId::Select,
        navigation_id: "gallery.navigation.select",
        group: "Data Entry",
        title: "Select",
        description: "A dropdown menu for displaying choices.",
    },
    Component {
        id: ComponentId::Slider,
        navigation_id: "gallery.navigation.slider",
        group: "Data Entry",
        title: "Slider",
        description: "Select a value or an interval within a numeric range.",
    },
    Component {
        id: ComponentId::Segmented,
        navigation_id: "gallery.navigation.segmented",
        group: "Data Entry",
        title: "Segmented",
        description: "Display multiple options and select a single option.",
    },
    Component {
        id: ComponentId::Switch,
        navigation_id: "gallery.navigation.switch",
        group: "Data Entry",
        title: "Switch",
        description: "Used to toggle between two states.",
    },
    Component {
        id: ComponentId::Tag,
        navigation_id: "gallery.navigation.tag",
        group: "Data Display",
        title: "Tag",
        description: "Used for marking and categorization.",
    },
    Component {
        id: ComponentId::Tooltip,
        navigation_id: "gallery.navigation.tooltip",
        group: "Data Display",
        title: "Tooltip",
        description: "Simple text popup tips.",
    },
    Component {
        id: ComponentId::Popover,
        navigation_id: "gallery.navigation.popover",
        group: "Data Display",
        title: "Popover",
        description: "A floating card with additional content.",
    },
    Component {
        id: ComponentId::Popconfirm,
        navigation_id: "gallery.navigation.popconfirm",
        group: "Feedback",
        title: "Popconfirm",
        description: "A compact confirmation of an action.",
    },
];
