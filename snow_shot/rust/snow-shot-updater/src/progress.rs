//! Cosmetic updater progress. Installation never depends on the desktop UI.
use serde::{Deserialize, Serialize};

/// Opaque colors and typography resolved from the application's Ant Design Qt theme.
/// The native presenter has no Qt dependency and keeps the same appearance after handoff.
#[derive(Clone, Debug, Deserialize, Serialize, PartialEq, Eq)]
#[serde(rename_all = "camelCase", default)]
pub struct ProgressAppearance {
    pub background: u32,
    pub border: u32,
    pub text: u32,
    pub text_secondary: u32,
    pub text_tertiary: u32,
    pub fill_secondary: u32,
    pub fill_tertiary: u32,
    pub primary: u32,
    pub primary_background: u32,
    pub success: u32,
    pub error: u32,
    pub font_family: String,
    pub font_size: u32,
    pub small_font_size: u32,
    pub border_radius: u32,
    pub motion: bool,
}

impl Default for ProgressAppearance {
    fn default() -> Self {
        Self {
            background: 0xffffff,
            border: 0xf0f0f0,
            text: 0x1f1f1f,
            text_secondary: 0x595959,
            text_tertiary: 0x8c8c8c,
            fill_secondary: 0xf0f0f0,
            fill_tertiary: 0xf5f5f5,
            primary: 0x1677ff,
            primary_background: 0xe6f4ff,
            success: 0x52c41a,
            error: 0xff4d4f,
            font_family: "Segoe UI".to_owned(),
            font_size: 14,
            small_font_size: 12,
            border_radius: 8,
            motion: true,
        }
    }
}

impl ProgressAppearance {
    pub fn normalized(mut self) -> Self {
        let defaults = Self::default();
        for (color, fallback) in [
            (&mut self.background, defaults.background),
            (&mut self.border, defaults.border),
            (&mut self.text, defaults.text),
            (&mut self.text_secondary, defaults.text_secondary),
            (&mut self.text_tertiary, defaults.text_tertiary),
            (&mut self.fill_secondary, defaults.fill_secondary),
            (&mut self.fill_tertiary, defaults.fill_tertiary),
            (&mut self.primary, defaults.primary),
            (&mut self.primary_background, defaults.primary_background),
            (&mut self.success, defaults.success),
            (&mut self.error, defaults.error),
        ] {
            if *color > 0xffffff {
                *color = fallback;
            }
        }
        self.font_family = self
            .font_family
            .trim()
            .chars()
            .filter(|character| !character.is_control())
            .take(63)
            .collect();
        if self.font_family.is_empty() {
            self.font_family = defaults.font_family;
        }
        self.font_size = self.font_size.clamp(10, 24);
        self.small_font_size = self.small_font_size.clamp(9, 20);
        self.border_radius = self.border_radius.min(16);
        self
    }
}

#[derive(Clone, Copy, Debug, Deserialize, Serialize, PartialEq, Eq)]
#[serde(rename_all = "snake_case")]
pub enum ProgressPhase {
    Downloading,
    Verifying,
    Preparing,
    Waiting,
    Extracting,
    BackingUp,
    Installing,
    Probing,
    Restoring,
    Complete,
    Ready,
    Failed,
}

#[derive(Clone, Copy, Debug, Deserialize, Serialize, PartialEq, Eq)]
pub struct Progress {
    pub phase: ProgressPhase,
    pub completed: u64,
    pub total: u64,
}

impl Progress {
    pub fn new(phase: ProgressPhase, completed: u64, total: u64) -> Self {
        Self {
            phase,
            completed,
            total,
        }
    }

    /// A zero total means that the operation cannot provide a meaningful percentage.
    #[cfg(any(windows, test))]
    fn permille(self) -> Option<u32> {
        (self.total > 0).then(|| {
            ((u128::from(self.completed.min(self.total)) * 1000) / u128::from(self.total)) as u32
        })
    }
}

