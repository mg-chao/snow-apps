// Copyright (C) 2026 mg-chao
// SPDX-License-Identifier: Apache-2.0
// Validation patterns adapted from @rc-component/async-validator (MIT).

//! Synchronous rules and foreground-safe native asynchronous validators.

use gpui::{App, SharedString, Task, Window};
use regex::Regex;
use std::{
    collections::BTreeMap,
    fmt,
    future::{Future, poll_fn},
    pin::Pin,
    rc::Rc,
    task::Poll,
};

use super::{
    model::FieldId,
    value::{FormSnapshot, FormValue, FormValueKind, NamePath, NameSegment},
};
use crate::{input_number::ExactDecimal, locale::FormMessages};

#[derive(Clone, Copy, Debug, Default, PartialEq, Eq, Hash)]
pub enum ValidationTrigger {
    #[default]
    Change,
    Blur,
    Submit,
    Programmatic,
}

#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
pub enum RuleMode {
    #[default]
    ParallelAll,
    SerialFirst,
    ParallelFirst,
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum RuleType {
    Any,
    Enum,
    String,
    Number,
    Integer,
    Float,
    Hex,
    Tel,
    Regexp,
    Method,
    Boolean,
    Array,
    Object,
    Date,
    Email,
    Url,
}
impl RuleType {
    pub fn as_str(self) -> &'static str {
        match self {
            Self::Any => "any",
            Self::Enum => "enum",
            Self::String => "string",
            Self::Number => "number",
            Self::Integer => "integer",
            Self::Float => "float",
            Self::Hex => "hex",
            Self::Tel => "tel",
            Self::Regexp => "regexp",
            Self::Method => "method",
            Self::Boolean => "boolean",
            Self::Array => "array",
            Self::Object => "object",
            Self::Date => "date",
            Self::Email => "email",
            Self::Url => "url",
        }
    }
}

#[derive(Clone)]
pub struct FormValidationRequest {
    pub field: FieldId,
    pub name: NamePath,
    pub label: SharedString,
    pub value: FormValue,
    pub values: FormSnapshot,
    pub trigger: ValidationTrigger,
    pub messages: FormMessages,
    pub variables: BTreeMap<SharedString, SharedString>,
}

pub enum FormRuleResult {
    Ready(Result<(), SharedString>),
    Pending(Task<Result<(), SharedString>>),
    ReadyIssues(Vec<FormRuleIssue>),
    PendingIssues(Task<Vec<FormRuleIssue>>),
}
#[derive(Clone, Debug)]
pub struct FormRuleIssue {
    pub name: NamePath,
    pub message: SharedString,
    pub warning_only: bool,
}
impl FormRuleResult {
    pub(crate) async fn into_issues(self, name: NamePath, warning: bool) -> Vec<FormRuleIssue> {
        let mut issues = match self {
            Self::Ready(result) => result
                .err()
                .into_iter()
                .map(|message| FormRuleIssue {
                    name: name.clone(),
                    message,
                    warning_only: warning,
                })
                .collect(),
            Self::Pending(task) => task
                .await
                .err()
                .into_iter()
                .map(|message| FormRuleIssue {
                    name: name.clone(),
                    message,
                    warning_only: warning,
                })
                .collect(),
            Self::ReadyIssues(issues) => issues,
            Self::PendingIssues(task) => task.await,
        };
        for issue in &mut issues {
            issue.warning_only |= warning;
        }
        issues
    }
}
/// A compiled native regular expression with equality by its source pattern.
#[derive(Clone, Debug)]
pub struct FormRegexp {
    source: SharedString,
    regex: Regex,
}
impl PartialEq for FormRegexp {
    fn eq(&self, other: &Self) -> bool {
        self.source == other.source
    }
}
impl FormRegexp {
    pub fn new(source: impl Into<SharedString>) -> Result<Self, regex::Error> {
        let source = source.into();
        let regex = Regex::new(&source)?;
        Ok(Self { source, regex })
    }
    pub fn pattern(&self) -> &str {
        &self.source
    }
    pub fn is_match(&self, text: &str) -> bool {
        self.regex.is_match(text)
    }
}
type FormMethodCallback = dyn Fn(&[FormValue]) -> FormValue;
/// A native callable value. Validation checks its type without invoking it.
#[derive(Clone)]
pub struct FormMethod(Rc<FormMethodCallback>);
impl PartialEq for FormMethod {
    fn eq(&self, other: &Self) -> bool {
        Rc::ptr_eq(&self.0, &other.0)
    }
}
impl fmt::Debug for FormMethod {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        f.write_str("FormMethod")
    }
}
impl FormMethod {
    pub fn new(function: impl Fn(&[FormValue]) -> FormValue + 'static) -> Self {
        Self(Rc::new(function))
    }
    pub fn call(&self, arguments: &[FormValue]) -> FormValue {
        (self.0)(arguments)
    }
}
pub type FormValidator = Rc<dyn Fn(FormValidationRequest, &mut Window, &mut App) -> FormRuleResult>;
pub type FormRuleFactory = Rc<dyn Fn(&FormSnapshot) -> FormRule>;
pub type FormTransform = Rc<dyn Fn(&FormValue) -> FormValue>;

