use crate::{app_client::AppClient, schemas};
use base64::{Engine as _, engine::general_purpose::STANDARD};
use rmcp::{
    ErrorData as McpError, RoleServer,
    handler::server::ServerHandler,
    model::{
        CallToolRequestParams, CallToolResponse, CallToolResult, ContentBlock, ErrorCode,
        ListToolsResult, PaginatedRequestParams, ServerCapabilities, ServerConfig, Tool,
        ToolAnnotations,
    },
    service::RequestContext,
};
use serde_json::{Value, json};
use std::{borrow::Cow, sync::Arc};

const TOOLS: &[(&str, &str, bool)] = &[
    (
        "snow_shot_status",
        "Report Snow Shot MCP availability and capabilities.",
        true,
    ),
    (
        "screenshot_begin",
        "Start a Snow Shot screenshot editing session.",
        false,
    ),
    (
        "screenshot_state",
        "Read the current screenshot session state.",
        true,
    ),
    (
        "screenshot_set_selection",
        "Set or modify the screenshot selection.",
        false,
    ),
    (
        "screenshot_set_tool",
        "Select a Snow Shot annotation tool.",
        false,
    ),
    (
        "screenshot_apply_annotations",
        "Apply a typed annotation transaction.",
        false,
    ),
    (
        "screenshot_undo",
        "Undo the latest screenshot annotation transaction.",
        false,
    ),
    (
        "screenshot_redo",
        "Redo the latest screenshot annotation transaction.",
        false,
    ),
    (
        "screenshot_render",
        "Render the current screenshot selection as an image.",
        true,
    ),
    (
        "screenshot_save",
        "Save the current screenshot selection to a file.",
        false,
    ),
    (
        "screenshot_copy",
        "Copy the current screenshot selection to the clipboard.",
        false,
    ),
    (
        "screenshot_pin",
        "Pin the current screenshot selection to the screen.",
        false,
    ),
    (
        "screenshot_finish",
        "Finish and release a screenshot session.",
        false,
    ),
    ("screenshot_cancel", "Cancel a screenshot session.", false),
    (
        "screenshot_direct_capture",
        "Capture the current monitor or focused window directly.",
        false,
    ),
];

#[derive(Clone)]
pub struct SnowShotMcp {
    client: AppClient,
}

impl SnowShotMcp {
    pub fn new() -> Self {
        Self {
            client: AppClient::new(),
        }
    }

    fn tools() -> Vec<Tool> {
        TOOLS
            .iter()
            .map(|(name, description, read_only)| {
                Tool::new(
                    Cow::Borrowed(*name),
                    Cow::Borrowed(*description),
                    Arc::new(schemas::schema(name, None).expect("static tool schema")),
                )
                .with_annotations(ToolAnnotations::from_raw(
                    None,
                    Some(*read_only),
                    Some(!read_only),
                    Some(*read_only),
                    Some(false),
                ))
            })
            .collect()
    }

    async fn invoke(
        &self,
        name: &str,
        arguments: Value,
        context: RequestContext<RoleServer>,
    ) -> Result<CallToolResult, McpError> {
        schemas::schema(name, Some(arguments.clone()))
            .map_err(|_| McpError::invalid_params("Invalid Snow Shot tool arguments", None))?;
        let session_id = arguments
            .get("session_id")
            .and_then(Value::as_str)
            .map(str::to_owned);
        let expected_revision = arguments.get("expected_revision").and_then(Value::as_u64);
        let tool_timer = std::time::Instant::now();
        let reply = self
            .client
            .request(name, session_id, expected_revision, arguments, context.ct)
            .await;
        match reply {
            Ok(mut reply) => {
                if let Some(object) = reply.response.result.as_object_mut() {
                    object.insert(
                        "bridge_total_ms".into(),
                        json!(tool_timer.elapsed().as_secs_f64() * 1000.0),
                    );
                }
                let structured = serde_json::to_value(&reply.response)
                    .map_err(|_| McpError::internal_error("Invalid local response", None))?;
                let mut content = vec![ContentBlock::text(structured.to_string())];
                if reply.response.ok && !reply.attachment.is_empty() {
                    content.push(ContentBlock::image(
                        STANDARD.encode(reply.attachment),
                        "image/png",
                    ));
                }
                let mut result = if reply.response.ok {
                    CallToolResult::success(content)
                } else {
                    CallToolResult::error(content)
                };
                result.structured_content = Some(structured);
                Ok(result)
            }
            Err(error) => {
                let structured = json!({"reachable":false,"mcp_enabled":null,"error":{"code":error.code(),"message":error.to_string()}});
                let mut result = if name == "snow_shot_status" {
                    CallToolResult::success(vec![ContentBlock::text(structured.to_string())])
                } else {
                    CallToolResult::error(vec![ContentBlock::text(structured.to_string())])
                };
                result.structured_content = Some(structured);
                Ok(result)
            }
        }
    }
}

impl Default for SnowShotMcp {
    fn default() -> Self {
        Self::new()
    }
}

impl ServerHandler for SnowShotMcp {
    fn get_info(&self) -> ServerConfig {
        ServerConfig::new(ServerCapabilities::builder().enable_tools().build()).with_instructions(
            "Snow Shot provides a local, same-user screenshot workflow. ".to_owned()
                + "The application must already be running with MCP enabled.",
        )
    }

    fn list_tools(
        &self,
        _request: Option<PaginatedRequestParams>,
        _context: RequestContext<RoleServer>,
    ) -> impl std::future::Future<Output = Result<ListToolsResult, McpError>> + Send {
        std::future::ready(Ok(ListToolsResult::with_all_items(Self::tools())))
    }

    async fn call_tool(
        &self,
        request: CallToolRequestParams,
        context: RequestContext<RoleServer>,
    ) -> Result<CallToolResponse, McpError> {
        let name = request.name.to_string();
        if !TOOLS.iter().any(|(tool, _, _)| *tool == name) {
            return Err(McpError::new(
                ErrorCode::METHOD_NOT_FOUND,
                format!("unknown Snow Shot tool: {name}"),
                None,
            ));
        }
        let arguments = request
            .arguments
            .map(Value::Object)
            .unwrap_or_else(|| json!({}));
        Ok(self.invoke(&name, arguments, context).await?.into())
    }
}
