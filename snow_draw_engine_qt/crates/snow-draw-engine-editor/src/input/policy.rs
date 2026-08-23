use super::*;

impl Editor {
    pub(crate) fn tool_policy(&self) -> ToolPolicy {
        let mut policy = Self::tool_policy_for(self.state.active_tool);
        policy.quick_selection_enabled = self.state.active_tool == ActiveTool::Select
            || (self.quick_selection_disabled_tools & self.state.active_tool.policy_bit()) == 0;
        policy
    }

    pub(crate) fn tool_policy_for(active_tool: ActiveTool) -> ToolPolicy {
        match active_tool {
            ActiveTool::Select => ToolPolicy {
                selection_scope: ToolSelectionScope::All,
                quick_selection_enabled: true,
                clear_selection_on_activate: false,
                empty_canvas_action: ToolEmptyCanvasAction::MarqueeSelect,
                allow_shift_toggle: true,
                default_cursor: CursorStyle::Default,
            },
            ActiveTool::Shape => ToolPolicy {
                selection_scope: ToolSelectionScope::RectangleOnly,
                quick_selection_enabled: true,
                clear_selection_on_activate: true,
                empty_canvas_action: ToolEmptyCanvasAction::CreateRectangle,
                allow_shift_toggle: true,
                default_cursor: CursorStyle::Crosshair,
            },
            ActiveTool::Arrow => ToolPolicy {
                selection_scope: ToolSelectionScope::ArrowOnly,
                quick_selection_enabled: true,
                clear_selection_on_activate: true,
                empty_canvas_action: ToolEmptyCanvasAction::CreateArrow,
                allow_shift_toggle: true,
                default_cursor: CursorStyle::Crosshair,
            },
            ActiveTool::Line => ToolPolicy {
                selection_scope: ToolSelectionScope::LineOnly,
                quick_selection_enabled: true,
                clear_selection_on_activate: true,
                empty_canvas_action: ToolEmptyCanvasAction::CreateArrow,
                allow_shift_toggle: true,
                default_cursor: CursorStyle::Crosshair,
            },
            ActiveTool::FreeDraw => ToolPolicy {
                selection_scope: ToolSelectionScope::FreeDrawOnly,
                quick_selection_enabled: true,
                clear_selection_on_activate: true,
                empty_canvas_action: ToolEmptyCanvasAction::CreateFreeDraw,
                allow_shift_toggle: true,
                default_cursor: CursorStyle::Crosshair,
            },
            ActiveTool::RectangleHighlight => ToolPolicy {
                selection_scope: ToolSelectionScope::RectangleHighlightOnly,
                quick_selection_enabled: true,
                clear_selection_on_activate: true,
                empty_canvas_action: ToolEmptyCanvasAction::CreateHighlight,
                allow_shift_toggle: true,
                default_cursor: CursorStyle::Crosshair,
            },
            ActiveTool::PenHighlight => ToolPolicy {
                selection_scope: ToolSelectionScope::PenHighlightOnly,
                quick_selection_enabled: true,
                clear_selection_on_activate: true,
                empty_canvas_action: ToolEmptyCanvasAction::CreatePenHighlight,
                allow_shift_toggle: true,
                default_cursor: CursorStyle::Crosshair,
            },
            ActiveTool::RectangleFilter => ToolPolicy {
                selection_scope: ToolSelectionScope::FilterOnly,
                quick_selection_enabled: true,
                clear_selection_on_activate: true,
                empty_canvas_action: ToolEmptyCanvasAction::CreateRectangle,
                allow_shift_toggle: true,
                default_cursor: CursorStyle::Crosshair,
            },
            ActiveTool::Spotlight => ToolPolicy {
                selection_scope: ToolSelectionScope::SpotlightOnly,
                quick_selection_enabled: true,
                clear_selection_on_activate: true,
                empty_canvas_action: ToolEmptyCanvasAction::CreateRectangle,
                allow_shift_toggle: true,
                default_cursor: CursorStyle::Crosshair,
            },
            ActiveTool::PenFilter => ToolPolicy {
                selection_scope: ToolSelectionScope::PenFilterOnly,
                quick_selection_enabled: true,
                clear_selection_on_activate: true,
                empty_canvas_action: ToolEmptyCanvasAction::CreatePenFilter,
                allow_shift_toggle: true,
                default_cursor: CursorStyle::Crosshair,
            },
            ActiveTool::Watermark => ToolPolicy {
                selection_scope: ToolSelectionScope::None,
                quick_selection_enabled: true,
                clear_selection_on_activate: false,
                empty_canvas_action: ToolEmptyCanvasAction::Configure,
                allow_shift_toggle: false,
                default_cursor: CursorStyle::Default,
            },
            ActiveTool::Eraser => ToolPolicy {
                selection_scope: ToolSelectionScope::All,
                quick_selection_enabled: true,
                clear_selection_on_activate: false,
                empty_canvas_action: ToolEmptyCanvasAction::MarqueeSelect,
                allow_shift_toggle: false,
                default_cursor: CursorStyle::Hidden,
            },
            ActiveTool::Text => ToolPolicy {
                selection_scope: ToolSelectionScope::TextOnly,
                quick_selection_enabled: true,
                clear_selection_on_activate: true,
                empty_canvas_action: ToolEmptyCanvasAction::CreateText,
                allow_shift_toggle: true,
                default_cursor: CursorStyle::Text,
            },
            ActiveTool::SerialNumber => ToolPolicy {
                selection_scope: ToolSelectionScope::SerialNumberOnly,
                quick_selection_enabled: true,
                clear_selection_on_activate: true,
                empty_canvas_action: ToolEmptyCanvasAction::CreateSerialNumber,
                allow_shift_toggle: true,
                default_cursor: CursorStyle::Crosshair,
            },
        }
    }

