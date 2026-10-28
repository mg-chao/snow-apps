// Copyright (C) 2026 mg-chao
// SPDX-License-Identifier: Apache-2.0
// Common messages adapted from Ant Design 6.6.7 (MIT); see THIRD_PARTY_NOTICES.md.

//! Typed, immutable locale catalogs. Locale selection never changes layout direction.

use gpui::SharedString;
use std::sync::{Arc, LazyLock};

macro_rules! common_messages {
    ($($field:ident => $key:ident),+ $(,)?) => {
        /// All common messages required by a complete locale catalog.
        #[derive(Clone, Debug, PartialEq, Eq)]
        pub struct CommonMessages { $(pub $field: SharedString,)+ }

        /// A partial override. An absent field inherits its parent's message.
        #[derive(Clone, Debug, Default, PartialEq, Eq)]
        pub struct CommonMessagePatch { $(pub $field: Option<SharedString>,)+ }

        /// Stable message keys, independent of their translated text.
        #[derive(Clone, Copy, Debug, PartialEq, Eq, Hash)]
        pub enum MessageKey { $($key,)+ }

        impl MessageKey {
            pub const ALL: &'static [Self] = &[$(Self::$key,)+];
        }

        impl CommonMessages {
            pub fn text(&self, key: MessageKey) -> &SharedString {
                match key { $(MessageKey::$key => &self.$field,)+ }
            }

            pub fn patched(&self, patch: &CommonMessagePatch) -> Self {
                Self { $($field: patch.$field.clone().unwrap_or_else(|| self.$field.clone()),)+ }
            }
        }

        impl CommonMessagePatch {
            pub fn is_empty(&self) -> bool { $(self.$field.is_none())&&+ }
        }
    }
}

common_messages! {
    placeholder => Placeholder,
    close => Close,
    sortable => Sortable,
    show => Show,
    hide => Hide,
    clear_input => ClearInput,
    search => Search,
    show_password => ShowPassword,
    hide_password => HidePassword,
    increase_value => IncreaseValue,
    decrease_value => DecreaseValue,
    verification_code => VerificationCode,
    code_character => CodeCharacter,
    no_data => NoData,
    clear_selection => ClearSelection,
    remove_selection => RemoveSelection,
    context_menu => ContextMenu,
    navigation_menu => NavigationMenu,
    more_navigation_items => MoreNavigationItems,
    horizontal_scroll_bar => HorizontalScrollBar,
    vertical_scroll_bar => VerticalScrollBar,
    add_tab => AddTab,
    more_tabs => MoreTabs,
}

/// Popconfirm-owned messages, independent of application-supplied titles.
#[derive(Clone, Debug, PartialEq, Eq)]
pub struct PopconfirmMessages {
    pub ok_text: SharedString,
    pub cancel_text: SharedString,
}

#[derive(Clone, Debug, Default, PartialEq, Eq)]
pub struct PopconfirmMessagePatch {
    pub ok_text: Option<SharedString>,
    pub cancel_text: Option<SharedString>,
}

impl PopconfirmMessages {
    fn patched(&self, patch: &PopconfirmMessagePatch) -> Self {
        Self {
            ok_text: patch
                .ok_text
                .clone()
                .unwrap_or_else(|| self.ok_text.clone()),
            cancel_text: patch
                .cancel_text
                .clone()
                .unwrap_or_else(|| self.cancel_text.clone()),
        }
    }
}

macro_rules! pagination_messages {
    ($($field:ident),+ $(,)?) => {
        /// Pagination-owned messages. `{page}` is replaced with a page number.
        #[derive(Clone, Debug, PartialEq, Eq)]
        pub struct PaginationMessages { $(pub $field: SharedString,)+ }

        #[derive(Clone, Debug, Default, PartialEq, Eq)]
        pub struct PaginationMessagePatch { $(pub $field: Option<SharedString>,)+ }

        impl PaginationMessages {
            pub fn patched(&self, patch: &PaginationMessagePatch) -> Self {
                Self { $($field: patch.$field.clone().unwrap_or_else(|| self.$field.clone()),)+ }
            }
        }

        impl PaginationMessagePatch {
            pub fn is_empty(&self) -> bool { $(self.$field.is_none())&&+ }
        }
    };
}

