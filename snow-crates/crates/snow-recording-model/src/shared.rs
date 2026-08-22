use serde::{Deserialize, Serialize};

#[derive(Clone, Copy, Debug, Default, Serialize, Deserialize, PartialEq, Eq)]
pub enum IntermediateRecordingProfile {
    #[default]
    EditFast,
}

#[derive(Clone, Copy, Debug, Default, Serialize, Deserialize, PartialEq, Eq)]
pub enum VideoEncodingSpeed {
    UltraFast,
    SuperFast,
    VeryFast,
    Faster,
    Fast,
    #[default]
    Medium,
    Slow,
    Slower,
    VerySlow,
    Placebo,
}

impl VideoEncodingSpeed {
    pub const fn as_x264_preset(self) -> &'static str {
        match self {
            Self::UltraFast => "ultrafast",
            Self::SuperFast => "superfast",
            Self::VeryFast => "veryfast",
            Self::Faster => "faster",
            Self::Fast => "fast",
            Self::Medium => "medium",
            Self::Slow => "slow",
            Self::Slower => "slower",
            Self::VerySlow => "veryslow",
            Self::Placebo => "placebo",
        }
    }
}

#[derive(Clone, Copy, Debug, Default, Serialize, Deserialize, PartialEq, Eq)]
pub enum VideoCodec {
    #[default]
    H264,
    H265,
}

#[derive(Clone, Copy, Debug, Serialize, Deserialize, PartialEq, Eq)]
pub struct VideoEncodeConfig {
    pub quality: u8,
    pub speed: VideoEncodingSpeed,
}

impl Default for VideoEncodeConfig {
    fn default() -> Self {
        Self {
            quality: 80,
            speed: VideoEncodingSpeed::Medium,
        }
    }
}

impl VideoEncodeConfig {
    pub fn validate(&self, prefix: &str) -> Result<(), String> {
        if self.quality > 100 {
            return Err(format!("{prefix}.quality must be in 0..=100"));
        }

        Ok(())
    }
}
