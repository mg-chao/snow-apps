use crate::app_client::AppClientError;
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
use std::{
    borrow::Cow,
    sync::{Arc, LazyLock},
};
#[path = "discovery.rs"]
mod discovery;
#[path = "subscriptions.rs"]
mod subscriptions;
#[path = "tasks.rs"]
mod tasks;

pub(crate) const TOOLS: &[(&str, &str, bool)] = &[
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
    (
        "screenshot_set_selection_style",
        "Set selection decoration and aspect ratio lock.",
        false,
    ),
    (
        "screenshot_set_tool_style",
        "Apply a partial toolbar style to the selected elements and drawing tool.",
        false,
    ),
    (
        "screenshot_edit_elements",
        "Edit selected screenshot elements using typed toolbar actions.",
        false,
    ),
    (
        "screenshot_recapture",
        "Replace the screenshot capture while retaining this session.",
        false,
    ),
    (
        "screenshot_scrolling",
        "Control scrolling capture, axis, automatic scrolling, crop, and position.",
        false,
    ),
    (
        "screenshot_scroll_once",
        "Scroll one wheel notch and wait for a processed, stable scrolling snapshot.",
        false,
    ),
    (
        "screenshot_recognize",
        "Start text, table, QR, Markdown, or HTML recognition; poll the returned operation ID.",
        false,
    ),
    (
        "screenshot_translate",
        "Translate recognized text with the configured provider; poll the returned operation ID.",
        false,
    ),
    (
        "screenshot_auto_filter",
        "Detect and filter sensitive information categories; poll the returned operation ID.",
        false,
    ),
    (
        "screenshot_operation",
        "Read status and typed results for a screenshot operation.",
        true,
    ),
    (
        "screenshot_edit_recognition",
        "Edit recognized text and table cells.",
        false,
    ),
    (
        "screenshot_export_recognition",
        "Return, copy, or save recognized content without dialogs.",
        false,
    ),
    (
        "screenshot_draw_template",
        "Export selected drawing elements or insert a validated drawing template.",
        false,
    ),
];

#[derive(Clone)]
pub struct SnowShotMcp {
    client: AppClient,
    subscriptions: Arc<subscriptions::Subscriptions>,
}

impl SnowShotMcp {
    pub fn new() -> Self {
        Self {
            client: AppClient::new(),
            subscriptions: Arc::new(subscriptions::Subscriptions::default()),
        }
    }
    pub fn with_launch_app(mut self, enabled: bool) -> Self {
        self.client = self.client.with_launch_app(enabled);
        self
    }

    fn tools() -> Vec<Tool> {
        static CATALOG: LazyLock<Vec<Tool>> = LazyLock::new(|| {
            TOOLS
                .iter()
                .chain(schemas::domains::TOOLS.iter())
                .map(|(name, description, read_only)| {
                    let mut tool = Tool::new(
                        Cow::Borrowed(*name),
                        Cow::Borrowed(*description),
                        Arc::new(schemas::schema(name, None).expect("static tool schema")),
                    )
                    .with_annotations(ToolAnnotations::from_raw(
                        None,
                        Some(*read_only),
                        Some(!read_only && destructive(name)),
                        Some(*read_only),
                        Some(open_world(name)),
                    ));
                    tool.output_schema = Some(Arc::new(output_schema()));
                    tool
                })
                .collect()
        });
        CATALOG.clone()
    }