#[derive(Clone, Debug, Deserialize, Serialize)]
#[serde(rename_all = "camelCase", default)]
pub struct ProgressTexts {
    pub title: String,
    pub downloading: String,
    pub verifying: String,
    pub preparing: String,
    pub waiting: String,
    pub extracting: String,
    pub backing_up: String,
    pub installing: String,
    pub probing: String,
    pub restoring: String,
    pub complete: String,
    pub ready: String,
    pub failed: String,
    pub bytes: String,
    pub files: String,
}

impl Default for ProgressTexts {
    fn default() -> Self {
        Self {
            title: format!("Updating {}", crate::edition::PRODUCT_NAME),
            downloading: "Downloading update".to_owned(),
            verifying: "Verifying update".to_owned(),
            preparing: "Preparing update".to_owned(),
            waiting: format!("Waiting for {} to close", crate::edition::PRODUCT_NAME),
            extracting: "Extracting update".to_owned(),
            backing_up: "Backing up application files".to_owned(),
            installing: "Installing update".to_owned(),
            probing: "Checking the updated application".to_owned(),
            restoring: "Restoring the previous version".to_owned(),
            complete: "Update complete".to_owned(),
            ready: "Update ready".to_owned(),
            failed: "Update failed".to_owned(),
            bytes: "%1 / %2 bytes".to_owned(),
            files: "%1 / %2 files".to_owned(),
        }
    }
}

#[cfg(any(windows, test))]
impl ProgressTexts {
    fn phase(&self, phase: ProgressPhase) -> &str {
        match phase {
            ProgressPhase::Downloading => &self.downloading,
            ProgressPhase::Verifying => &self.verifying,
            ProgressPhase::Preparing => &self.preparing,
            ProgressPhase::Waiting => &self.waiting,
            ProgressPhase::Extracting => &self.extracting,
            ProgressPhase::BackingUp => &self.backing_up,
            ProgressPhase::Installing => &self.installing,
            ProgressPhase::Probing => &self.probing,
            ProgressPhase::Restoring => &self.restoring,
            ProgressPhase::Complete => &self.complete,
            ProgressPhase::Ready => &self.ready,
            ProgressPhase::Failed => &self.failed,
        }
    }

    fn counts(&self, progress: Progress) -> Option<String> {
        let template = match progress.phase {
            ProgressPhase::Downloading | ProgressPhase::Extracting => &self.bytes,
            ProgressPhase::BackingUp | ProgressPhase::Installing | ProgressPhase::Restoring => {
                &self.files
            }
            _ => return None,
        };
        (progress.total > 0).then(|| {
            template
                .replace("%1", &progress.completed.min(progress.total).to_string())
                .replace("%2", &progress.total.to_string())
        })
    }
}

#[cfg(any(windows, test))]
#[derive(Default)]
struct Visibility {
    requested: bool,
    dismissed: bool,
    finished: bool,
}

#[cfg(any(windows, test))]
impl Visibility {
    fn visible(&self) -> bool {
        self.requested && !self.dismissed && !self.finished
    }

    fn request(&mut self, visible: bool) {
        self.requested = visible;
    }

    fn dismiss(&mut self) {
        self.dismissed = true;
    }

    fn finish(&mut self) {
        self.finished = true;
    }
}

#[cfg(any(windows, test))]
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
struct Bounds {
    left: i32,
    top: i32,
    right: i32,
    bottom: i32,
}

#[cfg(any(windows, test))]
fn scaled(dip: i32, dpi: u32) -> i32 {
    ((i64::from(dip) * i64::from(dpi.max(96)) + 48) / 96).clamp(0, i64::from(i32::MAX)) as i32
}

#[cfg(any(windows, test))]
fn window_bounds(work: Bounds, dpi: u32, content_height: i32) -> Bounds {
    let inset = scaled(16, dpi);
    let available_width = work.right.saturating_sub(work.left).max(1);
    let available_height = work.bottom.saturating_sub(work.top).max(1);
    let width = scaled(360, dpi).min(
        available_width
            .saturating_sub(inset.saturating_mul(2))
            .max(1),
    );
    let height = content_height.max(1).min(
        available_height
            .saturating_sub(inset.saturating_mul(2))
            .max(1),
    );
    let right = work
        .right
        .saturating_sub(inset)
        .max(work.left.saturating_add(width));
    let bottom = work
        .bottom
        .saturating_sub(inset)
        .max(work.top.saturating_add(height));
    Bounds {
        left: right - width,
        top: bottom - height,
        right,
        bottom,
    }
}

