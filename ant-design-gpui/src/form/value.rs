// Copyright (C) 2026 mg-chao
// SPDX-License-Identifier: Apache-2.0

//! Lossless, structurally shared values and literal field paths.

use std::{any::Any, fmt, rc::Rc, sync::Arc};

use gpui::SharedString;
use imbl::{GenericOrdMap, GenericVector, shared_ptr::RcK};

use crate::input_number::{ExactDecimal, NumericValue};

#[derive(Clone, Debug, PartialEq, Eq, PartialOrd, Ord, Hash)]
pub enum NameSegment {
    Key(SharedString),
    Index(usize),
}

impl From<&str> for NameSegment {
    fn from(value: &str) -> Self {
        Self::Key(value.to_owned().into())
    }
}
impl From<String> for NameSegment {
    fn from(value: String) -> Self {
        Self::Key(value.into())
    }
}
impl From<SharedString> for NameSegment {
    fn from(value: SharedString) -> Self {
        Self::Key(value)
    }
}
impl From<usize> for NameSegment {
    fn from(value: usize) -> Self {
        Self::Index(value)
    }
}

/// Strings are literal keys: `"user.name"` is distinct from `["user", "name"]`.
#[derive(Clone, Debug, Default, PartialEq, Eq, PartialOrd, Ord, Hash)]
pub struct NamePath(Arc<[NameSegment]>);

impl NamePath {
    pub fn new(segments: impl IntoIterator<Item = impl Into<NameSegment>>) -> Self {
        Self(segments.into_iter().map(Into::into).collect())
    }
    pub fn root() -> Self {
        Self::default()
    }
    pub fn segments(&self) -> &[NameSegment] {
        &self.0
    }
    pub fn len(&self) -> usize {
        self.0.len()
    }
    pub fn is_empty(&self) -> bool {
        self.0.is_empty()
    }
    pub fn child(&self, segment: impl Into<NameSegment>) -> Self {
        let mut path = self.0.to_vec();
        path.push(segment.into());
        Self(path.into())
    }
    pub fn index(&self, index: usize) -> Self {
        self.child(index)
    }
    pub fn join(&self, suffix: &Self) -> Self {
        Self(self.0.iter().chain(suffix.0.iter()).cloned().collect())
    }
    pub fn starts_with(&self, prefix: &Self) -> bool {
        self.0.starts_with(&prefix.0)
    }
    pub fn intersects(&self, other: &Self) -> bool {
        self.starts_with(other) || other.starts_with(self)
    }
    pub fn parent(&self) -> Option<Self> {
        (!self.is_empty()).then(|| Self(self.0[..self.len() - 1].into()))
    }
    pub(crate) fn replace_prefix(&self, old: &Self, new: &Self) -> Self {
        if self.starts_with(old) {
            new.join(&Self(self.0[old.len()..].into()))
        } else {
            self.clone()
        }
    }
}
impl From<&str> for NamePath {
    fn from(value: &str) -> Self {
        Self::new([value])
    }
}
impl From<String> for NamePath {
    fn from(value: String) -> Self {
        Self::new([value])
    }
}
impl From<SharedString> for NamePath {
    fn from(value: SharedString) -> Self {
        Self::new([value])
    }
}
impl From<&NamePath> for NamePath {
    fn from(value: &NamePath) -> Self {
        value.clone()
    }
}
impl<T: Into<NameSegment>, const N: usize> From<[T; N]> for NamePath {
    fn from(value: [T; N]) -> Self {
        Self::new(value)
    }
}
impl From<Vec<NameSegment>> for NamePath {
    fn from(value: Vec<NameSegment>) -> Self {
        Self(value.into())
    }
}
impl fmt::Display for NamePath {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        for (i, segment) in self.0.iter().enumerate() {
            match segment {
                NameSegment::Key(key) => {
                    if i != 0 {
                        f.write_str(".")?;
                    }
                    f.write_str(key)?;
                }
                NameSegment::Index(index) => write!(f, "[{index}]")?,
            }
        }
        Ok(())
    }
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum FormValueKind {
    Undefined,
    Null,
    Bool,
    Number,
    Text,
    Array,
    Object,
    Native,
}

#[derive(Clone, Debug, PartialEq, Eq)]
pub struct FormTypeError {
    pub expected: &'static str,
    pub actual: &'static str,
}
impl fmt::Display for FormTypeError {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        write!(f, "expected {}, found {}", self.expected, self.actual)
    }
}
impl std::error::Error for FormTypeError {}

