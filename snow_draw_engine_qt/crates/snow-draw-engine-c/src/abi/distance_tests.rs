use super::{convert::*, exports::*, handles::*, style_exports::*, text_exports::*, types::*};
use snow_draw_engine::{
    ActiveTool, InputEvent, Modifiers, Point, PointerButton, PointerButtons, PointerDevice,
    PointerEvent, PointerEventType, Runtime, ViewportConfig,
};

#[test]
fn presentation_scope_ffi_validation_keeps_scopes_balanced() {
    let mut state = SnowRuntimeImpl {
        runtime: Runtime::default(),
    };
    let mut changed = std::ptr::null_mut();
    unsafe {
        assert_eq!(
            snow_runtime_begin_presentation_update(std::ptr::null_mut()),
            SnowError::InvalidArgument
        );
        assert_eq!(
            snow_runtime_end_presentation_update_ex(&mut state, &mut changed),
            SnowError::InvalidState
        );
        assert!(changed.is_null());
        assert_eq!(
            snow_runtime_begin_presentation_update(&mut state),
            SnowError::Ok
        );
        assert_eq!(
            snow_runtime_end_presentation_update_ex(&mut state, std::ptr::null_mut()),
            SnowError::InvalidArgument
        );
        assert_eq!(
            snow_runtime_end_presentation_update_ex(&mut state, &mut changed),
            SnowError::Ok
        );
        snow_changed_viewports_destroy(changed);
        assert_eq!(
            snow_runtime_end_presentation_update_ex(&mut state, &mut changed),
            SnowError::InvalidState
        );
        assert!(changed.is_null());
    }
}

#[test]
fn distance_abi_values_layout_and_round_trip() {
    use super::raw_enum::SnowRawEnum;
    for invalid in [-1, 9, i32::MAX] {
        assert_eq!(SnowDistanceUnit::from_raw(invalid), None);
    }
    assert_eq!(SnowDistanceStyle::default().unit, SnowDistanceUnit::Cm);
    assert_eq!(SnowDistanceUnit::Px as u32, 0);
    assert_eq!(SnowDistanceUnit::Cm as u32, 1);
    assert_eq!(SnowDistanceUnit::M as u32, 2);
    assert_eq!(SnowDistanceUnit::Km as u32, 3);
    assert_eq!(SnowDistanceUnit::Mm as u32, 4);
    for (unit, value, suffix) in [
        (SnowDistanceUnit::Pt, 5, "pt"),
        (SnowDistanceUnit::In, 6, "in"),
        (SnowDistanceUnit::Ft, 7, "ft"),
        (SnowDistanceUnit::Um, 8, "μm"),
    ] {
        assert_eq!(unit as u32, value);
        assert_eq!(SnowDistanceUnit::from_raw(value as i32), Some(unit));
        let style = SnowDistanceStyle {
            unit,
            ..SnowDistanceStyle::default()
        };
        let rust: snow_draw_engine::DistanceStyle = style.into();
        assert_eq!(rust.unit.suffix(), suffix);
        assert_eq!(SnowDistanceStyle::from(rust), style);
    }
    assert_eq!(SnowActiveTool::Distance as u32, 17);
    assert_eq!(SnowStyleToolbarSource::DefaultDistance as u32, 26);
    assert_eq!(SnowStyleToolbarSource::SelectedDistance as u32, 27);
    assert_eq!(std::mem::size_of::<SnowDistanceStyle>(), 48);
    assert_eq!(std::mem::offset_of!(SnowDistanceStyle, stroke_width), 8);
    assert_eq!(std::mem::offset_of!(SnowDistanceStyle, unit), 24);
    assert_eq!(std::mem::offset_of!(SnowDistanceStyle, endpoint_scale), 32);
    assert_eq!(std::mem::offset_of!(SnowDistanceStyle, endpoint_style), 40);
    let style = SnowDistanceStyle {
        factor: 2.31,
        unit: SnowDistanceUnit::Km,
        decimal_places: 3,
        endpoint_scale: 2.5,
        endpoint_style: SnowArrowhead::IndentedTriangle,
        ..SnowDistanceStyle::default()
    };
    let rust: snow_draw_engine::DistanceStyle = style.into();
    assert_eq!(SnowDistanceStyle::from(rust), style);
    let millimeters = SnowDistanceStyle {
        unit: SnowDistanceUnit::Mm,
        ..style
    };
    let rust: snow_draw_engine::DistanceStyle = millimeters.into();
    assert_eq!(rust.unit, snow_draw_engine::DistanceUnit::Mm);
    assert_eq!(SnowDistanceStyle::from(rust), millimeters);
    assert_eq!(
        snow_active_tool_to_rust(SnowActiveTool::Distance),
        ActiveTool::Distance
    );
    assert_eq!(
        snow_active_tool_from_rust(ActiveTool::Distance),
        SnowActiveTool::Distance
    );
    assert_eq!(
        snow_active_tool_mask_to_rust(1 << 17),
        ActiveTool::Distance.policy_bit()
    );
}

