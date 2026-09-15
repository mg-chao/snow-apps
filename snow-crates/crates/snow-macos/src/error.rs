use objc2_foundation::NSError;
#[derive(Clone, Debug, thiserror::Error)]
pub enum MacError {
    #[error("screen recording permission is required")]
    PermissionDenied,
    #[error("microphone permission is required")]
    MicrophonePermissionDenied,
    #[error("input monitoring permission is required")]
    InputPermissionDenied,
    #[error("macOS 15 or later is required")]
    UnsupportedOs,
    #[error("capture target is unavailable")]
    TargetUnavailable,
    #[error("unsupported capture capability: {0}")]
    Unsupported(String),
    #[error("invalid capture configuration: {0}")]
    InvalidConfig(String),
    #[error("capture operation timed out")]
    Timeout,
    #[error("capture operation was canceled")]
    Canceled,
    #[error("capture source is inactive")]
    Inactive,
    #[error("{domain} ({code}): {message}")]
    Native {
        domain: String,
        code: isize,
        message: String,
    },
}
impl MacError {
    pub(crate) fn from_native(error: &NSError) -> Self {
        let domain = error.domain().to_string();
        let code = error.code();
        if domain == unsafe { objc2_screen_capture_kit::SCStreamErrorDomain }.to_string() {
            use objc2_screen_capture_kit::SCStreamErrorCode as Code;
            match Code(code) {
                Code::UserDeclined | Code::MissingEntitlements => Self::PermissionDenied,
                Code::NoWindowList | Code::NoDisplayList | Code::NoCaptureSource => {
                    Self::TargetUnavailable
                }
                Code::UserStopped => Self::Canceled,
                Code::FailedApplicationConnectionInterrupted | Code::SystemStoppedStream => {
                    Self::Inactive
                }
                _ => Self::Native {
                    domain,
                    code,
                    message: error.localizedDescription().to_string(),
                },
            }
        } else {
            Self::Native {
                domain,
                code,
                message: error.localizedDescription().to_string(),
            }
        }
    }
}
pub type MacResult<T> = Result<T, MacError>;

impl From<snow_core::cancellation::WaitError> for MacError {
    fn from(error: snow_core::cancellation::WaitError) -> Self {
        match error {
            snow_core::cancellation::WaitError::Canceled => Self::Canceled,
            snow_core::cancellation::WaitError::Timeout => Self::Timeout,
            snow_core::cancellation::WaitError::Disconnected => Self::Native {
                domain: "snow.native.callback".into(),
                code: 1,
                message: "native callback ended without completing the operation".into(),
            },
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    #[test]
    fn missing_completion_is_not_reported_as_user_cancellation() {
        use snow_core::cancellation::WaitError;
        assert!(matches!(
            MacError::from(WaitError::Disconnected),
            MacError::Native { .. }
        ));
        assert!(matches!(
            MacError::from(WaitError::Canceled),
            MacError::Canceled
        ));
        assert!(matches!(
            MacError::from(WaitError::Timeout),
            MacError::Timeout
        ));
    }
}