#[derive(Clone, Debug, PartialEq, Eq)]
pub struct FormPathError {
    pub path: NamePath,
    pub expected: &'static str,
    pub actual: &'static str,
}
impl fmt::Display for FormPathError {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        write!(
            f,
            "{}: expected {}, found {}",
            self.path, self.expected, self.actual
        )
    }
}
impl std::error::Error for FormPathError {}

trait ErasedValue {
    fn as_any(&self) -> &dyn Any;
    fn type_name(&self) -> &'static str;
    fn equals(&self, other: &dyn ErasedValue) -> bool;
}
struct NativeValue<T>(T);
impl<T: PartialEq + 'static> ErasedValue for NativeValue<T> {
    fn as_any(&self) -> &dyn Any {
        &self.0
    }
    fn type_name(&self) -> &'static str {
        std::any::type_name::<T>()
    }
    fn equals(&self, other: &dyn ErasedValue) -> bool {
        other
            .as_any()
            .downcast_ref::<T>()
            .is_some_and(|value| self.0 == *value)
    }
}

#[derive(Clone, Default, PartialEq)]
pub struct FormArray(GenericVector<FormValue, RcK>);
impl FormArray {
    pub fn new() -> Self {
        Self::default()
    }
    pub fn len(&self) -> usize {
        self.0.len()
    }
    pub fn is_empty(&self) -> bool {
        self.0.is_empty()
    }
    pub fn get(&self, index: usize) -> Option<&FormValue> {
        self.0.get(index)
    }
    pub fn iter(&self) -> impl DoubleEndedIterator<Item = &FormValue> + ExactSizeIterator {
        self.0.iter()
    }
    pub fn push(&mut self, value: impl Into<FormValue>) {
        self.0.push_back(value.into());
    }
    pub fn insert(&mut self, index: usize, value: FormValue) {
        self.0.insert(index.min(self.len()), value);
    }
    pub fn remove(&mut self, index: usize) -> Option<FormValue> {
        (index < self.len()).then(|| self.0.remove(index))
    }
    pub fn set(&mut self, index: usize, value: FormValue) {
        while self.len() <= index {
            self.push(FormValue::undefined());
        }
        self.0[index] = value;
    }
}
impl<T: Into<FormValue>> FromIterator<T> for FormArray {
    fn from_iter<I: IntoIterator<Item = T>>(iter: I) -> Self {
        Self(iter.into_iter().map(Into::into).collect())
    }
}
impl fmt::Debug for FormArray {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        f.debug_list().entries(self.iter()).finish()
    }
}

#[derive(Clone, Default, PartialEq)]
pub struct FormObject(GenericOrdMap<SharedString, FormValue, RcK>);
impl FormObject {
    pub fn new() -> Self {
        Self::default()
    }
    pub fn len(&self) -> usize {
        self.0.len()
    }
    pub fn is_empty(&self) -> bool {
        self.0.is_empty()
    }
    pub fn get(&self, key: &str) -> Option<&FormValue> {
        self.0.get(key)
    }
    pub fn iter(
        &self,
    ) -> impl DoubleEndedIterator<Item = (&SharedString, &FormValue)> + ExactSizeIterator {
        self.0.iter()
    }
    pub fn insert(
        &mut self,
        key: impl Into<SharedString>,
        value: impl Into<FormValue>,
    ) -> Option<FormValue> {
        self.0.insert(key.into(), value.into())
    }
    pub fn remove(&mut self, key: &str) -> Option<FormValue> {
        self.0.remove(key)
    }
}
impl<K: Into<SharedString>, V: Into<FormValue>> FromIterator<(K, V)> for FormObject {
    fn from_iter<I: IntoIterator<Item = (K, V)>>(iter: I) -> Self {
        Self(
            iter.into_iter()
                .map(|(k, v)| (k.into(), v.into()))
                .collect(),
        )
    }
}
impl fmt::Debug for FormObject {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        f.debug_map().entries(self.iter()).finish()
    }
}

enum ValueNode {
    Undefined,
    Null,
    Bool(bool),
    Number(NumericValue),
    Text(SharedString),
    Array(FormArray),
    Object(FormObject),
    Native(Box<dyn ErasedValue>),
}

