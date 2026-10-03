use crate::error::UpdateError;
use crate::progress::{ProgressAppearance, ProgressTexts};
use serde::{Deserialize, Serialize};

pub const PROTOCOL_VERSION: u32 = 2;
pub const MAX_FRAME_BYTES: usize = 64 * 1024;

#[derive(Default)]
pub struct FrameDecoder {
    buffer: Vec<u8>,
}

impl FrameDecoder {
    pub fn push(&mut self, bytes: &[u8]) -> crate::Result<Vec<Command>> {
        let mut commands = Vec::new();
        for &byte in bytes {
            if byte == b'\n' {
                if self.buffer.ends_with(b"\r") {
                    self.buffer.pop();
                }
                commands.push(parse_frame(&self.buffer)?);
                self.buffer.clear();
            } else {
                crate::error::require(
                    self.buffer.len() < MAX_FRAME_BYTES,
                    "protocol_frame_too_large",
                    "The update service sent an oversized protocol message",
                )?;
                self.buffer.push(byte);
            }
        }
        Ok(commands)
    }

    pub fn finish(&self) -> crate::Result<()> {
        crate::error::require(
            self.buffer.is_empty(),
            "protocol_message_invalid",
            "The update service protocol message is invalid",
        )
    }
}

#[derive(Clone, Debug, Deserialize)]
#[serde(rename_all = "camelCase")]
pub struct Command {
    pub protocol: u32,
    pub id: u64,
    pub command: String,
    #[serde(default)]
    pub operation: Option<String>,
    #[serde(default)]
    pub trigger: Option<String>,
    #[serde(default)]
    pub mode: Option<String>,
    #[serde(default)]
    pub system_proxy: Option<bool>,
    #[serde(default)]
    pub proceed: Option<bool>,
    #[serde(default)]
    pub reason: Option<String>,
    #[serde(default)]
    pub progress_texts: Option<ProgressTexts>,
    #[serde(default, deserialize_with = "deserialize_progress_appearance")]
    pub progress_appearance: Option<ProgressAppearance>,
    #[serde(default)]
    pub relaunch_arguments: Option<Vec<String>>,
}

pub(crate) fn deserialize_progress_appearance<'de, D>(
    deserializer: D,
) -> Result<Option<ProgressAppearance>, D::Error>
where
    D: serde::Deserializer<'de>,
{
    // Appearance is cosmetic. A malformed optional theme must never prevent an update.
    let value = serde_json::Value::deserialize(deserializer)?;
    if !value.is_object() {
        return Ok(None);
    }
    Ok(serde_json::from_value::<ProgressAppearance>(value)
        .ok()
        .map(ProgressAppearance::normalized))
}

pub fn validate_relaunch_arguments(arguments: &[String]) -> crate::Result<()> {
    let mut seen = std::collections::HashSet::new();
    crate::error::require(
        arguments.len() <= 3
            && arguments.iter().all(|argument| {
                matches!(
                    argument.as_str(),
                    "--autostart" | "--show-main-window" | "--skip-startup-update"
                ) && seen.insert(argument.as_str())
            })
            && !(seen.contains("--autostart") && seen.contains("--show-main-window")),
        "invalid_updater_argument",
        "Invalid updater command argument",
    )
}

#[derive(Clone, Debug, Default, Serialize)]
#[serde(rename_all = "camelCase")]
pub struct Status {
    pub state: String,
    #[serde(default, skip_serializing_if = "String::is_empty")]
    pub version: String,
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub error: Option<UpdateError>,
    #[serde(default)]
    pub received: u64,
    #[serde(default)]
    pub total: u64,
}

#[derive(Serialize)]
#[serde(rename_all = "camelCase")]
pub struct Event<'a> {
    pub protocol: u32,
    #[serde(rename = "type")]
    pub event_type: &'a str,
    #[serde(skip_serializing_if = "Option::is_none")]
    pub id: Option<u64>,
    #[serde(skip_serializing_if = "Option::is_none")]
    pub status: Option<&'a Status>,
    #[serde(skip_serializing_if = "Option::is_none")]
    pub error: Option<&'a UpdateError>,
    #[serde(skip_serializing_if = "Option::is_none")]
    pub updater_version: Option<&'a str>,
    #[serde(skip_serializing_if = "Option::is_none")]
    pub platform: Option<&'a str>,
    #[serde(skip_serializing_if = "Option::is_none")]
    pub capabilities: Option<&'a [&'a str]>,
}