#[cfg(windows)]
mod native;

#[cfg(test)]
thread_local! {
    static CREATED_WINDOWS: std::cell::Cell<usize> = const { std::cell::Cell::new(0) };
}

/// Updates are asynchronous and best effort. Closing the window only hides it.
pub struct ProgressWindow {
    #[cfg(windows)]
    sender: Option<native::Sender>,
}

impl ProgressWindow {
    pub fn new(texts: ProgressTexts) -> Self {
        Self::new_with_appearance(texts, ProgressAppearance::default())
    }

    pub fn new_with_appearance(texts: ProgressTexts, appearance: ProgressAppearance) -> Self {
        #[cfg(test)]
        CREATED_WINDOWS.with(|count| count.set(count.get() + 1));
        let appearance = appearance.normalized();
        #[cfg(windows)]
        {
            Self {
                // Inline updater tests exercise policy and transactions without
                // opening windows on the user's desktop. Native fixtures below
                // explicitly create hidden HWNDs to validate window behavior.
                sender: if cfg!(test) {
                    None
                } else {
                    native::spawn(texts, appearance)
                },
            }
        }
        #[cfg(not(windows))]
        {
            let _ = (texts, appearance);
            Self {}
        }
    }

    #[cfg(test)]
    pub(crate) fn created_on_current_thread() -> usize {
        CREATED_WINDOWS.with(std::cell::Cell::get)
    }

    pub fn update(&self, progress: Progress) {
        #[cfg(windows)]
        if let Some(sender) = &self.sender {
            sender.send(native::Command::Update(progress));
        }
        #[cfg(not(windows))]
        let _ = progress;
    }

    pub fn set_visible(&self, visible: bool) {
        #[cfg(windows)]
        if let Some(sender) = &self.sender {
            sender.send(native::Command::Visible(visible));
        }
        #[cfg(not(windows))]
        let _ = visible;
    }

    pub fn finish(&self) {
        #[cfg(windows)]
        if let Some(sender) = &self.sender {
            sender.send(native::Command::Finish);
        }
    }
}