    async fn invoke(
        &self,
        name: &str,
        arguments: Value,
        context: RequestContext<RoleServer>,
    ) -> Result<CallToolResult, McpError> {
        schemas::schema(name, Some(arguments.clone())).map_err(|error| {
            McpError::invalid_params(
                "Invalid Snow Shot tool arguments",
                Some(json!({"detail":error.to_string()})),
            )
        })?;
        let session_id = arguments
            .get("session_id")
            .and_then(Value::as_str)
            .map(str::to_owned);
        let expected_revision = arguments.get("expected_revision").and_then(Value::as_u64);
        let tool_timer = std::time::Instant::now();
        let progress = if matches!(
            name,
            "snow_shot_job_get"
                | "snow_shot_job_cancel"
                | "snow_shot_document_recognize"
                | "snow_shot_document_auto_filter"
                | "snow_shot_translation_start"
                | "snow_shot_updates_action"
                | "snow_shot_document_open"
                | "snow_shot_settings_action"
                | "snow_shot_settings_update"
        ) {
            None
        } else {
            context.meta.get_progress_token()
        };
        if let Some(token) = progress.clone() {
            let _ = context
                .peer
                .notify_progress(
                    rmcp::model::ProgressNotificationParam::new(token, 0.0)
                        .with_message("Dispatching the local application request."),
                )
                .await;
        }
        let reply = self
            .client
            .request(
                name,
                session_id,
                expected_revision,
                arguments,
                context.ct.clone(),
            )
            .await;
        if !context.ct.is_cancelled()
            && let Some(token) = progress
        {
            let _ = context
                .peer
                .notify_progress(
                    rmcp::model::ProgressNotificationParam::new(token, 1.0)
                        .with_total(1.0)
                        .with_message("Local application request completed."),
                )
                .await;
        }
        match reply {
            Ok(reply) => {
                let artifact_chunk = name == "snow_shot_artifact_read";
                if reply.json_bytes + reply.attachment.len() > 256 * 1024 {
                    static ASSEMBLY: tokio::sync::Semaphore = tokio::sync::Semaphore::const_new(2);
                    let permit = ASSEMBLY
                        .acquire()
                        .await
                        .map_err(|_| McpError::internal_error("Output worker unavailable", None))?;
                    tokio::task::spawn_blocking(move || {
                        let _permit = permit;
                        assemble_reply(reply, tool_timer, artifact_chunk)
                    })
                    .await
                    .map_err(|_| McpError::internal_error("Output worker failed", None))?
                } else {
                    assemble_reply(reply, tool_timer, artifact_chunk)
                }
            }
            Err(error) => {
                let reachable = match error {
                    AppClientError::Unavailable => Some(false),
                    AppClientError::QueueFull => Some(true),
                    _ => None,
                };
                let structured = json!({"reachable":reachable,"mcp_enabled":null,"error":{"code":error.code(),"message":error.to_string()}});
                let mut result = if matches!(name, "snow_shot_status" | "snow_shot_app_status") {
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

fn assemble_reply(
    mut reply: crate::app_client::AppReply,
    timer: std::time::Instant,
    artifact_chunk: bool,
) -> Result<CallToolResult, McpError> {
    let image = if reply.response.ok && artifact_chunk {
        reply.response.result["data_base64"] = json!(STANDARD.encode(reply.attachment));
        reply.response.attachment_length = 0;
        reply.response.attachment_mime = None;
        None
    } else if reply.response.ok && !reply.attachment.is_empty() {
        Some(ContentBlock::image(
            STANDARD.encode(reply.attachment),
            reply
                .response
                .attachment_mime
                .as_deref()
                .unwrap_or("image/png"),
        ))
    } else {
        None
    };
    if let Some(object) = reply.response.result.as_object_mut() {
        object.insert(
            "bridge_total_ms".into(),
            json!(timer.elapsed().as_secs_f64() * 1000.0),
        );
    }
    // Move potentially large recognition data instead of serializing/cloning it twice.
    let application_result = std::mem::take(&mut reply.response.result);
    let mut structured = serde_json::to_value(&reply.response)
        .map_err(|_| McpError::internal_error("Invalid local response", None))?;
    structured["result"] = application_result;
    let mut content = vec![ContentBlock::text(structured.to_string())];
    if let Some(image) = image {
        content.push(image);
    }
    let mut result = if reply.response.ok {
        CallToolResult::success(content)
    } else {
        CallToolResult::error(content)
    };
    result.structured_content = Some(structured);
    Ok(result)
}

impl Default for SnowShotMcp {
    fn default() -> Self {
        Self::new()
    }
}

fn destructive(name: &str) -> bool {
    !matches!(
        name,
        "screenshot_begin"
            | "screenshot_recognize"
            | "screenshot_translate"
            | "screenshot_pin"
            | "snow_shot_document_open"
            | "snow_shot_document_clone"
            | "snow_shot_document_recognize"
            | "snow_shot_pinned_create"
            | "snow_shot_recording_start"
            | "snow_shot_translation_start"
            | "snow_shot_document_pin"
            | "snow_shot_document_present"
            | "snow_shot_history_action"
            | "snow_shot_permissions_request"
    )
}
fn open_world(name: &str) -> bool {
    matches!(
        name,
        "screenshot_recognize"
            | "screenshot_translate"
            | "screenshot_auto_filter"
            | "snow_shot_document_recognize"
            | "snow_shot_document_auto_filter"
            | "snow_shot_pinned_edit"
            | "snow_shot_updates_action"
            | "snow_shot_translation_start"
    )
}
fn output_schema() -> serde_json::Map<String, Value> {
    json!({"type":"object","oneOf":[
        {"required":["protocol","request_id","ok","result","attachment_length"],"properties":{
            "protocol":{"type":"string"},"request_id":{"type":"string"},"ok":{"type":"boolean"},
            "session_id":{"type":"string"},"revision":{"type":"integer","minimum":0},
            "result":{},"attachment_length":{"type":"integer","minimum":0},
            "error":{"type":"object","required":["code","message"],"properties":{"code":{"type":"string"},"message":{"type":"string"},"details":{}}}}},
        {"required":["reachable","mcp_enabled","error"],"properties":{
            "reachable":{"type":["boolean","null"]},"mcp_enabled":{"type":["boolean","null"]},
            "error":{"type":"object","required":["code","message"],"properties":{"code":{"type":"string"},"message":{"type":"string"}}}}}
    ]}).as_object().unwrap().clone()
}

impl ServerHandler for SnowShotMcp {
    fn get_info(&self) -> ServerConfig {
        ServerConfig::new(ServerCapabilities::builder().enable_tools().enable_resources().enable_resources_subscribe().enable_prompts().enable_completions().enable_tasks().build()).with_instructions(
            "Snow Shot provides local application, screenshot, background document, recording and pinned-image workflows. ".to_owned()
                + "Start with snow_shot_app_status. Keep returned resource IDs and revisions; refresh after conflicts. Background documents are silent and client-owned. The application must have MCP enabled. Credentials are write-only.",
        )
    }

    fn list_tools(
        &self,
        request: Option<PaginatedRequestParams>,
        _context: RequestContext<RoleServer>,
    ) -> impl std::future::Future<Output = Result<ListToolsResult, McpError>> + Send {
        std::future::ready(discovery::reject_cursor(request).map(|_| {
            ListToolsResult::with_all_items(Self::tools())
                .with_ttl_ms(300000)
                .with_cache_scope(rmcp::model::CacheScope::Public)
        }))
    }

    async fn list_resources(
        &self,
        request: Option<PaginatedRequestParams>,
        _context: RequestContext<RoleServer>,
    ) -> Result<rmcp::model::ListResourcesResult, McpError> {
        discovery::reject_cursor(request)?;
        Ok(discovery::resources())
    }
    async fn list_resource_templates(
        &self,
        request: Option<PaginatedRequestParams>,
        _context: RequestContext<RoleServer>,
    ) -> Result<rmcp::model::ListResourceTemplatesResult, McpError> {
        discovery::reject_cursor(request)?;
        Ok(discovery::templates())
    }
    async fn read_resource(
        &self,
        request: rmcp::model::ReadResourceRequestParams,
        context: RequestContext<RoleServer>,
    ) -> Result<rmcp::model::ReadResourceResponse, McpError> {
        let uri = request.uri;
        if uri == "snow-shot://capabilities" {
            return Ok(discovery::resource_result(&uri, json!({"tools":Self::tools(),"coordinate_system":"canvas","credentials":"write_only"}), true).into());
        }
        let (method, arguments) = discovery::resource_request(&uri)?;
        let result = self.invoke(method, arguments, context).await?;
        if result.is_error == Some(true) {
            return Err(McpError::invalid_params(
                "Snow Shot resource is unavailable",
                result.structured_content,
            ));
        }
        Ok(discovery::resource_result(
            &uri,
            result.structured_content.unwrap_or(Value::Null),
            false,
        )
        .into())
    }
    async fn list_prompts(
        &self,
        request: Option<PaginatedRequestParams>,
        _context: RequestContext<RoleServer>,
    ) -> Result<rmcp::model::ListPromptsResult, McpError> {
        discovery::reject_cursor(request)?;
        Ok(discovery::prompts())
    }
    async fn get_prompt(
        &self,
        request: rmcp::model::GetPromptRequestParams,
        _context: RequestContext<RoleServer>,
    ) -> Result<rmcp::model::GetPromptResponse, McpError> {
        Ok(discovery::prompt(request)?.into())
    }
    async fn complete(
        &self,
        request: rmcp::model::CompleteRequestParams,
        _context: RequestContext<RoleServer>,
    ) -> Result<rmcp::model::CompleteResult, McpError> {
        discovery::complete(request)
    }
    fn accepted_subscription_filter(
        &self,
        requested: &rmcp::model::SubscriptionFilter,
    ) -> Option<rmcp::model::SubscriptionFilter> {
        Some(subscriptions::accepted(requested))
    }
    async fn listen(&self, context: rmcp::service::SubscriptionContext) -> Result<(), McpError> {
        self.subscriptions.listen(&self.client, context).await
    }
    #[allow(deprecated)]
    async fn subscribe(
        &self,
        request: rmcp::model::SubscribeRequestParams,
        context: RequestContext<RoleServer>,
    ) -> Result<(), McpError> {
        self.subscriptions
            .subscribe(&self.client, request.uri, context)
            .await
    }
    #[allow(deprecated)]
    async fn unsubscribe(
        &self,
        request: rmcp::model::UnsubscribeRequestParams,
        _context: RequestContext<RoleServer>,
    ) -> Result<(), McpError> {
        self.subscriptions.unsubscribe(&request.uri)
    }
    async fn get_task(
        &self,
        request: rmcp::model::GetTaskParams,
        context: RequestContext<RoleServer>,
    ) -> Result<rmcp::model::GetTaskResult, McpError> {
        let result = self
            .invoke(
                "snow_shot_job_get",
                json!({"job_id":request.task_id}),
                context,
            )
            .await?;
        Ok(rmcp::model::GetTaskResult::new(tasks::detailed(result)?))
    }
    async fn cancel_task(
        &self,
        request: rmcp::model::CancelTaskParams,
        context: RequestContext<RoleServer>,
    ) -> Result<(), McpError> {
        let result = self
            .invoke(
                "snow_shot_job_cancel",
                json!({"job_id":request.task_id}),
                context,
            )
            .await?;
        tasks::successful(&result)?;
        Ok(())
    }
    async fn update_task(
        &self,
        request: rmcp::model::UpdateTaskParams,
        context: RequestContext<RoleServer>,
    ) -> Result<(), McpError> {
        // These jobs never request client input. Ignore unknown/already-satisfied responses,
        // while still enforcing existence and same-client ownership through the application.
        let result = self
            .invoke(
                "snow_shot_job_get",
                json!({"job_id":request.task_id}),
                context,
            )
            .await?;
        tasks::successful(&result)?;
        Ok(())
    }

    async fn call_tool(
        &self,
        request: CallToolRequestParams,
        context: RequestContext<RoleServer>,
    ) -> Result<CallToolResponse, McpError> {
        let name = request.name.to_string();
        if !TOOLS
            .iter()
            .chain(schemas::domains::TOOLS.iter())
            .any(|(tool, _, _)| *tool == name)
        {
            return Err(McpError::new(
                ErrorCode::METHOD_NOT_FOUND,
                format!("unknown Snow Shot tool: {name}"),
                None,
            ));
        }
        let mut arguments = request
            .arguments
            .map(Value::Object)
            .unwrap_or_else(|| json!({}));
        let task_capable = context.protocol_version()
            == Some(rmcp::model::ProtocolVersion::V_2026_07_28)
            && context
                .client_capabilities()
                .is_some_and(|c| c.supports_tasks());
        if task_capable && name == "snow_shot_document_open" {
            arguments["as_job"] = Value::Bool(true);
        }
        let result = self.invoke(&name, arguments, context.clone()).await?;
        if task_capable
            && !name.starts_with("snow_shot_job_")
            && result.is_error != Some(true)
            && result
                .structured_content
                .as_ref()
                .and_then(|s| s.get("result"))
                .and_then(|s| s.get("job_id"))
                .and_then(Value::as_str)
                .is_some()
        {
            let job_id = result.structured_content.as_ref().unwrap()["result"]["job_id"]
                .as_str()
                .unwrap();
            // Some application commands return only the handle plus committed field
            // metadata. Resolve the actual owned registry entry before creating a Task.
            let job = self
                .invoke("snow_shot_job_get", json!({"job_id":job_id}), context)
                .await?;
            return Ok(rmcp::model::CreateTaskResult::new(tasks::detailed(job)?.task).into());
        }
        Ok(result.into())
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    #[test]
    fn artifact_raw_chunks_become_json_data_without_image_content() {
        for bytes in [Vec::new(), br#"{"text":"owned"}"#.to_vec()] {
            let response = serde_json::from_value(json!({
                "protocol":crate::wire::PROTOCOL,"request_id":"one","ok":true,
                "result":{"artifact_id":"one","offset":0,"next_offset":bytes.len(),"eof":true},
                "attachment_mime":"application/json","attachment_length":bytes.len()
            }))
            .unwrap();
            let result = assemble_reply(
                crate::app_client::AppReply {
                    response,
                    attachment: bytes.clone(),
                    json_bytes: 200,
                },
                std::time::Instant::now(),
                true,
            )
            .unwrap();
            assert_eq!(result.content.len(), 1);
            let value = result.structured_content.unwrap();
            assert_eq!(value["result"]["data_base64"], STANDARD.encode(bytes));
            assert_eq!(value["attachment_length"], 0);
            assert!(value.get("attachment_mime").is_none());
        }
    }
}
