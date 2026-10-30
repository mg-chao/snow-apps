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

/// Modal-owned action labels, including the single-button acknowledgement.
#[derive(Clone, Debug, PartialEq, Eq)]
pub struct ModalMessages {
    pub ok_text: SharedString,
    pub cancel_text: SharedString,
    pub just_ok_text: SharedString,
}
#[derive(Clone, Debug, Default, PartialEq, Eq)]
pub struct ModalMessagePatch {
    pub ok_text: Option<SharedString>,
    pub cancel_text: Option<SharedString>,
    pub just_ok_text: Option<SharedString>,
}
impl ModalMessages {
    fn patched(&self, patch: &ModalMessagePatch) -> Self {
        Self {
            ok_text: patch
                .ok_text
                .clone()
                .unwrap_or_else(|| self.ok_text.clone()),
            cancel_text: patch
                .cancel_text
                .clone()
                .unwrap_or_else(|| self.cancel_text.clone()),
            just_ok_text: patch
                .just_ok_text
                .clone()
                .unwrap_or_else(|| self.just_ok_text.clone()),
        }
    }
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

/// Carousel-owned navigation labels; indices are one-based in spoken text.
#[derive(Clone, Debug, PartialEq, Eq)]
pub struct CarouselMessages {
    pub root_label: SharedString,
    pub prev_slide: SharedString,
    pub next_slide: SharedString,
    pub slide_label: SharedString,
    pub go_to_slide: SharedString,
}
#[derive(Clone, Debug, Default, PartialEq, Eq)]
pub struct CarouselMessagePatch {
    pub root_label: Option<SharedString>,
    pub prev_slide: Option<SharedString>,
    pub next_slide: Option<SharedString>,
    pub slide_label: Option<SharedString>,
    pub go_to_slide: Option<SharedString>,
}
impl CarouselMessages {
    fn patched(&self, patch: &CarouselMessagePatch) -> Self {
        Self {
            root_label: patch
                .root_label
                .clone()
                .unwrap_or_else(|| self.root_label.clone()),
            prev_slide: patch
                .prev_slide
                .clone()
                .unwrap_or_else(|| self.prev_slide.clone()),
            next_slide: patch
                .next_slide
                .clone()
                .unwrap_or_else(|| self.next_slide.clone()),
            slide_label: patch
                .slide_label
                .clone()
                .unwrap_or_else(|| self.slide_label.clone()),
            go_to_slide: patch
                .go_to_slide
                .clone()
                .unwrap_or_else(|| self.go_to_slide.clone()),
        }
    }
}

macro_rules! image_messages {
    ($($field:ident),+ $(,)?) => {
        /// Image-owned labels. Count patterns use `{current}` and `{total}`.
        #[derive(Clone, Debug, PartialEq, Eq)]
        pub struct ImageMessages { $(pub $field: SharedString,)+ }
        #[derive(Clone, Debug, Default, PartialEq, Eq)]
        pub struct ImageMessagePatch { $(pub $field: Option<SharedString>,)+ }
        impl ImageMessages {
            fn patched(&self, patch: &ImageMessagePatch) -> Self {
                Self { $($field: patch.$field.clone().unwrap_or_else(|| self.$field.clone()),)+ }
            }
        }
    };
}
image_messages! { root_label, preview, loading, load_error, previous, next, flip_y, flip_x,
rotate_left, rotate_right, zoom_out, zoom_in, reset, count }

macro_rules! color_picker_messages {
    ($($field:ident),+ $(,)?) => {
        /// Color picker labels. Stop and preset patterns use `{index}` and `{color}`.
        #[derive(Clone, Debug, PartialEq, Eq)]
        pub struct ColorPickerMessages { $(pub $field: SharedString,)+ }
        #[derive(Clone, Debug, Default, PartialEq, Eq)]
        pub struct ColorPickerMessagePatch { $(pub $field: Option<SharedString>,)+ }
        impl ColorPickerMessages {
            fn patched(&self, patch: &ColorPickerMessagePatch) -> Self {
                Self { $($field: patch.$field.clone().unwrap_or_else(|| self.$field.clone()),)+ }
            }
        }
        impl ColorPickerMessagePatch {
            pub fn is_empty(&self) -> bool { $(self.$field.is_none())&&+ }
        }
    };
}
color_picker_messages! {
    root_label, panel_label, clear, color, hex, red, green, blue, hue, saturation,
    brightness, alpha, format, single, gradient, gradient_stop, add_stop, remove_stop,
    preset_color, preset_empty, transparent, expand_presets, collapse_presets,
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
    pub image: ImageMessages,
    pub popconfirm: PopconfirmMessages,
    pub modal: ModalMessages,
    pub pagination: PaginationMessages,
    pub carousel: CarouselMessages,
    pub color_picker: ColorPickerMessages,
}

impl Locale {
    pub fn en_us() -> Arc<Self> {
        static VALUE: LazyLock<Arc<Locale>> = LazyLock::new(|| {
            Arc::new(Locale {
                id: LocaleId::EnUs,
                color_picker: ColorPickerMessages {
                    root_label: "Choose color".into(),
                    panel_label: "Color picker".into(),
                    clear: "Clear color".into(),
                    color: "Color".into(),
                    hex: "Hex color".into(),
                    red: "Red".into(),
                    green: "Green".into(),
                    blue: "Blue".into(),
                    hue: "Hue".into(),
                    saturation: "Saturation".into(),
                    brightness: "Brightness".into(),
                    alpha: "Alpha".into(),
                    format: "Color format".into(),
                    single: "Single".into(),
                    gradient: "Gradient".into(),
                    gradient_stop: "Gradient stop {index}".into(),
                    add_stop: "Add gradient stop".into(),
                    remove_stop: "Remove gradient stop".into(),
                    preset_color: "Preset {color}".into(),
                    preset_empty: "Empty".into(),
                    transparent: "Transparent".into(),
                    expand_presets: "Expand preset colors".into(),
                    collapse_presets: "Collapse preset colors".into(),
                },
                image: ImageMessages {
                    root_label: "Image preview".into(),
                    preview: "Preview".into(),
                    loading: "Loading".into(),
                    load_error: "Failed to load image".into(),
                    previous: "Previous image".into(),
                    next: "Next image".into(),
                    flip_y: "Flip vertically".into(),
                    flip_x: "Flip horizontally".into(),
                    rotate_left: "Rotate left".into(),
                    rotate_right: "Rotate right".into(),
                    zoom_out: "Zoom out".into(),
                    zoom_in: "Zoom in".into(),
                    reset: "Reset".into(),
                    count: "{current} / {total}".into(),
                },
                carousel: CarouselMessages {
                    root_label: "Carousel".into(),
                    prev_slide: "Previous slide".into(),
                    next_slide: "Next slide".into(),
                    slide_label: "Slide {index} of {count}".into(),
                    go_to_slide: "Go to slide {index}".into(),
                },
                modal: ModalMessages {
                    ok_text: "OK".into(),
                    cancel_text: "Cancel".into(),
                    just_ok_text: "OK".into(),
                },
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
                color_picker: ColorPickerMessages {
                    root_label: "选择颜色".into(),
                    panel_label: "颜色选择器".into(),
                    clear: "清空颜色".into(),
                    color: "颜色".into(),
                    hex: "十六进制颜色".into(),
                    red: "红色".into(),
                    green: "绿色".into(),
                    blue: "蓝色".into(),
                    hue: "色相".into(),
                    saturation: "饱和度".into(),
                    brightness: "亮度".into(),
                    alpha: "透明度".into(),
                    format: "颜色格式".into(),
                    single: "单色".into(),
                    gradient: "渐变色".into(),
                    gradient_stop: "渐变色节点 {index}".into(),
                    add_stop: "添加渐变色节点".into(),
                    remove_stop: "移除渐变色节点".into(),
                    preset_color: "预设颜色 {color}".into(),
                    preset_empty: "暂无".into(),
                    transparent: "无色".into(),
                    expand_presets: "展开预设颜色".into(),
                    collapse_presets: "收起预设颜色".into(),
                },
                image: ImageMessages {
                    root_label: "图片预览".into(),
                    preview: "预览".into(),
                    loading: "加载中".into(),
                    load_error: "图片加载失败".into(),
                    previous: "上一张图片".into(),
                    next: "下一张图片".into(),
                    flip_y: "垂直翻转".into(),
                    flip_x: "水平翻转".into(),
                    rotate_left: "向左旋转".into(),
                    rotate_right: "向右旋转".into(),
                    zoom_out: "缩小".into(),
                    zoom_in: "放大".into(),
                    reset: "重置".into(),
                    count: "{current} / {total}".into(),
                },
                carousel: CarouselMessages {
                    root_label: "轮播图".into(),
                    prev_slide: "上一张".into(),
                    next_slide: "下一张".into(),
                    slide_label: "第 {index} 张，共 {count} 张".into(),
                    go_to_slide: "转到第 {index} 张".into(),
                },
                modal: ModalMessages {
                    ok_text: "确定".into(),
                    cancel_text: "取消".into(),
                    just_ok_text: "知道了".into(),
                },
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
                color_picker: ColorPickerMessages {
                    root_label: "選擇顏色".into(),
                    panel_label: "顏色選擇器".into(),
                    clear: "清除顏色".into(),
                    color: "顏色".into(),
                    hex: "十六進位顏色".into(),
                    red: "紅色".into(),
                    green: "綠色".into(),
                    blue: "藍色".into(),
                    hue: "色相".into(),
                    saturation: "飽和度".into(),
                    brightness: "亮度".into(),
                    alpha: "透明度".into(),
                    format: "顏色格式".into(),
                    single: "單色".into(),
                    gradient: "漸層色".into(),
                    gradient_stop: "漸層色節點 {index}".into(),
                    add_stop: "新增漸層色節點".into(),
                    remove_stop: "移除漸層色節點".into(),
                    preset_color: "預設顏色 {color}".into(),
                    preset_empty: "暫無".into(),
                    transparent: "透明".into(),
                    expand_presets: "展開預設顏色".into(),
                    collapse_presets: "收合預設顏色".into(),
                },
                image: ImageMessages {
                    root_label: "圖片預覽".into(),
                    preview: "預覽".into(),
                    loading: "載入中".into(),
                    load_error: "圖片載入失敗".into(),
                    previous: "上一張圖片".into(),
                    next: "下一張圖片".into(),
                    flip_y: "垂直翻轉".into(),
                    flip_x: "水平翻轉".into(),
                    rotate_left: "向左旋轉".into(),
                    rotate_right: "向右旋轉".into(),
                    zoom_out: "縮小".into(),
                    zoom_in: "放大".into(),
                    reset: "重設".into(),
                    count: "{current} / {total}".into(),
                },
                carousel: CarouselMessages {
                    root_label: "輪播圖".into(),
                    prev_slide: "上一張".into(),
                    next_slide: "下一張".into(),
                    slide_label: "第 {index} 張，共 {count} 張".into(),
                    go_to_slide: "跳至第 {index} 張".into(),
                },
                modal: ModalMessages {
                    ok_text: "確定".into(),
                    cancel_text: "取消".into(),
                    just_ok_text: "知道了".into(),
                },
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
            image: Self::en_us().image.clone(),
            popconfirm: Self::en_us().popconfirm.clone(),
            modal: Self::en_us().modal.clone(),
            pagination: Self::en_us().pagination.clone(),
            carousel: Self::en_us().carousel.clone(),
            color_picker: Self::en_us().color_picker.clone(),
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
    pub image: ImageMessagePatch,
    pub popconfirm: PopconfirmMessagePatch,
    pub modal: ModalMessagePatch,
    pub pagination: PaginationMessagePatch,
    pub carousel: CarouselMessagePatch,
    pub color_picker: ColorPickerMessagePatch,
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
            && self.image == ImageMessagePatch::default()
            && self.popconfirm == PopconfirmMessagePatch::default()
            && self.modal == ModalMessagePatch::default()
            && self.pagination.is_empty()
            && self.carousel == CarouselMessagePatch::default()
            && self.color_picker.is_empty()
        {
            return base.clone();
        }
        let common = base.common.patched(&self.common);
        let image = base.image.patched(&self.image);
        let popconfirm = base.popconfirm.patched(&self.popconfirm);
        let modal = base.modal.patched(&self.modal);
        let pagination = base.pagination.patched(&self.pagination);
        let carousel = base.carousel.patched(&self.carousel);
        let color_picker = base.color_picker.patched(&self.color_picker);
        if common == base.common
            && image == base.image
            && popconfirm == base.popconfirm
            && pagination == base.pagination
            && modal == base.modal
            && carousel == base.carousel
            && color_picker == base.color_picker
        {
            return base.clone();
        }
        if let Some(previous) = previous.filter(|old| {
            old.id == base.id
                && old.image == image
                && old.common == common
                && old.popconfirm == popconfirm
                && old.modal == modal
                && old.pagination == pagination
                && old.carousel == carousel
                && old.color_picker == color_picker
        }) {
            return previous.clone();
        }
        Arc::new(Locale {
            id: base.id.clone(),
            common,
            image,
            popconfirm,
            modal,
            pagination,
            carousel,
            color_picker,
        })
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn color_picker_catalogs_preserve_reference_copy_and_inherit_overrides() {
        let english = Locale::en_us();
        assert_eq!(english.color_picker.single.as_ref(), "Single");
        assert_eq!(english.color_picker.gradient.as_ref(), "Gradient");
        assert_eq!(english.color_picker.preset_empty.as_ref(), "Empty");
        assert_eq!(english.color_picker.transparent.as_ref(), "Transparent");
        assert_eq!(Locale::zh_cn().color_picker.preset_empty.as_ref(), "暂无");
        assert_eq!(Locale::zh_cn().color_picker.transparent.as_ref(), "无色");
        assert_eq!(Locale::zh_tw().color_picker.gradient.as_ref(), "漸層色");
        let patch = LocalePatch {
            color_picker: ColorPickerMessagePatch {
                root_label: Some("Accent color".into()),
                ..Default::default()
            },
            ..Default::default()
        };
        for locale in [Locale::en_us(), Locale::zh_cn(), Locale::zh_tw()] {
            let resolved = patch.resolve(&locale);
            assert_eq!(resolved.color_picker.root_label.as_ref(), "Accent color");
            assert_eq!(resolved.color_picker.clear, locale.color_picker.clear);
            assert!(resolved.color_picker.gradient_stop.contains("{index}"));
            assert!(resolved.color_picker.preset_color.contains("{color}"));
            assert!(Arc::ptr_eq(
                &resolved,
                &patch.resolve_with_previous(&locale, Some(&resolved))
            ));
        }
        let equivalent = LocalePatch {
            color_picker: ColorPickerMessagePatch {
                root_label: Some(english.color_picker.root_label.clone()),
                ..Default::default()
            },
            ..Default::default()
        };
        assert!(Arc::ptr_eq(&english, &equivalent.resolve(&english)));
        assert_eq!(
            Locale::custom("custom", CommonMessagePatch::default()).color_picker,
            english.color_picker
        );
    }

    #[test]
    fn carousel_catalogs_preserve_placeholders_and_inherit_per_field() {
        let patch = LocalePatch {
            carousel: CarouselMessagePatch {
                root_label: Some("Featured content".into()),
                ..Default::default()
            },
            ..Default::default()
        };
        for locale in [Locale::en_us(), Locale::zh_cn(), Locale::zh_tw()] {
            let resolved = patch.resolve(&locale);
            assert_eq!(resolved.carousel.root_label.as_ref(), "Featured content");
            assert_eq!(resolved.carousel.prev_slide, locale.carousel.prev_slide);
            assert_eq!(resolved.carousel.next_slide, locale.carousel.next_slide);
            assert!(resolved.carousel.slide_label.contains("{index}"));
            assert!(resolved.carousel.slide_label.contains("{count}"));
            assert!(resolved.carousel.go_to_slide.contains("{index}"));
            assert!(Arc::ptr_eq(
                &resolved,
                &patch.resolve_with_previous(&locale, Some(&resolved))
            ));
        }
        let english = Locale::en_us();
        let equivalent = LocalePatch {
            carousel: CarouselMessagePatch {
                root_label: Some(english.carousel.root_label.clone()),
                ..Default::default()
            },
            ..Default::default()
        };
        assert!(Arc::ptr_eq(&english, &equivalent.resolve(&english)));
        assert_eq!(
            Locale::custom("custom", CommonMessagePatch::default()).carousel,
            english.carousel
        );
    }

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
