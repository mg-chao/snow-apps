// Non-Windows hosts expose only the unsupported-platform protocol endpoint.
// The Windows service implementation is retained here for shared protocol tests.
#![cfg_attr(not(windows), allow(dead_code))]

use crate::contract::{MAX_METADATA_BYTES, UpdateRelease, compare_versions, verify_release};
use crate::error::{Result, UpdateError, io_error, require};
use crate::fsutil;
use crate::protocol::{Command, FrameDecoder, MAX_FRAME_BYTES, PROTOCOL_VERSION, Status};
use crate::transaction;
use bytes::Bytes;
use futures_util::{Stream, StreamExt};
use reqwest::header::{
    ACCEPT_ENCODING, CACHE_CONTROL, CONTENT_RANGE, ETAG, IF_RANGE, RANGE, USER_AGENT,
};
use reqwest::{Client, StatusCode, Url, redirect};
use serde::{Deserialize, Serialize};
use serde_json::{Value, json};
use std::future::Future;
use std::path::{Path, PathBuf};
use std::pin::Pin;
use std::sync::Arc;
use std::time::Duration;
use time::OffsetDateTime;
use time::format_description::well_known::Rfc3339;
use tokio::io::{AsyncReadExt, AsyncSeekExt, AsyncWriteExt, BufWriter, SeekFrom};
use tokio::sync::mpsc;
use tokio_util::sync::CancellationToken;

const METADATA_TIMEOUT: Duration = Duration::from_secs(30);
const PACKAGE_TIMEOUT: Duration = Duration::from_secs(30 * 60);
const DOWNLOAD_RESERVE_BYTES: u64 = 64 * 1024 * 1024;
const RESULT_LIMIT: u64 = 8 * 1024;
const STATE_LIMIT: u64 = 8 * 1024 * 1024;
const FAILED_VERSION_LIMIT: u64 = 256;

type SleepFuture = Pin<Box<dyn Future<Output = ()> + Send>>;
type ResponseBody = Pin<Box<dyn Stream<Item = Result<Bytes>> + Send>>;

/// Clock used by update scheduling, retry delays, and persisted check timestamps.
///
/// The executable uses [`SystemClock`]. Tests can supply a deterministic clock through
/// [`run_with_dependencies`] without adding a production command-line escape hatch.
pub trait Clock: Send + Sync {
    fn now_utc(&self) -> OffsetDateTime;
    fn sleep(&self, duration: Duration) -> SleepFuture;
}

#[derive(Default)]
pub struct SystemClock;

impl Clock for SystemClock {
    fn now_utc(&self) -> OffsetDateTime {
        OffsetDateTime::now_utc()
    }

    fn sleep(&self, duration: Duration) -> SleepFuture {
        Box::pin(tokio::time::sleep(duration))
    }
}

#[derive(Clone, Debug, PartialEq, Eq)]
pub struct HttpRange {
    pub offset: u64,
    pub validator: String,
}

#[derive(Clone, Debug)]
pub struct HttpRequest {
    pub origin: Url,
    pub url: Url,
    pub installed_version: String,
    pub system_proxy: bool,
    pub timeout: Duration,
    pub range: Option<HttpRange>,
}

pub struct HttpResponse {
    pub status: u16,
    pub content_length: Option<u64>,
    pub content_range: Option<String>,
    pub etag: Option<String>,
    pub body: ResponseBody,
}

/// HTTP transport used by the update service.
///
/// Implementations receive a fully described request, including proxy policy, timeout, and
/// resume validator. The production implementation remains Reqwest with native platform TLS.
pub trait Network: Send + Sync {
    fn get(
        &self,
        request: HttpRequest,
    ) -> Pin<Box<dyn Future<Output = Result<HttpResponse>> + Send>>;
}

#[derive(Default)]
pub struct ReqwestNetwork;

impl Network for ReqwestNetwork {
    fn get(
        &self,
        request: HttpRequest,
    ) -> Pin<Box<dyn Future<Output = Result<HttpResponse>> + Send>> {
        Box::pin(async move {
            let client = client(&request.origin, request.system_proxy)?;
            let mut builder = http_request(&client, request.url, &request.installed_version)
                .timeout(request.timeout);
            if let Some(range) = &request.range {
                builder = builder
                    .header(RANGE, format!("bytes={}-", range.offset))
                    .header(IF_RANGE, &range.validator);
            }
            let response = builder.send().await.map_err(|error| {
                UpdateError::new("network_request_failed", "The update request failed")
                    .detail(error)
            })?;
            let status = response.status().as_u16();
            let content_length = response.content_length();
            let content_range = response
                .headers()
                .get(CONTENT_RANGE)
                .and_then(|value| value.to_str().ok())
                .map(str::to_owned);
            let etag = response
                .headers()
                .get(ETAG)
                .and_then(|value| value.to_str().ok())
                .map(str::to_owned);
            let body = response.bytes_stream().map(|chunk| {
                chunk.map_err(|error| {
                    UpdateError::new(
                        "network_stream_failed",
                        "The update response was interrupted",
                    )
                    .detail(error)
                })
            });
            Ok(HttpResponse {
                status,
                content_length,
                content_range,
                etag,
                body: Box::pin(body),
            })
        })
    }
}

#[derive(Clone)]
pub struct ServiceDependencies {
    pub clock: Arc<dyn Clock>,
    pub network: Arc<dyn Network>,
}

impl Default for ServiceDependencies {
    fn default() -> Self {
        Self {
            clock: Arc::new(SystemClock),
            network: Arc::new(ReqwestNetwork),
        }
    }
}

#[derive(Clone, Debug)]
pub struct ServiceOptions {
    pub root: PathBuf,
    pub cache_directory: PathBuf,
    pub base_url: String,
    pub allow_local_http: bool,
    pub parent_pid: u32,
}

#[derive(Clone, Debug, Default, Deserialize, Serialize)]
#[serde(rename_all = "camelCase")]
struct PersistedState {
    #[serde(default, skip_serializing_if = "String::is_empty")]
    observed_version: String,
    #[serde(default, skip_serializing_if = "String::is_empty")]
    observed_hash: String,
    #[serde(default, skip_serializing_if = "String::is_empty")]
    checked_at: String,
    #[serde(default, skip_serializing_if = "String::is_empty")]
    partial_hash: String,
    #[serde(default, skip_serializing_if = "String::is_empty")]
    validator: String,
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
enum Mode {
    Manual,
    Check,
    Download,
}

impl Mode {
    fn parse(value: &str) -> Option<Self> {
        match value {
            "manual" => Some(Self::Manual),
            "check" => Some(Self::Check),
            "download" => Some(Self::Download),
            _ => None,
        }
    }
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
enum ActiveOperation {
    Check,
    Download,
    Apply,
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
enum RequestedOperation {
    Probe,
    Check,
    Download,
    Apply,
}

impl RequestedOperation {
    fn parse(value: &str) -> Option<Self> {
        match value {
            "probe" => Some(Self::Probe),
            "check" => Some(Self::Check),
            "download" => Some(Self::Download),
            "apply" => Some(Self::Apply),
            _ => None,
        }
    }

    fn name(self) -> &'static str {
        match self {
            Self::Probe => "probe",
            Self::Check => "check",
            Self::Download => "download",
            Self::Apply => "apply",
        }
    }
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
enum Trigger {
    Startup,
    Periodic,
    User,
    PolicyChange,
}

impl Trigger {
    fn parse(value: &str) -> Option<Self> {
        match value {
            "startup" => Some(Self::Startup),
            "periodic" => Some(Self::Periodic),
            "user" => Some(Self::User),
            "policyChange" => Some(Self::PolicyChange),
            _ => None,
        }
    }