pagination_messages! {
    root_label, page_label, items_per_page, jump_to, jump_to_confirm, page,
    prev_page, next_page, prev_5, next_5, prev_3, next_3, page_size,
}

/// Locale identity is metadata, never a translated display label.
#[derive(Clone, Debug, PartialEq, Eq, Hash, Default)]
pub enum LocaleId {
    #[default]
    EnUs,
    ZhCn,
    ZhTw,
    Custom(SharedString),
}

impl LocaleId {
    pub fn as_str(&self) -> &str {
        match self {
            Self::EnUs => "en_US",
            Self::ZhCn => "zh_CN",
            Self::ZhTw => "zh_TW",
            Self::Custom(id) => id.as_ref(),
        }
    }
}

/// A fully resolved catalog. Future controls add typed component catalogs.
#[derive(Clone, Debug, PartialEq, Eq)]
pub struct Locale {
    pub id: LocaleId,
    pub common: CommonMessages,
    pub popconfirm: PopconfirmMessages,
    pub pagination: PaginationMessages,
}

impl Locale {
    pub fn en_us() -> Arc<Self> {
        static VALUE: LazyLock<Arc<Locale>> = LazyLock::new(|| {
            Arc::new(Locale {
                id: LocaleId::EnUs,
                popconfirm: PopconfirmMessages {
                    ok_text: "OK".into(),
                    cancel_text: "Cancel".into(),
                },
                pagination: PaginationMessages {
                    root_label: "Pagination".into(),
                    page_label: "Page {page}".into(),
                    items_per_page: "/ page".into(),
                    jump_to: "Go to".into(),
                    jump_to_confirm: "confirm".into(),
                    page: "Page".into(),
                    prev_page: "Previous Page".into(),
                    next_page: "Next Page".into(),
                    prev_5: "Previous 5 Pages".into(),
                    next_5: "Next 5 Pages".into(),
                    prev_3: "Previous 3 Pages".into(),
                    next_3: "Next 3 Pages".into(),
                    page_size: "Page Size".into(),
                },
                common: CommonMessages {
                    placeholder: "Please select".into(),
                    close: "Close".into(),
                    sortable: "sortable".into(),
                    show: "Show".into(),
                    hide: "Hide".into(),
                    clear_input: "Clear input".into(),
                    search: "Search".into(),
                    show_password: "Show password".into(),
                    hide_password: "Hide password".into(),
                    increase_value: "Increase value".into(),
                    decrease_value: "Decrease value".into(),
                    verification_code: "Verification code".into(),
                    code_character: "Character {index} of {length}".into(),
                    no_data: "No data".into(),
                    clear_selection: "Clear selection".into(),
                    remove_selection: "Remove {label}".into(),
                    context_menu: "Context menu".into(),
                    navigation_menu: "Navigation menu".into(),
                    more_navigation_items: "More navigation items".into(),
                    horizontal_scroll_bar: "Horizontal scroll bar".into(),
                    vertical_scroll_bar: "Vertical scroll bar".into(),
                    add_tab: "Add tab".into(),
                    more_tabs: "More tabs".into(),
                },
            })
        });
        VALUE.clone()
    }

    pub fn zh_cn() -> Arc<Self> {
        static VALUE: LazyLock<Arc<Locale>> = LazyLock::new(|| {
            Arc::new(Locale {
                id: LocaleId::ZhCn,
                popconfirm: PopconfirmMessages {
                    ok_text: "确定".into(),
                    cancel_text: "取消".into(),
                },
                pagination: PaginationMessages {
                    root_label: "分页".into(),
                    page_label: "第 {page} 页".into(),
                    items_per_page: "条/页".into(),
                    jump_to: "跳至".into(),
                    jump_to_confirm: "确定".into(),
                    page: "页".into(),
                    prev_page: "上一页".into(),
                    next_page: "下一页".into(),
                    prev_5: "向前 5 页".into(),
                    next_5: "向后 5 页".into(),
                    prev_3: "向前 3 页".into(),
                    next_3: "向后 3 页".into(),
                    page_size: "页码".into(),
                },
                common: CommonMessages {
                    placeholder: "请选择".into(),
                    close: "关闭".into(),
                    sortable: "可排序".into(),
                    show: "显示".into(),
                    hide: "隐藏".into(),
                    clear_input: "清空输入".into(),
                    search: "搜索".into(),
                    show_password: "显示密码".into(),
                    hide_password: "隐藏密码".into(),
                    increase_value: "增加数值".into(),
                    decrease_value: "减少数值".into(),
                    verification_code: "验证码".into(),
                    code_character: "第 {index} 位，共 {length} 位".into(),
                    no_data: "暂无数据".into(),
                    clear_selection: "清空选择".into(),
                    remove_selection: "移除 {label}".into(),
                    context_menu: "上下文菜单".into(),
                    navigation_menu: "导航菜单".into(),
                    more_navigation_items: "更多导航项".into(),
                    horizontal_scroll_bar: "水平滚动条".into(),
                    vertical_scroll_bar: "垂直滚动条".into(),
                    add_tab: "添加标签页".into(),
                    more_tabs: "更多标签页".into(),
                },
            })
        });
        VALUE.clone()
    }

