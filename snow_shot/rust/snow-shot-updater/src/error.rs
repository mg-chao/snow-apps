use serde::Serialize;
use std::borrow::Cow;
use std::fmt;

pub type Result<T> = std::result::Result<T, UpdateError>;

#[derive(Debug, Clone, Serialize)]
#[serde(rename_all = "camelCase")]
pub struct UpdateError {
    pub code: Cow<'static, str>,
    pub message: Cow<'static, str>,
    #[serde(skip_serializing_if = "Option::is_none")]
    pub detail: Option<String>,
}

impl UpdateError {
    pub const fn new(code: &'static str, message: &'static str) -> Self {
        Self {
            code: Cow::Borrowed(code),
            message: Cow::Borrowed(message),
            detail: None,
        }
    }

    pub fn detail(mut self, detail: impl fmt::Display) -> Self {
        let mut text = detail.to_string();
        if text.len() > 1024 {
            text.truncate(1024);
        }
        self.detail = Some(text);
        self
    }

    pub fn from_message(message: impl Into<String>) -> Self {
        let message = message.into();
        Self {
            code: Cow::Borrowed("legacy_error"),
            message: Cow::Owned(message),
            detail: None,
        }
    }
}

impl fmt::Display for UpdateError {
    fn fmt(&self, formatter: &mut fmt::Formatter<'_>) -> fmt::Result {
        formatter.write_str(&self.message)
    }
}

impl std::error::Error for UpdateError {}

pub fn io_error(
    code: &'static str,
    message: &'static str,
    error: impl fmt::Display,
) -> UpdateError {
    UpdateError::new(code, message).detail(error)
}

pub fn require(condition: bool, code: &'static str, message: &'static str) -> Result<()> {
    condition
        .then_some(())
        .ok_or_else(|| UpdateError::new(code, message))
}
