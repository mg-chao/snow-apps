TEMPLATE = lib
TARGET = ant_design_qt

QT += core gui widgets network
CONFIG += c++17
CONFIG += staticlib

win32-msvc {
    QMAKE_CXXFLAGS += /MP
    CONFIG += precompile_header
    PRECOMPILED_HEADER = src/ant_design_qt_pch.h
}

win32:LIBS += -lcomctl32 -ldwmapi -luser32

INCLUDEPATH += $$PWD/src
INCLUDEPATH += $$clean_path($$PWD/../adqt_icon_core/src)
INCLUDEPATH += $$clean_path($$PWD/../ant_design_icons_qt/src)

HEADERS += \
    src/ant_design_qt_pch.h \
    src/icons/widget_icons.h \
    src/locale/locale.h \
    src/locale/locale_manager.h \
    src/locale/locale_types.h \
    src/widgets/abstract_select_widget.h \
    src/widgets/alert.h \
    src/widgets/alert_style.h \
    src/widgets/button.h \
    src/widgets/button_style.h \
    src/widgets/control_scale.h \
    src/widgets/dpi_stable_window_controller.h \
    src/widgets/floating_surface.h \
    src/widgets/carousel.h \
    src/widgets/carousel_style.h \
    src/widgets/checkbox.h \
    src/widgets/checkbox_group.h \
    src/widgets/checkbox_style.h \
    src/widgets/combo_box.h \
    src/widgets/color_picker.h \
    src/widgets/color_picker_style.h \
    src/widgets/color_selection.h \
    src/widgets/context_menu.h \
    src/widgets/date_picker.h \
    src/widgets/date_picker_style.h \
    src/widgets/descriptions.h \
    src/widgets/descriptions_style.h \
    src/widgets/divider.h \
    src/widgets/divider_style.h \
    src/widgets/detail/button_grouping.h \
    src/widgets/detail/button_rendering.h \
    src/widgets/detail/color_picker_value_model.h \
    src/widgets/detail/flow_layout.h \
    src/widgets/detail/form_value_adapter.h \
    src/widgets/detail/overlay_accessibility.h \
    src/widgets/detail/overlay_popup_controller.h \
    src/widgets/detail/overlay_popup_surface.h \
    src/widgets/detail/qt_tooltip_bridge.h \
    src/widgets/detail/popup_shadow.h \
    src/widgets/detail/navigation_menu_popup_state.h \
    src/widgets/detail/navigation_menu_state.h \
    src/widgets/detail/navigation_menu_view_state.h \
    src/widgets/detail/select_models.h \
    src/widgets/detail/select_option_utils.h \
    src/widgets/detail/select_selection_controller.h \
    src/widgets/detail/animated_scalar.h \
    src/widgets/detail/themed_scrollbar.h \
    src/widgets/detail/top_level_popup_window.h \
    src/widgets/detail/timing_hub.h \
    src/widgets/form.h \
    src/widgets/form_style.h \
    src/widgets/image.h \
    src/widgets/image_style.h \
    src/widgets/field_group.h \
    src/widgets/input.h \
    src/widgets/input_internal.h \
    src/widgets/input_line_edit.h \
    src/widgets/input_number.h \
    src/widgets/input_number_value_model.h \
    src/widgets/input_number_style.h \
    src/widgets/input_policies.h \
    src/widgets/input_otp_edit.h \
    src/widgets/input_password_edit.h \
    src/widgets/input_search_edit.h \
    src/widgets/input_text_edit.h \
    src/widgets/input_style.h \
    src/widgets/popup_interaction_host.h \
    src/widgets/interaction_overlay_manager.h \
    src/widgets/message.h \
    src/widgets/message_style.h \
    src/widgets/modal.h \
    src/widgets/multi_select.h \
    src/widgets/navigation_menu.h \
    src/widgets/menu_style.h \
    src/widgets/notification.h \
    src/widgets/notification_style.h \
    src/widgets/pagination.h \
    src/widgets/pagination_style.h \
    src/widgets/popover.h \
    src/widgets/popconfirm.h \
    src/widgets/popover_style.h \
    src/widgets/popup_types.h \
    src/widgets/popup_placement.h \
    src/widgets/radio.h \
    src/widgets/radio_button_group.h \
    src/widgets/radio_style.h \
    src/widgets/scroll_area.h \
    src/widgets/segmented.h \
    src/widgets/segmented_style.h \
    src/widgets/select.h \
    src/widgets/select_types.h \
    src/widgets/tag_select.h \
    src/widgets/tag.h \
    src/widgets/tag_group.h \
    src/widgets/tag_style.h \
    src/widgets/select_style.h \
    src/widgets/slider.h \
    src/widgets/slider_style.h \
    src/widgets/spin.h \
    src/widgets/spin_style.h \
    src/widgets/switch.h \
    src/widgets/switch_style.h \
    src/widgets/tabs.h \
    src/widgets/tabs_style.h \
    src/widgets/tooltip.h \
    src/widgets/tooltip_style.h \
    src/widgets/widgets.h \
    src/theme/fast_color_lite.h \
    src/theme/palette_generate.h \
    src/theme/theme_color_utils.h \
    src/theme/theme.h \
    src/theme/theme_manager.h \
    src/theme/theme_palette.h \
    src/theme/theme_types.h

