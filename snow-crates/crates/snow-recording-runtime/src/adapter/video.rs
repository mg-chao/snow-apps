use crate::config::RecordingTarget;
use crate::error::{Result, ScreenRecorderError};

/// Resolve a `MonitorSelector` against a set of known monitors.
/// Returns the matching `MonitorId` or an error if no match is found.
///
/// This is extracted from `resolve_capture_target` for testability.
pub(crate) fn resolve_monitor_selector(
    selector: &crate::config::MonitorSelector,
    monitors: &[snow_capture::region::MonitorGeometry],
) -> Result<snow_capture::MonitorId> {
    monitors
        .iter()
        .find(|m| m.monitor.stable_id() == selector.stable_id)
        .map(|m| m.monitor.clone())
        .ok_or_else(|| {
            ScreenRecorderError::Capture(snow_capture::error::CaptureError::InvalidConfig(format!(
                "monitor with stable_id '{}' not found or disconnected",
                selector.stable_id
            )))
        })
}

pub(crate) fn resolve_capture_target(
    target: &RecordingTarget,
) -> Result<snow_capture::CaptureTarget> {
    match target {
        RecordingTarget::PrimaryMonitor => Ok(snow_capture::CaptureTarget::PrimaryMonitor),
        RecordingTarget::Monitor(selector) => {
            let layout = snow_capture::MonitorLayout::snapshot()?;
            let monitor = resolve_monitor_selector(selector, &layout.monitors)?;
            Ok(snow_capture::CaptureTarget::Monitor(monitor))
        }
        RecordingTarget::Window(selector) => Ok(snow_capture::CaptureTarget::Window(*selector)),
        RecordingTarget::Region(region) => {
            let capture_region =
                snow_capture::CaptureRegion::new(region.x, region.y, region.width, region.height)?;
            Ok(snow_capture::CaptureTarget::Region(capture_region))
        }
    }
}

#[cfg(test)]
mod tests {
    use proptest::prelude::*;

    proptest! {
        #[test]
        fn prop_monitor_selector_resolution(
            adapter_luid in any::<u64>(),
            output_id in any::<u64>(),
            extra_monitors in 0usize..=3,
        ) {
            use crate::config::MonitorSelector;
            use super::resolve_monitor_selector;
            use snow_capture::region::MonitorGeometry;

            let target_stable_id = format!("{:016x}-{:016x}", adapter_luid, output_id);
            let target_monitor = snow_capture::MonitorId::from_parts(
                adapter_luid, output_id, 0, "Test Monitor", false,
            );
            let target_geo = MonitorGeometry {
                monitor: target_monitor.clone(),
                x: 0, y: 0, width: 1920, height: 1080,
            };

            let mut monitors = vec![target_geo];
            for i in 0..extra_monitors {
                let other = snow_capture::MonitorId::from_parts(
                    i as u64 + 1000, i as u64 + 2000, 0,
                    format!("Other {i}"), false,
                );
                monitors.push(MonitorGeometry {
                    monitor: other,
                    x: 1920 * (i as i32 + 1), y: 0, width: 1920, height: 1080,
                });
            }

            let selector = MonitorSelector::new(&target_stable_id);
            let result = resolve_monitor_selector(&selector, &monitors);
            prop_assert!(result.is_ok(), "matching stable_id should resolve");
            prop_assert_eq!(result.unwrap().stable_id(), target_stable_id);

            let bad_id = "0000000000000000-ffffffffffffffff";
            let bad_selector = MonitorSelector::new(bad_id);
            let bad_result = resolve_monitor_selector(&bad_selector, &monitors);
            prop_assert!(bad_result.is_err(), "non-matching stable_id should fail");
            let err_msg = format!("{}", bad_result.unwrap_err());
            prop_assert!(err_msg.contains(bad_id),
                "error should contain the unresolved stable_id, got: {}", err_msg);
        }
    }

    #[test]
    fn cursor_types_are_provided_by_cursor_crate() {
        let cursor_sample_type = std::any::type_name::<snow_cursor::AttachedCursorSample>();
        assert!(
            !cursor_sample_type.is_empty(),
            "AttachedCursorSample type must be available from snow_cursor"
        );
    }

    proptest! {
        /// PrimaryMonitor always resolves to CaptureTarget::PrimaryMonitor.
        #[test]
        fn prop_resolve_capture_target_primary_monitor(_dummy in 0u8..1) {
            use super::resolve_capture_target;
            use crate::config::RecordingTarget;

            let target = RecordingTarget::PrimaryMonitor;
            let result = resolve_capture_target(&target);
            prop_assert!(result.is_ok(), "PrimaryMonitor should always resolve");
            match result.unwrap() {
                snow_capture::CaptureTarget::PrimaryMonitor => { /* correct */ }
                _ => prop_assert!(false, "expected CaptureTarget::PrimaryMonitor"),
            }
        }

        /// Region with non-zero dimensions resolves to CaptureTarget::Region.
        #[test]
        fn prop_resolve_capture_target_region(
            x in -10_000i32..10_000,
            y in -10_000i32..10_000,
            width in 1u32..10_000,
            height in 1u32..10_000,
        ) {
            use super::resolve_capture_target;
            use crate::config::{RecordingRegion, RecordingTarget};

            let region = RecordingRegion::new(x, y, width, height);
            let target = RecordingTarget::Region(region);
            let result = resolve_capture_target(&target);
            prop_assert!(result.is_ok(), "valid region should resolve, got: {:?}", result.err());
            match result.unwrap() {
                snow_capture::CaptureTarget::Region(cr) => {
                    prop_assert_eq!(cr.x, x, "x coordinate must be preserved");
                    prop_assert_eq!(cr.y, y, "y coordinate must be preserved");
                    prop_assert_eq!(cr.width, width, "width must be preserved");
                    prop_assert_eq!(cr.height, height, "height must be preserved");
                }
                _ => prop_assert!(false, "expected CaptureTarget::Region"),
            }
        }
    }

    proptest! {
        #[test]
        fn prop_invalid_region_dimensions_produce_errors(
            x in -10_000i32..10_000,
            y in -10_000i32..10_000,
            zero_width in proptest::bool::ANY,
            non_zero_dim in 1u32..10_000,
        ) {
            use super::resolve_capture_target;
            use crate::config::{RecordingRegion, RecordingTarget};

            let (width, height) = if zero_width {
                (0, non_zero_dim)
            } else {
                (non_zero_dim, 0)
            };

            let region = RecordingRegion { x, y, width, height };
            let target = RecordingTarget::Region(region);
            let result = resolve_capture_target(&target);
            prop_assert!(result.is_err(), "zero-dimension region should produce an error");
        }
    }
}