    fn user_initiated(self) -> bool {
        matches!(self, Self::User | Self::PolicyChange)
    }
}

enum OperationMessage {
    Metadata {
        release: UpdateRelease,
        bytes: Vec<u8>,
        manual: bool,
    },
    DownloadValidator {
        package_hash: String,
        validator: String,
    },
    Progress {
        received: u64,
        total: u64,
    },
    Downloaded {
        partial: PathBuf,
        package: AvailableUpdate,
    },
    Failed {
        operation: ActiveOperation,
        error: UpdateError,
    },
    Cancelled(ActiveOperation),
    HandoffPrepared(Result<crate::coordination::ServiceHandoff>),
}

struct Service {
    options: ServiceOptions,
    dependencies: ServiceDependencies,
    base_url: Url,
    variant: String,
    installed_version: String,
    persisted: PersistedState,
    available: Option<AvailableUpdate>,
    status: Status,
    mode: Mode,
    system_proxy: bool,
    active: Option<ActiveOperation>,
    cancellation: Option<CancellationToken>,
    requested: Option<RequestedOperation>,
    awaiting_handoff: bool,
    handoff: Option<crate::coordination::ServiceHandoff>,
}

#[derive(Clone)]
struct AvailableUpdate {
    version: String,
    path: String,
    size: u64,
    sha256: String,
}

fn cache_path(options: &ServiceOptions, name: impl AsRef<Path>) -> PathBuf {
    options.cache_directory.join(name)
}

fn failed_version_path(root: &Path) -> PathBuf {
    root.join(transaction::UPDATE_WORK)
        .join("failed-version.txt")
}

fn strong_etag(value: &str) -> bool {
    !value.starts_with("W/") && value.len() >= 2 && value.starts_with('"') && value.ends_with('"')
}

fn now_text(clock: &dyn Clock) -> String {
    clock.now_utc().format(&Rfc3339).unwrap_or_default()
}

fn same_origin(left: &Url, right: &Url) -> bool {
    left.scheme() == right.scheme()
        && left.host_str() == right.host_str()
        && left.port_or_known_default() == right.port_or_known_default()
}

fn validate_base_url(text: &str, allow_local_http: bool) -> Result<Url> {
    let url = Url::parse(text).map_err(|error| {
        UpdateError::new("update_server_invalid", "The update server must use HTTPS").detail(error)
    })?;
    let loopback_http = allow_local_http
        && url.scheme() == "http"
        && matches!(url.host_str(), Some("127.0.0.1" | "localhost" | "::1"));
    require(
        (url.scheme() == "https" || loopback_http)
            && url.host_str().is_some()
            && url.username().is_empty()
            && url.password().is_none(),
        "update_server_invalid",
        "The update server must use HTTPS",
    )?;
    Ok(url)
}

fn client(base_url: &Url, system_proxy: bool) -> Result<Client> {
    let origin = base_url.clone();
    let policy = redirect::Policy::custom(move |attempt| {
        if attempt.previous().len() >= 10 {
            return attempt.error("too many redirects");
        }
        if same_origin(&origin, attempt.url()) {
            attempt.follow()
        } else {
            attempt.stop()
        }
    });
    let builder = Client::builder()
        .redirect(policy)
        .connect_timeout(METADATA_TIMEOUT)
        .tcp_nodelay(true);
    let builder = if system_proxy {
        builder
    } else {
        builder.no_proxy()
    };
    builder.build().map_err(|error| {
        UpdateError::new(
            "service_not_initialized",
            "The update service could not be initialized",
        )
        .detail(error)
    })
}

fn read_persisted(path: &Path) -> PersistedState {
    fsutil::read_limited(path, STATE_LIMIT)
        .ok()
        .and_then(|bytes| serde_json::from_slice(&bytes).ok())
        .filter(|state: &PersistedState| {
            state.observed_version.is_empty()
                || crate::contract::parse_version(&state.observed_version).is_ok()
        })
        .unwrap_or_default()
}

fn write_persisted(options: &ServiceOptions, persisted: &PersistedState) -> Result<()> {
    let bytes = serde_json::to_vec(persisted).map_err(|error| {
        UpdateError::new("update_state_save_failed", "Could not save update state").detail(error)
    })?;
    fsutil::write_atomic(&cache_path(options, "state.json"), &bytes)
}

fn is_failed_version(root: &Path, version: &str) -> bool {
    fsutil::read_limited(&failed_version_path(root), FAILED_VERSION_LIMIT)
        .ok()
        .and_then(|bytes| String::from_utf8(bytes).ok())
        .is_some_and(|text| text.trim() == version)
}

fn release_retry_is_allowed(user_initiated: bool, failed_version: bool) -> bool {
    user_initiated || !failed_version
}

fn cached_payload_is_ready(user_initiated: bool, failed_version: bool, valid: bool) -> bool {
    valid && release_retry_is_allowed(user_initiated, failed_version)
}

fn retain_release_payload(cache: &Path, accepted_hash: &str) {
    let Ok(entries) = std::fs::read_dir(cache) else {
        return;
    };
    for entry in entries.flatten() {
        let name = entry.file_name().to_string_lossy().to_string();
        let named_payload = name.len() == 68 + usize::from(name.ends_with(".part"))
            && (name.ends_with(".zip") || name.ends_with(".part"))
            && name[..64]
                .bytes()
                .all(|byte| byte.is_ascii_digit() || (b'a'..=b'f').contains(&byte));
        if named_payload
            && !name.starts_with(accepted_hash)
            && entry
                .file_type()
                .is_ok_and(|kind| kind.is_file() && !kind.is_symlink())
        {
            let _ = std::fs::remove_file(entry.path());
        }
    }
}

impl Service {
    fn initialize(options: ServiceOptions, dependencies: ServiceDependencies) -> Result<Self> {
        let base_url = validate_base_url(&options.base_url, options.allow_local_http)?;
        transaction::validate_root(&options.root)?;
        let record = transaction::installation_record(&options.root)?;
        std::fs::create_dir_all(&options.cache_directory).map_err(|error| {
            io_error(
                "update_cache_create_failed",
                "Could not create update cache",
                error,
            )
        })?;
        let mut service = Self {
            persisted: read_persisted(&cache_path(&options, "state.json")),
            options,
            dependencies,
            base_url,
            variant: record.variant,
            installed_version: record.version,
            available: None,
            status: Status {
                state: "Idle".to_owned(),
                ..Status::default()
            },
            mode: Mode::Download,
            system_proxy: false,
            active: None,
            cancellation: None,
            requested: None,
            awaiting_handoff: false,
            handoff: None,
        };
        service.consume_legacy_result();
        Ok(service)
    }

    fn consume_legacy_result(&mut self) {
        let path = cache_path(&self.options, "result.txt");
        let Ok(bytes) = fsutil::read_limited(&path, RESULT_LIMIT) else {
            return;
        };
        let _ = std::fs::remove_file(path);
        let text = String::from_utf8_lossy(&bytes);
        if let Some(message) = text.strip_prefix("failed:") {
            self.status.state = "Failed".to_owned();
            self.status.error = Some(UpdateError::from_handoff_message(message.trim()));
        }
    }

    fn package(&self) -> Result<AvailableUpdate> {
        self.available
            .as_ref()
            .ok_or_else(|| {
                UpdateError::new(
                    "update_not_available",
                    "No update package matches this installation",
                )
            })
            .cloned()
    }