    pub(crate) fn capture_command_for_start(&self, pointer_id: u32) -> PointerCaptureCommand {
        if self.config.enable_pointer_capture {
            PointerCaptureCommand::Capture(pointer_id)
        } else {
            PointerCaptureCommand::NoChange
        }
    }

    pub(crate) fn release_capture_command(&self) -> PointerCaptureCommand {
        if self.config.enable_pointer_capture {
            PointerCaptureCommand::Release
        } else {
            PointerCaptureCommand::NoChange
        }
    }

    pub(crate) fn selection_scope_matches(scope: ToolSelectionScope, kind: ElementKind) -> bool {
        match scope {
            ToolSelectionScope::None => false,
            ToolSelectionScope::All => true,
            ToolSelectionScope::RectangleOnly => kind == ElementKind::Rectangle,
            ToolSelectionScope::ArrowOnly => kind == ElementKind::Arrow,
            ToolSelectionScope::LineOnly => kind == ElementKind::Line,
            ToolSelectionScope::FreeDrawOnly => kind == ElementKind::FreeDraw,
            ToolSelectionScope::RectangleHighlightOnly => kind == ElementKind::RectangleHighlight,
            ToolSelectionScope::SpotlightOnly => kind == ElementKind::Spotlight,
            ToolSelectionScope::PenHighlightOnly => kind == ElementKind::PenHighlight,
            ToolSelectionScope::FilterOnly => kind == ElementKind::Filter,
            ToolSelectionScope::PenFilterOnly => kind == ElementKind::PenFilter,
            ToolSelectionScope::TextOnly => kind == ElementKind::Text,
            ToolSelectionScope::SerialNumberOnly => {
                kind == ElementKind::SerialNumber || kind == ElementKind::Text
            }
        }
    }

    pub(crate) fn selection_scope_matches_document(
        document: &DocumentModel,
        scope: ToolSelectionScope,
        id: ElementId,
        kind: ElementKind,
    ) -> bool {
        match scope {
            ToolSelectionScope::SerialNumberOnly => {
                kind == ElementKind::SerialNumber
                    || (kind == ElementKind::Text && document.is_text_bound_to_serial_number(id))
            }
            _ => Self::selection_scope_matches(scope, kind),
        }
    }
}

#[cfg(test)]
mod line_policy_tests {
    use super::*;

    #[test]
    fn shape_uses_a_crosshair_creation_cursor() {
        assert_eq!(
            Editor::tool_policy_for(ActiveTool::Shape).default_cursor,
            CursorStyle::Crosshair
        );
    }

    #[test]
    fn arrow_and_line_creation_scopes_are_distinct() {
        let arrow_scope = Editor::tool_policy_for(ActiveTool::Arrow).selection_scope;
        let line_scope = Editor::tool_policy_for(ActiveTool::Line).selection_scope;
        assert!(Editor::selection_scope_matches(
            arrow_scope,
            ElementKind::Arrow
        ));
        assert!(!Editor::selection_scope_matches(
            arrow_scope,
            ElementKind::Line
        ));
        assert!(Editor::selection_scope_matches(
            line_scope,
            ElementKind::Line
        ));
        assert!(!Editor::selection_scope_matches(
            line_scope,
            ElementKind::Arrow
        ));
    }

    #[test]
    fn watermark_is_a_neutral_configuration_mode() {
        let policy = Editor::tool_policy_for(ActiveTool::Watermark);
        assert_eq!(policy.selection_scope, ToolSelectionScope::None);
        assert_eq!(policy.empty_canvas_action, ToolEmptyCanvasAction::Configure);
        assert_eq!(policy.default_cursor, CursorStyle::Default);
        assert!(!policy.clear_selection_on_activate);
        assert!(!policy.allow_shift_toggle);
    }

    #[test]
    fn quick_selection_is_controlled_by_the_runtime_tool_mask() {
        let mut editor = Editor::new(EngineConfig::default()).unwrap();
        editor.set_quick_selection_disabled_tools(
            ActiveTool::FreeDraw.policy_bit() | ActiveTool::PenFilter.policy_bit(),
        );

        editor.set_active_tool(ActiveTool::FreeDraw).unwrap();
        assert!(!editor.tool_policy().quick_selection_enabled);
        editor.set_active_tool(ActiveTool::PenFilter).unwrap();
        assert!(!editor.tool_policy().quick_selection_enabled);
        editor.set_active_tool(ActiveTool::RectangleFilter).unwrap();
        assert!(editor.tool_policy().quick_selection_enabled);
        editor.set_active_tool(ActiveTool::Select).unwrap();
        assert!(editor.tool_policy().quick_selection_enabled);
    }

    #[test]
    fn spotlight_uses_an_isolated_crosshair_rectangle_policy() {
        let policy = Editor::tool_policy_for(ActiveTool::Spotlight);
        assert_eq!(policy.selection_scope, ToolSelectionScope::SpotlightOnly);
        assert_eq!(
            policy.empty_canvas_action,
            ToolEmptyCanvasAction::CreateRectangle
        );
        assert_eq!(policy.default_cursor, CursorStyle::Crosshair);
        assert!(policy.clear_selection_on_activate);
        assert!(policy.allow_shift_toggle);
        assert!(Editor::selection_scope_matches(
            policy.selection_scope,
            ElementKind::Spotlight
        ));
        assert!(!Editor::selection_scope_matches(
            policy.selection_scope,
            ElementKind::RectangleHighlight
        ));
    }
}