/// An immutable value. Cloning even a large native value is constant time.
/// Native values must not be mutated through shared interior-mutable handles.
#[derive(Clone)]
pub struct FormValue(Rc<ValueNode>);
impl Default for FormValue {
    fn default() -> Self {
        Self::undefined()
    }
}
impl FormValue {
    pub fn undefined() -> Self {
        Self(Rc::new(ValueNode::Undefined))
    }
    pub fn null() -> Self {
        Self(Rc::new(ValueNode::Null))
    }
    pub fn text(value: impl Into<SharedString>) -> Self {
        Self(Rc::new(ValueNode::Text(value.into())))
    }
    pub fn number(value: impl Into<NumericValue>) -> Self {
        Self(Rc::new(ValueNode::Number(value.into())))
    }
    pub fn array(values: impl IntoIterator<Item = impl Into<FormValue>>) -> Self {
        Self::from(values.into_iter().collect::<FormArray>())
    }
    pub fn object(
        values: impl IntoIterator<Item = (impl Into<SharedString>, impl Into<FormValue>)>,
    ) -> Self {
        Self::from(values.into_iter().collect::<FormObject>())
    }
    pub fn native<T: Clone + PartialEq + 'static>(value: T) -> Self {
        Self(Rc::new(ValueNode::Native(Box::new(NativeValue(value)))))
    }
    pub fn kind(&self) -> FormValueKind {
        match &*self.0 {
            ValueNode::Undefined => FormValueKind::Undefined,
            ValueNode::Null => FormValueKind::Null,
            ValueNode::Bool(_) => FormValueKind::Bool,
            ValueNode::Number(_) => FormValueKind::Number,
            ValueNode::Text(_) => FormValueKind::Text,
            ValueNode::Array(_) => FormValueKind::Array,
            ValueNode::Object(_) => FormValueKind::Object,
            ValueNode::Native(_) => FormValueKind::Native,
        }
    }
    pub fn type_name(&self) -> &'static str {
        match &*self.0 {
            ValueNode::Undefined => "undefined",
            ValueNode::Null => "null",
            ValueNode::Bool(_) => "bool",
            ValueNode::Number(_) => "number",
            ValueNode::Text(_) => "text",
            ValueNode::Array(_) => "array",
            ValueNode::Object(_) => "object",
            ValueNode::Native(value) => value.type_name(),
        }
    }
    pub fn is_undefined(&self) -> bool {
        self.kind() == FormValueKind::Undefined
    }
    pub fn is_null(&self) -> bool {
        self.kind() == FormValueKind::Null
    }
    pub fn is_empty(&self) -> bool {
        match &*self.0 {
            ValueNode::Undefined | ValueNode::Null => true,
            ValueNode::Text(value) => value.is_empty(),
            ValueNode::Array(value) => value.is_empty(),
            _ => false,
        }
    }
    pub fn as_str(&self) -> Option<&str> {
        if let ValueNode::Text(v) = &*self.0 {
            Some(v)
        } else {
            None
        }
    }
    pub fn as_text(&self) -> Option<&SharedString> {
        if let ValueNode::Text(v) = &*self.0 {
            Some(v)
        } else {
            None
        }
    }
    pub fn as_bool(&self) -> Option<bool> {
        if let ValueNode::Bool(v) = &*self.0 {
            Some(*v)
        } else {
            None
        }
    }
    pub fn as_number(&self) -> Option<&NumericValue> {
        if let ValueNode::Number(v) = &*self.0 {
            Some(v)
        } else {
            None
        }
    }
    pub fn as_array(&self) -> Option<&FormArray> {
        if let ValueNode::Array(v) = &*self.0 {
            Some(v)
        } else {
            None
        }
    }
    pub fn as_object(&self) -> Option<&FormObject> {
        if let ValueNode::Object(v) = &*self.0 {
            Some(v)
        } else {
            None
        }
    }
    pub fn char_len(&self) -> Option<usize> {
        self.as_str().map(|value| value.chars().count())
    }
    pub fn native_ref<T: 'static>(&self) -> Result<&T, FormTypeError> {
        if let ValueNode::Native(value) = &*self.0
            && let Some(value) = value.as_any().downcast_ref()
        {
            return Ok(value);
        }
        Err(FormTypeError {
            expected: std::any::type_name::<T>(),
            actual: self.type_name(),
        })
    }
    pub fn ptr_eq(&self, other: &Self) -> bool {
        Rc::ptr_eq(&self.0, &other.0)
    }
    pub fn get(&self, path: &NamePath) -> Option<&Self> {
        let mut value = self;
        for segment in path.segments() {
            value = match segment {
                NameSegment::Key(key) => value.as_object()?.get(key)?,
                NameSegment::Index(index) => value.as_array()?.get(*index)?,
            };
        }
        Some(value)
    }
    pub fn with_path(&self, path: &NamePath, value: Self) -> Result<Self, FormPathError> {
        fn update(
            current: &FormValue,
            tail: &[NameSegment],
            full: &NamePath,
            value: FormValue,
        ) -> Result<FormValue, FormPathError> {
            let Some((segment, tail)) = tail.split_first() else {
                return Ok(value);
            };
            match segment {
                NameSegment::Key(key) => {
                    let mut object = if current.is_undefined() {
                        FormObject::new()
                    } else {
                        current.as_object().cloned().ok_or_else(|| FormPathError {
                            path: full.clone(),
                            expected: "object",
                            actual: current.type_name(),
                        })?
                    };
                    let child = object.get(key).cloned().unwrap_or_default();
                    let value = update(&child, tail, full, value)?;
                    if value.is_undefined() {
                        object.remove(key);
                    } else {
                        object.insert(key.clone(), value);
                    }
                    Ok(object.into())
                }
                NameSegment::Index(index) => {
                    let mut array = if current.is_undefined() {
                        FormArray::new()
                    } else {
                        current.as_array().cloned().ok_or_else(|| FormPathError {
                            path: full.clone(),
                            expected: "array",
                            actual: current.type_name(),
                        })?
                    };
                    let child = array.get(*index).cloned().unwrap_or_default();
                    array.set(*index, update(&child, tail, full, value)?);
                    Ok(array.into())
                }
            }
        }
        update(self, path.segments(), path, value)
    }
    /// Object properties merge recursively; arrays and native leaves replace.
    pub fn merged(&self, patch: &Self) -> Self {
        match (self.as_object(), patch.as_object()) {
            (Some(old), Some(patch)) => {
                let mut object = old.clone();
                for (key, value) in patch.iter() {
                    let next = old
                        .get(key)
                        .map_or_else(|| value.clone(), |old| old.merged(value));
                    if next.is_undefined() {
                        object.remove(key);
                    } else {
                        object.insert(key.clone(), next);
                    }
                }
                object.into()
            }
            _ => patch.clone(),
        }
    }
}
impl PartialEq for FormValue {
    fn eq(&self, other: &Self) -> bool {
        if self.ptr_eq(other) {
            return true;
        }
        match (&*self.0, &*other.0) {
            (ValueNode::Undefined, ValueNode::Undefined) | (ValueNode::Null, ValueNode::Null) => {
                true
            }
            (ValueNode::Bool(a), ValueNode::Bool(b)) => a == b,
            (ValueNode::Number(a), ValueNode::Number(b)) => a == b,
            (ValueNode::Text(a), ValueNode::Text(b)) => a == b,
            (ValueNode::Array(a), ValueNode::Array(b)) => a == b,
            (ValueNode::Object(a), ValueNode::Object(b)) => a == b,
            (ValueNode::Native(a), ValueNode::Native(b)) => a.equals(&**b),
            _ => false,
        }
    }
}
impl fmt::Debug for FormValue {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        match &*self.0 {
            ValueNode::Undefined => f.write_str("Undefined"),
            ValueNode::Null => f.write_str("Null"),
            ValueNode::Bool(v) => v.fmt(f),
            ValueNode::Number(v) => v.fmt(f),
            ValueNode::Text(v) => v.fmt(f),
            ValueNode::Array(v) => v.fmt(f),
            ValueNode::Object(v) => v.fmt(f),
            ValueNode::Native(v) => f.debug_tuple("Native").field(&v.type_name()).finish(),
        }
    }
}
impl From<bool> for FormValue {
    fn from(value: bool) -> Self {
        Self(Rc::new(ValueNode::Bool(value)))
    }
}
impl From<SharedString> for FormValue {
    fn from(value: SharedString) -> Self {
        Self::text(value)
    }
}
impl From<String> for FormValue {
    fn from(value: String) -> Self {
        Self::text(value)
    }
}
impl From<&str> for FormValue {
    fn from(value: &str) -> Self {
        Self::text(value.to_owned())
    }
}
impl From<NumericValue> for FormValue {
    fn from(value: NumericValue) -> Self {
        Self::number(value)
    }
}
impl From<ExactDecimal> for FormValue {
    fn from(value: ExactDecimal) -> Self {
        Self::number(value)
    }
}
impl From<f64> for FormValue {
    fn from(value: f64) -> Self {
        Self::number(value)
    }
}
impl From<FormArray> for FormValue {
    fn from(value: FormArray) -> Self {
        Self(Rc::new(ValueNode::Array(value)))
    }
}
impl From<FormObject> for FormValue {
    fn from(value: FormObject) -> Self {
        Self(Rc::new(ValueNode::Object(value)))
    }
}

