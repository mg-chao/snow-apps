//! Modern Tasks is a facade over the application's bounded, owner-scoped jobs.
//! It performs no background polling and retains no duplicate result payloads.
use rmcp::{ErrorData as McpError, model::*};
use serde_json::Value;

pub(super) fn successful(result: &CallToolResult) -> Result<(), McpError> {
    if result.is_error == Some(true) {
        return Err(McpError::invalid_params(
            "Snow Shot job is unavailable",
            result.structured_content.clone(),
        ));
    }
    Ok(())
}
pub(super) fn detailed(mut result: CallToolResult) -> Result<DetailedTask, McpError> {
    successful(&result)?;
    let job = result
        .structured_content
        .as_ref()
        .and_then(|v| v.get("result"))
        .ok_or_else(|| McpError::internal_error("Missing application job result", None))?;
    let text = |key| {
        job.get(key)
            .and_then(Value::as_str)
            .ok_or_else(|| McpError::internal_error("Invalid application job metadata", None))
    };
    let status = text("status")?;
    let task = Task::new(
        text("job_id")?,
        TaskStatus::Working,
        text("created_at")?,
        job.get("updated_at")
            .or_else(|| job.get("finished_at"))
            .and_then(Value::as_str)
            .unwrap_or(text("created_at")?),
    )
    .with_ttl_ms(job.get("ttl_ms").and_then(Value::as_u64).unwrap_or(900000))
    .with_poll_interval_ms(250);
    let payload = match status {
        "running" => TaskPayload::Working,
        "canceled" => TaskPayload::Cancelled,
        "completed" | "failed" => {
            // Application failures are tool errors, not JSON-RPC failures.
            result.is_error = Some(status == "failed");
            TaskPayload::Completed {
                result: serde_json::to_value(result)
                    .map_err(|_| McpError::internal_error("Invalid task result", None))?
                    .as_object()
                    .cloned()
                    .ok_or_else(|| McpError::internal_error("Invalid task result", None))?,
            }
        }
        _ => {
            return Err(McpError::internal_error(
                "Unknown application job status",
                None,
            ));
        }
    };
    Ok(DetailedTask::new(task, payload))
}

#[cfg(test)]
mod tests {
    use super::*;
    use serde_json::json;
    #[test]
    fn application_failures_are_completed_tool_errors_and_cancellation_is_distinct() {
        for (status, expected) in [
            ("running", TaskStatus::Working),
            ("completed", TaskStatus::Completed),
            ("failed", TaskStatus::Completed),
            ("canceled", TaskStatus::Cancelled),
        ] {
            let mut result = CallToolResult::success(vec![]);
            result.structured_content = Some(
                json!({"result":{"job_id":"one","status":status,"created_at":"2026-09-26T00:00:00Z"}}),
            );
            let task = detailed(result).unwrap();
            assert_eq!(task.status(), expected);
            if let TaskPayload::Completed { result } = task.payload {
                assert_eq!(result["isError"], status == "failed");
            }
        }
    }
}
