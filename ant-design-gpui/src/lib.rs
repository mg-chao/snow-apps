// Copyright (C) 2026 mg-chao
// SPDX-License-Identifier: Apache-2.0

#![doc = include_str!("../README.md")]

pub mod alert;
pub use alert::{
    Alert, AlertConfig, AlertConfigPatch, AlertIcon, AlertStyleResolver, AlertType, AlertVariant,
};
pub mod behavior;
pub mod button;
pub mod checkbox;
pub mod config;
pub mod context_menu;
pub mod descriptions;
pub use descriptions::{
    DescriptionItem, DescriptionItemStyles, Descriptions, DescriptionsBreakpoints,
    DescriptionsColumns, DescriptionsConfig, DescriptionsConfigPatch, DescriptionsLayout,
    DescriptionsSpan, DescriptionsStyleContext, DescriptionsStyleResolver, DescriptionsStyles,
};
pub mod divider;
pub mod icons;
pub mod input;
pub mod input_number;
pub use input_number::{
    ExactDecimal, InputNumber, InputNumberEvent, InputNumberState, NumberOptions, NumericMode,
    NumericValue,
};
pub mod locale;
pub mod menu;
pub use menu::{Menu, MenuMode, MenuState};
pub mod message;
pub use message::{
    Message, MessageCloseEvent, MessageCloseReason, MessageCompletion, MessageConfig,
    MessageConfigPatch, MessageHandle, MessageHost, MessageLayout, MessageOptions, MessageStack,
    MessageState, MessageStyleContext, MessageStyleResolver, MessageStyles, MessageType,
};
pub mod modal;
pub use modal::{Modal, ModalEvent, ModalHandle, ModalHost, ModalManager, ModalState};
pub mod notification;
pub use notification::{
    Notification, NotificationCloseEvent, NotificationCloseReason, NotificationCompletion,
    NotificationConfig, NotificationConfigPatch, NotificationHandle, NotificationHost,
    NotificationIcon, NotificationLayout, NotificationOptions, NotificationPlacement,
    NotificationStack, NotificationState, NotificationStyleContext, NotificationStyleResolver,
    NotificationStyles, NotificationType,
};
pub mod pagination;
pub use pagination::{
    Pagination, PaginationChange, PaginationEvent, PaginationSizeChange, PaginationState,
};
pub mod popconfirm;
pub mod popover;
pub mod popup;
pub mod progress;
pub use progress::{Progress, ProgressConfig, ProgressConfigPatch, ProgressKind, ProgressStatus};
pub mod radio;
pub mod result;
pub use result::{Result, ResultStatus};
pub mod root;
pub mod scroll_area;
pub mod segmented;
pub mod select;
pub use select::{
    Select, SelectEvent, SelectMode, SelectOption, SelectOptions, SelectSettings, SelectState,
    SelectValue,
};
#[cfg(test)]
mod select_slider_reference_tests;
pub mod slider;
pub mod spin;
pub use spin::{Spin, SpinConfig, SpinConfigPatch, SpinIndicator, SpinPercent, SpinStyleResolver};
pub mod switch;
pub mod tabs;
pub mod tag;
pub mod theme;
pub mod tooltip;

pub use button::Button;
pub use checkbox::{Checkbox, CheckboxChange, CheckboxGroup, CheckboxOption};
pub use config::{
    ConfigPatch, ConfigScope, Direction, PaginationConfig, PaginationConfigPatch, ResolvedConfig,
    Size, ValidationStatus, Variant,
};
pub use context_menu::{
    ContextMenu, ContextMenuState, MenuActivation, MenuActivationSource, MenuCheck, MenuEntry,
    MenuEvent, MenuItem, MenuItems, MenuItemsError,
};
pub use divider::Divider;
pub use input::{
    CountConfig, CountInfo, Input, InputChange, InputCommit, InputEvent, InputGroup,
    InputGroupContext, InputGroupItem, InputGroupOrientation, InputSearch, InputState, OtpEvent,
    OtpInput, OtpOptions, OtpState, PasswordInput, SearchInput, SearchSource, TextArea,
};
pub use popconfirm::Popconfirm;
pub use popover::Popover;
pub use popup::{Popup, PopupHandle, PopupHost};
pub use radio::{Radio, RadioChange, RadioGroup, RadioOption};
pub use root::UiRoot;
pub use scroll_area::{
    ScrollArea, ScrollAreaEvent, ScrollAreaState, ScrollAxis, ScrollbarVisibility,
};
pub use segmented::{Segmented, SegmentedOption};
pub use slider::{Slider, SliderChange, SliderEvent, SliderOptions, SliderState, SliderValue};
pub use switch::{Switch, SwitchChange};
pub use tabs::{TabItem, TabPlacement, Tabs, TabsEvent, TabsItems, TabsState, TabsType};
pub use tag::{CheckableTag, CheckableTagGroup, Tag, TagChange, TagClose, TagOption};
pub use tooltip::Tooltip;

/// Install application defaults and namespaced keyboard bindings once per GPUI app.
///
/// Existing application keymaps and assets are preserved. Native backends remain
/// the responsibility of the application.
pub fn init(cx: &mut gpui::App) {
    config::initialize(cx);
    behavior::initialize(cx);
}