#[derive(Clone, Debug, PartialEq)]
pub enum FormPresence<T> {
    Unset,
    Null,
    Value(T),
}
impl<T> FormPresence<T> {
    pub fn value(self) -> Option<T> {
        if let Self::Value(value) = self {
            Some(value)
        } else {
            None
        }
    }
}

type Encoder<T> = dyn Fn(T) -> FormValue;
type Decoder<T> = dyn Fn(&FormValue) -> Result<T, FormTypeError>;
pub struct FormCodec<T> {
    encode: Rc<Encoder<T>>,
    decode: Rc<Decoder<T>>,
}
impl<T> Clone for FormCodec<T> {
    fn clone(&self) -> Self {
        Self {
            encode: self.encode.clone(),
            decode: self.decode.clone(),
        }
    }
}
impl<T: 'static> FormCodec<T> {
    pub fn new(
        encode: impl Fn(T) -> FormValue + 'static,
        decode: impl Fn(&FormValue) -> Result<T, FormTypeError> + 'static,
    ) -> Self {
        Self {
            encode: Rc::new(encode),
            decode: Rc::new(decode),
        }
    }
    pub fn encode(&self, value: T) -> FormValue {
        (self.encode)(value)
    }
    pub fn decode(&self, value: &FormValue) -> Result<T, FormTypeError> {
        (self.decode)(value)
    }
    pub fn encode_presence(&self, value: FormPresence<T>) -> FormValue {
        match value {
            FormPresence::Unset => FormValue::undefined(),
            FormPresence::Null => FormValue::null(),
            FormPresence::Value(value) => self.encode(value),
        }
    }
    pub fn decode_presence(
        &self,
        value: Option<&FormValue>,
    ) -> Result<FormPresence<T>, FormTypeError> {
        match value {
            None => Ok(FormPresence::Unset),
            Some(value) if value.is_undefined() => Ok(FormPresence::Unset),
            Some(value) if value.is_null() => Ok(FormPresence::Null),
            Some(value) => self.decode(value).map(FormPresence::Value),
        }
    }
    pub fn array(self) -> FormCodec<Vec<T>> {
        let encoder = self.clone();
        FormCodec::new(
            move |values: Vec<T>| FormValue::array(values.into_iter().map(|v| encoder.encode(v))),
            move |value| {
                value
                    .as_array()
                    .ok_or(FormTypeError {
                        expected: "array",
                        actual: value.type_name(),
                    })?
                    .iter()
                    .map(|v| self.decode(v))
                    .collect()
            },
        )
    }
}
impl<T: Clone + PartialEq + 'static> FormCodec<T> {
    pub fn native() -> Self {
        Self::new(FormValue::native, |value| value.native_ref::<T>().cloned())
    }
}
impl FormCodec<FormValue> {
    pub fn value() -> Self {
        Self::new(|value| value, |value| Ok(value.clone()))
    }
}
impl FormCodec<SharedString> {
    pub fn text() -> Self {
        Self::new(FormValue::text, |value| {
            value.as_text().cloned().ok_or(FormTypeError {
                expected: "text",
                actual: value.type_name(),
            })
        })
    }
}
impl FormCodec<bool> {
    pub fn boolean() -> Self {
        Self::new(FormValue::from, |value| {
            value.as_bool().ok_or(FormTypeError {
                expected: "bool",
                actual: value.type_name(),
            })
        })
    }
}
impl FormCodec<NumericValue> {
    pub fn numeric() -> Self {
        Self::new(FormValue::number, |value| {
            value.as_number().cloned().ok_or(FormTypeError {
                expected: "number",
                actual: value.type_name(),
            })
        })
    }
}

/// A constant-time clone of values and the field-name index at one revision.
#[derive(Clone, Debug)]
pub struct FormSnapshot {
    pub(crate) form_id: u64,
    pub(crate) revision: u64,
    pub(crate) root: FormValue,
    pub(crate) field_paths: GenericOrdMap<u64, NamePath, RcK>,
}
impl FormSnapshot {
    pub fn empty() -> Self {
        Self {
            form_id: 0,
            revision: 0,
            root: FormObject::new().into(),
            field_paths: GenericOrdMap::new(),
        }
    }
    pub fn get(&self, path: &NamePath) -> Option<&FormValue> {
        self.root.get(path)
    }
    pub fn values(&self) -> &FormValue {
        &self.root
    }
    pub fn revision(&self) -> u64 {
        self.revision
    }
    pub fn form_id(&self) -> u64 {
        self.form_id
    }
}