impl Drop for ProgressWindow {
    fn drop(&mut self) {
        self.finish();
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn appearance_defaults_match_ant_tokens_and_partial_themes_round_trip() {
        let defaults = ProgressAppearance::default();
        assert_eq!(defaults.background, 0xffffff);
        assert_eq!(defaults.text, 0x1f1f1f);
        assert_eq!(defaults.text_secondary, 0x595959);
        assert_eq!(defaults.primary, 0x1677ff);
        assert_eq!(defaults.font_size, 14);
        assert_eq!(defaults.small_font_size, 12);
        assert_eq!(defaults.border_radius, 8);
        let dark: ProgressAppearance =
            serde_json::from_str(r#"{"background":1315860,"text":14540253,"primary":1456548}"#)
                .unwrap();
        assert_eq!(dark.background, 0x141414);
        assert_eq!(dark.text, 0xdddddd);
        assert_eq!(dark.font_family, defaults.font_family);
        let serialized = serde_json::to_vec(&dark).unwrap();
        assert_eq!(
            serde_json::from_slice::<ProgressAppearance>(&serialized).unwrap(),
            dark
        );
    }

    #[test]
    fn cosmetic_values_have_bounded_colors_typography_and_control_free_font_names() {
        let appearance = ProgressAppearance {
            background: u32::MAX,
            font_family: "\0\n ".to_owned(),
            font_size: u32::MAX,
            small_font_size: 0,
            border_radius: u32::MAX,
            ..Default::default()
        }
        .normalized();
        assert_eq!(
            appearance.background,
            ProgressAppearance::default().background
        );
        assert_eq!(appearance.font_family, "Segoe UI");
        assert_eq!(appearance.font_size, 24);
        assert_eq!(appearance.small_font_size, 9);
        assert_eq!(appearance.border_radius, 16);
        let long_font = ProgressAppearance {
            font_family: "A".repeat(1000),
            ..Default::default()
        }
        .normalized();
        assert_eq!(long_font.font_family.len(), 63);
    }

    #[test]
    fn progress_ranges_are_bounded_and_overflow_safe() {
        assert_eq!(Progress::new(ProgressPhase::Waiting, 0, 0).permille(), None);
        assert_eq!(
            Progress::new(ProgressPhase::Downloading, 2, 8).permille(),
            Some(250)
        );
        assert_eq!(
            Progress::new(ProgressPhase::Installing, 12, 8).permille(),
            Some(1000)
        );
        assert_eq!(
            Progress::new(ProgressPhase::Extracting, u64::MAX, u64::MAX).permille(),
            Some(1000)
        );
    }

    #[test]
    fn placement_scales_and_respects_negative_origins_and_small_workareas() {
        let work = Bounds {
            left: -1920,
            top: -200,
            right: 0,
            bottom: 880,
        };
        assert_eq!(
            window_bounds(work, 96, 124),
            Bounds {
                left: -376,
                top: 740,
                right: -16,
                bottom: 864
            }
        );
        assert_eq!(
            window_bounds(work, 144, 186),
            Bounds {
                left: -564,
                top: 670,
                right: -24,
                bottom: 856
            }
        );
        let tiny = Bounds {
            left: 100,
            top: 20,
            right: 120,
            bottom: 40,
        };
        let bounds = window_bounds(tiny, 192, 248);
        assert!(bounds.left >= tiny.left && bounds.right <= tiny.right);
        assert!(bounds.top >= tiny.top && bounds.bottom <= tiny.bottom);
    }

    #[test]
    fn dismissal_and_completion_cannot_reopen_the_operation() {
        let mut state = Visibility::default();
        assert!(!state.visible());
        state.request(true);
        assert!(state.visible());
        state.dismiss();
        state.request(false);
        state.request(true);
        assert!(!state.visible());
        let mut finished = Visibility::default();
        finished.request(true);
        finished.finish();
        finished.request(true);
        assert!(!finished.visible());
    }

    #[test]
    fn texts_and_progress_have_stable_protocol_names() {
        let texts: ProgressTexts = serde_json::from_str(r#"{"backingUp":"Saving files"}"#).unwrap();
        assert_eq!(texts.phase(ProgressPhase::BackingUp), "Saving files");
        assert_eq!(texts.phase(ProgressPhase::Installing), "Installing update");
        assert_eq!(
            serde_json::to_value(Progress::new(ProgressPhase::BackingUp, 1, 4)).unwrap()["phase"],
            "backing_up"
        );
    }

    #[test]
    fn counts_use_phase_units_and_localized_placeholder_order() {
        let texts = ProgressTexts {
            bytes: "%2 字节中的 %1".to_owned(),
            files: "%1 / %2 個檔案".to_owned(),
            ..Default::default()
        };
        assert_eq!(
            texts
                .counts(Progress::new(ProgressPhase::Downloading, 100, 400))
                .as_deref(),
            Some("400 字节中的 100")
        );
        assert_eq!(
            texts
                .counts(Progress::new(ProgressPhase::Extracting, 100, 400))
                .as_deref(),
            Some("400 字节中的 100")
        );
        for phase in [
            ProgressPhase::BackingUp,
            ProgressPhase::Installing,
            ProgressPhase::Restoring,
        ] {
            assert_eq!(
                texts.counts(Progress::new(phase, 2, 8)).as_deref(),
                Some("2 / 8 個檔案")
            );
        }
        assert_eq!(
            texts.counts(Progress::new(ProgressPhase::Verifying, 8, 8)),
            None
        );
        assert_eq!(
            texts.counts(Progress::new(ProgressPhase::Downloading, 0, 0)),
            None
        );
    }
}
