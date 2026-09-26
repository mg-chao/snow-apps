use crate::wire::{
    AppRequest, AppResponse, Descriptor, MAX_FRAME_BYTES, PROTOCOL, read_frame_async,
    write_frame_async,
};
use interprocess::local_socket::{
    GenericFilePath, GenericNamespaced,
    prelude::*,
    tokio::{SendHalf, Stream},
    traits::tokio::Stream as _,
};
use serde_json::Value;
use std::{
    collections::{HashMap, HashSet, VecDeque},
    fs,
    io::Read,
    path::PathBuf,
    sync::{
        Arc, Mutex,
        atomic::{AtomicBool, Ordering},
    },
    time::Duration,
};
use tokio::{
    sync::{Mutex as AsyncMutex, broadcast, oneshot},
    task::AbortHandle,
    time::timeout,
};
use tokio_util::sync::CancellationToken;
#[derive(Debug, thiserror::Error, Clone)]
pub enum AppClientError {
    #[error(
        "Snow Shot is not running or MCP is disabled. Start Snow Shot and enable MCP in Settings."
    )]
    Unavailable,
    #[error("Invalid or insecure Snow Shot MCP descriptor")]
    InvalidDescriptor,
    #[error("Snow Shot local protocol error")]
    Protocol,
    #[error(
        "Screenshot request timed out; its outcome may be unknown. Refresh the session state before editing."
    )]
    Timeout,
    #[error("Screenshot request canceled")]
    Canceled,
    #[error(
        "The local connection closed; the mutation was not replayed. Refresh the session state before editing."
    )]
    Disconnected,
    #[error("Snow Shot request capacity is full; retry after a pending request finishes")]
    QueueFull,
    #[error("Snow Shot request exceeds the maximum JSON size")]
    RequestTooLarge,
    #[error("This Snow Shot application does not support resource notifications")]
    EventsUnavailable,
}
impl AppClientError {
    pub fn code(&self) -> &'static str {
        match self {
            Self::Unavailable => "unavailable",
            Self::InvalidDescriptor => "invalid_descriptor",
            Self::Protocol => "protocol_error",
            Self::Timeout => "timeout",
            Self::Canceled => "canceled",
            Self::Disconnected => "disconnected",
            Self::QueueFull => "queue_full",
            Self::RequestTooLarge => "invalid_parameters",
            Self::EventsUnavailable => "unsupported_capability",
        }
    }
}
#[derive(Clone, Debug)]
pub struct AppReply {
    pub response: AppResponse,
    pub attachment: Vec<u8>,
    pub json_bytes: usize,
}
#[derive(Clone, Debug)]
pub struct AppEvent {
    pub uri: String,
}