    fn set_state(&mut self, state: &str, error: Option<UpdateError>) {
        self.status.state = state.to_owned();
        self.status.error = error;
        if !matches!(state, "Downloading" | "Verifying") {
            self.status.received = 0;
            self.status.total = 0;
        }
    }
}

async fn write_value(writer: &mut BufWriter<tokio::io::Stdout>, value: &Value) -> Result<()> {
    let mut bytes = serde_json::to_vec(value).map_err(|error| {
        UpdateError::new(
            "protocol_message_invalid",
            "The update service protocol message is invalid",
        )
        .detail(error)
    })?;
    require(
        bytes.len() <= MAX_FRAME_BYTES,
        "protocol_frame_too_large",
        "The update service sent an oversized protocol message",
    )?;
    bytes.push(b'\n');
    writer.write_all(&bytes).await.map_err(|error| {
        io_error(
            "protocol_write_failed",
            "Could not send updater status",
            error,
        )
    })?;
    writer.flush().await.map_err(|error| {
        io_error(
            "protocol_write_failed",
            "Could not send updater status",
            error,
        )
    })
}

async fn write_status(writer: &mut BufWriter<tokio::io::Stdout>, status: &Status) -> Result<()> {
    write_value(
        writer,
        &json!({"protocol": PROTOCOL_VERSION, "type": "status", "status": status}),
    )
    .await
}

async fn write_completion(
    writer: &mut BufWriter<tokio::io::Stdout>,
    operation: RequestedOperation,
    outcome: &str,
    status: &Status,
) -> Result<()> {
    write_value(
        writer,
        &json!({
            "protocol": PROTOCOL_VERSION,
            "type": "operation_complete",
            "operation": operation.name(),
            "outcome": outcome,
            "status": status,
        }),
    )
    .await
}

async fn write_result(
    writer: &mut BufWriter<tokio::io::Stdout>,
    id: u64,
    error: Option<&UpdateError>,
) -> Result<()> {
    write_value(
        writer,
        &json!({
            "protocol": PROTOCOL_VERSION,
            "type": "command_result",
            "id": id,
            "ok": error.is_none(),
            "error": error,
        }),
    )
    .await
}

async fn command_reader(sender: mpsc::Sender<Result<Command>>) {
    let mut input = tokio::io::stdin();
    let mut decoder = FrameDecoder::default();
    let mut chunk = [0_u8; 4096];
    loop {
        match input.read(&mut chunk).await {
            Ok(0) => {
                if let Err(error) = decoder.finish() {
                    let _ = sender.send(Err(error)).await;
                }
                break;
            }
            Ok(count) => match decoder.push(&chunk[..count]) {
                Ok(commands) => {
                    for command in commands {
                        if sender.send(Ok(command)).await.is_err() {
                            return;
                        }
                    }
                }
                Err(error) => {
                    let _ = sender.send(Err(error)).await;
                    return;
                }
            },
            Err(error) => {
                let _ = sender
                    .send(Err(io_error(
                        "protocol_read_failed",
                        "The update service protocol message is invalid",
                        error,
                    )))
                    .await;
                break;
            }
        }
    }
}

fn http_request(client: &Client, url: Url, installed_version: &str) -> reqwest::RequestBuilder {
    client
        .get(url)
        .header(CACHE_CONTROL, "no-cache")
        .header(ACCEPT_ENCODING, "identity")
        .header(USER_AGENT, format!("SnowShot/{installed_version}"))
}

fn transport_error(code: &'static str, message: &'static str, error: UpdateError) -> UpdateError {
    UpdateError::new(code, message)
        .detail(error.detail.unwrap_or_else(|| error.message.into_owned()))
}

struct MetadataInputs {
    clock: Arc<dyn Clock>,
    network: Arc<dyn Network>,
    base_url: Url,
    system_proxy: bool,
    installed_version: String,
    manual: bool,
}

async fn fetch_metadata(
    inputs: MetadataInputs,
    cancellation: CancellationToken,
    sender: mpsc::Sender<OperationMessage>,
) {
    let result = async {
        let url = inputs.base_url.join("/latest-version.json").map_err(|error| {
            UpdateError::new("metadata_download_failed", "Could not download signed update metadata")
                .detail(error)
        })?;
        let mut deadline = inputs.clock.sleep(METADATA_TIMEOUT);
        let response = tokio::select! {
            biased;
            _ = cancellation.cancelled() => return Err(UpdateError::new("operation_cancelled", "The update download was interrupted")),
            response = inputs.network.get(HttpRequest {
                origin: inputs.base_url,
                url,
                installed_version: inputs.installed_version,
                system_proxy: inputs.system_proxy,
                timeout: METADATA_TIMEOUT,
                range: None,
            }) => response.map_err(|error| transport_error(
                "metadata_download_failed",
                "Could not download signed update metadata",
                error,
            ))?,
            _ = deadline.as_mut() => return Err(UpdateError::new(
                "metadata_download_failed",
                "Could not download signed update metadata",
            )),
        };
        require(
            response.status == StatusCode::OK.as_u16(),
            "metadata_download_failed",
            "Could not download signed update metadata",
        )?;
        if let Some(length) = response.content_length {
            require(
                length <= MAX_METADATA_BYTES as u64,
                "metadata_too_large",
                "Update metadata is too large",
            )?;
        }
        let mut stream = response.body;
        let mut bytes = Vec::new();
        while let Some(chunk) = tokio::select! {
            biased;
            _ = cancellation.cancelled() => return Err(UpdateError::new("operation_cancelled", "The update download was interrupted")),
            chunk = stream.next() => chunk,
            _ = deadline.as_mut() => return Err(UpdateError::new(
                "metadata_download_failed",
                "Could not download signed update metadata",
            )),
        } {
            let chunk = chunk.map_err(|error| {
                transport_error(
                    "metadata_download_failed",
                    "Could not download signed update metadata",
                    error,
                )
            })?;
            require(
                bytes.len().saturating_add(chunk.len()) <= MAX_METADATA_BYTES,
                "metadata_too_large",
                "Update metadata is too large",
            )?;
            bytes.extend_from_slice(&chunk);
        }
        let release = verify_release(&bytes, None)?;
        Ok((release, bytes))
    }
    .await;
    let message = match result {
        Ok((release, bytes)) => OperationMessage::Metadata {
            release,
            bytes,
            manual: inputs.manual,
        },
        Err(error) if error.code == "operation_cancelled" => {
            OperationMessage::Cancelled(ActiveOperation::Check)
        }
        Err(error) => OperationMessage::Failed {
            operation: ActiveOperation::Check,
            error,
        },
    };
    let _ = sender.send(message).await;
}

fn content_range(package: &AvailableUpdate, offset: u64) -> String {
    format!("bytes {offset}-{}/{}", package.size - 1, package.size)
}

async fn download_once(
    inputs: &DownloadInputs,
    resume: &mut DownloadResume,
    cancellation: &CancellationToken,
    sender: &mpsc::Sender<OperationMessage>,
) -> Result<PathBuf> {
    let package = &inputs.package;
    let partial = cache_path(&inputs.options, format!("{}.part", package.sha256));
    let mut offset = tokio::fs::metadata(&partial)
        .await
        .map(|value| value.len())
        .unwrap_or(0);
    let can_resume = resume.package_hash == package.sha256
        && strong_etag(&resume.validator)
        && offset > 0
        && offset <= package.size;
    if !can_resume && offset > 0 {
        tokio::fs::remove_file(&partial).await.map_err(|error| {
            io_error(
                "partial_download_invalid",
                "The partial update download is invalid",
                error,
            )
        })?;
        offset = 0;
    }
    let available = fs2::available_space(&inputs.options.cache_directory).map_err(|error| {
        io_error(
            "download_space_check_failed",
            "Not enough free space to download the update",
            error,
        )
    })?;
    let needed = package
        .size
        .saturating_sub(offset)
        .checked_add(DOWNLOAD_RESERVE_BYTES)
        .ok_or_else(|| {
            UpdateError::new(
                "download_space_insufficient",
                "Not enough free space to download the update",
            )
        })?;
    require(
        available > needed,
        "download_space_insufficient",
        "Not enough free space to download the update",
    )?;

    let url = inputs
        .base_url
        .join(&format!("/{}", package.path))
        .map_err(|error| {
            UpdateError::new(
                "package_download_failed",
                "The update package could not be downloaded",
            )
            .detail(error)
        })?;
    let mut deadline = inputs.clock.sleep(PACKAGE_TIMEOUT);
    let response = tokio::select! {
        biased;
        _ = cancellation.cancelled() => return Err(UpdateError::new("operation_cancelled", "The update download was interrupted")),
        response = inputs.network.get(HttpRequest {
            origin: inputs.base_url.clone(),
            url,
            installed_version: inputs.installed_version.clone(),
            system_proxy: inputs.system_proxy,
            timeout: PACKAGE_TIMEOUT,
            range: can_resume.then(|| HttpRange {
                offset,
                validator: resume.validator.clone(),
            }),
        }) => response.map_err(|error| transport_error(
            "package_download_failed",
            "The update package could not be downloaded",
            error,
        ))?,
        _ = deadline.as_mut() => return Err(UpdateError::new(
            "package_download_failed",
            "The update package could not be downloaded",
        )),
    };
    require(
        matches!(
            response.status,
            status if status == StatusCode::OK.as_u16()
                || status == StatusCode::PARTIAL_CONTENT.as_u16()
        ),
        "package_download_failed",
        "The update package could not be downloaded",
    )?;
    if response.status == StatusCode::PARTIAL_CONTENT.as_u16() {
        require(
            can_resume
                && response.content_range.as_deref()
                    == Some(content_range(package, offset).as_str()),
            "download_range_invalid",
            "The server returned an invalid download range",
        )?;
    } else if offset > 0 {
        offset = 0;
    }
    if let Some(length) = response.content_length {
        require(
            length <= package.size.saturating_sub(offset),
            "download_size_exceeded",
            "The download exceeded its signed size or could not be saved",
        )?;
    }
    let validator = response
        .etag
        .as_deref()
        .filter(|value| strong_etag(value))
        .unwrap_or_default()
        .to_owned();
    resume.package_hash.clone_from(&package.sha256);
    resume.validator.clone_from(&validator);
    let _ = sender
        .send(OperationMessage::DownloadValidator {
            package_hash: package.sha256.clone(),
            validator,
        })
        .await;

    let mut output = tokio::fs::OpenOptions::new()
        .create(true)
        .read(true)
        .write(true)
        .truncate(offset == 0)
        .open(&partial)
        .await
        .map_err(|error| {
            io_error(
                "download_file_open_failed",
                "Could not open the update download file",
                error,
            )
        })?;
    output
        .seek(SeekFrom::Start(offset))
        .await
        .map_err(|error| {
            io_error(
                "download_restart_failed",
                "Could not restart the update download",
                error,
            )
        })?;
    let mut received = offset;
    let _ = sender
        .send(OperationMessage::Progress {
            received,
            total: package.size,
        })
        .await;
    let mut stream = response.body;
    while let Some(chunk) = tokio::select! {
        biased;
        _ = cancellation.cancelled() => return Err(UpdateError::new("operation_cancelled", "The update download was interrupted")),
        chunk = stream.next() => chunk,
        _ = deadline.as_mut() => return Err(UpdateError::new(
            "download_interrupted",
            "The update download was interrupted",
        )),
    } {
        let chunk = chunk.map_err(|error| {
            transport_error(
                "download_interrupted",
                "The update download was interrupted",
                error,
            )
        })?;
        received = received.checked_add(chunk.len() as u64).ok_or_else(|| {
            UpdateError::new(
                "download_size_exceeded",
                "The download exceeded its signed size or could not be saved",
            )
        })?;
        require(
            received <= package.size,
            "download_size_exceeded",
            "The download exceeded its signed size or could not be saved",
        )?;
        output.write_all(&chunk).await.map_err(|error| {
            io_error(
                "download_size_exceeded",
                "The download exceeded its signed size or could not be saved",
                error,
            )
        })?;
        let _ = sender
            .send(OperationMessage::Progress {
                received,
                total: package.size,
            })
            .await;
    }
    output.flush().await.map_err(|error| {
        io_error(
            "download_interrupted",
            "The update download was interrupted",
            error,
        )
    })?;
    output.sync_all().await.map_err(|error| {
        io_error(
            "download_interrupted",
            "The update download was interrupted",
            error,
        )
    })?;
    require(
        received == package.size,
        "download_interrupted",
        "The update download was interrupted",
    )?;
    Ok(partial)
}

struct DownloadInputs {
    options: ServiceOptions,
    clock: Arc<dyn Clock>,
    network: Arc<dyn Network>,
    base_url: Url,
    system_proxy: bool,
    installed_version: String,
    package: AvailableUpdate,
    saved_hash: String,
    saved_validator: String,
}

struct DownloadResume {
    package_hash: String,
    validator: String,
}

async fn download_package(
    inputs: DownloadInputs,
    cancellation: CancellationToken,
    sender: mpsc::Sender<OperationMessage>,
) {
    let mut last_error = None;
    let mut resume = DownloadResume {
        package_hash: inputs.saved_hash.clone(),
        validator: inputs.saved_validator.clone(),
    };
    for attempt in 0..3 {
        if attempt > 0 {
            let delay = Duration::from_secs((attempt * 2) as u64);
            tokio::select! {
                _ = cancellation.cancelled() => {
                    let _ = sender.send(OperationMessage::Cancelled(ActiveOperation::Download)).await;
                    return;
                }
                _ = inputs.clock.sleep(delay) => {}
            }
        }
        match download_once(&inputs, &mut resume, &cancellation, &sender).await {
            Ok(partial) => {
                let _ = sender
                    .send(OperationMessage::Downloaded {
                        partial,
                        package: inputs.package,
                    })
                    .await;
                return;
            }
            Err(error) if error.code == "operation_cancelled" => {
                let _ = sender
                    .send(OperationMessage::Cancelled(ActiveOperation::Download))
                    .await;
                return;
            }
            Err(error) => last_error = Some(error),
        }
    }
    let _ = sender
        .send(OperationMessage::Failed {
            operation: ActiveOperation::Download,
            error: last_error.unwrap_or_else(|| {
                UpdateError::new(
                    "package_download_failed",
                    "The update package could not be downloaded",
                )
            }),
        })
        .await;
}

fn start_check(
    service: &mut Service,
    user_initiated: bool,
    operation_sender: &mpsc::Sender<OperationMessage>,
) -> Result<()> {
    require(
        service.active.is_none() && service.status.state != "Applying",
        "operation_in_progress",
        "An update is still running",
    )?;
    service.active = Some(ActiveOperation::Check);
    service.set_state("Checking", None);
    let cancellation = CancellationToken::new();
    service.cancellation = Some(cancellation.clone());
    tokio::spawn(fetch_metadata(
        MetadataInputs {
            clock: service.dependencies.clock.clone(),
            network: service.dependencies.network.clone(),
            base_url: service.base_url.clone(),
            system_proxy: service.system_proxy,
            installed_version: service.installed_version.clone(),
            manual: user_initiated,
        },
        cancellation,
        operation_sender.clone(),
    ));
    Ok(())
}

fn start_download(
    service: &mut Service,
    operation_sender: &mpsc::Sender<OperationMessage>,
) -> Result<()> {
    require(
        service.active.is_none() && service.status.state != "Applying",
        "operation_in_progress",
        "An update is still running",
    )?;
    let package = service.package()?;
    service.active = Some(ActiveOperation::Download);
    service.set_state("Downloading", None);
    service.status.total = package.size;
    let cancellation = CancellationToken::new();
    service.cancellation = Some(cancellation.clone());
    tokio::spawn(download_package(
        DownloadInputs {
            options: service.options.clone(),
            clock: service.dependencies.clock.clone(),
            network: service.dependencies.network.clone(),
            base_url: service.base_url.clone(),
            system_proxy: service.system_proxy,
            installed_version: service.installed_version.clone(),
            package,
            saved_hash: service.persisted.partial_hash.clone(),
            saved_validator: service.persisted.validator.clone(),
        },
        cancellation,
        operation_sender.clone(),
    ));
    Ok(())
}

async fn accept_metadata(
    service: &mut Service,
    release: UpdateRelease,
    bytes: Vec<u8>,
    manual: bool,
    writer: &mut BufWriter<tokio::io::Stdout>,
) -> Result<()> {
    let package = release.update_package(&service.variant)?;
    if !service.persisted.observed_version.is_empty() {
        let order = compare_versions(&release.version, &service.persisted.observed_version)?;
        require(
            !order.is_lt(),
            "release_replay",
            "The server offered older release metadata",
        )?;
        require(
            !order.is_eq() || package.sha256 == service.persisted.observed_hash,
            "release_mutated",
            "The server changed an already published release",
        )?;
    }
    service
        .persisted
        .observed_version
        .clone_from(&release.version);
    service.persisted.observed_hash.clone_from(&package.sha256);
    service.persisted.checked_at = now_text(service.dependencies.clock.as_ref());
    write_persisted(&service.options, &service.persisted)?;
    let available = AvailableUpdate {
        version: release.version.clone(),
        path: package.path.clone(),
        size: package.size,
        sha256: package.sha256.clone(),
    };
    service.status.version.clone_from(&available.version);
    if !compare_versions(&release.version, &service.installed_version)?.is_gt() {
        service.available = None;
        service.set_state("Idle", None);
        write_status(writer, &service.status).await?;
        return Ok(());
    }
    fsutil::write_atomic(&cache_path(&service.options, "release.json"), &bytes)?;
    retain_release_payload(&service.options.cache_directory, &available.sha256);
    let suppressed = is_failed_version(&service.options.root, &release.version);
    let complete = cache_path(&service.options, format!("{}.zip", available.sha256));
    let complete_valid = fsutil::verify_file(&complete, available.size, &available.sha256).is_ok();
    if cached_payload_is_ready(manual, suppressed, complete_valid) {
        service.available = Some(available);
        service.set_state("Ready", None);
        write_status(writer, &service.status).await?;
        write_value(
            writer,
            &json!({"protocol": PROTOCOL_VERSION, "type": "update_ready"}),
        )
        .await?;
    } else {
        if !complete_valid && complete.exists() {
            let _ = std::fs::remove_file(&complete);
        }
        service.available = Some(available);
        service.set_state("Available", None);
        write_status(writer, &service.status).await?;
    }
    Ok(())
}

fn prepare_apply(
    service: &Service,
    operation_sender: &mpsc::Sender<OperationMessage>,
) -> Result<()> {
    let package = service.package()?;
    let archive = cache_path(&service.options, format!("{}.zip", package.sha256));
    let manifest = cache_path(&service.options, "release.json");
    let result = cache_path(&service.options, "result.txt");
    let args = vec![
        "--launch".to_owned(),
        "--target".to_owned(),
        service.options.root.to_string_lossy().to_string(),
        "--parent".to_owned(),
        service.options.parent_pid.to_string(),
        "--service-parent".to_owned(),
        std::process::id().to_string(),
        "--manifest".to_owned(),
        manifest.to_string_lossy().to_string(),
        "--archive".to_owned(),
        archive.to_string_lossy().to_string(),
        "--result".to_owned(),
        result.to_string_lossy().to_string(),
    ];
    let sender = operation_sender.clone();
    tokio::spawn(async move {
        let result = crate::coordination::prepare_service_handoff(args).await;
        let _ = sender.send(OperationMessage::HandoffPrepared(result)).await;
    });
    Ok(())
}

async fn handle_command(
    service: &mut Service,
    command: Command,
    writer: &mut BufWriter<tokio::io::Stdout>,
    operation_sender: &mpsc::Sender<OperationMessage>,
) -> Result<bool> {
    let mut complete = None;
    let result = match command.command.as_str() {
        "execute" => {
            let parsed = (|| {
                require(
                    service.requested.is_none(),
                    "protocol_state_invalid",
                    "Invalid updater command argument",
                )?;
                let operation = command
                    .operation
                    .as_deref()
                    .and_then(RequestedOperation::parse)
                    .ok_or_else(|| {
                        UpdateError::new(
                            "protocol_enum_invalid",
                            "Invalid updater command argument",
                        )
                    })?;
                let trigger = command
                    .trigger
                    .as_deref()
                    .and_then(Trigger::parse)
                    .ok_or_else(|| {
                        UpdateError::new(
                            "protocol_enum_invalid",
                            "Invalid updater command argument",
                        )
                    })?;
                let mode = command
                    .mode
                    .as_deref()
                    .and_then(Mode::parse)
                    .ok_or_else(|| {
                        UpdateError::new(
                            "protocol_enum_invalid",
                            "Invalid updater command argument",
                        )
                    })?;
                let system_proxy = command.system_proxy.ok_or_else(|| {
                    UpdateError::new(
                        "protocol_message_invalid",
                        "Invalid updater command argument",
                    )
                })?;
                Ok((operation, trigger, mode, system_proxy))
            })();
            match parsed {
                Ok((operation, trigger, mode, system_proxy)) => {
                    service.requested = Some(operation);
                    service.mode = mode;
                    service.system_proxy = system_proxy;
                    let started = match operation {
                        RequestedOperation::Probe => Ok(()),
                        RequestedOperation::Check => {
                            start_check(service, trigger.user_initiated(), operation_sender)
                        }
                        RequestedOperation::Download => {
                            let failed_version = service.available.as_ref().is_some_and(|update| {
                                is_failed_version(&service.options.root, &update.version)
                            });
                            if release_retry_is_allowed(trigger.user_initiated(), failed_version) {
                                start_download(service, operation_sender)
                            } else {
                                complete = Some("success");
                                Ok(())
                            }
                        }
                        RequestedOperation::Apply => {
                            if service.status.state != "Ready" || service.active.is_some() {
                                Err(UpdateError::new(
                                    "protocol_state_invalid",
                                    "Invalid updater command argument",
                                ))
                            } else {
                                start_check(service, true, operation_sender)
                            }
                        }
                    };
                    if operation == RequestedOperation::Probe && started.is_ok() {
                        complete = Some("success");
                    }
                    started
                }
                Err(error) => Err(error),
            }
        }
        "cancel" => {
            if let Some(cancellation) = service.cancellation.as_ref() {
                cancellation.cancel();
            }
            Ok(())
        }
        "handoff_decision" => {
            let decision = async {
                require(
                    service.awaiting_handoff && service.status.state == "Applying",
                    "protocol_state_invalid",
                    "Invalid updater command argument",
                )?;
                service.awaiting_handoff = false;
                let proceed = command.proceed.unwrap_or(false);
                let handoff = service.handoff.take().ok_or_else(|| {
                    UpdateError::new("protocol_state_invalid", "Invalid updater command argument")
                })?;
                match handoff.decide(proceed).await {
                    Ok(()) if proceed => {}
                    Ok(()) => {
                        service.set_state(
                            "Ready",
                            command.reason.as_deref().map(UpdateError::from_message),
                        );
                        complete = Some("cancelled");
                    }
                    Err(error) => {
                        service.set_state("Failed", Some(error.clone()));
                        complete = Some("failed");
                        return Err(error);
                    }
                }
                Ok(())
            };
            decision.await
        }
        "shutdown" => Ok(()),
        _ => Err(UpdateError::new(
            "protocol_command_unknown",
            "Invalid updater command argument",
        )),
    };
    let error = result.as_ref().err();
    write_result(writer, command.id, error).await?;
    if result.is_err() && command.command == "execute" {
        service.active = None;
        service.set_state("Failed", error.cloned());
        complete = Some("failed");
    }
    write_status(writer, &service.status).await?;
    if let Some(outcome) = complete {
        let operation = service.requested.unwrap_or(RequestedOperation::Probe);
        write_completion(writer, operation, outcome, &service.status).await?;
        return Ok(true);
    }
    if command.command == "shutdown"
        || (command.command == "handoff_decision"
            && command.proceed == Some(true)
            && result.is_ok())
    {
        return Ok(true);
    }
    Ok(false)
}

async fn handle_operation(
    service: &mut Service,
    message: OperationMessage,
    writer: &mut BufWriter<tokio::io::Stdout>,
    operation_sender: &mpsc::Sender<OperationMessage>,
) -> Result<Option<&'static str>> {
    match message {
        OperationMessage::Metadata {
            release,
            bytes,
            manual,
        } => {
            if service.active != Some(ActiveOperation::Check) {
                return Ok(None);
            }
            service.active = None;
            service.cancellation = None;
            if let Err(error) = accept_metadata(service, release, bytes, manual, writer).await {
                service.set_state("Failed", Some(error));
                write_status(writer, &service.status).await?;
                return Ok(Some("failed"));
            }
            if service.requested == Some(RequestedOperation::Apply)
                && service.status.state == "Ready"
            {
                service.active = Some(ActiveOperation::Apply);
                service.set_state("Applying", None);
                write_status(writer, &service.status).await?;
                prepare_apply(service, operation_sender)?;
                return Ok(None);
            }
            if service.active.is_none() {
                return Ok(Some("success"));
            }
        }
        OperationMessage::DownloadValidator {
            package_hash,
            validator,
        } => {
            if service.active == Some(ActiveOperation::Download) {
                service.persisted.partial_hash = package_hash;
                service.persisted.validator = validator;
                if let Err(error) = write_persisted(&service.options, &service.persisted) {
                    service.active = None;
                    if let Some(cancellation) = service.cancellation.take() {
                        cancellation.cancel();
                    }
                    service.set_state("Failed", Some(error));
                    write_status(writer, &service.status).await?;
                    return Ok(Some("failed"));
                }
            }
        }
        OperationMessage::Progress { received, total } => {
            if service.active == Some(ActiveOperation::Download) {
                service.status.received = received;
                service.status.total = total;
                write_status(writer, &service.status).await?;
            }
        }
        OperationMessage::Downloaded { partial, package } => {
            if service.active != Some(ActiveOperation::Download) {
                return Ok(None);
            }
            service.set_state("Verifying", None);
            service.status.received = package.size;
            service.status.total = package.size;
            write_status(writer, &service.status).await?;
            let verification_path = partial.clone();
            let verification_package = package.clone();
            let verified = tokio::task::spawn_blocking(move || {
                fsutil::verify_file(
                    &verification_path,
                    verification_package.size,
                    &verification_package.sha256,
                )
            })
            .await
            .map_err(|error| {
                UpdateError::new(
                    "payload_verify_failed",
                    "Update payload size or checksum does not match the signed release",
                )
                .detail(error)
            })?;
            if let Err(error) = verified {
                service.active = None;
                service.cancellation = None;
                service.set_state("Failed", Some(error));
                write_status(writer, &service.status).await?;
                return Ok(Some("failed"));
            }
            let complete = cache_path(&service.options, format!("{}.zip", package.sha256));
            crate::platform::replace_file(&partial, &complete)?;
            service.persisted.partial_hash.clear();
            service.persisted.validator.clear();
            write_persisted(&service.options, &service.persisted)?;
            service.active = None;
            service.cancellation = None;
            service.set_state("Ready", None);
            write_status(writer, &service.status).await?;
            write_value(
                writer,
                &json!({"protocol": PROTOCOL_VERSION, "type": "update_ready"}),
            )
            .await?;
            return Ok(Some("success"));
        }
        OperationMessage::Failed { operation, error } => {
            if service.active == Some(operation) {
                service.active = None;
                service.cancellation = None;
                service.set_state("Failed", Some(error));
                write_status(writer, &service.status).await?;
                return Ok(Some("failed"));
            }
        }
        OperationMessage::Cancelled(operation) => {
            if service.active == Some(operation) {
                service.active = None;
                service.cancellation = None;
                if service.available.is_some() {
                    service.set_state("Available", None);
                } else {
                    service.set_state("Idle", None);
                }
                write_status(writer, &service.status).await?;
                return Ok(Some("cancelled"));
            }
        }
        OperationMessage::HandoffPrepared(result) => match result {
            Ok(handoff) if service.status.state == "Applying" => {
                service.active = None;
                service.awaiting_handoff = true;
                service.handoff = Some(handoff);
                write_value(
                    writer,
                    &json!({"protocol": PROTOCOL_VERSION, "type": "handoff_ready"}),
                )
                .await?;
            }
            Ok(handoff) => {
                let _ = handoff.decide(false).await;
            }
            Err(error) => {
                service.active = None;
                service.awaiting_handoff = false;
                service.handoff = None;
                service.set_state("Failed", Some(error));
                write_status(writer, &service.status).await?;
                return Ok(Some("failed"));
            }
        },
    }
    Ok(None)
}

#[cfg(not(windows))]
async fn run_unsupported_service() -> Result<()> {
    let mut writer = BufWriter::new(tokio::io::stdout());
    let status = Status {
        state: "Unavailable".to_owned(),
        ..Status::default()
    };
    write_value(
        &mut writer,
        &json!({
            "protocol": PROTOCOL_VERSION,
            "type": "hello",
            "updaterVersion": env!("CARGO_PKG_VERSION"),
            "platform": std::env::consts::OS,
            "capabilities": [],
        }),
    )
    .await?;
    write_status(&mut writer, &status).await?;
    let (sender, mut receiver) = mpsc::channel(1);
    tokio::spawn(command_reader(sender));
    if let Some(command) = receiver.recv().await.transpose()? {
        let operation = command
            .operation
            .as_deref()
            .and_then(RequestedOperation::parse);
        let valid = command.protocol == PROTOCOL_VERSION
            && command.command == "execute"
            && operation.is_some()
            && command
                .trigger
                .as_deref()
                .and_then(Trigger::parse)
                .is_some()
            && command.mode.as_deref().and_then(Mode::parse).is_some()
            && command.system_proxy.is_some();
        let error = (!valid).then(|| {
            UpdateError::new(
                "protocol_command_unknown",
                "Invalid updater command argument",
            )
        });
        write_result(&mut writer, command.id, error.as_ref()).await?;
        write_status(&mut writer, &status).await?;
        if error.is_none() {
            write_completion(
                &mut writer,
                operation.expect("validated operation"),
                "success",
                &status,
            )
            .await?;
        }
    }
    Ok(())
}

pub async fn run(options: ServiceOptions) -> Result<()> {
    run_with_dependencies(options, ServiceDependencies::default()).await
}

/// Runs one operation-scoped service session with injected clock and network dependencies.
///
/// This entry point is intended for deterministic library tests and embedders. The public CLI
/// always calls [`run`] and therefore always uses native TLS and the real system clock.
pub async fn run_with_dependencies(
    options: ServiceOptions,
    dependencies: ServiceDependencies,
) -> Result<()> {
    #[cfg(not(windows))]
    {
        drop(options);
        drop(dependencies);
        run_unsupported_service().await
    }

    #[cfg(windows)]
    {
        let mut service = Service::initialize(options, dependencies)?;
        let mut writer = BufWriter::new(tokio::io::stdout());
        write_value(
            &mut writer,
            &json!({
                "protocol": PROTOCOL_VERSION,
                "type": "hello",
                "updaterVersion": env!("CARGO_PKG_VERSION"),
                "platform": "windows-x64",
                "capabilities": ["check", "download", "apply", "recovery"],
            }),
        )
        .await?;
        write_status(&mut writer, &service.status).await?;

        let (command_sender, mut command_receiver) = mpsc::channel(16);
        tokio::spawn(command_reader(command_sender));
        let (operation_sender, mut operation_receiver) = mpsc::channel(64);
        let mut last_request_id = 0_u64;

        loop {
            tokio::select! {
                command = command_receiver.recv() => {
                    let Some(command) = command else {
                        if let Some(cancellation) = service.cancellation.take() {
                            cancellation.cancel();
                        }
                        return Ok(());
                    };
                    let command = match command {
                        Ok(command) => command,
                        Err(error) => {
                            write_value(
                                &mut writer,
                                &json!({"protocol": PROTOCOL_VERSION, "type": "fatal", "error": error}),
                            ).await?;
                            return Ok(());
                        }
                    };
                    if command.id == 0 || command.id <= last_request_id {
                        let error = UpdateError::new(
                            "protocol_request_id_invalid",
                            "The update service protocol message is invalid",
                        );
                        write_result(&mut writer, command.id, Some(&error)).await?;
                        continue;
                    }
                    last_request_id = command.id;
                    if handle_command(
                        &mut service,
                        command,
                        &mut writer,
                        &operation_sender,
                    ).await? {
                        if let Some(cancellation) = service.cancellation.take() {
                            cancellation.cancel();
                        }
                        return Ok(());
                    }
                }
                operation = operation_receiver.recv() => {
                    let outcome = if let Some(operation) = operation {
                        match handle_operation(
                            &mut service,
                            operation,
                            &mut writer,
                            &operation_sender,
                        ).await {
                            Ok(outcome) => outcome,
                            Err(error) => {
                                service.active = None;
                                if let Some(cancellation) = service.cancellation.take() {
                                    cancellation.cancel();
                                }
                                service.set_state("Failed", Some(error));
                                write_status(&mut writer, &service.status).await?;
                                Some("failed")
                            }
                        }
                    } else {
                        None
                    };
                    if let Some(outcome) = outcome {
                        let requested = service.requested.ok_or_else(|| {
                            UpdateError::new(
                                "protocol_state_invalid",
                                "Invalid updater command argument",
                            )
                        })?;
                        write_completion(&mut writer, requested, outcome, &service.status).await?;
                        return Ok(());
                    }
                }
            }
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::contract::UpdatePackage;
    use futures_util::{future, stream};
    use sha2::{Digest, Sha256};
    use std::collections::VecDeque;
    use std::sync::Mutex;
    use tempfile::TempDir;

    enum FakeReply {
        Response {
            status: u16,
            content_length: Option<u64>,
            content_range: Option<String>,
            etag: Option<String>,
            chunks: Vec<Result<Vec<u8>>>,
        },
        Error(UpdateError),
        Pending,
    }

    #[derive(Clone)]
    struct FakeNetwork {
        replies: Arc<Mutex<VecDeque<FakeReply>>>,
        requests: Arc<Mutex<Vec<HttpRequest>>>,
    }

    impl FakeNetwork {
        fn new(replies: impl IntoIterator<Item = FakeReply>) -> Self {
            Self {
                replies: Arc::new(Mutex::new(replies.into_iter().collect())),
                requests: Arc::new(Mutex::new(Vec::new())),
            }
        }

        fn requests(&self) -> Vec<HttpRequest> {
            self.requests.lock().unwrap().clone()
        }
    }

    impl Network for FakeNetwork {
        fn get(
            &self,
            request: HttpRequest,
        ) -> Pin<Box<dyn Future<Output = Result<HttpResponse>> + Send>> {
            let requests = self.requests.clone();
            let replies = self.replies.clone();
            Box::pin(async move {
                requests.lock().unwrap().push(request);
                let reply = replies
                    .lock()
                    .unwrap()
                    .pop_front()
                    .expect("fake network reply");
                match reply {
                    FakeReply::Response {
                        status,
                        content_length,
                        content_range,
                        etag,
                        chunks,
                    } => Ok(HttpResponse {
                        status,
                        content_length,
                        content_range,
                        etag,
                        body: Box::pin(stream::iter(
                            chunks.into_iter().map(|chunk| chunk.map(Bytes::from)),
                        )),
                    }),
                    FakeReply::Error(error) => Err(error),
                    FakeReply::Pending => future::pending().await,
                }
            })
        }
    }

    #[derive(Clone)]
    struct FakeClock {
        now: OffsetDateTime,
        ready_delays: Arc<Vec<Duration>>,
        sleeps: Arc<Mutex<Vec<Duration>>>,
    }

    impl FakeClock {
        fn new(now: OffsetDateTime, ready_delays: Vec<Duration>) -> Self {
            Self {
                now,
                ready_delays: Arc::new(ready_delays),
                sleeps: Arc::new(Mutex::new(Vec::new())),
            }
        }

        fn sleeps(&self) -> Vec<Duration> {
            self.sleeps.lock().unwrap().clone()
        }
    }

    impl Clock for FakeClock {
        fn now_utc(&self) -> OffsetDateTime {
            self.now
        }

        fn sleep(&self, duration: Duration) -> SleepFuture {
            self.sleeps.lock().unwrap().push(duration);
            if self.ready_delays.contains(&duration) {
                Box::pin(future::ready(()))
            } else {
                Box::pin(future::pending())
            }
        }
    }

    fn reply(status: StatusCode, bytes: &[u8]) -> FakeReply {
        FakeReply::Response {
            status: status.as_u16(),
            content_length: Some(bytes.len() as u64),
            content_range: None,
            etag: None,
            chunks: vec![Ok(bytes.to_vec())],
        }
    }

    fn fixed_time() -> OffsetDateTime {
        OffsetDateTime::parse("2026-09-20T00:00:00Z", &Rfc3339).unwrap()
    }

    fn download_inputs(
        temporary: &TempDir,
        network: Arc<dyn Network>,
        clock: Arc<dyn Clock>,
        size: u64,
    ) -> DownloadInputs {
        DownloadInputs {
            options: ServiceOptions {
                root: temporary.path().join("root"),
                cache_directory: temporary.path().join("cache"),
                base_url: "http://127.0.0.1:8080".to_owned(),
                allow_local_http: true,
                parent_pid: 1,
            },
            clock,
            network,
            base_url: Url::parse("http://127.0.0.1:8080").unwrap(),
            system_proxy: false,
            installed_version: "1.0.0".to_owned(),
            package: AvailableUpdate {
                version: "2.0.0".to_owned(),
                path: "snow-shot-portable.zip".to_owned(),
                size,
                sha256: "a".repeat(64),
            },
            saved_hash: "a".repeat(64),
            saved_validator: "\"release-1\"".to_owned(),
        }
    }

    fn initialize_cache(inputs: &DownloadInputs) {
        std::fs::create_dir_all(&inputs.options.cache_directory).unwrap();
    }

    fn ready_service(temporary: &TempDir, network: FakeNetwork) -> Service {
        let options = ServiceOptions {
            root: temporary.path().join("root"),
            cache_directory: temporary.path().join("cache"),
            base_url: "http://127.0.0.1:8080".to_owned(),
            allow_local_http: true,
            parent_pid: 1,
        };
        std::fs::create_dir_all(&options.cache_directory).unwrap();
        let hash = format!("{:x}", Sha256::digest(b"old release"));
        std::fs::write(cache_path(&options, format!("{hash}.zip")), b"old release").unwrap();
        Service {
            options,
            dependencies: ServiceDependencies {
                clock: Arc::new(FakeClock::new(fixed_time(), Vec::new())),
                network: Arc::new(network),
            },
            base_url: Url::parse("http://127.0.0.1:8080").unwrap(),
            variant: "portable".to_owned(),
            installed_version: "1.0.0".to_owned(),
            persisted: PersistedState {
                observed_version: "2.0.0".to_owned(),
                observed_hash: hash.clone(),
                ..PersistedState::default()
            },
            available: Some(AvailableUpdate {
                version: "2.0.0".to_owned(),
                path: "setup/snow-shot_windows-x64-portable.zip".to_owned(),
                size: 11,
                sha256: hash,
            }),
            status: Status {
                state: "Ready".to_owned(),
                version: "2.0.0".to_owned(),
                ..Status::default()
            },
            mode: Mode::Download,
            system_proxy: false,
            active: None,
            cancellation: None,
            requested: None,
            awaiting_handoff: false,
            handoff: None,
        }
    }

    fn apply_command() -> Command {
        serde_json::from_value(json!({
            "protocol": PROTOCOL_VERSION,
            "id": 1,
            "command": "execute",
            "operation": "apply",
            "trigger": "user",
            "mode": "download",
            "systemProxy": false
        }))
        .unwrap()
    }

    #[tokio::test]
    async fn apply_checks_for_a_new_release_before_using_a_cached_payload() {
        let temporary = TempDir::new().unwrap();
        let network = FakeNetwork::new([FakeReply::Pending]);
        let mut service = ready_service(&temporary, network.clone());
        let old_hash = service.available.as_ref().unwrap().sha256.clone();
        let mut writer = BufWriter::new(tokio::io::stdout());
        let (sender, _receiver) = mpsc::channel(4);

        assert!(
            !handle_command(&mut service, apply_command(), &mut writer, &sender)
                .await
                .unwrap()
        );
        tokio::task::yield_now().await;
        assert_eq!(service.active, Some(ActiveOperation::Check));
        assert_eq!(service.status.state, "Checking");
        assert_eq!(network.requests().len(), 1);

        let release = UpdateRelease {
            version: "3.0.0".to_owned(),
            packages: vec![UpdatePackage {
                variant: "portable".to_owned(),
                kind: "portable".to_owned(),
                path: "setup/snow-shot_windows-x64-portable.zip".to_owned(),
                size: 12,
                sha256: "a".repeat(64),
                files: Vec::new(),
            }],
            envelope: Vec::new(),
        };
        let outcome = handle_operation(
            &mut service,
            OperationMessage::Metadata {
                release,
                bytes: b"verified newer release".to_vec(),
                manual: true,
            },
            &mut writer,
            &sender,
        )
        .await
        .unwrap();
        assert_eq!(outcome, Some("success"));
        assert_eq!(service.status.state, "Available");
        assert_eq!(service.status.version, "3.0.0");
        assert_eq!(service.available.as_ref().unwrap().version, "3.0.0");
        assert!(!cache_path(&service.options, format!("{old_hash}.zip")).exists());
    }

    #[tokio::test]
    async fn failed_apply_check_does_not_install_the_cached_release() {
        let temporary = TempDir::new().unwrap();
        let network = FakeNetwork::new([FakeReply::Error(UpdateError::new(
            "network_request_failed",
            "The update request failed",
        ))]);
        let mut service = ready_service(&temporary, network);
        let old_hash = service.available.as_ref().unwrap().sha256.clone();
        let mut writer = BufWriter::new(tokio::io::stdout());
        let (sender, mut receiver) = mpsc::channel(4);

        assert!(
            !handle_command(&mut service, apply_command(), &mut writer, &sender)
                .await
                .unwrap()
        );
        let outcome = handle_operation(
            &mut service,
            receiver.recv().await.unwrap(),
            &mut writer,
            &sender,
        )
        .await
        .unwrap();
        assert_eq!(outcome, Some("failed"));
        assert_eq!(service.status.state, "Failed");
        assert!(!service.awaiting_handoff);
        assert!(cache_path(&service.options, format!("{old_hash}.zip")).exists());
    }

    #[tokio::test]
    async fn successful_check_reuses_only_the_current_cached_payload() {
        let temporary = TempDir::new().unwrap();
        let mut service = ready_service(&temporary, FakeNetwork::new([]));
        let package = service.available.take().unwrap();
        service.status = Status {
            state: "Idle".to_owned(),
            ..Status::default()
        };
        let release = UpdateRelease {
            version: package.version.clone(),
            packages: vec![UpdatePackage {
                variant: "portable".to_owned(),
                kind: "portable".to_owned(),
                path: package.path.clone(),
                size: package.size,
                sha256: package.sha256.clone(),
                files: Vec::new(),
            }],
            envelope: Vec::new(),
        };
        let mut writer = BufWriter::new(tokio::io::stdout());

        assert!(service.available.is_none());
        assert_eq!(service.status.state, "Idle");
        accept_metadata(
            &mut service,
            release,
            b"verified current release".to_vec(),
            false,
            &mut writer,
        )
        .await
        .unwrap();
        assert_eq!(service.status.state, "Ready");
        assert_eq!(service.status.version, "2.0.0");
        assert_eq!(service.available.as_ref().unwrap().sha256, package.sha256);
    }

    #[test]
    fn accepts_only_strong_etags() {
        assert!(strong_etag("\"release-1\""));
        assert!(!strong_etag("W/\"release-1\""));
        assert!(!strong_etag("release-1"));
    }

    #[test]
    fn redirect_origin_includes_effective_port() {
        let https = Url::parse("https://example.test/a").unwrap();
        let same = Url::parse("https://example.test/b").unwrap();
        let other_port = Url::parse("https://example.test:444/b").unwrap();
        assert!(same_origin(&https, &same));
        assert!(!same_origin(&https, &other_port));
    }

    #[test]
    fn local_http_requires_explicit_loopback_allowance() {
        assert!(validate_base_url("http://127.0.0.1:8080", true).is_ok());
        assert!(validate_base_url("http://127.0.0.1:8080", false).is_err());
        assert!(validate_base_url("http://example.test", true).is_err());
    }

    #[test]
    fn failed_cached_payload_requires_a_manual_retry() {
        assert!(release_retry_is_allowed(false, false));
        assert!(!release_retry_is_allowed(false, true));
        assert!(release_retry_is_allowed(true, true));
        assert!(cached_payload_is_ready(false, false, true));
        assert!(!cached_payload_is_ready(false, true, true));
        assert!(cached_payload_is_ready(true, true, true));
        assert!(!cached_payload_is_ready(true, false, false));
    }

    #[tokio::test]
    async fn metadata_request_exposes_proxy_timeout_and_cancellation_policy() {
        let network = FakeNetwork::new([reply(StatusCode::OK, b"signed-envelope")]);
        let clock = FakeClock::new(fixed_time(), Vec::new());
        let (sender, mut receiver) = mpsc::channel(4);
        fetch_metadata(
            MetadataInputs {
                clock: Arc::new(clock.clone()),
                network: Arc::new(network.clone()),
                base_url: Url::parse("http://127.0.0.1:8080/releases/").unwrap(),
                system_proxy: true,
                installed_version: "1.2.3".to_owned(),
                manual: true,
            },
            CancellationToken::new(),
            sender,
        )
        .await;
        match receiver.recv().await.unwrap() {
            OperationMessage::Failed { operation, error } => {
                assert_eq!(operation, ActiveOperation::Check);
                assert_eq!(error.code, "unsupported_signature_schema");
            }
            _ => panic!("expected signature failure"),
        }
        let requests = network.requests();
        assert_eq!(requests.len(), 1);
        assert_eq!(
            requests[0].url.as_str(),
            "http://127.0.0.1:8080/latest-version.json"
        );
        assert_eq!(requests[0].installed_version, "1.2.3");
        assert!(requests[0].system_proxy);
        assert_eq!(requests[0].timeout, METADATA_TIMEOUT);
        assert!(requests[0].range.is_none());
        assert_eq!(clock.sleeps(), vec![METADATA_TIMEOUT]);
    }

    #[tokio::test]
    async fn metadata_timeout_is_driven_by_the_injected_clock() {
        let network = FakeNetwork::new([FakeReply::Pending]);
        let clock = FakeClock::new(fixed_time(), vec![METADATA_TIMEOUT]);
        let (sender, mut receiver) = mpsc::channel(4);
        fetch_metadata(
            MetadataInputs {
                clock: Arc::new(clock),
                network: Arc::new(network),
                base_url: Url::parse("http://127.0.0.1:8080").unwrap(),
                system_proxy: false,
                installed_version: "1.0.0".to_owned(),
                manual: false,
            },
            CancellationToken::new(),
            sender,
        )
        .await;
        match receiver.recv().await.unwrap() {
            OperationMessage::Failed { operation, error } => {
                assert_eq!(operation, ActiveOperation::Check);
                assert_eq!(error.code, "metadata_download_failed");
            }
            _ => panic!("expected metadata timeout"),
        }
    }

    #[tokio::test]
    async fn strong_etag_resume_validates_the_exact_content_range() {
        let temporary = TempDir::new().unwrap();
        let response = FakeReply::Response {
            status: StatusCode::PARTIAL_CONTENT.as_u16(),
            content_length: Some(3),
            content_range: Some("bytes 3-5/6".to_owned()),
            etag: Some("\"release-1\"".to_owned()),
            chunks: vec![Ok(b"def".to_vec())],
        };
        let network = FakeNetwork::new([response]);
        let clock = FakeClock::new(fixed_time(), Vec::new());
        let inputs = download_inputs(
            &temporary,
            Arc::new(network.clone()),
            Arc::new(clock.clone()),
            6,
        );
        initialize_cache(&inputs);
        let partial = cache_path(&inputs.options, format!("{}.part", inputs.package.sha256));
        std::fs::write(&partial, b"abc").unwrap();
        let mut resume = DownloadResume {
            package_hash: inputs.saved_hash.clone(),
            validator: inputs.saved_validator.clone(),
        };
        let (sender, _receiver) = mpsc::channel(8);
        let path = download_once(&inputs, &mut resume, &CancellationToken::new(), &sender)
            .await
            .unwrap();
        assert_eq!(std::fs::read(path).unwrap(), b"abcdef");
        let requests = network.requests();
        assert_eq!(
            requests[0].range,
            Some(HttpRange {
                offset: 3,
                validator: "\"release-1\"".to_owned(),
            })
        );
        assert_eq!(requests[0].timeout, PACKAGE_TIMEOUT);
        assert_eq!(clock.sleeps(), vec![PACKAGE_TIMEOUT]);
    }

    #[tokio::test]
    async fn full_response_to_resume_truncates_the_partial_file() {
        let temporary = TempDir::new().unwrap();
        let network = FakeNetwork::new([reply(StatusCode::OK, b"uvwxyz")]);
        let inputs = download_inputs(
            &temporary,
            Arc::new(network.clone()),
            Arc::new(FakeClock::new(fixed_time(), Vec::new())),
            6,
        );
        initialize_cache(&inputs);
        let partial = cache_path(&inputs.options, format!("{}.part", inputs.package.sha256));
        std::fs::write(&partial, b"abc").unwrap();
        let mut resume = DownloadResume {
            package_hash: inputs.saved_hash.clone(),
            validator: inputs.saved_validator.clone(),
        };
        let (sender, _receiver) = mpsc::channel(8);
        download_once(&inputs, &mut resume, &CancellationToken::new(), &sender)
            .await
            .unwrap();
        assert_eq!(std::fs::read(partial).unwrap(), b"uvwxyz");
        assert_eq!(network.requests()[0].range.as_ref().unwrap().offset, 3);
    }

    #[tokio::test]
    async fn invalid_resume_range_is_rejected_before_writing() {
        let temporary = TempDir::new().unwrap();
        let response = FakeReply::Response {
            status: StatusCode::PARTIAL_CONTENT.as_u16(),
            content_length: Some(3),
            content_range: Some("bytes 2-4/6".to_owned()),
            etag: Some("\"release-1\"".to_owned()),
            chunks: vec![Ok(b"def".to_vec())],
        };
        let inputs = download_inputs(
            &temporary,
            Arc::new(FakeNetwork::new([response])),
            Arc::new(FakeClock::new(fixed_time(), Vec::new())),
            6,
        );
        initialize_cache(&inputs);
        let partial = cache_path(&inputs.options, format!("{}.part", inputs.package.sha256));
        std::fs::write(&partial, b"abc").unwrap();
        let mut resume = DownloadResume {
            package_hash: inputs.saved_hash.clone(),
            validator: inputs.saved_validator.clone(),
        };
        let (sender, _receiver) = mpsc::channel(8);
        let error = download_once(&inputs, &mut resume, &CancellationToken::new(), &sender)
            .await
            .unwrap_err();
        assert_eq!(error.code, "download_range_invalid");
        assert_eq!(std::fs::read(partial).unwrap(), b"abc");
    }

    #[tokio::test]
    async fn download_retries_after_two_and_four_seconds() {
        let temporary = TempDir::new().unwrap();
        let network = FakeNetwork::new([
            FakeReply::Error(UpdateError::new("network_request_failed", "first")),
            FakeReply::Error(UpdateError::new("network_request_failed", "second")),
            reply(StatusCode::OK, b"abc"),
        ]);
        let clock = FakeClock::new(
            fixed_time(),
            vec![Duration::from_secs(2), Duration::from_secs(4)],
        );
        let inputs = download_inputs(
            &temporary,
            Arc::new(network.clone()),
            Arc::new(clock.clone()),
            3,
        );
        initialize_cache(&inputs);
        let (sender, mut receiver) = mpsc::channel(16);
        download_package(inputs, CancellationToken::new(), sender).await;
        let mut downloaded = false;
        while let Ok(message) = receiver.try_recv() {
            if matches!(message, OperationMessage::Downloaded { .. }) {
                downloaded = true;
            }
        }
        assert!(downloaded);
        assert_eq!(network.requests().len(), 3);
        assert_eq!(
            clock.sleeps(),
            vec![
                PACKAGE_TIMEOUT,
                Duration::from_secs(2),
                PACKAGE_TIMEOUT,
                Duration::from_secs(4),
                PACKAGE_TIMEOUT,
            ]
        );
    }

    #[tokio::test]
    async fn cancellation_wins_before_a_network_request() {
        let temporary = TempDir::new().unwrap();
        let network = FakeNetwork::new([FakeReply::Pending]);
        let inputs = download_inputs(
            &temporary,
            Arc::new(network.clone()),
            Arc::new(FakeClock::new(fixed_time(), Vec::new())),
            3,
        );
        initialize_cache(&inputs);
        let cancellation = CancellationToken::new();
        cancellation.cancel();
        let (sender, mut receiver) = mpsc::channel(4);
        download_package(inputs, cancellation, sender).await;
        assert!(matches!(
            receiver.recv().await,
            Some(OperationMessage::Cancelled(ActiveOperation::Download))
        ));
        assert!(network.requests().is_empty());
    }
}
