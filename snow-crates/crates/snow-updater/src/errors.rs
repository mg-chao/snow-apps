//! Coded English diagnostics shared with the application's translation catalog.
//!
//! The helper sends these exact strings over the update pipes and in
//! `result.txt`; the receiving application looks them up in the `UpdateErrors`
//! translation context (`snow_shot/src/update/updateerrors.cpp`). A focused
//! consistency test asserts that every message here exists in that catalog, so
//! the two lists cannot drift apart. Diagnostics that were never catalogued
//! (for example Windows error codes) travel as raw text and simply display
//! untranslated, exactly as before.

use std::fmt;

/// Canonical helper-side diagnostics. Index + 1 is the stable C FFI code.
pub const MESSAGES: &[&str] = &[
    "Invalid semantic version",
    "Invalid update file size",
    "Invalid update checksum",
    "Invalid update signature encoding",
    "Invalid release public key",
    "Update file could not be read or exceeds its size limit",
    "Could not read update file",
    "Could not save update state",
    "Could not persist update state",
    "Could not commit update state",
    "Could not read update payload",
    "Could not hash update payload",
    "Update payload size or checksum does not match the signed release",
    "Invalid update file inventory",
    "Unsafe update file inventory",
    "Update payload is too large",
    "Conflicting update file paths",
    "Update metadata is too large",
    "Unsupported update signature schema",
    "Release signature is invalid or its signing key is not trusted",
    "Unsupported update release",
    "The release must contain all five Windows packages",
    "Unknown update package variant",
    "Unexpected update package URL",
    "Incomplete update archive",
    "No update package matches this installation",
    "Update paths must not contain symbolic links",
    "Update paths must not contain reparse points",
    "Invalid Snow Shot installation root",
    "Could not create update destination",
    "Could not stage an update file",
    "Could not clear update staging directory",
    "Could not update the registered application version",
    "Could not create update archive reader",
    "Could not open update archive",
    "Invalid update archive entry",
    "Unexpected, duplicate, or unsafe update archive entry",
    "Could not create update staging directory",
    "Could not extract update archive entry",
    "Corrupt update archive entry",
    "Could not write update archive entry",
    "Corrupt update archive checksum",
    "Unsupported update recovery journal",
    "Could not finalize committed update state",
    "Could not remove an incomplete update file",
    "Could not finalize update recovery",
    "This copy does not have valid Snow Shot installation metadata",
    "Refusing to change a file outside the application payload",
    "An update file would overwrite the selected data directory",
    "Another update transaction is running",
    "Could not create update work directory",
    "The update must be newer than the installed release",
    "Not enough free space to stage and recover this update",
    "Update installation metadata does not match the signed release",
    "An update file conflicts with an existing user file",
    "An update file conflicts with a directory",
    "Could not back up the current application",
    "Could not remove obsolete application file",
    "The new application failed its startup check; restoring the previous version",
    "Could not create uninstall lock directory",
    "An update is still running",
    "Could not remove an owned application file",
    "Could not create release audit directory",
    "Release archive installation metadata mismatch",
    "The packaged application failed its isolated startup probe",
    "Could not replace an update file; close applications using it",
    "The update coordinator identity could not be verified",
    "Could not verify the running update coordinator",
    "Invalid previous installation path",
    "Missing updater argument",
    "Could not contact the update coordinator",
    "Could not send updater status",
    "The update handoff was not acknowledged",
    "Invalid updater command argument",
    "Elevation requires a registered Snow Shot installation",
    "Could not open the application process",
    "The update target does not match its application process",
    "The release is not newer than this installation",
    "Could not stage the verified update package",
    "The application cancelled the update handoff",
    "The application did not exit; the update was cancelled",
    "Could not create updater coordinator",
    "Could not launch the installed update helper",
    "Missing updater operation",
    "Could not create updater coordinator directory",
    "Could not copy updater coordinator",
    "Could not launch updater coordinator",
    "Update permission was declined or could not be obtained",
    "Could not create update worker directory",
    "Could not launch update worker",
    "Elevation is unavailable on this platform",
    "Unknown updater operation",
    "The update helper timed out",
    "Application coordinator disconnected",
    "Application cancelled the update handoff",
];

/// Returns the catalog diagnostic for a C FFI error code (1-based).
pub fn message_for_code(code: u32) -> Option<&'static str> {
    if code == 0 {
        return None;
    }
    MESSAGES.get(code as usize - 1).copied()
}

/// Number of valid FFI error codes.
pub fn message_count() -> u32 {
    MESSAGES.len() as u32
}

/// Helper failure carrying a catalog diagnostic or raw pass-through text.
#[derive(Clone, Debug)]
pub struct Error {
    text: String,
    code: u32,
}

impl Error {
    /// A fixed catalog diagnostic. Unknown strings still carry their text but
    /// lose their code; the catalog consistency test rejects that case.
    pub fn fixed(text: &'static str) -> Self {
        let code = MESSAGES
            .iter()
            .position(|m| *m == text)
            .map_or(0, |i| i as u32 + 1);
        Error {
            text: text.to_owned(),
            code,
        }
    }

    /// Uncatalogued diagnostic text (for example a formatted Windows error).
    pub fn raw(text: impl Into<String>) -> Self {
        Error {
            text: text.into(),
            code: 0,
        }
    }

    pub fn message(&self) -> &str {
        &self.text
    }

    pub fn code(&self) -> u32 {
        self.code
    }
}

impl fmt::Display for Error {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        f.write_str(&self.text)
    }
}

impl std::error::Error for Error {}

pub type Result<T> = std::result::Result<T, Error>;

/// The `requireUpdate` equivalent: fail with a fixed catalog diagnostic.
pub fn require(condition: bool, message: &'static str) -> Result<()> {
    if condition {
        Ok(())
    } else {
        Err(Error::fixed(message))
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn messages_are_unique_and_code_round_trips() {
        let mut sorted = MESSAGES.to_vec();
        sorted.sort_unstable();
        let unique = sorted.windows(2).all(|pair| pair[0] != pair[1]);
        assert!(unique, "diagnostic table must not repeat messages");
        for (index, message) in MESSAGES.iter().enumerate() {
            assert_eq!(message_for_code(index as u32 + 1), Some(*message));
        }
        assert_eq!(message_for_code(0), None);
        assert_eq!(message_for_code(MESSAGES.len() as u32 + 1), None);
        assert_eq!(message_count() as usize, MESSAGES.len());
    }

    #[test]
    fn fixed_errors_carry_codes_and_raw_errors_do_not() {
        assert_eq!(Error::fixed("Invalid semantic version").code(), 1);
        assert_eq!(Error::raw("Windows error 0x5").code(), 0);
        let unknown = Error::fixed("Not a catalog string");
        assert_eq!(unknown.code(), 0);
        assert_eq!(unknown.message(), "Not a catalog string");
    }
}