pub fn parse_frame(line: &[u8]) -> crate::Result<Command> {
    crate::error::require(
        line.len() <= MAX_FRAME_BYTES,
        "protocol_frame_too_large",
        "The update service sent an oversized protocol message",
    )?;
    let command: Command = serde_json::from_slice(line).map_err(|error| {
        UpdateError::new(
            "protocol_message_invalid",
            "The update service protocol message is invalid",
        )
        .detail(error)
    })?;
    crate::error::require(
        command.protocol == PROTOCOL_VERSION,
        "protocol_version_unsupported",
        "The update service protocol version is unsupported",
    )?;
    Ok(command)
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn optional_appearance_keeps_protocol_compatible_and_cosmetic_errors_nonfatal() {
        let absent = parse_frame(br#"{"protocol":2,"id":1,"command":"execute"}"#).unwrap();
        assert!(absent.progress_appearance.is_none());
        let configured = parse_frame(
            br#"{"protocol":2,"id":2,"command":"configure","progressAppearance":{"background":1315860,"fontSize":14}}"#,
        )
        .unwrap();
        let appearance = configured.progress_appearance.unwrap();
        assert_eq!(appearance.background, 0x141414);
        assert_eq!(appearance.font_size, 14);
        for invalid in ["null", "[]", "\"bad\"", r#"{"fontSize":"bad"}"#] {
            let frame = format!(
                r#"{{"protocol":2,"id":3,"command":"execute","progressAppearance":{invalid}}}"#
            );
            assert!(
                parse_frame(frame.as_bytes())
                    .unwrap()
                    .progress_appearance
                    .is_none()
            );
        }
        let bounded = parse_frame(
            br#"{"protocol":2,"id":4,"command":"configure","progressAppearance":{"fontSize":4294967295,"borderRadius":4294967295}}"#,
        )
        .unwrap()
        .progress_appearance
        .unwrap();
        assert_eq!(bounded.font_size, 24);
        assert_eq!(bounded.border_radius, 16);
    }

    #[test]
    fn rejects_oversized_and_wrong_version_frames() {
        assert!(parse_frame(&vec![b'x'; MAX_FRAME_BYTES + 1]).is_err());
        assert!(parse_frame(br#"{"protocol":1,"id":1,"command":"execute"}"#).is_err());
    }

    #[test]
    fn decoder_accepts_partial_and_multiple_frames() {
        let mut decoder = FrameDecoder::default();
        assert!(
            decoder
                .push(br#"{"protocol":2,"id":1,"comm"#)
                .unwrap()
                .is_empty()
        );
        let commands = decoder
            .push(b"and\":\"execute\"}\r\n{\"protocol\":2,\"id\":2,\"command\":\"shutdown\"}\n")
            .unwrap();
        assert_eq!(commands.len(), 2);
        assert_eq!(commands[0].id, 1);
        assert_eq!(commands[1].command, "shutdown");
        decoder.finish().unwrap();
    }

    #[test]
    fn decoder_rejects_malformed_oversized_and_truncated_frames() {
        let mut malformed = FrameDecoder::default();
        assert!(malformed.push(b"not-json\n").is_err());

        let mut oversized = FrameDecoder::default();
        assert!(oversized.push(&vec![b'x'; MAX_FRAME_BYTES + 1]).is_err());

        let mut truncated = FrameDecoder::default();
        truncated
            .push(br#"{"protocol":2,"id":1,"command":"execute"}"#)
            .unwrap();
        assert!(truncated.finish().is_err());
    }

    #[test]
    fn relaunch_arguments_preserve_only_normal_launch_intent() {
        for arguments in [
            vec![],
            vec!["--show-main-window"],
            vec!["--autostart", "--skip-startup-update"],
            vec!["--skip-startup-update"],
        ] {
            assert!(
                validate_relaunch_arguments(
                    &arguments.into_iter().map(str::to_owned).collect::<Vec<_>>()
                )
                .is_ok()
            );
        }
        for arguments in [
            vec!["--update-probe"],
            vec!["--show-main-window", "--autostart"],
            vec!["--autostart", "--autostart"],
            vec!["--restart-helper", "some-pipe"],
        ] {
            assert!(
                validate_relaunch_arguments(
                    &arguments.into_iter().map(str::to_owned).collect::<Vec<_>>()
                )
                .is_err()
            );
        }
    }
}