SOURCES += \
    src/placeholder.cpp \
    src/icons/widget_icons.cpp \
    src/locale/locale_manager.cpp \
    src/widgets/abstract_select_widget.cpp \
    src/widgets/alert.cpp \
    src/widgets/alert_style.cpp \
    src/widgets/button.cpp \
    src/widgets/button_style.cpp \
    src/widgets/control_scale.cpp \
    src/widgets/dpi_stable_window_controller.cpp \
    src/widgets/floating_surface.cpp \
    src/widgets/carousel.cpp \
    src/widgets/carousel_style.cpp \
    src/widgets/checkbox.cpp \
    src/widgets/checkbox_group.cpp \
    src/widgets/checkbox_style.cpp \
    src/widgets/combo_box.cpp \
    src/widgets/color_picker.cpp \
    src/widgets/color_picker_style.cpp \
    src/widgets/context_menu.cpp \
    src/widgets/date_picker.cpp \
    src/widgets/date_picker_style.cpp \
    src/widgets/descriptions.cpp \
    src/widgets/descriptions_style.cpp \
    src/widgets/divider.cpp \
    src/widgets/divider_style.cpp \
    src/widgets/detail/color_picker_value_model.cpp \
    src/widgets/detail/flow_layout.cpp \
    src/widgets/detail/form_value_adapter.cpp \
    src/widgets/detail/overlay_accessibility.cpp \
    src/widgets/detail/overlay_popup_controller.cpp \
    src/widgets/detail/overlay_popup_surface.cpp \
    src/widgets/detail/qt_tooltip_bridge.cpp \
    src/widgets/detail/navigation_menu_state.cpp \
    src/widgets/detail/select_models.cpp \
    src/widgets/detail/select_option_utils.cpp \
    src/widgets/detail/select_selection_controller.cpp \
    src/widgets/detail/themed_scrollbar.cpp \
    src/widgets/detail/top_level_popup_window.cpp \
    src/widgets/detail/timing_hub.cpp \
    src/widgets/form.cpp \
    src/widgets/form_style.cpp \
    src/widgets/image.cpp \
    src/widgets/image_style.cpp \
    src/widgets/field_group.cpp \
    src/widgets/input_internal.cpp \
    src/widgets/input_line_edit.cpp \
    src/widgets/input_otp_edit.cpp \
    src/widgets/input_password_edit.cpp \
    src/widgets/input_search_edit.cpp \
    src/widgets/input_text_edit.cpp \
    src/widgets/input_number.cpp \
    src/widgets/input_number_value_model.cpp \
    src/widgets/input_number_style.cpp \
    src/widgets/input_style.cpp \
    src/widgets/popup_interaction_host.cpp \
    src/widgets/interaction_overlay_manager.cpp \
    src/widgets/message.cpp \
    src/widgets/message_style.cpp \
    src/widgets/modal.cpp \
    src/widgets/multi_select.cpp \
    src/widgets/navigation_menu.cpp \
    src/widgets/menu_style.cpp \
    src/widgets/notification.cpp \
    src/widgets/notification_style.cpp \
    src/widgets/pagination.cpp \
    src/widgets/pagination_style.cpp \
    src/widgets/popover.cpp \
    src/widgets/popconfirm.cpp \
    src/widgets/popover_style.cpp \
    src/widgets/popup_placement.cpp \
    src/widgets/radio.cpp \
    src/widgets/radio_button_group.cpp \
    src/widgets/radio_style.cpp \
    src/widgets/scroll_area.cpp \
    src/widgets/segmented.cpp \
    src/widgets/segmented_style.cpp \
    src/widgets/select.cpp \
    src/widgets/tag_select.cpp \
    src/widgets/tag.cpp \
    src/widgets/tag_group.cpp \
    src/widgets/tag_style.cpp \
    src/widgets/select_style.cpp \
    src/widgets/slider.cpp \
    src/widgets/slider_style.cpp \
    src/widgets/spin.cpp \
    src/widgets/spin_style.cpp \
    src/widgets/switch.cpp \
    src/widgets/switch_style.cpp \
    src/widgets/tabs.cpp \
    src/widgets/tabs_style.cpp \
    src/widgets/tooltip.cpp \
    src/widgets/tooltip_style.cpp \
    src/theme/fast_color_lite.cpp \
    src/theme/palette_generate.cpp \
    src/theme/theme_color_utils.cpp \
    src/theme/theme_manager.cpp \
    src/theme/theme_palette.cpp \
    src/theme/theme_types.cpp