/// The rule transforms a validation copy only; it never changes stored values.
#[derive(Clone, Default)]
pub struct FormRule {
    pub required: bool,
    pub whitespace: bool,
    pub warning_only: bool,
    pub rule_type: Option<RuleType>,
    pub len: Option<f64>,
    pub min: Option<f64>,
    pub max: Option<f64>,
    pub decimal_min: Option<ExactDecimal>,
    pub decimal_max: Option<ExactDecimal>,
    pub enum_values: Option<Vec<FormValue>>,
    pub pattern: Option<Regex>,
    pub message: Option<SharedString>,
    pub triggers: Vec<ValidationTrigger>,
    pub transform: Option<FormTransform>,
    pub validator: Option<FormValidator>,
    pub factory: Option<FormRuleFactory>,
    pub fields: BTreeMap<NameSegment, Vec<FormRule>>,
    pub default_field: Vec<FormRule>,
    pub nested_mode: RuleMode,
}
impl fmt::Debug for FormRule {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        f.debug_struct("FormRule")
            .field("required", &self.required)
            .field("rule_type", &self.rule_type)
            .field("warning_only", &self.warning_only)
            .finish_non_exhaustive()
    }
}
impl FormRule {
    pub fn new() -> Self {
        Self::default()
    }
    pub fn required() -> Self {
        Self {
            required: true,
            ..Self::default()
        }
    }
    pub fn string() -> Self {
        Self::new().value_type(RuleType::String)
    }
    pub fn number() -> Self {
        Self::new().value_type(RuleType::Number)
    }
    pub fn integer() -> Self {
        Self::new().value_type(RuleType::Integer)
    }
    pub fn float() -> Self {
        Self::new().value_type(RuleType::Float)
    }
    pub fn hex() -> Self {
        Self::new().value_type(RuleType::Hex)
    }
    pub fn tel() -> Self {
        Self::new().value_type(RuleType::Tel)
    }
    pub fn regexp() -> Self {
        Self::new().value_type(RuleType::Regexp)
    }
    pub fn method() -> Self {
        Self::new().value_type(RuleType::Method)
    }
    pub fn object() -> Self {
        Self::new().value_type(RuleType::Object)
    }
    pub fn field(
        mut self,
        name: impl Into<NameSegment>,
        rules: impl IntoIterator<Item = FormRule>,
    ) -> Self {
        self.fields.insert(name.into(), rules.into_iter().collect());
        self
    }
    pub fn default_field(mut self, rule: FormRule) -> Self {
        self.default_field.push(rule);
        self
    }
    pub fn default_fields(mut self, rules: impl IntoIterator<Item = FormRule>) -> Self {
        self.default_field.extend(rules);
        self
    }
    pub fn nested_mode(mut self, mode: RuleMode) -> Self {
        self.nested_mode = mode;
        self
    }
    pub fn email() -> Self {
        Self::new().value_type(RuleType::Email)
    }
    pub fn url() -> Self {
        Self::new().value_type(RuleType::Url)
    }
    pub fn array() -> Self {
        Self::new().value_type(RuleType::Array)
    }
    pub fn value_type(mut self, value: RuleType) -> Self {
        self.rule_type = Some(value);
        self
    }
    pub fn whitespace(mut self, value: bool) -> Self {
        self.whitespace = value;
        self
    }
    pub fn warning_only(mut self, value: bool) -> Self {
        self.warning_only = value;
        self
    }
    pub fn len(mut self, value: f64) -> Self {
        self.len = Some(value);
        self
    }
    pub fn min(mut self, value: f64) -> Self {
        self.min = Some(value);
        self
    }
    pub fn max(mut self, value: f64) -> Self {
        self.max = Some(value);
        self
    }
    pub fn decimal_min(mut self, value: ExactDecimal) -> Self {
        self.decimal_min = Some(value);
        self
    }
    pub fn decimal_max(mut self, value: ExactDecimal) -> Self {
        self.decimal_max = Some(value);
        self
    }
    pub fn enum_values(mut self, values: impl IntoIterator<Item = impl Into<FormValue>>) -> Self {
        self.enum_values = Some(values.into_iter().map(Into::into).collect());
        self
    }
    pub fn pattern(mut self, pattern: Regex) -> Self {
        self.pattern = Some(pattern);
        self
    }
    pub fn message(mut self, message: impl Into<SharedString>) -> Self {
        self.message = Some(message.into());
        self
    }
    pub fn triggers(mut self, values: impl IntoIterator<Item = ValidationTrigger>) -> Self {
        self.triggers = values.into_iter().collect();
        self
    }
    pub fn transform(mut self, transform: impl Fn(&FormValue) -> FormValue + 'static) -> Self {
        self.transform = Some(Rc::new(transform));
        self
    }
    pub fn validator(
        mut self,
        validator: impl Fn(FormValidationRequest, &mut Window, &mut App) -> FormRuleResult + 'static,
    ) -> Self {
        self.validator = Some(Rc::new(validator));
        self
    }
    pub fn dynamic(factory: impl Fn(&FormSnapshot) -> FormRule + 'static) -> Self {
        Self {
            factory: Some(Rc::new(factory)),
            ..Self::default()
        }
    }
    pub fn custom(
        validator: impl Fn(FormValidationRequest, &mut Window, &mut App) -> FormRuleResult + 'static,
    ) -> Self {
        Self::new().validator(validator)
    }
    pub fn applies(&self, trigger: ValidationTrigger) -> bool {
        matches!(
            trigger,
            ValidationTrigger::Submit | ValidationTrigger::Programmatic
        ) || self.triggers.is_empty()
            || self.triggers.contains(&trigger)
    }
    pub fn resolved(&self, values: &FormSnapshot) -> Self {
        self.factory
            .as_ref()
            .map_or_else(|| self.clone(), |factory| factory(values))
    }
    pub fn evaluate(
        &self,
        request: FormValidationRequest,
        window: &mut Window,
        cx: &mut App,
    ) -> FormRuleResult {
        self.evaluate_inner(request, window, cx, true)
    }
    fn evaluate_inner(
        &self,
        mut request: FormValidationRequest,
        window: &mut Window,
        cx: &mut App,
        descend: bool,
    ) -> FormRuleResult {
        if descend && let Some(transform) = &self.transform {
            request.value = transform(&request.value);
            if let Ok(root) = request
                .values
                .root
                .with_path(&request.name, request.value.clone())
            {
                request.values.root = root;
            }
        }
        if let Err(message) = self.check(&request) {
            return FormRuleResult::Ready(Err(message));
        }
        let mut nested = BTreeMap::new();
        if descend && !self.default_field.is_empty() {
            if let Some(object) = request.value.as_object() {
                for (key, _) in object.iter() {
                    nested.insert(NameSegment::Key(key.clone()), self.default_field.clone());
                }
            }
            if let Some(array) = request.value.as_array() {
                for index in 0..array.len() {
                    nested.insert(NameSegment::Index(index), self.default_field.clone());
                }
            }
        }
        if descend && (request.value.as_object().is_some() || request.value.as_array().is_some()) {
            nested.extend(self.fields.clone());
        }
        if !nested.is_empty() {
            let mut children = Vec::new();
            for (segment, rules) in nested {
                let name = request.name.child(segment.clone());
                let value = match &segment {
                    NameSegment::Key(key) => {
                        request.value.as_object().and_then(|object| object.get(key))
                    }
                    NameSegment::Index(index) => {
                        request.value.as_array().and_then(|array| array.get(*index))
                    }
                }
                .cloned()
                .unwrap_or_default();
                let child = FormValidationRequest {
                    name: name.clone(),
                    label: name.to_string().into(),
                    value,
                    ..request.clone()
                };
                for rule in rules {
                    let rule = rule.resolved(&request.values);
                    if rule.applies(request.trigger) {
                        children.push((rule, child.clone()));
                    }
                }
            }
            let first = self.evaluate_inner(request.clone(), window, cx, false);
            let mode = self.nested_mode;
            let warning = self.warning_only;
            if mode == RuleMode::SerialFirst {
                return FormRuleResult::PendingIssues(window.spawn(cx, async move |cx| {
                    let mut issues = first.into_issues(request.name.clone(), warning).await;
                    if !issues.is_empty() {
                        return issues;
                    }
                    for (rule, child) in children {
                        let result = match cx
                            .update(|window, cx| rule.evaluate(child.clone(), window, cx))
                        {
                            Ok(result) => result,
                            Err(_) => return issues,
                        };
                        issues.extend(
                            result
                                .into_issues(child.name, warning || rule.warning_only)
                                .await,
                        );
                        if !issues.is_empty() {
                            break;
                        }
                    }
                    issues
                }));
            }
            let mut results = vec![(warning, request.name.clone(), first)];
            for (rule, child) in children {
                results.push((
                    warning || rule.warning_only,
                    child.name.clone(),
                    rule.evaluate(child, window, cx),
                ));
            }
            return FormRuleResult::PendingIssues(
                cx.spawn(async move |_| collect_rule_issues(results, mode).await),
            );
        }
        if let Some(validator) = &self.validator {
            let result = validator(request.clone(), window, cx);
            if matches!(&result, FormRuleResult::Ready(Ok(())))
                || matches!(&result, FormRuleResult::ReadyIssues(issues) if issues.is_empty())
            {
                return result;
            }
            let formatter = RuleMessageFormatter::new(self, &request);
            return match result {
                FormRuleResult::Ready(result) => {
                    FormRuleResult::Ready(result.map_err(|message| formatter.format(&message)))
                }
                FormRuleResult::Pending(task) => {
                    FormRuleResult::Pending(cx.spawn(async move |_| {
                        task.await.map_err(|message| formatter.format(&message))
                    }))
                }
                FormRuleResult::ReadyIssues(mut issues) => {
                    for issue in &mut issues {
                        issue.message = formatter.format(&issue.message);
                    }
                    FormRuleResult::ReadyIssues(issues)
                }
                FormRuleResult::PendingIssues(task) => {
                    FormRuleResult::PendingIssues(cx.spawn(async move |_| {
                        let mut issues = task.await;
                        for issue in &mut issues {
                            issue.message = formatter.format(&issue.message);
                        }
                        issues
                    }))
                }
            };
        }
        FormRuleResult::Ready(Ok(()))
    }
    pub fn check(&self, request: &FormValidationRequest) -> Result<(), SharedString> {
        let value = &request.value;
        let messages = &request.messages;
        let fail = |template: &SharedString| -> Result<(), SharedString> {
            Err(self.format_message(template, request))
        };
        if value.is_empty() {
            if self.required {
                return fail(&messages.required);
            }
            return Ok(());
        }
        if self.whitespace
            && value
                .as_str()
                .is_some_and(|text| text.chars().all(form_whitespace))
        {
            return fail(&messages.whitespace);
        }
        if let Some(kind) = self.rule_type {
            let valid = match kind {
                RuleType::Any | RuleType::Enum => true,
                RuleType::String => value.as_str().is_some(),
                RuleType::Number => value.as_number().is_some_and(|n| n.decimal().is_ok()),
                RuleType::Integer => value
                    .as_number()
                    .and_then(|n| n.decimal().ok())
                    .is_some_and(|n| !n.to_string().contains('.')),
                RuleType::Float => value
                    .as_number()
                    .and_then(|n| n.decimal().ok())
                    .is_some_and(|n| n.to_string().contains('.')),
                RuleType::Hex => value.as_str().is_some_and(hex_valid),
                RuleType::Tel => value.as_str().is_some_and(tel_valid),
                RuleType::Regexp => {
                    value.native_ref::<FormRegexp>().is_ok()
                        || value.as_str().is_some_and(|text| Regex::new(text).is_ok())
                }
                RuleType::Method => value.native_ref::<FormMethod>().is_ok(),
                RuleType::Boolean => value.as_bool().is_some(),
                RuleType::Array => value.as_array().is_some(),
                RuleType::Object => value.as_object().is_some(),
                RuleType::Date => {
                    value.native_ref::<chrono::NaiveDate>().is_ok()
                        || value.native_ref::<chrono::NaiveDateTime>().is_ok()
                        || value.native_ref::<chrono::NaiveTime>().is_ok()
                }
                RuleType::Email => value.as_str().is_some_and(email_valid),
                RuleType::Url => value.as_str().is_some_and(url_valid),
            };
            if !valid {
                return fail(&messages.type_invalid);
            }
        }
        if self
            .enum_values
            .as_ref()
            .map_or(self.rule_type == Some(RuleType::Enum), |values| {
                !values.iter().any(|candidate| candidate == value)
            })
        {
            return fail(&messages.enum_values);
        }
        if let Some(pattern) = &self.pattern
            && value.as_str().is_some_and(|text| !pattern.is_match(text))
        {
            return fail(&messages.pattern_mismatch);
        }
        if let Some(number) = value.as_number().and_then(|n| n.decimal().ok()) {
            if let Some(len) = self.len.and_then(|v| ExactDecimal::from_number(v).ok())
                && number != len
            {
                return fail(&messages.number_len);
            }
            let min = self
                .decimal_min
                .clone()
                .or_else(|| self.min.and_then(|v| ExactDecimal::from_number(v).ok()));
            let max = self
                .decimal_max
                .clone()
                .or_else(|| self.max.and_then(|v| ExactDecimal::from_number(v).ok()));
            if min.as_ref().is_some_and(|min| number < *min)
                || max.as_ref().is_some_and(|max| number > *max)
            {
                return fail(if min.is_some() && max.is_some() {
                    &messages.number_range
                } else if min.is_some() {
                    &messages.number_min
                } else {
                    &messages.number_max
                });
            }
        } else if let Some(length) = value
            .char_len()
            .or_else(|| value.as_array().map(|a| a.len()))
        {
            let array = value.kind() == FormValueKind::Array;
            if self.len.is_some_and(|len| length as f64 != len) {
                return fail(if array {
                    &messages.array_len
                } else {
                    &messages.string_len
                });
            }
            if self.min.is_some_and(|min| (length as f64) < min)
                || self.max.is_some_and(|max| length as f64 > max)
            {
                let template = match (array, self.min.is_some(), self.max.is_some()) {
                    (true, true, true) => &messages.array_range,
                    (true, true, false) => &messages.array_min,
                    (true, false, _) => &messages.array_max,
                    (false, true, true) => &messages.string_range,
                    (false, true, false) => &messages.string_min,
                    (false, false, _) => &messages.string_max,
                };
                return fail(template);
            }
        }
        Ok(())
    }
    fn format_message(
        &self,
        template: &SharedString,
        request: &FormValidationRequest,
    ) -> SharedString {
        let actual = self.message.as_ref().unwrap_or(template);
        if !actual.contains("${") {
            return actual.clone();
        }
        RuleMessageFormatter::new(self, request).format(template)
    }
}