type ReplySender = oneshot::Sender<Result<AppReply, AppClientError>>;
#[derive(Default)]
struct Requests {
    pending: HashMap<String, ReplySender>,
    controls: HashSet<String>,
    retired: VecDeque<String>,
}
impl Requests {
    fn retire(&mut self, id: String) {
        self.controls.remove(&id);
        if self.retired.len() >= 64 {
            self.retired.pop_front();
        }
        self.retired.push_back(id);
    }
}
struct Connection {
    writer: AsyncMutex<SendHalf>,
    requests: Mutex<Requests>,
    alive: AtomicBool,
    reader: Mutex<Option<AbortHandle>>,
    cancel_requests: bool,
    events_supported: bool,
}
impl Connection {
    async fn send(&self, request: &AppRequest) -> Result<(), AppClientError> {
        let result = timeout(Duration::from_secs(5), async {
            let mut writer = self.writer.lock().await;
            write_frame_async(&mut *writer, request, &[]).await
        })
        .await;
        if !matches!(result, Ok(Ok(()))) {
            self.abort_requests();
            return Err(AppClientError::Disconnected);
        }
        Ok(())
    }
    fn abort_requests(&self) {
        self.alive.store(false, Ordering::Release);
        if let Ok(mut requests) = self.requests.lock() {
            requests.controls.clear();
            for (_, sender) in requests.pending.drain() {
                let _ = sender.send(Err(AppClientError::Disconnected));
            }
        }
        if let Ok(reader) = self.reader.lock()
            && let Some(reader) = reader.as_ref()
        {
            reader.abort();
        }
    }
}
impl Drop for Connection {
    fn drop(&mut self) {
        if let Ok(reader) = self.reader.lock()
            && let Some(reader) = reader.as_ref()
        {
            reader.abort();
        }
    }
}
// Dropping a canceled MCP future must cancel the local operation too. The request remains
// retired until its late response arrives, so an unrelated/mismatched ID is still an error.
struct PendingGuard {
    connection: Arc<Connection>,
    id: String,
    session: Option<String>,
    armed: bool,
    legacy: bool,
}
impl Drop for PendingGuard {
    fn drop(&mut self) {
        if !self.armed {
            return;
        }
        if let Ok(mut requests) = self.connection.requests.lock()
            && requests.pending.remove(&self.id).is_some()
        {
            requests.retire(self.id.clone());
        }
        if !self.connection.cancel_requests && !self.legacy {
            return;
        }
        let Ok(id) = request_id() else { return };
        if let Ok(mut requests) = self.connection.requests.lock() {
            requests.retire(id.clone());
        }
        let cancel = AppRequest {
            protocol: PROTOCOL.into(),
            request_id: id,
            method: if self.connection.cancel_requests {
                "snow_shot_request_cancel"
            } else {
                "screenshot_cancel"
            }
            .into(),
            session_id: self.session.clone(),
            expected_revision: None,
            idempotency_key: String::new(),
            params: serde_json::json!({"request_id":self.id}),
        };
        let connection = self.connection.clone();
        tokio::spawn(async move {
            let _ = connection.send(&cancel).await;
        });
    }
}
#[derive(Clone)]
pub struct AppClient {
    descriptor_path: PathBuf,
    connection: Arc<AsyncMutex<Option<Arc<Connection>>>>,
    timeout: Duration,
    launch_enabled: bool,
    launch_attempted: Arc<AsyncMutex<bool>>,
    events: broadcast::Sender<AppEvent>,
}
impl AppClient {
    pub fn new() -> Self {
        Self {
            descriptor_path: std::env::var_os("SNOW_SHOT_MCP_DESCRIPTOR")
                .map(PathBuf::from)
                .unwrap_or_else(default_descriptor_path),
            connection: Arc::new(AsyncMutex::new(None)),
            timeout: Duration::from_secs(65),
            launch_enabled: false,
            launch_attempted: Arc::new(AsyncMutex::new(false)),
            events: broadcast::channel(128).0,
        }
    }
    pub fn with_launch_app(mut self, enabled: bool) -> Self {
        self.launch_enabled = enabled;
        self
    }
    pub fn subscribe_events(&self) -> broadcast::Receiver<AppEvent> {
        self.events.subscribe()
    }
    pub async fn connect_events(&self) -> Result<(), AppClientError> {
        if self.connect_or_launch().await?.events_supported {
            Ok(())
        } else {
            Err(AppClientError::EventsUnavailable)
        }
    }
    async fn connect_or_launch(&self) -> Result<Arc<Connection>, AppClientError> {
        match self.connect().await {
            Err(AppClientError::Unavailable) if self.launch_enabled => {}
            result => return result,
        }
        let mut attempted = self.launch_attempted.lock().await;
        if *attempted {
            return self.connect().await;
        }
        *attempted = true;
        let executable = std::env::current_exe().map_err(|_| AppClientError::Unavailable)?;
        let application = executable
            .parent()
            .ok_or(AppClientError::Unavailable)?
            .join(if cfg!(windows) {
                "snow_shot.exe"
            } else {
                "snow_shot"
            });
        if !application.is_file() {
            return Err(AppClientError::Unavailable);
        }
        let mut command = std::process::Command::new(application);
        command
            .stdin(std::process::Stdio::null())
            .stdout(std::process::Stdio::null())
            .stderr(std::process::Stdio::null());
        #[cfg(windows)]
        {
            use std::os::windows::process::CommandExt;
            command.creation_flags(0x08000000); // No helper console window.
        }
        let child = command.spawn().map_err(|_| AppClientError::Unavailable)?;
        // Reap the child without tying the application lifetime to this bridge.
        std::thread::spawn(move || {
            let mut child = child;
            let _ = child.wait();
        });
        // The application reads its saved MCP preference. Never override it or replay requests.
        let deadline = tokio::time::Instant::now() + Duration::from_secs(10);
        loop {
            match self.connect().await {
                Err(AppClientError::Unavailable) if tokio::time::Instant::now() < deadline => {
                    tokio::time::sleep(Duration::from_millis(100)).await
                }
                result => return result,
            }
        }
    }
    async fn connect(&self) -> Result<Arc<Connection>, AppClientError> {
        let mut slot = self.connection.lock().await;
        if let Some(connection) = slot.as_ref().filter(|c| c.alive.load(Ordering::Acquire)) {
            return Ok(connection.clone());
        }
        let file =
            fs::File::open(&self.descriptor_path).map_err(|_| AppClientError::Unavailable)?;
        let metadata = file
            .metadata()
            .map_err(|_| AppClientError::InvalidDescriptor)?;
        if !metadata.is_file() || metadata.len() > 16384 {
            return Err(AppClientError::InvalidDescriptor);
        }
        #[cfg(unix)]
        {
            use std::os::unix::fs::PermissionsExt;
            if metadata.permissions().mode() & 0o077 != 0 {
                return Err(AppClientError::InvalidDescriptor);
            }
        }
        let mut bytes = Vec::new();
        file.take(16385)
            .read_to_end(&mut bytes)
            .map_err(|_| AppClientError::InvalidDescriptor)?;
        if bytes.len() > 16384 {
            return Err(AppClientError::InvalidDescriptor);
        }
        let descriptor: Descriptor =
            serde_json::from_slice(&bytes).map_err(|_| AppClientError::InvalidDescriptor)?;
        validate_descriptor(&descriptor)?;
        #[cfg(not(windows))]
        if PathBuf::from(&descriptor.socket).parent() != self.descriptor_path.parent() {
            return Err(AppClientError::InvalidDescriptor);
        }
        let name = if cfg!(windows) {
            descriptor.socket.as_str().to_ns_name::<GenericNamespaced>()
        } else {
            descriptor.socket.as_str().to_fs_name::<GenericFilePath>()
        }
        .map_err(|_| AppClientError::InvalidDescriptor)?;
        let stream = timeout(Duration::from_secs(5), async {
            let mut stream = Stream::connect(name)
                .await
                .map_err(|_| AppClientError::Unavailable)?;
            let handshake = AppRequest {
                protocol: PROTOCOL.into(),
                request_id: request_id()?,
                method: "handshake".into(),
                session_id: None,
                expected_revision: None,
                idempotency_key: String::new(),
                params: serde_json::json!({"token":descriptor.token,"client_protocol":PROTOCOL,"capabilities":["events","cancel_request"]}),
            };
            write_frame_async(&mut stream, &handshake, &[])
                .await
                .map_err(|_| AppClientError::Unavailable)?;
            let frame = read_frame_async(&mut stream)
                .await
                .map_err(|_| AppClientError::Protocol)?
                .ok_or(AppClientError::Protocol)?;
            let response: AppResponse =
                serde_json::from_value(frame.value).map_err(|_| AppClientError::Protocol)?;
            validate_response(&response, &handshake.request_id, frame.attachment.len())?;
            if !response.ok
                || response.result.get("protocol").and_then(Value::as_str) != Some(PROTOCOL)
            {
                return Err(AppClientError::Protocol);
            }
            let capabilities=response.result.get("capabilities").and_then(Value::as_array);
            let supports=|name:&str|capabilities.is_some_and(|caps|caps.iter().any(|v|v.as_str()==Some(name)));
            Ok((stream,supports("events"),supports("cancel_request")))
        })
        .await
        .map_err(|_| AppClientError::Timeout)??;
        let (stream, events_supported, cancel_requests) = stream;
        let (mut reader, writer) = stream.split();
        let connection = Arc::new(Connection {
            writer: AsyncMutex::new(writer),
            requests: Mutex::new(Requests::default()),
            alive: AtomicBool::new(true),
            reader: Mutex::new(None),
            cancel_requests,
            events_supported,
        });
        let events = self.events.clone();
        let weak = Arc::downgrade(&connection);
        let task = tokio::spawn(async move {
            loop {
                // Idle connections may remain open indefinitely; a partial response has a deadline.
                let mut first = [0u8; 1];
                if tokio::io::AsyncReadExt::read_exact(&mut reader, &mut first)
                    .await
                    .is_err()
                {
                    break;
                }
                let mut prefixed =
                    tokio::io::AsyncReadExt::chain(std::io::Cursor::new(first), &mut reader);
                let Ok(Ok(Some(frame))) =
                    timeout(Duration::from_secs(10), read_frame_async(&mut prefixed)).await
                else {
                    break;
                };
                if frame.value.get("kind").and_then(Value::as_str) == Some("event") {
                    if !events_supported
                        || !frame.attachment.is_empty()
                        || frame.value.get("protocol").and_then(Value::as_str) != Some(PROTOCOL)
                        || frame.value.get("event").and_then(Value::as_str)
                            != Some("resource_changed")
                    {
                        break;
                    }
                    let Some(uri) = frame
                        .value
                        .get("uri")
                        .and_then(Value::as_str)
                        .filter(|u| u.starts_with("snow-shot://") && u.len() <= 1024)
                    else {
                        break;
                    };
                    let _ = events.send(AppEvent {
                        uri: uri.to_owned(),
                    });
                    continue;
                }
                let Ok(response) = serde_json::from_value::<AppResponse>(frame.value) else {
                    break;
                };
                if validate_response(&response, &response.request_id, frame.attachment.len())
                    .is_err()
                {
                    break;
                }
                let Some(connection) = weak.upgrade() else {
                    break;
                };
                let Ok(mut requests) = connection.requests.lock() else {
                    break;
                };
                if let Some(sender) = requests.pending.remove(&response.request_id) {
                    requests.controls.remove(&response.request_id);
                    let _ = sender.send(Ok(AppReply {
                        response,
                        attachment: frame.attachment,
                        json_bytes: frame.json_bytes,
                    }));
                } else if let Some(index) = requests
                    .retired
                    .iter()
                    .position(|id| id == &response.request_id)
                {
                    requests.retired.remove(index);
                } else {
                    break;
                }
            }
            // Owned handles expire on disconnect. Invalidate subscriptions without
            // transmitting private state, even when no request was pending.
            let _ = events.send(AppEvent { uri: String::new() });
            if let Some(connection) = weak.upgrade() {
                connection.abort_requests();
            }
        });
        *connection
            .reader
            .lock()
            .map_err(|_| AppClientError::Disconnected)? = Some(task.abort_handle());
        *slot = Some(connection.clone());
        Ok(connection)
    }
    pub async fn request(
        &self,
        method: &str,
        session_id: Option<String>,
        expected_revision: Option<u64>,
        params: Value,
        canceled: CancellationToken,
    ) -> Result<AppReply, AppClientError> {
        let id = request_id()?;
        let key = params
            .get("idempotency_key")
            .and_then(Value::as_str)
            .unwrap_or(&id)
            .to_owned();
        let request = AppRequest {
            protocol: PROTOCOL.into(),
            request_id: id.clone(),
            method: method.into(),
            session_id: session_id.clone(),
            expected_revision,
            idempotency_key: key,
            params,
        };
        if serde_json::to_vec(&request)
            .map_err(|_| AppClientError::Protocol)?
            .len()
            > crate::wire::MAX_REQUEST_JSON_BYTES
        {
            return Err(AppClientError::RequestTooLarge);
        }
        // Only connection establishment may be retried. No application request is replayed.
        let connection = tokio::select! {
            _=canceled.cancelled()=>return Err(AppClientError::Canceled),
            result=self.connect_or_launch()=>result?,
        };
        let (sender, receiver) = oneshot::channel();
        {
            let mut requests = connection
                .requests
                .lock()
                .map_err(|_| AppClientError::Disconnected)?;
            let control = is_control(method);
            if (control && requests.controls.len() >= 4)
                || (!control && requests.pending.len() - requests.controls.len() >= 8)
            {
                return Err(AppClientError::QueueFull);
            }
            if control {
                requests.controls.insert(id.clone());
            }
            requests.pending.insert(id.clone(), sender);
        }
        let mut guard = PendingGuard {
            connection: connection.clone(),
            id,
            session: session_id,
            armed: true,
            legacy: method.starts_with("screenshot_") || method == "snow_shot_status",
        };
        connection.send(&request).await?;
        let reply = tokio::select! {
            _=canceled.cancelled()=>return Err(AppClientError::Canceled),
            result=timeout(self.timeout,receiver)=>result.map_err(|_|AppClientError::Timeout)?.map_err(|_|AppClientError::Disconnected)?,
        };
        guard.armed = false;
        let reply = reply?;
        if (method == "snow_shot_artifact_read" && reply.attachment.len() > 262144)
            || (method != "snow_shot_artifact_read"
                && !reply.attachment.is_empty()
                && reply.response.attachment_mime.as_deref() != Some("image/png"))
        {
            return Err(AppClientError::Protocol);
        }
        Ok(reply)
    }
}
fn is_control(method: &str) -> bool {
    matches!(
        method,
        "snow_shot_status"
            | "screenshot_state"
            | "screenshot_cancel"
            | "screenshot_operation"
            | "snow_shot_app_status"
            | "snow_shot_document_state"
            | "snow_shot_job_get"
            | "snow_shot_job_cancel"
            | "snow_shot_recording_state"
            | "snow_shot_recording_control"
    )
}
fn validate_descriptor(d: &Descriptor) -> Result<(), AppClientError> {
    if d.protocol != PROTOCOL
        || d.max_frame_bytes != MAX_FRAME_BYTES
        || d.pid == 0
        || (d.generation.len() != 36
            || !d.generation.bytes().enumerate().all(|(i, b)| {
                if [8, 13, 18, 23].contains(&i) {
                    b == b'-'
                } else {
                    b.is_ascii_hexdigit()
                }
            }))
        || d.token.len() != 64
        || !d.token.bytes().all(|b| b.is_ascii_hexdigit())
        || d.socket.is_empty()
        || d.socket.len() > 240
    {
        return Err(AppClientError::InvalidDescriptor);
    }
    #[cfg(windows)]
    if !d.socket.starts_with("snow-shot-mcp-")
        || !d
            .socket
            .bytes()
            .all(|b| b.is_ascii_alphanumeric() || b == b'-')
    {
        return Err(AppClientError::InvalidDescriptor);
    }
    Ok(())
}
fn validate_response(r: &AppResponse, id: &str, attachment: usize) -> Result<(), AppClientError> {
    if r.protocol != PROTOCOL
        || r.request_id != id
        || id.is_empty()
        || r.attachment_length != attachment
        || !r.result.is_object()
        || (attachment > 0 && !r.attachment_mime.as_deref().is_some_and(valid_mime))
    {
        return Err(AppClientError::Protocol);
    }
    Ok(())
}
fn valid_mime(value: &str) -> bool {
    let essence = value.split(';').next().unwrap_or_default().trim();
    value.len() <= 128
        && value.bytes().all(|b| b.is_ascii_graphic() || b == b' ')
        && essence.split_once('/').is_some_and(|(kind, subtype)| {
            !kind.is_empty()
                && !subtype.is_empty()
                && !subtype.contains('/')
                && kind
                    .bytes()
                    .chain(subtype.bytes())
                    .all(|b| b.is_ascii_alphanumeric() || b"!#$&^_.+-".contains(&b))
        })
}
fn request_id() -> Result<String, AppClientError> {
    let mut bytes = [0u8; 16];
    getrandom::fill(&mut bytes).map_err(|_| AppClientError::Unavailable)?;
    Ok(bytes.iter().map(|b| format!("{b:02x}")).collect())
}
fn default_descriptor_path() -> PathBuf {
    if cfg!(windows) {
        std::env::var_os("LOCALAPPDATA")
            .map(PathBuf::from)
            .unwrap_or_default()
            .join("SnowShot/mcp/snow-shot-mcp.json")
    } else if cfg!(target_os = "macos") {
        std::env::var_os("HOME")
            .map(PathBuf::from)
            .unwrap_or_default()
            .join("Library/Application Support/SnowShot/mcp/snow-shot-mcp.json")
    } else {
        std::env::var_os("XDG_DATA_HOME")
            .map(PathBuf::from)
            .unwrap_or_else(|| {
                std::env::var_os("HOME")
                    .map(PathBuf::from)
                    .unwrap_or_default()
                    .join(".local/share")
            })
            .join("SnowShot/mcp/snow-shot-mcp.json")
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::wire::{read_frame, write_frame};
    use interprocess::local_socket::{ListenerOptions, Stream};
    use serde_json::json;
    fn response(request: &AppRequest) -> AppResponse {
        AppResponse {
            protocol: PROTOCOL.into(),
            request_id: request.request_id.clone(),
            ok: true,
            session_id: request.session_id.clone(),
            revision: Some(1),
            result: json!({"protocol":PROTOCOL}),
            error: None,
            attachment_mime: None,
            attachment_length: 0,
        }
    }
    fn fixture(
        handler: impl FnOnce(Stream) + Send + 'static,
    ) -> (AppClient, std::thread::JoinHandle<()>, PathBuf) {
        fixture_with_capabilities(handler, &[])
    }
    fn fixture_with_capabilities(
        handler: impl FnOnce(Stream) + Send + 'static,
        capabilities: &[&str],
    ) -> (AppClient, std::thread::JoinHandle<()>, PathBuf) {
        let capabilities: Vec<String> = capabilities.iter().map(|s| (*s).to_owned()).collect();
        let id = request_id().unwrap();
        let directory = std::env::temp_dir().join(format!("snow-shot-mcp-test-{id}"));
        fs::create_dir(&directory).unwrap();
        let socket = if cfg!(windows) {
            format!("snow-shot-mcp-{id}")
        } else {
            directory.join("socket").to_string_lossy().into_owned()
        };
        let name = if cfg!(windows) {
            socket.as_str().to_ns_name::<GenericNamespaced>()
        } else {
            socket.as_str().to_fs_name::<GenericFilePath>()
        }
        .unwrap();
        let listener = ListenerOptions::new().name(name).create_sync().unwrap();
        let descriptor = Descriptor {
            protocol: PROTOCOL.into(),
            socket,
            token: "a".repeat(64),
            pid: std::process::id(),
            generation: "11111111-1111-4111-8111-111111111111".into(),
            max_frame_bytes: MAX_FRAME_BYTES,
        };
        let path = directory.join("snow-shot-mcp.json");
        fs::write(&path, serde_json::to_vec(&descriptor).unwrap()).unwrap();
        #[cfg(unix)]
        {
            use std::os::unix::fs::PermissionsExt;
            fs::set_permissions(&path, fs::Permissions::from_mode(0o600)).unwrap();
        }
        let client = AppClient {
            descriptor_path: path,
            connection: Arc::new(AsyncMutex::new(None)),
            timeout: Duration::from_millis(250),
            launch_enabled: false,
            launch_attempted: Arc::new(AsyncMutex::new(false)),
            events: broadcast::channel(128).0,
        };
        let thread = std::thread::spawn(move || {
            let mut stream = listener.accept().unwrap();
            let frame = read_frame(&mut stream).unwrap().unwrap();
            let hello: AppRequest = serde_json::from_slice(&frame.json).unwrap();
            assert_eq!(hello.method, "handshake");
            assert_eq!(hello.params["token"], "a".repeat(64));
            let mut reply = response(&hello);
            reply.result["capabilities"] = json!(capabilities);
            write_frame(&mut stream, &reply, &[]).unwrap();
            handler(stream);
        });
        (client, thread, directory)
    }
    fn request(stream: &mut Stream) -> AppRequest {
        serde_json::from_slice(&read_frame(stream).unwrap().unwrap().json).unwrap()
    }
    #[tokio::test]
    async fn persistent_connection_preserves_ids_and_does_not_replay_mutations() {
        let (client, thread, dir) = fixture(|mut stream| {
            let one = request(&mut stream);
            assert_eq!(one.method, "screenshot_begin");
            write_frame(&mut stream, &response(&one), &[]).unwrap();
            let two = request(&mut stream);
            assert_eq!(two.method, "screenshot_set_selection");
            assert_ne!(one.request_id, two.request_id);
            assert!(!two.idempotency_key.is_empty());
            // Close after accepting a mutation. The client must surface the ambiguous outcome.
        });
        assert!(
            client
                .request(
                    "screenshot_begin",
                    None,
                    None,
                    json!({}),
                    CancellationToken::new()
                )
                .await
                .is_ok()
        );
        assert!(matches!(
            client
                .request(
                    "screenshot_set_selection",
                    Some("session".into()),
                    Some(1),
                    json!({}),
                    CancellationToken::new()
                )
                .await,
            Err(AppClientError::Disconnected)
        ));
        tokio::task::spawn_blocking(move || thread.join().unwrap())
            .await
            .unwrap();
        drop(client);
        fs::remove_dir_all(dir).unwrap();
    }
    #[tokio::test]
    async fn status_and_cancel_capacity_survive_work_saturation() {
        let (filled_tx, filled_rx) = oneshot::channel();
        let (mut client, thread, dir) = fixture(move |mut stream| {
            let work: Vec<_> = (0..8).map(|_| request(&mut stream)).collect();
            filled_tx.send(()).unwrap();
            let status = request(&mut stream);
            assert_eq!(status.method, "snow_shot_status");
            write_frame(&mut stream, &response(&status), &[]).unwrap();
            let cancel = request(&mut stream);
            assert_eq!(cancel.method, "screenshot_cancel");
            write_frame(&mut stream, &response(&cancel), &[]).unwrap();
            for item in work {
                write_frame(&mut stream, &response(&item), &[]).unwrap();
            }
        });
        client.timeout = Duration::from_secs(5);
        let mut workers = Vec::new();
        for _ in 0..8 {
            let client = client.clone();
            workers.push(tokio::spawn(async move {
                client
                    .request(
                        "screenshot_render",
                        Some("s".into()),
                        Some(1),
                        json!({}),
                        CancellationToken::new(),
                    )
                    .await
            }));
        }
        filled_rx.await.unwrap();
        assert!(matches!(
            client
                .request(
                    "screenshot_render",
                    Some("s".into()),
                    Some(1),
                    json!({}),
                    CancellationToken::new()
                )
                .await,
            Err(AppClientError::QueueFull)
        ));
        for method in ["snow_shot_status", "screenshot_cancel"] {
            assert!(
                client
                    .request(method, None, None, json!({}), CancellationToken::new())
                    .await
                    .is_ok()
            );
        }
        for worker in workers {
            assert!(worker.await.unwrap().is_ok());
        }
        tokio::task::spawn_blocking(move || thread.join().unwrap())
            .await
            .unwrap();
        drop(client);
        fs::remove_dir_all(dir).unwrap();
    }
    #[tokio::test]
    async fn oversized_request_fails_before_discovery_or_connection() {
        let mut client = AppClient::new();
        client.descriptor_path =
            std::env::temp_dir().join(format!("absent-{}", request_id().unwrap()));
        assert!(matches!(
            client
                .request(
                    "snow_shot_settings_update",
                    None,
                    Some(1),
                    json!({"values":{"text":"x".repeat(crate::wire::MAX_REQUEST_JSON_BYTES)}}),
                    CancellationToken::new()
                )
                .await,
            Err(AppClientError::RequestTooLarge)
        ));
    }
    #[tokio::test]
    async fn negotiated_events_interleave_with_responses_and_cancel_new_domain_requests() {
        let (client, thread, dir) = fixture_with_capabilities(
            |mut stream| {
                let pending = request(&mut stream);
                assert_eq!(pending.method, "snow_shot_document_render");
                write_frame(&mut stream,&json!({"protocol":PROTOCOL,"kind":"event","event":"resource_changed","uri":"snow-shot://documents/one","attachment_length":0}),&[]).unwrap();
                let mut canceled = false;
                for _ in 0..2 {
                    let control = request(&mut stream);
                    if control.method == "snow_shot_request_cancel" {
                        assert_eq!(control.params["request_id"], pending.request_id);
                        canceled = true;
                        write_frame(&mut stream, &response(&pending), &[]).unwrap();
                    } else {
                        assert_eq!(control.method, "snow_shot_status");
                    }
                    write_frame(&mut stream, &response(&control), &[]).unwrap();
                }
                assert!(canceled);
            },
            &["events", "cancel_request"],
        );
        let mut events = client.subscribe_events();
        assert!(matches!(
            client
                .request(
                    "snow_shot_document_render",
                    None,
                    Some(1),
                    json!({"document_id":"one"}),
                    CancellationToken::new()
                )
                .await,
            Err(AppClientError::Timeout)
        ));
        assert_eq!(
            events.recv().await.unwrap().uri,
            "snow-shot://documents/one"
        );
        assert!(
            client
                .request(
                    "snow_shot_status",
                    None,
                    None,
                    json!({}),
                    CancellationToken::new()
                )
                .await
                .is_ok()
        );
        tokio::task::spawn_blocking(move || thread.join().unwrap())
            .await
            .unwrap();
        drop(client);
        fs::remove_dir_all(dir).unwrap();
    }
    #[tokio::test]
    async fn timeout_propagates_request_cancellation() {
        let (client, thread, dir) = fixture(|mut stream| {
            let pending = request(&mut stream);
            let cancel = request(&mut stream);
            assert_eq!(cancel.method, "screenshot_cancel");
            assert_eq!(cancel.params["request_id"], pending.request_id);
        });
        assert!(matches!(
            client
                .request(
                    "screenshot_render",
                    Some("session".into()),
                    Some(1),
                    json!({}),
                    CancellationToken::new()
                )
                .await,
            Err(AppClientError::Timeout)
        ));
        tokio::task::spawn_blocking(move || thread.join().unwrap())
            .await
            .unwrap();
        drop(client);
        fs::remove_dir_all(dir).unwrap();
    }
    #[test]
    fn descriptor_and_protocol_reject_invalid_values() {
        assert!(valid_mime("application/json"));
        assert!(valid_mime("text/plain; charset=\"utf-8\""));
        for invalid in [
            "image",
            "text/",
            "/json",
            "a/b/c",
            "text/plain\r\nInjected:yes",
        ] {
            assert!(!valid_mime(invalid));
        }
        let mut d = Descriptor {
            protocol: PROTOCOL.into(),
            socket: "snow-shot-mcp-test".into(),
            token: "f".repeat(64),
            pid: 1,
            generation: "11111111-1111-4111-8111-111111111111".into(),
            max_frame_bytes: MAX_FRAME_BYTES,
        };
        assert!(validate_descriptor(&d).is_ok());
        d.token = "short".into();
        assert!(validate_descriptor(&d).is_err());
        let request = AppRequest {
            protocol: PROTOCOL.into(),
            request_id: "id".into(),
            method: "status".into(),
            session_id: None,
            expected_revision: None,
            idempotency_key: String::new(),
            params: json!({}),
        };
        let mut r = response(&request);
        assert!(validate_response(&r, "other", 0).is_err());
        r.protocol = "future".into();
        assert!(validate_response(&r, "id", 0).is_err());
    }
}
