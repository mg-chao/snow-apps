use std::{collections::HashMap, sync::Arc};

use futures::TryStreamExt;
use gemini_rust::{Gemini, Model};
use serde::{Deserialize, Serialize};
use tauri::ipc::Channel;
use tokio::sync::Mutex;
use tokio_util::sync::CancellationToken;

#[derive(Default)]
pub struct GeminiRequestManager {
    pub tasks: Arc<Mutex<HashMap<String, CancellationToken>>>,
}

#[derive(Debug, Deserialize)]
#[serde(rename_all = "camelCase")]
pub struct GeminiMessagePayload {
    role: String,
    content: String,
}

#[derive(Debug, Deserialize)]
#[serde(rename_all = "camelCase")]
pub struct GeminiThinkingPayload {
    enabled: bool,
    include_thoughts: bool,
    #[serde(default)]
    budget_tokens: Option<i32>,
}

#[derive(Debug, Deserialize)]
#[serde(rename_all = "camelCase")]
pub struct GeminiStreamRequest {
    model: String,
    api_key: String,
    #[serde(default)]
    messages: Vec<GeminiMessagePayload>,
    #[serde(default)]
    temperature: Option<f32>,
    #[serde(default)]
    max_output_tokens: Option<i32>,
    #[serde(default)]
    thinking: Option<GeminiThinkingPayload>,
}

#[derive(Debug, Serialize)]
#[serde(tag = "type", rename_all = "snake_case")]
pub enum GeminiStreamEvent {
    Chunk {
        #[serde(skip_serializing_if = "Option::is_none")]
        content: Option<String>,
        #[serde(skip_serializing_if = "Option::is_none")]
        reasoning_content: Option<String>,
    },
    Done,
    Error { message: String },
    Aborted,
}

fn normalize_model(model: &str) -> String {
    if model.starts_with("models/") {
        model.to_string()
    } else {
        format!("models/{model}")
    }
}

#[tauri::command]
pub async fn gemini_generate_content_stream(
    state: tauri::State<'_, GeminiRequestManager>,
    request_id: String,
    request: GeminiStreamRequest,
    channel: Channel<GeminiStreamEvent>,
) -> Result<(), String> {
    if request.api_key.trim().is_empty() {
        return Err("Gemini API key is required".to_string());
    }

    let cancel_token = CancellationToken::new();
    state
        .tasks
        .lock()
        .await
        .insert(request_id.clone(), cancel_token.clone());

    let tasks_map = state.tasks.clone();
    tauri::async_runtime::spawn(async move {
        let result = run_gemini_stream(request, cancel_token, channel).await;
        tasks_map.lock().await.remove(&request_id);

        if let Err(err) = result {
            log::error!("[gemini_generate_content_stream] {}", err);
        }
    });

    Ok(())
}

#[tauri::command]
pub async fn gemini_cancel_stream(
    state: tauri::State<'_, GeminiRequestManager>,
    request_id: String,
) -> Result<(), String> {
    if let Some(token) = state.tasks.lock().await.remove(&request_id) {
        token.cancel();
    }
    Ok(())
}

async fn run_gemini_stream(
    request: GeminiStreamRequest,
    cancel_token: CancellationToken,
    channel: Channel<GeminiStreamEvent>,
) -> Result<(), String> {
    if request.messages.is_empty() {
        return Err("Gemini request requires at least one message".to_string());
    }

    let api_key = request.api_key.trim().to_string();
    let model_name = normalize_model(&request.model);

    let client =
        Gemini::with_model(api_key, Model::Custom(model_name)).map_err(|e| e.to_string())?;

    let mut builder = client.generate_content();
    let mut system_set = false;

    for message in &request.messages {
        let content = message.content.trim();
        if content.is_empty() {
            continue;
        }

        match message.role.as_str() {
            "system" => {
                if !system_set {
                    builder = builder.with_system_instruction(content.to_string());
                    system_set = true;
                } else {
                    builder = builder.with_user_message(content.to_string());
                }
            }
            "assistant" | "model" => {
                builder = builder.with_model_message(content.to_string());
            }
            _ => {
                builder = builder.with_user_message(content.to_string());
            }
        }
    }

    if let Some(temp) = request.temperature {
        builder = builder.with_temperature(temp);
    }

    if let Some(max_tokens) = request.max_output_tokens {
        builder = builder.with_max_output_tokens(max_tokens);
    }

    if let Some(thinking) = request.thinking {
        if thinking.enabled {
            if let Some(budget) = thinking.budget_tokens {
                builder = builder.with_thinking_budget(budget);
            } else {
                builder = builder.with_dynamic_thinking();
            }
            builder = builder.with_thoughts_included(thinking.include_thoughts);
        }
    }

    let mut stream = builder.execute_stream().await.map_err(|e| e.to_string())?;

    loop {
        tokio::select! {
            _ = cancel_token.cancelled() => {
                let _ = channel.send(GeminiStreamEvent::Aborted);
                break;
            }
            chunk = stream.try_next() => {
                match chunk {
                    Ok(Some(response)) => {
                        let mut content = String::new();
                        let mut reasoning = String::new();

                        for (text, is_thought) in response.all_text() {
                            if is_thought {
                                reasoning.push_str(&text);
                            } else {
                                content.push_str(&text);
                            }
                        }

                        if content.is_empty() && reasoning.is_empty() {
                            continue;
                        }

                        let content_payload = if content.is_empty() {
                            None
                        } else {
                            Some(content)
                        };
                        let reasoning_payload = if reasoning.is_empty() {
                            None
                        } else {
                            Some(reasoning)
                        };
                        let _ = channel.send(GeminiStreamEvent::Chunk {
                            content: content_payload,
                            reasoning_content: reasoning_payload,
                        });
                    }
                    Ok(None) => {
                        let _ = channel.send(GeminiStreamEvent::Done);
                        break;
                    }
                    Err(err) => {
                        let msg = err.to_string();
                        let _ = channel.send(GeminiStreamEvent::Error { message: msg.clone() });
                        return Err(msg);
                    }
                }
            }
        }
    }

    Ok(())
}