    pub fn zh_tw() -> Arc<Self> {
        static VALUE: LazyLock<Arc<Locale>> = LazyLock::new(|| {
            Arc::new(Locale {
                id: LocaleId::ZhTw,
                popconfirm: PopconfirmMessages {
                    ok_text: "確定".into(),
                    cancel_text: "取消".into(),
                },
                pagination: PaginationMessages {
                    root_label: "分頁".into(),
                    page_label: "第 {page} 頁".into(),
                    items_per_page: "條/頁".into(),
                    jump_to: "跳至".into(),
                    jump_to_confirm: "確定".into(),
                    page: "頁".into(),
                    prev_page: "上一頁".into(),
                    next_page: "下一頁".into(),
                    prev_5: "向前 5 頁".into(),
                    next_5: "向後 5 頁".into(),
                    prev_3: "向前 3 頁".into(),
                    next_3: "向後 3 頁".into(),
                    page_size: "頁碼".into(),
                },
                common: CommonMessages {
                    placeholder: "請選擇".into(),
                    close: "關閉".into(),
                    sortable: "可排序".into(),
                    show: "顯示".into(),
                    hide: "隱藏".into(),
                    clear_input: "清空輸入".into(),
                    search: "搜尋".into(),
                    show_password: "顯示密碼".into(),
                    hide_password: "隱藏密碼".into(),
                    increase_value: "增加數值".into(),
                    decrease_value: "減少數值".into(),
                    verification_code: "驗證碼".into(),
                    code_character: "第 {index} 位，共 {length} 位".into(),
                    no_data: "暫無數據".into(),
                    clear_selection: "清空選擇".into(),
                    remove_selection: "移除 {label}".into(),
                    context_menu: "快顯功能表".into(),
                    navigation_menu: "導覽功能表".into(),
                    more_navigation_items: "更多導覽項目".into(),
                    horizontal_scroll_bar: "水平捲動軸".into(),
                    vertical_scroll_bar: "垂直捲動軸".into(),
                    add_tab: "新增分頁".into(),
                    more_tabs: "更多分頁".into(),
                },
            })
        });
        VALUE.clone()
    }

    /// Create a custom catalog with English fallback for every unspecified field.
    pub fn custom(id: impl Into<SharedString>, common: CommonMessagePatch) -> Arc<Self> {
        Arc::new(Self {
            id: LocaleId::Custom(id.into()),
            common: Self::en_us().common.patched(&common),
            popconfirm: Self::en_us().popconfirm.clone(),
            pagination: Self::en_us().pagination.clone(),
        })
    }

    pub fn text(&self, key: MessageKey) -> &SharedString {
        self.common.text(key)
    }
}

/// Selecting a locale replaces the base catalog; common overrides are applied afterward.
#[derive(Clone, Debug, Default, PartialEq, Eq)]
pub struct LocalePatch {
    pub locale: Option<Arc<Locale>>,
    pub common: CommonMessagePatch,
    pub popconfirm: PopconfirmMessagePatch,
    pub pagination: PaginationMessagePatch,
}

impl LocalePatch {
    pub fn resolve(&self, parent: &Arc<Locale>) -> Arc<Locale> {
        self.resolve_with_previous(parent, None)
    }