/// Interpolate once, preserving escaped placeholders and replacement text. This
/// small context can cross an async task without retaining a rule schema or form.
struct RuleMessageFormatter {
    message: Option<SharedString>,
    variables: BTreeMap<SharedString, SharedString>,
}
impl RuleMessageFormatter {
    fn new(rule: &FormRule, request: &FormValidationRequest) -> Self {
        let min = rule
            .decimal_min
            .as_ref()
            .map(ToString::to_string)
            .or_else(|| rule.min.map(|v| v.to_string()))
            .unwrap_or_else(|| "undefined".into());
        let max = rule
            .decimal_max
            .as_ref()
            .map(ToString::to_string)
            .or_else(|| rule.max.map(|v| v.to_string()))
            .unwrap_or_else(|| "undefined".into());
        let enumeration = rule
            .enum_values
            .iter()
            .flatten()
            .map(|value| {
                value
                    .as_str()
                    .map(str::to_owned)
                    .unwrap_or_else(|| format!("{value:?}"))
            })
            .collect::<Vec<_>>()
            .join(", ");
        let mut variables = BTreeMap::from([
            ("label".into(), request.label.clone()),
            ("name".into(), request.name.to_string().into()),
            (
                "type".into(),
                rule.rule_type.map_or("undefined", RuleType::as_str).into(),
            ),
            (
                "len".into(),
                rule.len
                    .map(|value| value.to_string())
                    .unwrap_or_else(|| "undefined".into())
                    .into(),
            ),
            ("min".into(), min.into()),
            ("max".into(), max.into()),
            ("enum".into(), enumeration.into()),
            (
                "pattern".into(),
                rule.pattern
                    .as_ref()
                    .map_or("undefined", Regex::as_str)
                    .into(),
            ),
        ]);
        variables.extend(request.variables.clone());
        Self {
            message: rule.message.clone(),
            variables,
        }
    }
    fn format(&self, template: &SharedString) -> SharedString {
        let template = self.message.as_ref().unwrap_or(template);
        if !template.contains("${") {
            return template.clone();
        }
        use std::sync::LazyLock;
        static VARIABLE: LazyLock<Regex> = LazyLock::new(|| {
            Regex::new(r"\\?\$\{([A-Za-z0-9_]+)\}").expect("valid form message variable expression")
        });
        VARIABLE
            .replace_all(template, |capture: &regex::Captures<'_>| {
                if capture[0].starts_with('\\') {
                    capture[0][1..].to_owned()
                } else {
                    self.variables
                        .get(&capture[1])
                        .map_or_else(|| "undefined".to_owned(), ToString::to_string)
                }
            })
            .into_owned()
            .into()
    }
}
fn email_valid(text: &str) -> bool {
    use std::sync::LazyLock;
    static EMAIL: LazyLock<Regex> = LazyLock::new(|| {
        Regex::new(r#"^(([^<>()\[\]\\.,;:\s@"]+(\.[^<>()\[\]\\.,;:\s@"]+)*)|(".+"))@((\[[0-9]{1,3}\.[0-9]{1,3}\.[0-9]{1,3}\.[0-9]{1,3}])|(([a-zA-Z\-0-9\u{00A0}-\u{D7FF}\u{F900}-\u{FDCF}\u{FDF0}-\u{FFEF}]+\.)+[a-zA-Z\u{00A0}-\u{D7FF}\u{F900}-\u{FDCF}\u{FDF0}-\u{FFEF}]{2,}))$"#).expect("valid email expression")
    });
    text.encode_utf16().count() <= 320 && EMAIL.is_match(text)
}

fn hex_valid(text: &str) -> bool {
    let text = text.strip_prefix('#').unwrap_or(text);
    matches!(text.len(), 3 | 6) && text.bytes().all(|byte| byte.is_ascii_hexdigit())
}

// The reference validator uses ECMAScript whitespace, which includes BOM and
// excludes NEL. Rust's Unicode White_Space property has the opposite treatment.
fn form_whitespace(value: char) -> bool {
    matches!(value, '\u{9}'..='\u{d}' | '\u{20}' | '\u{a0}' | '\u{1680}'
        | '\u{2000}'..='\u{200a}' | '\u{2028}' | '\u{2029}' | '\u{202f}'
        | '\u{205f}' | '\u{3000}' | '\u{feff}')
}

fn url_valid(text: &str) -> bool {
    if text.encode_utf16().count() > 2048 || text.chars().any(form_whitespace) {
        return false;
    }
    let normalized;
    let absolute = if text.starts_with("//") {
        normalized = format!("https:{text}");
        normalized.as_str()
    } else if text
        .get(..4)
        .is_some_and(|prefix| prefix.eq_ignore_ascii_case("www."))
    {
        normalized = format!("https://{text}");
        normalized.as_str()
    } else {
        let Some((scheme, _)) = text.split_once("://") else {
            return false;
        };
        if scheme.is_empty() || !scheme.bytes().all(|byte| byte.is_ascii_alphabetic()) {
            return false;
        }
        text
    };
    let Ok(parsed) = url::Url::parse(absolute) else {
        return false;
    };
    match parsed.host() {
        Some(url::Host::Ipv4(_) | url::Host::Ipv6(_)) => true,
        Some(url::Host::Domain(host)) => {
            use std::sync::LazyLock;
            static DOMAIN: LazyLock<Regex> = LazyLock::new(|| {
                Regex::new(r"(?i)^(?:[a-z\u{a1}-\u{10ffff}0-9][-_]*)*[a-z\u{a1}-\u{10ffff}0-9]+(?:\.(?:[a-z\u{a1}-\u{10ffff}0-9]-*)*[a-z\u{a1}-\u{10ffff}0-9]+)*\.[a-z\u{a1}-\u{10ffff}]{2,}$").expect("valid form URL host expression")
            });
            host == "localhost" || DOMAIN.is_match(host)
        }
        None => false,
    }
}
fn tel_valid(text: &str) -> bool {
    use std::sync::LazyLock;
    static TEL: LazyLock<Regex> = LazyLock::new(|| {
        Regex::new(r"^(\+[0-9]{1,3}[-\s\u{2011}]?)?(\([0-9]{1,4}\)[-\s\u{2011}]?)?([0-9]+[-\s\u{2011}]?)*[0-9]+$").expect("valid telephone expression")
    });
    text.encode_utf16().count() <= 32 && TEL.is_match(text)
}

type IssueFuture = Pin<Box<dyn Future<Output = Vec<FormRuleIssue>>>>;
pub(crate) async fn collect_rule_issues(
    results: Vec<(bool, NamePath, FormRuleResult)>,
    mode: RuleMode,
) -> Vec<FormRuleIssue> {
    let mut pending = results
        .into_iter()
        .map(|(warning, name, result)| {
            Some(Box::pin(result.into_issues(name, warning)) as IssueFuture)
        })
        .collect::<Vec<_>>();
    let mut output = vec![None; pending.len()];
    poll_fn(|cx| {
        for (index, future) in pending.iter_mut().enumerate() {
            if let Some(current) = future
                && let Poll::Ready(issues) = current.as_mut().poll(cx)
            {
                *future = None;
                if mode == RuleMode::ParallelFirst && !issues.is_empty() {
                    return Poll::Ready(issues);
                }
                output[index] = Some(issues);
            }
        }
        if output.iter().all(Option::is_some) {
            Poll::Ready(
                output
                    .iter_mut()
                    .filter_map(Option::take)
                    .flatten()
                    .collect(),
            )
        } else {
            Poll::Pending
        }
    })
    .await
}
