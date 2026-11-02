// Copyright (C) 2026 mg-chao
// SPDX-License-Identifier: Apache-2.0
// Catalog text adapted from Ant Design 6.6.7 (MIT).

use gpui::SharedString;

macro_rules! form_messages {
    ($($field:ident => [$en:literal, $cn:literal, $tw:literal]),+ $(,)?) => {
        /// Form-owned copy and validation templates. Template variables use `${name}`.
        #[derive(Clone, Debug, PartialEq, Eq)]
        pub struct FormMessages { $(pub $field: SharedString,)+ }

        /// Partial Form catalog overrides; absent messages inherit their parent.
        #[derive(Clone, Debug, Default, PartialEq, Eq)]
        pub struct FormMessagePatch { $(pub $field: Option<SharedString>,)+ }

        impl FormMessages {
            pub fn en_us() -> Self { Self { $($field: $en.into(),)+ } }
            pub fn zh_cn() -> Self { Self { $($field: $cn.into(),)+ } }
            pub fn zh_tw() -> Self { Self { $($field: $tw.into(),)+ } }
            pub fn patched(&self, patch: &FormMessagePatch) -> Self {
                Self { $($field: patch.$field.clone().unwrap_or_else(|| self.$field.clone()),)+ }
            }
        }

        impl FormMessagePatch {
            pub fn is_empty(&self) -> bool { $(self.$field.is_none())&&+ }
        }
    };
}

form_messages! {
    optional => ["(optional)", "（可选）", "（選填）"],
    default => ["Field validation error for ${label}", "字段验证错误${label}", "欄位驗證錯誤：${label}"],
    required => ["Please enter ${label}", "请输入${label}", "請輸入${label}"],
    enum_values => ["${label} must be one of [${enum}]", "${label}必须是其中一个[${enum}]", "${label}必須是 [${enum}] 其中之一"],
    whitespace => ["${label} cannot be a blank character", "${label}不能为空字符", "${label}不能是空白字元"],
    type_invalid => ["${label} is not a valid ${type}", "${label}不是一个有效的${type}", "${label}不是有效的${type}"],
    date_format => ["${label} date format is invalid", "${label}日期格式无效", "${label}的日期格式無效"],
    date_parse => ["${label} cannot be converted to a date", "${label}不能转换为日期", "${label}無法轉換為日期"],
    date_invalid => ["${label} is an invalid date", "${label}是一个无效日期", "${label}是無效的日期"],
    string_len => ["${label} must be ${len} characters", "${label}须为${len}个字符", "${label}須為${len}個字元"],
    string_min => ["${label} must be at least ${min} characters", "${label}最少${min}个字符", "${label}最少需要${min}個字元"],
    string_max => ["${label} must be up to ${max} characters", "${label}最多${max}个字符", "${label}最多可為${max}個字元"],
    string_range => ["${label} must be between ${min}-${max} characters", "${label}须在${min}-${max}字符之间", "${label}須介於${min}-${max}個字元之間"],
    number_len => ["${label} must be equal to ${len}", "${label}必须等于${len}", "${label}必須等於${len}"],
    number_min => ["${label} must be minimum ${min}", "${label}最小值为${min}", "${label}最小值為${min}"],
    number_max => ["${label} must be maximum ${max}", "${label}最大值为${max}", "${label}最大值為${max}"],
    number_range => ["${label} must be between ${min}-${max}", "${label}须在${min}-${max}之间", "${label}須在${min}-${max}之間"],
    array_len => ["Must be ${len} ${label}", "须为${len}个${label}", "須為${len}個${label}"],
    array_min => ["At least ${min} ${label}", "最少${min}个${label}", "最少${min}個${label}"],
    array_max => ["At most ${max} ${label}", "最多${max}个${label}", "最多${max}個${label}"],
    array_range => ["The amount of ${label} must be between ${min}-${max}", "${label}数量须在${min}-${max}之间", "${label}數量須在${min}-${max}之間"],
    pattern_mismatch => ["${label} does not match the pattern ${pattern}", "${label}与模式不匹配${pattern}", "${label}與模式 ${pattern} 不相符"],
}