    /// Retain a scoped catalog when an unrelated parent setting changes.
    pub fn resolve_with_previous(
        &self,
        parent: &Arc<Locale>,
        previous: Option<&Arc<Locale>>,
    ) -> Arc<Locale> {
        let base = self.locale.as_ref().unwrap_or(parent);
        if self.common.is_empty()
            && self.popconfirm == PopconfirmMessagePatch::default()
            && self.pagination.is_empty()
        {
            return base.clone();
        }
        let common = base.common.patched(&self.common);
        let popconfirm = base.popconfirm.patched(&self.popconfirm);
        let pagination = base.pagination.patched(&self.pagination);
        if common == base.common && popconfirm == base.popconfirm && pagination == base.pagination {
            return base.clone();
        }
        if let Some(previous) = previous.filter(|old| {
            old.id == base.id
                && old.common == common
                && old.popconfirm == popconfirm
                && old.pagination == pagination
        }) {
            return previous.clone();
        }
        Arc::new(Locale {
            id: base.id.clone(),
            common,
            popconfirm,
            pagination,
        })
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn pagination_catalogs_match_reference_and_patch_per_field() {
        let english = Locale::en_us();
        assert_eq!(english.pagination.items_per_page.as_ref(), "/ page");
        assert_eq!(english.pagination.prev_5.as_ref(), "Previous 5 Pages");
        assert_eq!(english.pagination.page_label.as_ref(), "Page {page}");
        assert_eq!(Locale::zh_cn().pagination.items_per_page.as_ref(), "条/页");
        assert_eq!(Locale::zh_tw().pagination.items_per_page.as_ref(), "條/頁");
        assert_eq!(Locale::zh_cn().pagination.page_size.as_ref(), "页码");
        assert_eq!(Locale::zh_tw().pagination.page_size.as_ref(), "頁碼");
        let patch = LocalePatch {
            pagination: PaginationMessagePatch {
                jump_to: Some("Jump to".into()),
                ..Default::default()
            },
            ..Default::default()
        };
        let scoped = patch.resolve(&english);
        assert_eq!(scoped.pagination.jump_to.as_ref(), "Jump to");
        assert_eq!(scoped.pagination.page.as_ref(), "Page");
        let chinese = patch.resolve(&Locale::zh_tw());
        assert_eq!(chinese.pagination.jump_to.as_ref(), "Jump to");
        assert_eq!(chinese.pagination.page.as_ref(), "頁");
        assert_eq!(scoped.common, english.common);
        assert_eq!(scoped.popconfirm, english.popconfirm);
        assert_eq!(
            Locale::custom("custom", CommonMessagePatch::default()).pagination,
            english.pagination
        );
    }

    #[test]
    fn pagination_locale_no_op_and_repeated_patches_share_catalogs() {
        let english = Locale::en_us();
        let equivalent = LocalePatch {
            pagination: PaginationMessagePatch {
                root_label: Some("Pagination".into()),
                ..Default::default()
            },
            ..Default::default()
        };
        assert!(Arc::ptr_eq(&english, &equivalent.resolve(&english)));
        let patch = LocalePatch {
            pagination: PaginationMessagePatch {
                page_label: Some("Page number {page}".into()),
                ..Default::default()
            },
            ..Default::default()
        };
        let previous = patch.resolve(&english);
        assert!(Arc::ptr_eq(
            &previous,
            &patch.resolve_with_previous(&english, Some(&previous))
        ));
        let switched = LocalePatch {
            locale: Some(Locale::zh_tw()),
            ..Default::default()
        }
        .resolve(&previous);
        assert_eq!(switched.pagination.page_label.as_ref(), "第 {page} 頁");
    }

    #[test]
    fn popconfirm_catalogs_and_field_overrides_follow_locale_changes() {
        assert_eq!(Locale::en_us().popconfirm.ok_text.as_ref(), "OK");
        assert_eq!(Locale::en_us().popconfirm.cancel_text.as_ref(), "Cancel");
        assert_eq!(Locale::zh_cn().popconfirm.ok_text.as_ref(), "确定");
        assert_eq!(Locale::zh_tw().popconfirm.ok_text.as_ref(), "確定");
        let patch = LocalePatch {
            popconfirm: PopconfirmMessagePatch {
                ok_text: Some("Delete".into()),
                ..Default::default()
            },
            ..Default::default()
        };
        let english = patch.resolve(&Locale::en_us());
        assert_eq!(english.popconfirm.ok_text.as_ref(), "Delete");
        assert_eq!(english.popconfirm.cancel_text.as_ref(), "Cancel");
        let chinese = patch.resolve(&Locale::zh_cn());
        assert_eq!(chinese.popconfirm.ok_text.as_ref(), "Delete");
        assert_eq!(chinese.popconfirm.cancel_text.as_ref(), "取消");
        let switched = LocalePatch {
            locale: Some(Locale::zh_tw()),
            ..Default::default()
        }
        .resolve(&english);
        assert_eq!(switched.popconfirm.ok_text.as_ref(), "確定");
    }

    #[test]
    fn popconfirm_equivalent_overrides_and_previous_results_share_catalogs() {
        let english = Locale::en_us();
        let equivalent = LocalePatch {
            popconfirm: PopconfirmMessagePatch {
                ok_text: Some("OK".into()),
                ..Default::default()
            },
            ..Default::default()
        };
        assert!(Arc::ptr_eq(&english, &equivalent.resolve(&english)));
        let patch = LocalePatch {
            popconfirm: PopconfirmMessagePatch {
                ok_text: Some("Delete".into()),
                ..Default::default()
            },
            ..Default::default()
        };
        let previous = patch.resolve(&english);
        assert!(Arc::ptr_eq(
            &previous,
            &patch.resolve_with_previous(&english, Some(&previous))
        ));
        let custom = Locale::custom("fr_FR", CommonMessagePatch::default());
        assert_eq!(custom.popconfirm, english.popconfirm);
    }

    #[test]
    fn builtin_catalogs_are_complete_and_english_is_identity() {
        for locale in [Locale::en_us(), Locale::zh_cn(), Locale::zh_tw()] {
            for key in MessageKey::ALL {
                assert!(
                    !locale.text(*key).is_empty(),
                    "{}: {key:?}",
                    locale.id.as_str()
                );
            }
        }
        assert_eq!(Locale::en_us().common.placeholder.as_ref(), "Please select");
        assert_eq!(Locale::zh_cn().common.close.as_ref(), "关闭");
        assert_eq!(Locale::zh_tw().common.close.as_ref(), "關閉");
        assert_eq!(
            Locale::en_us().text(MessageKey::ContextMenu).as_ref(),
            "Context menu"
        );
        assert_eq!(
            Locale::zh_cn().text(MessageKey::VerticalScrollBar).as_ref(),
            "垂直滚动条"
        );
        assert_eq!(
            Locale::zh_tw()
                .text(MessageKey::HorizontalScrollBar)
                .as_ref(),
            "水平捲動軸"
        );
    }

    #[test]
    fn custom_and_scoped_overrides_fall_back_per_field() {
        let custom = Locale::custom(
            "fr_FR",
            CommonMessagePatch {
                close: Some("Fermer".into()),
                ..Default::default()
            },
        );
        assert_eq!(custom.common.close.as_ref(), "Fermer");
        assert_eq!(custom.common.hide.as_ref(), "Hide");
        let scoped = LocalePatch {
            common: CommonMessagePatch {
                show: Some("Afficher".into()),
                ..Default::default()
            },
            ..Default::default()
        }
        .resolve(&custom);
        assert_eq!(scoped.common.close.as_ref(), "Fermer");
        assert_eq!(scoped.common.show.as_ref(), "Afficher");
        let switched = LocalePatch {
            locale: Some(Locale::zh_tw()),
            ..Default::default()
        }
        .resolve(&scoped);
        assert_eq!(switched.common.show.as_ref(), "顯示");
    }

    #[test]
    fn empty_and_equivalent_overrides_reuse_catalogs() {
        let english = Locale::en_us();
        assert!(Arc::ptr_eq(
            &english,
            &LocalePatch::default().resolve(&english)
        ));
        let patch = LocalePatch {
            common: CommonMessagePatch {
                close: Some("Close".into()),
                ..Default::default()
            },
            ..Default::default()
        };
        assert!(Arc::ptr_eq(&english, &patch.resolve(&english)));
    }
}