#[test]
fn distance_ffi_validates_and_supplies_complete_provisional_label() {
    let mut state = SnowRuntimeImpl {
        runtime: Runtime::default(),
    };
    let id = state
        .runtime
        .create_viewport(ViewportConfig::default())
        .unwrap();
    state
        .runtime
        .set_viewport_surface_size(id, 600, 360)
        .unwrap();
    state
        .runtime
        .set_viewport_active_tool(id, ActiveTool::Distance)
        .unwrap();
    let mut viewport = SnowViewportImpl { id };
    let mut changed = std::ptr::null_mut();
    unsafe {
        assert_eq!(
            snow_viewport_set_distance_pixel_scale_ex(
                &mut state,
                &mut viewport,
                2.0,
                3.0,
                &mut changed
            ),
            SnowError::Ok
        );
        assert!(changed.is_null(), "calibration has no presentation changes");
        snow_changed_viewports_destroy(changed);
        changed = std::ptr::null_mut();
        assert_eq!(
            snow_viewport_set_distance_pixel_scale_ex(
                &mut state,
                &mut viewport,
                0.0,
                1.0,
                &mut changed
            ),
            SnowError::InvalidArgument
        );
        assert!(changed.is_null());
        let invalid = SnowDistanceStyle {
            factor: 1001.0,
            ..SnowDistanceStyle::default()
        };
        assert_eq!(
            snow_viewport_set_distance_style_patch_ex(
                &mut state,
                &mut viewport,
                &invalid,
                SNOW_DISTANCE_STYLE_PROPERTY_ALL,
                &mut changed
            ),
            SnowError::InvalidArgument
        );
        let invalid = SnowDistanceStyle {
            decimal_places: 256,
            ..SnowDistanceStyle::default()
        };
        assert_eq!(
            snow_viewport_set_distance_style_patch_ex(
                &mut state,
                &mut viewport,
                &invalid,
                SNOW_DISTANCE_STYLE_PROPERTY_ALL,
                &mut changed
            ),
            SnowError::InvalidArgument
        );
        let invalid = SnowDistanceStyle {
            endpoint_scale: 0.4,
            ..SnowDistanceStyle::default()
        };
        assert_eq!(
            snow_viewport_set_distance_style_patch_ex(
                &mut state,
                &mut viewport,
                &invalid,
                SNOW_DISTANCE_STYLE_PROPERTY_ENDPOINT_SCALE,
                &mut changed
            ),
            SnowError::InvalidArgument
        );
        assert_eq!(
            snow_viewport_set_distance_style_patch_ex(
                &mut state,
                &mut viewport,
                std::ptr::null(),
                1,
                &mut changed
            ),
            SnowError::InvalidArgument
        );
    }
    for (event_type, x) in [
        (PointerEventType::Down, 100.0),
        (PointerEventType::Move, 300.0),
    ] {
        state
            .runtime
            .process_input(
                id,
                InputEvent::Pointer(PointerEvent {
                    pressure: None,
                    timestamp_ms: 0,
                    pointer_id: 1,
                    event_type,
                    device: PointerDevice::Mouse,
                    position: Point::new(x, 180.0),
                    button: Some(PointerButton::Primary),
                    buttons: PointerButtons(PointerButtons::PRIMARY),
                    modifiers: Modifiers::default(),
                }),
            )
            .unwrap();
    }
    assert!(!state.runtime.has_document_content());
    let mut count = 0;
    let mut request = SnowArrowTextLayoutRequest::default();
    unsafe {
        assert_eq!(
            snow_viewport_get_arrow_text_layout_requests(
                &mut state,
                &mut viewport,
                &mut request,
                1,
                &mut count
            ),
            SnowError::Ok
        );
    }
    assert_eq!(count, 1);
    let text = String::from_utf8(
        request.info.text_utf8[..request.info.text_utf8_len as usize]
            .iter()
            .map(|c| *c as u8)
            .collect(),
    )
    .unwrap();
    assert_eq!(text, "400 cm");
    assert_eq!(request.info.font_size, 20.0);
    assert_eq!(request.style.font_size, 20.0);
    assert_eq!(request.info.center_x, -100.0);
    assert_eq!(request.info.rotation, 0.0);
    let old_key = request.key;
    state
        .runtime
        .process_input(
            id,
            InputEvent::Pointer(PointerEvent {
                pressure: None,
                timestamp_ms: 0,
                pointer_id: 1,
                event_type: PointerEventType::Move,
                device: PointerDevice::Mouse,
                position: Point::new(400.0, 180.0),
                button: Some(PointerButton::Primary),
                buttons: PointerButtons(PointerButtons::PRIMARY),
                modifiers: Modifiers::default(),
            }),
        )
        .unwrap();
    unsafe {
        assert_eq!(
            snow_viewport_get_arrow_text_layout_requests(
                &mut state,
                &mut viewport,
                &mut request,
                1,
                &mut count
            ),
            SnowError::Ok
        );
    }
    assert_ne!(request.key, old_key);
    let text = String::from_utf8(
        request.info.text_utf8[..request.info.text_utf8_len as usize]
            .iter()
            .map(|c| *c as u8)
            .collect(),
    )
    .unwrap();
    assert_eq!(text, "600 cm");
}

