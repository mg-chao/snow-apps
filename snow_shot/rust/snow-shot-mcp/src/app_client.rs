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
    collections::{HashMap, VecDeque},
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
    sync::{Mutex as AsyncMutex, oneshot},
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
        }
    }
}
#[derive(Clone, Debug)]
pub struct AppReply {
    pub response: AppResponse,
    pub attachment: Vec<u8>,
}

type ReplySender = oneshot::Sender<Result<AppReply, AppClientError>>;
#[derive(Default)]
struct Requests {
    pending: HashMap<String, ReplySender>,
    retired: VecDeque<String>,
}
impl Requests {
    fn retire(&mut self, id: String) {
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
        let Ok(id) = request_id() else { return };
        if let Ok(mut requests) = self.connection.requests.lock() {
            requests.retire(id.clone());
        }
        let cancel = AppRequest {
            protocol: PROTOCOL.into(),
            request_id: id,
            method: "screenshot_cancel".into(),
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
}
impl AppClient {
    pub fn new() -> Self {
        Self {
            descriptor_path: std::env::var_os("SNOW_SHOT_MCP_DESCRIPTOR")
                .map(PathBuf::from)
                .unwrap_or_else(default_descriptor_path),
            connection: Arc::new(AsyncMutex::new(None)),
            timeout: Duration::from_secs(65),
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
                params: serde_json::json!({"token":descriptor.token,"client_protocol":PROTOCOL}),
            };
            write_frame_async(&mut stream, &handshake, &[])
                .await
                .map_err(|_| AppClientError::Unavailable)?;
            let frame = read_frame_async(&mut stream)
                .await
                .map_err(|_| AppClientError::Protocol)?
                .ok_or(AppClientError::Protocol)?;
            let response: AppResponse =
                serde_json::from_slice(&frame.json).map_err(|_| AppClientError::Protocol)?;
            validate_response(&response, &handshake.request_id, frame.attachment.len())?;
            if !response.ok
                || response.result.get("protocol").and_then(Value::as_str) != Some(PROTOCOL)
            {
                return Err(AppClientError::Protocol);
            }
            Ok(stream)
        })
        .await
        .map_err(|_| AppClientError::Timeout)??;
        let (mut reader, writer) = stream.split();
        let connection = Arc::new(Connection {
            writer: AsyncMutex::new(writer),
            requests: Mutex::new(Requests::default()),
            alive: AtomicBool::new(true),
            reader: Mutex::new(None),
        });
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
                let Ok(response) = serde_json::from_slice::<AppResponse>(&frame.json) else {
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
                    let _ = sender.send(Ok(AppReply {
                        response,
                        attachment: frame.attachment,
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
        // Only connection establishment may be retried. No application request is replayed.
        let connection = tokio::select! {
            _=canceled.cancelled()=>return Err(AppClientError::Canceled),
            result=async {match self.connect().await {Err(AppClientError::Unavailable)=>self.connect().await,result=>result}}=>result?,
        };
        let (sender, receiver) = oneshot::channel();
        {
            let mut requests = connection
                .requests
                .lock()
                .map_err(|_| AppClientError::Disconnected)?;
            if requests.pending.len() >= 8 {
                return Err(AppClientError::Unavailable);
            }
            requests.pending.insert(id.clone(), sender);
        }
        let mut guard = PendingGuard {
            connection: connection.clone(),
            id,
            session: session_id,
            armed: true,
        };
        connection.send(&request).await?;
        let reply = tokio::select! {
            _=canceled.cancelled()=>return Err(AppClientError::Canceled),
            result=timeout(self.timeout,receiver)=>result.map_err(|_|AppClientError::Timeout)?.map_err(|_|AppClientError::Disconnected)?,
        };
        guard.armed = false;
        reply
    }
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
        || (attachment > 0 && r.attachment_mime.as_deref() != Some("image/png"))
    {
        return Err(AppClientError::Protocol);
    }
    Ok(())
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
        };
        let thread = std::thread::spawn(move || {
            let mut stream = listener.accept().unwrap();
            let frame = read_frame(&mut stream).unwrap().unwrap();
            let hello: AppRequest = serde_json::from_slice(&frame.json).unwrap();
            assert_eq!(hello.method, "handshake");
            assert_eq!(hello.params["token"], "a".repeat(64));
            write_frame(&mut stream, &response(&hello), &[]).unwrap();
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
