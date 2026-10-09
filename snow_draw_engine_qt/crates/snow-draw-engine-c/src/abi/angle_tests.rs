use super::{convert::*, handles::*, style_exports::*, types::*};
use snow_draw_engine::{ActiveTool, AngleStyle, AngleUnit, Runtime, ViewportConfig};

#[test]
fn angle_abi_values_layout_round_trip_and_tool_masks() {
    assert_eq!(SnowActiveTool::Angle as u32, 18);
    assert_eq!(SnowStyleToolbarSource::DefaultAngle as u32, 28);
    assert_eq!(SnowStyleToolbarSource::SelectedAngle as u32, 29);
    assert_eq!(std::mem::size_of::<SnowAngleStyle>(), 24);
    assert_eq!(std::mem::offset_of!(SnowAngleStyle, stroke_width), 8);
    assert_eq!(std::mem::offset_of!(SnowAngleStyle, unit), 16);
    assert_eq!(std::mem::offset_of!(SnowAngleStyle, decimal_places), 20);
    let style = AngleStyle {
        unit: AngleUnit::Radians,
        decimal_places: 3,
        stroke_width: 5.0,
        ..Default::default()
    };
    assert_eq!(AngleStyle::from(SnowAngleStyle::from(style)), style);
    assert_eq!(
        snow_active_tool_to_rust(SnowActiveTool::Angle),
        ActiveTool::Angle
    );
    assert_eq!(
        snow_active_tool_from_rust(ActiveTool::Angle),
        SnowActiveTool::Angle
    );
    assert_eq!(
        snow_active_tool_mask_to_rust(1 << 18),
        ActiveTool::Angle.policy_bit()
    );
}

#[test]
fn angle_ffi_target_token_handles_null_and_tool_changes() {
    let mut runtime = SnowRuntimeImpl {
        runtime: Runtime::default(),
    };
    let viewport = runtime
        .runtime
        .create_viewport(ViewportConfig::default())
        .unwrap();
    unsafe {
        assert_eq!(
            super::annotation_exports::snow_runtime_angle_adjustment_target_revision(
                std::ptr::null_mut()
            ),
            0
        );
        let original =
            super::annotation_exports::snow_runtime_angle_adjustment_target_revision(&mut runtime);
        assert_ne!(original, 0);
        runtime
            .runtime
            .set_viewport_active_tool(viewport, ActiveTool::Angle)
            .unwrap();
        assert_ne!(
            super::annotation_exports::snow_runtime_angle_adjustment_target_revision(&mut runtime),
            original
        );
    }
}

#[test]
fn angle_ffi_style_validation_is_atomic_and_adjustment_rejects_nonfinite_delta() {
    let mut runtime = SnowRuntimeImpl {
        runtime: Runtime::default(),
    };
    let id = runtime
        .runtime
        .create_viewport(ViewportConfig::default())
        .unwrap();
    runtime
        .runtime
        .set_viewport_active_tool(id, ActiveTool::Angle)
        .unwrap();
    let mut viewport = SnowViewportImpl { id };
    let mut changed = std::ptr::null_mut();
    let original = runtime
        .runtime
        .viewport_style_toolbar_state(id)
        .unwrap()
        .angle_style;
    unsafe {
        for style in [
            SnowAngleStyle {
                stroke_width: f64::INFINITY,
                ..Default::default()
            },
            SnowAngleStyle {
                decimal_places: 4,
                ..Default::default()
            },
        ] {
            assert_eq!(
                snow_viewport_set_angle_style_patch_ex(
                    &mut runtime,
                    &mut viewport,
                    &style,
                    SNOW_ANGLE_STYLE_PROPERTY_ALL,
                    &mut changed
                ),
                SnowError::InvalidArgument
            );
            assert_eq!(
                runtime
                    .runtime
                    .viewport_style_toolbar_state(id)
                    .unwrap()
                    .angle_style,
                original
            );
        }
        assert_eq!(
            snow_viewport_set_angle_style_patch_ex(
                &mut runtime,
                &mut viewport,
                std::ptr::null(),
                15,
                &mut changed
            ),
            SnowError::InvalidArgument
        );
        assert_eq!(
            snow_viewport_adjust_angle_value_ex(
                &mut runtime,
                &mut viewport,
                f64::NAN,
                &mut changed
            ),
            SnowError::InvalidArgument
        );
        let valid = SnowAngleStyle {
            unit: SnowAngleUnit::Radians,
            decimal_places: 2,
            ..Default::default()
        };
        assert_eq!(
            snow_viewport_set_angle_style_patch_ex(
                &mut runtime,
                &mut viewport,
                &valid,
                15,
                &mut changed
            ),
            SnowError::Ok
        );
        super::exports::snow_changed_viewports_destroy(changed);
        assert_eq!(
            runtime
                .runtime
                .viewport_style_toolbar_state(id)
                .unwrap()
                .angle_style
                .unit,
            AngleUnit::Radians
        );
        let mut invalid = valid;
        std::ptr::addr_of_mut!(invalid.unit)
            .cast::<i32>()
            .write(999);
        assert_eq!(
            snow_viewport_set_angle_style_patch_ex(
                &mut runtime,
                &mut viewport,
                &invalid,
                15,
                &mut changed
            ),
            SnowError::InvalidArgument
        );
    }
}