#[test]
fn distance_inline_value_ffi_validates_and_commits_owner_values() {
    let mut state = SnowRuntimeImpl {
        runtime: Runtime::default(),
    };
    let id = state
        .runtime
        .create_viewport(ViewportConfig::default())
        .unwrap();
    let mut viewport = SnowViewportImpl { id };
    state
        .runtime
        .apply_annotation_json(
            br#"{"version":1,"operations":[{"type":"distance","points":[[0,0],[100,0]]}]}"#,
        )
        .unwrap();
    let request = state
        .runtime
        .arrow_text_layout_requests(id)
        .unwrap()
        .remove(0);
    let owner: SnowElementId = snow_element_id_from_rust(request.arrow_id);
    let mut changed = std::ptr::null_mut();
    let mut info = SnowTextElementInfo::default();
    let mut style = SnowTextStyle::default();
    let mut found = 9;
    unsafe {
        assert_eq!(
            snow_viewport_take_distance_value_edit_target(
                &mut state,
                &mut viewport,
                &mut info,
                &mut style,
                &mut found
            ),
            SnowError::Ok
        );
        assert_eq!(found, 0);
        assert_eq!(
            snow_viewport_take_distance_value_edit_target(
                &mut state,
                &mut viewport,
                std::ptr::null_mut(),
                &mut style,
                &mut found
            ),
            SnowError::InvalidArgument
        );
        let layout = SnowTextLayoutSize {
            width: 80.0,
            height: 24.0,
            content_width: 80.0,
            content_height: 24.0,
        };
        assert_eq!(
            snow_viewport_set_distance_value_draft_ex(
                &mut state,
                &mut viewport,
                owner,
                c"25".as_ptr(),
                2,
                layout,
                &mut changed
            ),
            SnowError::Ok
        );
        snow_changed_viewports_destroy(changed);
        changed = std::ptr::null_mut();
        assert_eq!(
            snow_viewport_commit_distance_value_ex(
                &mut state,
                &mut viewport,
                owner,
                f64::NAN,
                &mut changed
            ),
            SnowError::InvalidArgument
        );
        assert!(changed.is_null());
        assert_eq!(
            snow_viewport_commit_distance_value_ex(
                &mut state,
                &mut viewport,
                owner,
                25.0,
                &mut changed
            ),
            SnowError::Ok
        );
        snow_changed_viewports_destroy(changed);
        changed = std::ptr::null_mut();
        assert_eq!(
            snow_viewport_commit_distance_value_ex(
                &mut state,
                &mut viewport,
                SnowElementId::default(),
                25.0,
                &mut changed
            ),
            SnowError::NotFound
        );
    }
    assert_eq!(
        state
            .runtime
            .text_element_info(request.text_id)
            .unwrap()
            .text,
        "25 cm"
    );
}
