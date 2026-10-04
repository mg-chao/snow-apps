// Non-Windows hosts expose only the unsupported-platform protocol endpoint.
// The Windows service implementation is retained here for shared protocol tests.
#![cfg_attr(not(windows), allow(dead_code))]

use crate::contract::{MAX_METADATA_BYTES, UpdateRelease, compare_versions, verify_release};
use crate::error::{Result, UpdateError, io_error, require};
use crate::fsutil;
use crate::gitee;
use crate::github;
use crate::progress::{Progress, ProgressAppearance, ProgressPhase, ProgressTexts, ProgressWindow};
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
    pub github_api_url: String,
    pub gitee_api_url: String,
    pub allow_local_http: bool,
    pub parent_pid: u32,
}

#[derive(Clone, Copy, Debug, Deserialize, Serialize, PartialEq, Eq)]
#[serde(rename_all = "lowercase")]
enum ReleaseSource {
    GitHub,
    Gitee,
}

impl ReleaseSource {
    fn other(self) -> Self {
        match self {
            Self::GitHub => Self::Gitee,
            Self::Gitee => Self::GitHub,
        }
    }
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
    #[serde(default, skip_serializing)]
    github_source: bool,
    #[serde(default, skip_serializing_if = "Option::is_none")]
    source: Option<ReleaseSource>,
    #[serde(default)]
    partial_url: String,
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
enum Mode {
    Manual,
    Check,
    Download,
    NextLaunch,
}

impl Mode {
    fn parse(value: &str) -> Option<Self> {
        match value {
            "manual" => Some(Self::Manual),
            "check" => Some(Self::Check),
            "download" => Some(Self::Download),
            "next_launch" => Some(Self::NextLaunch),
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
        source: ReleaseSource,
    },
    DownloadValidator {
        package_hash: String,
        validator: String,
        url: String,
        source: ReleaseSource,
    },
    Progress {
        received: u64,
        total: u64,
    },
    Downloaded {
        partial: PathBuf,
        package: AvailableUpdate,
        source: ReleaseSource,
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
    github_api_url: Url,
    gitee_api_url: Url,
    variant: String,
    installed_version: String,
    persisted: PersistedState,
    available: Option<AvailableUpdate>,
    status: Status,
    mode: Mode,
    trigger: Trigger,
    progress_texts: ProgressTexts,
    progress_appearance: ProgressAppearance,
    progress_window: Option<ProgressWindow>,
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

fn validate_api_url(text: &str, allow_local_http: bool) -> Result<Url> {
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
        if github::redirect_allowed(&origin, attempt.url())
            || gitee::redirect_allowed(&origin, attempt.url())
        {
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
        .map(|mut state| {
            if state.source.is_none() && state.github_source {
                state.source = Some(ReleaseSource::GitHub);
            }
            state.github_source = false;
            state
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
        let github_api_url = validate_api_url(&options.github_api_url, options.allow_local_http)?;
        let gitee_api_url = validate_api_url(&options.gitee_api_url, options.allow_local_http)?;
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
            github_api_url,
            gitee_api_url,
            variant: record.variant,
            installed_version: record.version,
            available: None,
            status: Status {
                state: "Idle".to_owned(),
                ..Status::default()
            },
            mode: Mode::Download,
            trigger: Trigger::Startup,
            progress_texts: ProgressTexts::default(),
            progress_appearance: ProgressAppearance::default(),
            progress_window: None,
            system_proxy: false,
            active: None,
            cancellation: None,
            requested: None,
            awaiting_handoff: false,
            handoff: None,
        };
        service.restore_cached_release();
        service.consume_legacy_result();
        Ok(service)
    }

    fn restore_cached_release(&mut self) {
        let Ok(release) =
            crate::contract::verify_release_file(&cache_path(&self.options, "release.json"))
        else {
            return;
        };
        self.restore_verified_release(release);
    }

    fn restore_verified_release(&mut self, release: UpdateRelease) {
        let Ok(package) = release.update_package(&self.variant) else {
            return;
        };
        if release.version != self.persisted.observed_version
            || package.sha256 != self.persisted.observed_hash
            || !compare_versions(&release.version, &self.installed_version)
                .is_ok_and(|order| order.is_gt())
        {
            return;
        }
        self.status.version.clone_from(&release.version);
        self.status.state = if fsutil::verify_file(
            &cache_path(&self.options, format!("{}.zip", package.sha256)),
            package.size,
            &package.sha256,
        )
        .is_ok()
            && !is_failed_version(&self.options.root, &release.version)
        {
            "Ready"
        } else {
            "Available"
        }
        .to_owned();
        self.available = Some(AvailableUpdate {
            version: release.version.clone(),
            path: package.path.clone(),
            size: package.size,
            sha256: package.sha256.clone(),
        });
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
        self.refresh_progress();
    }

    fn cached_payload_matches(&self) -> bool {
        self.available.as_ref().is_some_and(|package| {
            fsutil::verify_file(
                &cache_path(&self.options, format!("{}.zip", package.sha256)),
                package.size,
                &package.sha256,
            )
            .is_ok()
        })
    }

    fn download_progress_visible(&self) -> bool {
        self.trigger == Trigger::User || self.mode == Mode::Download
    }

    fn download_progress(&self) -> Option<Progress> {
        match self.status.state.as_str() {
            "Downloading" => Some(Progress {
                phase: ProgressPhase::Downloading,
                completed: self.status.received,
                total: self.status.total,
            }),
            "Verifying" => Some(Progress {
                phase: ProgressPhase::Verifying,
                completed: 0,
                total: 0,
            }),
            _ => None,
        }
    }

    fn refresh_progress(&mut self) {
        if let Some(progress) = self.download_progress() {
            if self.download_progress_visible() {
                let window = self.progress_window.get_or_insert_with(|| {
                    ProgressWindow::new_with_appearance(
                        self.progress_texts.clone(),
                        self.progress_appearance.clone(),
                    )
                });
                window.set_visible(true);
                window.update(progress);
            } else if let Some(window) = &self.progress_window {
                window.set_visible(false);
            }
        } else if let Some(window) = self.progress_window.take() {
            window.finish();
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
        .header(
            USER_AGENT,
            format!("{}/{installed_version}", crate::edition::REGISTRY_NAME),
        )
}

fn transport_error(code: &'static str, message: &'static str, error: UpdateError) -> UpdateError {
    UpdateError::new(code, message)
        .detail(error.detail.unwrap_or_else(|| error.message.into_owned()))
}

#[derive(Clone)]
struct ReleaseEligibility {
    variant: String,
    observed_version: String,
    observed_hash: String,
}

impl ReleaseEligibility {
    fn for_service(service: &Service) -> Self {
        Self {
            variant: service.variant.clone(),
            observed_version: service.persisted.observed_version.clone(),
            observed_hash: service.persisted.observed_hash.clone(),
        }
    }

    fn validate(&self, release: &UpdateRelease) -> Result<()> {
        let package = release.update_package(&self.variant)?;
        if !self.observed_version.is_empty() {
            let order = compare_versions(&release.version, &self.observed_version)?;
            require(
                !order.is_lt(),
                "release_replay",
                "The server offered older release metadata",
            )?;
            require(
                !order.is_eq() || package.sha256 == self.observed_hash,
                "release_mutated",
                "The server changed an already published release",
            )?;
        }
        Ok(())
    }
}

#[derive(Clone)]
struct MetadataInputs {
    clock: Arc<dyn Clock>,
    network: Arc<dyn Network>,
    github_api_url: Url,
    gitee_api_url: Url,
    system_proxy: bool,
    installed_version: String,
    manual: bool,
}

async fn metadata_bytes(
    inputs: &MetadataInputs,
    url: Url,
    cancellation: &CancellationToken,
) -> Result<Vec<u8>> {
    let mut deadline = inputs.clock.sleep(METADATA_TIMEOUT);
    let response = tokio::select! {
        biased;
        _ = cancellation.cancelled() => return Err(cancelled()),
        response = inputs.network.get(HttpRequest {
            origin: url.clone(), url, installed_version: inputs.installed_version.clone(),
            system_proxy: inputs.system_proxy, timeout: METADATA_TIMEOUT, range: None,
        }) => response.map_err(|error| transport_error("metadata_download_failed", "Could not download signed update metadata", error))?,
        _ = deadline.as_mut() => return Err(github::error()),
    };
    require(
        response.status == 200,
        "metadata_download_failed",
        "Could not download signed update metadata",
    )?;
    require(
        response
            .content_length
            .is_none_or(|length| length <= MAX_METADATA_BYTES as u64),
        "metadata_too_large",
        "Update metadata is too large",
    )?;
    let mut stream = response.body;
    let mut bytes = Vec::new();
    while let Some(chunk) = tokio::select! {
        biased;
        _ = cancellation.cancelled() => return Err(cancelled()),
        chunk = stream.next() => chunk,
        _ = deadline.as_mut() => return Err(github::error()),
    } {
        let chunk = chunk?;
        require(
            bytes.len().saturating_add(chunk.len()) <= MAX_METADATA_BYTES,
            "metadata_too_large",
            "Update metadata is too large",
        )?;
        bytes.extend_from_slice(&chunk);
    }
    Ok(bytes)
}

fn cancelled() -> UpdateError {
    UpdateError::new("operation_cancelled", "The update download was interrupted")
}

async fn github_release(
    inputs: &MetadataInputs,
    exact: Option<&str>,
    cancellation: &CancellationToken,
) -> Result<Value> {
    let mut best: Option<(semver::Version, Value)> = None;
    let base = inputs.github_api_url.as_str().trim_end_matches('/');
    for page in 1..=10 {
        let url = match exact {
            Some(version) => format!("{base}/tags/v{version}_snow-shot"),
            None => format!("{base}?per_page=100&page={page}"),
        };
        let bytes = metadata_bytes(
            inputs,
            Url::parse(&url).map_err(|_| github::error())?,
            cancellation,
        )
        .await?;
        let value: Value = serde_json::from_slice(&bytes).map_err(|_| github::error())?;
        if let Some(exact) = exact {
            require(
                github::version(&value).is_some_and(|version| version.to_string() == exact),
                "metadata_download_failed",
                "Could not download signed update metadata",
            )?;
            return Ok(value);
        }
        let releases = value.as_array().ok_or_else(github::error)?;
        require(
            releases.len() <= 100,
            "metadata_too_large",
            "Update metadata is too large",
        )?;
        for release in releases {
            if let Some(version) = github::version(release)
                && best
                    .as_ref()
                    .is_none_or(|(current, _)| version.cmp_precedence(current).is_gt())
            {
                best = Some((version, release.clone()));
            }
        }
        if releases.len() < 100 {
            return best.map(|(_, release)| release).ok_or_else(github::error);
        }
    }
    Err(github::error().detail("GitHub release discovery exceeded ten pages"))
}

async fn gitee_release(
    inputs: &MetadataInputs,
    exact: Option<&str>,
    cancellation: &CancellationToken,
) -> Result<Value> {
    let candidates = release_candidates(inputs, ReleaseSource::Gitee, cancellation).await?;
    candidates
        .into_iter()
        .find(|release| {
            let version = gitee::version(release);
            exact.map_or(version.is_some(), |exact| {
                version.is_some_and(|version| version.to_string() == exact)
            })
        })
        .ok_or_else(gitee::error)
}
async fn gitee_assets(
    inputs: &MetadataInputs,
    release: &Value,
    cancellation: &CancellationToken,
) -> Result<Value> {
    let id = release["id"].as_u64().ok_or_else(gitee::error)?;
    let url = Url::parse(&format!(
        "{}/{id}/attach_files?per_page=100",
        inputs.gitee_api_url.as_str().trim_end_matches('/')
    ))
    .map_err(|_| gitee::error())?;
    let bytes = metadata_bytes(inputs, url, cancellation).await?;
    let files: Value = serde_json::from_slice(&bytes).map_err(|_| gitee::error())?;
    require(
        files.as_array().is_some_and(|files| files.len() < 100),
        "metadata_download_failed",
        "Could not download signed update metadata",
    )?;
    Ok(files)
}

async fn release_candidates(
    inputs: &MetadataInputs,
    source: ReleaseSource,
    cancellation: &CancellationToken,
) -> Result<Vec<Value>> {
    let base = match source {
        ReleaseSource::GitHub => inputs.github_api_url.as_str(),
        ReleaseSource::Gitee => inputs.gitee_api_url.as_str(),
    }
    .trim_end_matches('/');
    let mut candidates = Vec::new();
    for page in 1..=10 {
        let url =
            Url::parse(&format!("{base}?per_page=100&page={page}")).map_err(|_| github::error())?;
        let bytes = metadata_bytes(inputs, url, cancellation).await?;
        let response: Value = serde_json::from_slice(&bytes).map_err(|_| github::error())?;
        let releases = response.as_array().ok_or_else(github::error)?;
        require(
            releases.len() <= 100,
            "metadata_too_large",
            "Update metadata is too large",
        )?;
        for release in releases {
            let version = match source {
                ReleaseSource::GitHub => github::version(release),
                ReleaseSource::Gitee => gitee::version(release),
            };
            if let Some(version) = version {
                candidates.push((version, release.clone()));
            }
        }
        if releases.len() < 100 {
            candidates.sort_by(|left, right| right.0.cmp_precedence(&left.0));
            return Ok(candidates.into_iter().map(|(_, release)| release).collect());
        }
    }
    Err(github::error().detail("Release discovery exceeded ten pages"))
}

async fn source_metadata(
    inputs: &MetadataInputs,
    source: ReleaseSource,
    eligibility: &ReleaseEligibility,
    cancellation: &CancellationToken,
    trusted_keys: Option<&[u8]>,
) -> Result<(UpdateRelease, Vec<u8>, ReleaseSource)> {
    let candidates = release_candidates(inputs, source, cancellation).await?;
    for release in candidates {
        let attempt: Result<(UpdateRelease, Vec<u8>, ReleaseSource)> = async {
            let (url, files) = match source {
                ReleaseSource::GitHub => {
                    (github::asset(&release, crate::edition::FEED_NAME)?, None)
                }
                ReleaseSource::Gitee => {
                    let files = gitee_assets(inputs, &release, cancellation).await?;
                    let tag = release["tag_name"].as_str().ok_or_else(gitee::error)?;
                    (
                        gitee::asset(&files, tag, crate::edition::FEED_NAME)?,
                        Some(files),
                    )
                }
            };
            let bytes = metadata_bytes(inputs, url, cancellation).await?;
            let verified = verify_release(&bytes, trusted_keys)?;
            eligibility.validate(&verified)?;
            let version = match source {
                ReleaseSource::GitHub => github::version(&release),
                ReleaseSource::Gitee => gitee::version(&release),
            };
            require(
                version.is_some_and(|version| version.to_string() == verified.version),
                "metadata_download_failed",
                "Could not download signed update metadata",
            )?;
            for package in &verified.packages {
                let name = github::package_name(&verified.version, &package.path)?;
                match source {
                    ReleaseSource::GitHub => {
                        github::asset(&release, &name)?;
                    }
                    ReleaseSource::Gitee => {
                        let tag = release["tag_name"].as_str().ok_or_else(gitee::error)?;
                        gitee::asset(files.as_ref().ok_or_else(gitee::error)?, tag, &name)?;
                    }
                }
            }
            Ok((verified, bytes, source))
        }
        .await;
        match attempt {
            Ok(release) => return Ok(release),
            Err(error) if error.code == "operation_cancelled" => return Err(error),
            Err(_) => continue,
        }
    }
    Err(github::error())
}

async fn fetch_metadata(
    inputs: MetadataInputs,
    eligibility: ReleaseEligibility,
    cancellation: CancellationToken,
    sender: mpsc::Sender<OperationMessage>,
) {
    fetch_metadata_trusted(inputs, eligibility, cancellation, sender, None).await;
}

async fn fetch_metadata_trusted(
    inputs: MetadataInputs,
    eligibility: ReleaseEligibility,
    cancellation: CancellationToken,
    sender: mpsc::Sender<OperationMessage>,
    trusted_keys: Option<&[u8]>,
) {
    let mut tasks = tokio::task::JoinSet::new();
    for source in [ReleaseSource::GitHub, ReleaseSource::Gitee] {
        let inputs = inputs.clone();
        let cancellation = cancellation.clone();
        let keys = trusted_keys.map(ToOwned::to_owned);
        let eligibility = eligibility.clone();
        tasks.spawn(async move {
            source_metadata(
                &inputs,
                source,
                &eligibility,
                &cancellation,
                keys.as_deref(),
            )
            .await
        });
    }
    tokio::task::yield_now().await;
    let mut result = Err(github::error());
    for _ in 0..2 {
        let next = tokio::select! {
            biased;
            _ = cancellation.cancelled() => { result = Err(cancelled()); break; }
            next = tasks.join_next() => next,
        };
        match next {
            Some(Ok(Ok(value))) => {
                result = Ok(value);
                break;
            }
            Some(Ok(Err(error))) => result = Err(error),
            Some(Err(error)) => result = Err(github::error().detail(error)),
            None => break,
        }
    }
    tasks.abort_all();
    let message = match result {
        Ok((release, bytes, source)) => OperationMessage::Metadata {
            release,
            bytes,
            manual: inputs.manual,
            source,
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
    let url = inputs.package_url.clone().ok_or_else(github::error)?;
    let partial = cache_path(&inputs.options, format!("{}.part", package.sha256));
    let mut offset = tokio::fs::metadata(&partial)
        .await
        .map(|value| value.len())
        .unwrap_or(0);
    let can_resume = resume.url == url.as_str()
        && resume.package_hash == package.sha256
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

    let mut deadline = inputs.clock.sleep(PACKAGE_TIMEOUT);
    let response = tokio::select! {
        biased;
        _ = cancellation.cancelled() => return Err(UpdateError::new("operation_cancelled", "The update download was interrupted")),
        response = inputs.network.get(HttpRequest {
            origin: url.clone(),
            url: url.clone(),
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
    resume.url = url.to_string();
    let _ = sender
        .send(OperationMessage::DownloadValidator {
            package_hash: package.sha256.clone(),
            validator,
            url: url.to_string(),
            source: inputs.source,
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
    github_api_url: Url,
    gitee_api_url: Url,
    system_proxy: bool,
    installed_version: String,
    package: AvailableUpdate,
    saved_hash: String,
    saved_validator: String,
    saved_url: String,
    source: ReleaseSource,
    package_url: Option<Url>,
}

struct DownloadResume {
    package_hash: String,
    validator: String,
    url: String,
}

async fn download_package(
    mut inputs: DownloadInputs,
    cancellation: CancellationToken,
    sender: mpsc::Sender<OperationMessage>,
) {
    let mut last_error = None;
    if cancellation.is_cancelled() {
        let _ = sender
            .send(OperationMessage::Cancelled(ActiveOperation::Download))
            .await;
        return;
    }
    let mut resume = DownloadResume {
        package_hash: inputs.saved_hash.clone(),
        validator: inputs.saved_validator.clone(),
        url: inputs.saved_url.clone(),
    };
    for source in [inputs.source, inputs.source.other()] {
        inputs.source = source;
        if inputs.package_url.is_none() {
            let metadata = MetadataInputs {
                clock: inputs.clock.clone(),
                network: inputs.network.clone(),
                github_api_url: inputs.github_api_url.clone(),
                gitee_api_url: inputs.gitee_api_url.clone(),
                system_proxy: inputs.system_proxy,
                installed_version: inputs.installed_version.clone(),
                manual: false,
            };
            let resolved = async {
                let name = github::package_name(&inputs.package.version, &inputs.package.path)?;
                match source {
                    ReleaseSource::GitHub => {
                        let release =
                            github_release(&metadata, Some(&inputs.package.version), &cancellation)
                                .await?;
                        github::asset(&release, &name)
                    }
                    ReleaseSource::Gitee => {
                        let release =
                            gitee_release(&metadata, Some(&inputs.package.version), &cancellation)
                                .await?;
                        let files = gitee_assets(&metadata, &release, &cancellation).await?;
                        let tag = release["tag_name"].as_str().ok_or_else(gitee::error)?;
                        gitee::asset(&files, tag, &name)
                    }
                }
            }
            .await;
            match resolved {
                Ok(url) => inputs.package_url = Some(url),
                Err(error) if error.code == "operation_cancelled" => {
                    let _ = sender
                        .send(OperationMessage::Cancelled(ActiveOperation::Download))
                        .await;
                    return;
                }
                Err(error) => {
                    if last_error.is_none() {
                        last_error = Some(error);
                    }
                    continue;
                }
            }
        }
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
            let result = async {
                let partial = download_once(&inputs, &mut resume, &cancellation, &sender).await?;
                let path = partial.clone();
                let package = inputs.package.clone();
                tokio::task::spawn_blocking(move || {
                    fsutil::verify_file(&path, package.size, &package.sha256)
                })
                .await
                .map_err(|error| github::error().detail(error))??;
                Ok::<_, UpdateError>(partial)
            }
            .await;
            match result {
                Ok(partial) => {
                    let _ = sender
                        .send(OperationMessage::Downloaded {
                            partial,
                            package: inputs.package,
                            source,
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
                Err(error) => {
                    if error.code == "payload_verify_failed"
                        || error.code == "update_payload_mismatch"
                    {
                        resume.validator.clear();
                    }
                    last_error = Some(error);
                }
            }
        }
        inputs.package_url = None;
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
            github_api_url: service.github_api_url.clone(),
            gitee_api_url: service.gitee_api_url.clone(),
            system_proxy: service.system_proxy,
            installed_version: service.installed_version.clone(),
            manual: user_initiated,
        },
        ReleaseEligibility::for_service(service),
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
    service.refresh_progress();
    let cancellation = CancellationToken::new();
    service.cancellation = Some(cancellation.clone());
    tokio::spawn(download_package(
        DownloadInputs {
            options: service.options.clone(),
            clock: service.dependencies.clock.clone(),
            network: service.dependencies.network.clone(),
            github_api_url: service.github_api_url.clone(),
            gitee_api_url: service.gitee_api_url.clone(),
            system_proxy: service.system_proxy,
            installed_version: service.installed_version.clone(),
            package,
            saved_hash: service.persisted.partial_hash.clone(),
            saved_validator: service.persisted.validator.clone(),
            saved_url: service.persisted.partial_url.clone(),
            source: service.persisted.source.unwrap_or(ReleaseSource::GitHub),
            package_url: None,
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
    ReleaseEligibility::for_service(service).validate(&release)?;
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
    let texts = service.progress_texts.clone();
    let appearance = service.progress_appearance.clone();
    tokio::spawn(async move {
        let result = crate::coordination::prepare_service_handoff(args, texts, appearance).await;
        let _ = sender.send(OperationMessage::HandoffPrepared(result)).await;
    });
    Ok(())
}

fn start_cached_apply(service: &mut Service) -> bool {
    if service.status.state != "Ready" || service.active.is_some() {
        return false;
    }
    service.active = Some(ActiveOperation::Apply);
    service.set_state("Applying", None);
    true
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
                    service.trigger = trigger;
                    service.system_proxy = system_proxy;
                    if let Some(texts) = command.progress_texts {
                        service.progress_texts = texts;
                    }
                    if let Some(appearance) = command.progress_appearance {
                        service.progress_appearance = appearance;
                    }
                    let started = match operation {
                        RequestedOperation::Probe => Ok(()),
                        RequestedOperation::Check => {
                            start_check(service, trigger.user_initiated(), operation_sender)
                        }
                        RequestedOperation::Download => {
                            let failed_version = service.available.as_ref().is_some_and(|update| {
                                is_failed_version(&service.options.root, &update.version)
                            });
                            let explicit_retry = trigger == Trigger::User
                                || (mode != Mode::NextLaunch && trigger.user_initiated());
                            if release_retry_is_allowed(explicit_retry, failed_version) {
                                start_download(service, operation_sender)
                            } else {
                                complete = Some("success");
                                Ok(())
                            }
                        }
                        RequestedOperation::Apply => {
                            if trigger == Trigger::Startup && mode == Mode::NextLaunch {
                                if start_cached_apply(service) {
                                    prepare_apply(service, operation_sender)
                                } else {
                                    // Next-launch updates use only an already verified cache. A
                                    // missing, stale, partial, or failed release is a startup no-op.
                                    complete = Some("success");
                                    Ok(())
                                }
                            } else if service.active.is_some()
                                || !(service.status.state == "Ready"
                                    || (trigger == Trigger::User
                                        && matches!(
                                            service.status.state.as_str(),
                                            "Available" | "Failed"
                                        )
                                        && service.cached_payload_matches()))
                            {
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
        "configure" => (|| {
            let mode = command
                .mode
                .as_deref()
                .and_then(Mode::parse)
                .ok_or_else(|| {
                    UpdateError::new("protocol_enum_invalid", "Invalid updater command argument")
                })?;
            service.mode = mode;
            if let Some(texts) = command.progress_texts {
                service.progress_texts = texts;
                if let Some(window) = &service.progress_window {
                    window.set_texts(service.progress_texts.clone());
                }
            }
            if let Some(appearance) = command.progress_appearance {
                service.progress_appearance = appearance;
                if let Some(window) = &service.progress_window {
                    window.set_appearance(service.progress_appearance.clone());
                }
            }
            service.refresh_progress();
            Ok(())
        })(),
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
                let handoff = service.handoff.as_mut().ok_or_else(|| {
                    UpdateError::new("protocol_state_invalid", "Invalid updater command argument")
                })?;
                match handoff.decide(proceed, command.relaunch_arguments).await {
                    Ok(()) if proceed => {}
                    Ok(()) => {
                        service.handoff = None;
                        service.set_state(
                            "Ready",
                            command.reason.as_deref().map(UpdateError::from_message),
                        );
                        complete = Some("cancelled");
                    }
                    Err(error) => {
                        service.handoff = None;
                        service.set_state("Failed", Some(error.clone()));
                        complete = Some("failed");
                        return Err(error);
                    }
                }
                Ok(())
            };
            decision.await
        }
        "relaunch_arguments" => {
            let update = async {
                require(
                    !service.awaiting_handoff && service.status.state == "Applying",
                    "protocol_state_invalid",
                    "Invalid updater command argument",
                )?;
                let arguments = command.relaunch_arguments.ok_or_else(|| {
                    UpdateError::new(
                        "invalid_updater_argument",
                        "Invalid updater command argument",
                    )
                })?;
                let handoff = service.handoff.as_mut().ok_or_else(|| {
                    UpdateError::new("protocol_state_invalid", "Invalid updater command argument")
                })?;
                handoff.set_relaunch_arguments(arguments).await
            };
            let result = update.await;
            if let Err(error) = &result {
                service.handoff = None;
                service.set_state("Failed", Some(error.clone()));
                complete = Some("failed");
            }
            result
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
    if command.command == "shutdown" {
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
            source,
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
            service.persisted.source = Some(source);
            write_persisted(&service.options, &service.persisted)?;
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
            url,
            source,
        } => {
            if service.active == Some(ActiveOperation::Download) {
                service.persisted.partial_hash = package_hash;
                service.persisted.validator = validator;
                service.persisted.partial_url = url;
                service.persisted.source = Some(source);
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
                service.refresh_progress();
                write_status(writer, &service.status).await?;
            }
        }
        OperationMessage::Downloaded {
            partial,
            package,
            source,
        } => {
            if service.active != Some(ActiveOperation::Download) {
                return Ok(None);
            }
            service.set_state("Verifying", None);
            service.status.received = package.size;
            service.status.total = package.size;
            service.refresh_progress();
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
            service.persisted.partial_url.clear();
            service.persisted.source = Some(source);
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
            Ok(mut handoff) => {
                let _ = handoff.decide(false, None).await;
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
    use std::collections::{HashMap, VecDeque};
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

    type FakeRoutes = Arc<Mutex<HashMap<String, VecDeque<FakeReply>>>>;

    #[derive(Clone)]
    struct FakeNetwork {
        replies: Arc<Mutex<VecDeque<FakeReply>>>,
        routes: Option<FakeRoutes>,
        requests: Arc<Mutex<Vec<HttpRequest>>>,
    }

    impl FakeNetwork {
        fn new(replies: impl IntoIterator<Item = FakeReply>) -> Self {
            Self {
                replies: Arc::new(Mutex::new(replies.into_iter().collect())),
                routes: None,
                requests: Arc::new(Mutex::new(Vec::new())),
            }
        }

        fn routed(routes: impl IntoIterator<Item = (String, Vec<FakeReply>)>) -> Self {
            let mut network = Self::new([]);
            network.routes = Some(Arc::new(Mutex::new(
                routes
                    .into_iter()
                    .map(|(url, replies)| (url, replies.into()))
                    .collect(),
            )));
            network
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
            let routes = self.routes.clone();
            Box::pin(async move {
                let url = request.url.to_string();
                requests.lock().unwrap().push(request);
                let reply = if let Some(routes) = routes {
                    routes
                        .lock()
                        .unwrap()
                        .get_mut(&url)
                        .unwrap_or_else(|| panic!("missing fake route: {url}"))
                        .pop_front()
                        .expect("fake route reply")
                } else {
                    replies
                        .lock()
                        .unwrap()
                        .pop_front()
                        .expect("fake network reply")
                };
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
                github_api_url: "http://127.0.0.1:8080".to_owned(),
                gitee_api_url: "http://127.0.0.1:8080".to_owned(),
                allow_local_http: true,
                parent_pid: 1,
            },
            clock,
            network,
            github_api_url: Url::parse("http://127.0.0.1:8080").unwrap(),
            gitee_api_url: Url::parse("http://127.0.0.1:8080").unwrap(),
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
            saved_url: "http://127.0.0.1:8080/snow-shot-portable.zip".to_owned(),
            source: ReleaseSource::GitHub,
            package_url: Some(Url::parse("http://127.0.0.1:8080/snow-shot-portable.zip").unwrap()),
        }
    }

    fn initialize_cache(inputs: &DownloadInputs) {
        std::fs::create_dir_all(&inputs.options.cache_directory).unwrap();
    }

    fn ready_service(temporary: &TempDir, network: FakeNetwork) -> Service {
        let options = ServiceOptions {
            root: temporary.path().join("root"),
            cache_directory: temporary.path().join("cache"),
            github_api_url: "http://127.0.0.1:8080".to_owned(),
            gitee_api_url: "http://127.0.0.1:8080".to_owned(),
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
            github_api_url: Url::parse("http://127.0.0.1:8080").unwrap(),
            gitee_api_url: Url::parse("http://127.0.0.1:8080").unwrap(),
            variant: "portable".to_owned(),
            installed_version: "1.0.0".to_owned(),
            persisted: PersistedState {
                observed_version: "2.0.0".to_owned(),
                observed_hash: hash.clone(),
                ..PersistedState::default()
            },
            available: Some(AvailableUpdate {
                version: "2.0.0".to_owned(),
                path: format!("{}portable.zip", crate::edition::PACKAGE_PREFIX),
                size: 11,
                sha256: hash,
            }),
            status: Status {
                state: "Ready".to_owned(),
                version: "2.0.0".to_owned(),
                ..Status::default()
            },
            mode: Mode::Download,
            trigger: Trigger::Startup,
            progress_texts: ProgressTexts::default(),
            progress_appearance: ProgressAppearance::default(),
            progress_window: None,
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

    fn startup_apply_command() -> Command {
        serde_json::from_value(json!({
            "protocol": PROTOCOL_VERSION,
            "id": 1,
            "command": "execute",
            "operation": "apply",
            "trigger": "startup",
            "mode": "next_launch",
            "systemProxy": false
        }))
        .unwrap()
    }

    #[tokio::test]
    async fn relaunch_updates_require_a_committed_handoff() {
        let temporary = TempDir::new().unwrap();
        let mut service = ready_service(&temporary, FakeNetwork::new([]));
        service.requested = Some(RequestedOperation::Apply);
        service.active = Some(ActiveOperation::Apply);
        service.status.state = "Applying".to_owned();
        let command = serde_json::from_value(json!({
            "protocol": PROTOCOL_VERSION, "id": 2, "command": "relaunch_arguments",
            "relaunchArguments": ["--show-main-window", "--skip-startup-update"]
        }))
        .unwrap();
        let (sender, _receiver) = mpsc::channel(4);
        let mut writer = BufWriter::new(tokio::io::stdout());
        assert!(
            handle_command(&mut service, command, &mut writer, &sender)
                .await
                .unwrap()
        );
        assert_eq!(service.status.state, "Failed");
        assert!(service.handoff.is_none());
    }

    #[tokio::test]
    async fn next_launch_startup_without_a_ready_cache_is_an_offline_no_op() {
        for state in ["Idle", "Available", "Failed"] {
            let temporary = TempDir::new().unwrap();
            let network = FakeNetwork::new([]);
            let mut service = ready_service(&temporary, network.clone());
            service.status.state = state.to_owned();
            let mut writer = BufWriter::new(tokio::io::stdout());
            let (sender, mut receiver) = mpsc::channel(4);
            assert!(
                handle_command(&mut service, startup_apply_command(), &mut writer, &sender)
                    .await
                    .unwrap()
            );
            assert_eq!(service.status.state, state);
            assert!(service.active.is_none());
            assert!(receiver.try_recv().is_err());
            assert!(network.requests().is_empty());
        }
    }

    #[test]
    fn next_launch_cached_apply_starts_without_a_network_check() {
        let temporary = TempDir::new().unwrap();
        let network = FakeNetwork::new([]);
        let mut service = ready_service(&temporary, network.clone());
        assert!(start_cached_apply(&mut service));
        assert_eq!(service.active, Some(ActiveOperation::Apply));
        assert_eq!(service.status.state, "Applying");
        assert!(network.requests().is_empty());
        assert!(!start_cached_apply(&mut service));
    }

    #[test]
    fn next_launch_cache_readiness_rejects_partial_stale_and_failed_releases() {
        for scenario in [
            "ready",
            "missing",
            "corrupt",
            "stale",
            "wrong_variant",
            "failed",
        ] {
            let temporary = TempDir::new().unwrap();
            let mut service = ready_service(&temporary, FakeNetwork::new([]));
            let update = service.available.take().unwrap();
            let release = UpdateRelease {
                version: update.version.clone(),
                packages: vec![UpdatePackage {
                    variant: if scenario == "wrong_variant" {
                        "offline"
                    } else {
                        "portable"
                    }
                    .to_owned(),
                    kind: "portable".to_owned(),
                    path: update.path.clone(),
                    size: update.size,
                    sha256: update.sha256.clone(),
                    files: Vec::new(),
                }],
                envelope: Vec::new(),
            };
            let payload = cache_path(&service.options, format!("{}.zip", update.sha256));
            match scenario {
                "missing" => {
                    std::fs::rename(&payload, payload.with_extension("part")).unwrap();
                }
                "corrupt" => std::fs::write(&payload, b"corrupt payload").unwrap(),
                "stale" => service.installed_version.clone_from(&update.version),
                "failed" => {
                    std::fs::create_dir_all(
                        failed_version_path(&service.options.root).parent().unwrap(),
                    )
                    .unwrap();
                    std::fs::write(failed_version_path(&service.options.root), &update.version)
                        .unwrap();
                }
                _ => {}
            }
            service.status = Status {
                state: "Idle".to_owned(),
                ..Status::default()
            };
            service.restore_verified_release(release);
            assert_eq!(
                service.status.state == "Ready",
                scenario == "ready",
                "{scenario}"
            );
            assert_eq!(
                start_cached_apply(&mut service),
                scenario == "ready",
                "{scenario}"
            );
        }
    }

    #[tokio::test]
    async fn next_launch_policy_change_does_not_retry_a_failed_release() {
        let temporary = TempDir::new().unwrap();
        let network = FakeNetwork::new([]);
        let mut service = ready_service(&temporary, network.clone());
        std::fs::create_dir_all(failed_version_path(&service.options.root).parent().unwrap())
            .unwrap();
        std::fs::write(failed_version_path(&service.options.root), "2.0.0").unwrap();
        service.status.state = "Available".to_owned();
        let command = serde_json::from_value(json!({
            "protocol": PROTOCOL_VERSION, "id": 1, "command": "execute",
            "operation": "download", "trigger": "policyChange", "mode": "next_launch",
            "systemProxy": false
        }))
        .unwrap();
        let (sender, mut receiver) = mpsc::channel(4);
        let mut writer = BufWriter::new(tokio::io::stdout());
        assert!(
            handle_command(&mut service, command, &mut writer, &sender)
                .await
                .unwrap()
        );
        assert!(service.active.is_none());
        assert!(receiver.try_recv().is_err());
        assert!(network.requests().is_empty());
    }

    #[tokio::test]
    async fn configure_changes_download_visibility_without_restarting_the_operation() {
        let temporary = TempDir::new().unwrap();
        let network = FakeNetwork::new([]);
        let mut service = ready_service(&temporary, network.clone());
        service.requested = Some(RequestedOperation::Download);
        service.trigger = Trigger::PolicyChange;
        service.active = Some(ActiveOperation::Download);
        // Test policy independently of native HWND creation.
        service.status.state = "Available".to_owned();
        let (sender, _receiver) = mpsc::channel(4);
        let mut writer = BufWriter::new(tokio::io::stdout());
        for (id, mode, visible) in [
            (1, "next_launch", false),
            (2, "download", true),
            (3, "manual", false),
        ] {
            let command = serde_json::from_value(json!({
                "protocol": PROTOCOL_VERSION,
                "id": id,
                "command": "configure",
                "mode": mode,
                "progressTexts": {"title": "Translated update title"},
                "progressAppearance": {
                    "background": if id == 2 { 0x141414 } else { 0xffffff },
                    "primary": 0x52c41a,
                    "motion": false
                }
            }))
            .unwrap();
            assert!(
                !handle_command(&mut service, command, &mut writer, &sender)
                    .await
                    .unwrap()
            );
            assert_eq!(service.download_progress_visible(), visible);
            assert_eq!(service.active, Some(ActiveOperation::Download));
            assert_eq!(service.requested, Some(RequestedOperation::Download));
            assert_eq!(service.progress_texts.title, "Translated update title");
            assert_eq!(
                service.progress_appearance.background,
                if id == 2 { 0x141414 } else { 0xffffff }
            );
            assert_eq!(service.progress_appearance.primary, 0x52c41a);
            assert!(!service.progress_appearance.motion);
        }
        service.trigger = Trigger::User;
        service.mode = Mode::NextLaunch;
        assert!(service.download_progress_visible());
        assert!(network.requests().is_empty());
    }

    #[test]
    fn verification_progress_is_indeterminate_without_changing_protocol_bytes() {
        let temporary = TempDir::new().unwrap();
        let mut service = ready_service(&temporary, FakeNetwork::new([]));
        service.status.state = "Verifying".to_owned();
        service.status.received = 11;
        service.status.total = 11;
        assert_eq!(
            service.download_progress(),
            Some(Progress::new(ProgressPhase::Verifying, 0, 0))
        );
        assert_eq!((service.status.received, service.status.total), (11, 11));
        service.status.state = "Downloading".to_owned();
        service.status.received = 4;
        assert_eq!(
            service.download_progress(),
            Some(Progress::new(ProgressPhase::Downloading, 4, 11))
        );
    }

    #[tokio::test]
    async fn apply_checks_for_a_new_release_before_using_a_cached_payload() {
        let temporary = TempDir::new().unwrap();
        let network = FakeNetwork::new([FakeReply::Pending, FakeReply::Pending]);
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
        assert_eq!(network.requests().len(), 2);

        let release = UpdateRelease {
            version: "3.0.0".to_owned(),
            packages: vec![UpdatePackage {
                variant: "portable".to_owned(),
                kind: "portable".to_owned(),
                path: format!("{}portable.zip", crate::edition::PACKAGE_PREFIX),
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
                source: ReleaseSource::GitHub,
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
    async fn manual_apply_revalidates_a_cached_payload_after_a_failed_installation() {
        for state in ["Available", "Failed"] {
            let temporary = TempDir::new().unwrap();
            let network = FakeNetwork::new([FakeReply::Pending, FakeReply::Pending]);
            let mut service = ready_service(&temporary, network.clone());
            fsutil::write_atomic(&failed_version_path(&service.options.root), b"2.0.0").unwrap();
            service.status.state = state.to_owned();
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
            assert_eq!(network.requests().len(), 2);
            assert!(!service.awaiting_handoff);
        }
    }

    #[tokio::test]
    async fn failed_apply_check_does_not_install_the_cached_release() {
        let temporary = TempDir::new().unwrap();
        let network = FakeNetwork::new([
            FakeReply::Error(UpdateError::new(
                "network_request_failed",
                "The update request failed",
            )),
            reply(StatusCode::SERVICE_UNAVAILABLE, b""),
        ]);
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
        assert!(github::redirect_allowed(&https, &same));
        assert!(!github::redirect_allowed(&https, &other_port));
    }

    #[test]
    fn local_http_requires_explicit_loopback_allowance() {
        assert!(validate_api_url("http://127.0.0.1:8080", true).is_ok());
        assert!(validate_api_url("http://127.0.0.1:8080", false).is_err());
        assert!(validate_api_url("http://example.test", true).is_err());
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
        let network = FakeNetwork::new([
            reply(StatusCode::OK, b"signed-envelope"),
            reply(StatusCode::SERVICE_UNAVAILABLE, b""),
        ]);
        let clock = FakeClock::new(fixed_time(), Vec::new());
        let (sender, mut receiver) = mpsc::channel(4);
        fetch_metadata(
            MetadataInputs {
                clock: Arc::new(clock.clone()),
                network: Arc::new(network.clone()),
                github_api_url: Url::parse("http://127.0.0.1:8080/releases/").unwrap(),
                gitee_api_url: Url::parse("http://127.0.0.1:8080/releases/").unwrap(),
                system_proxy: true,
                installed_version: "1.2.3".to_owned(),
                manual: true,
            },
            fresh_eligibility(),
            CancellationToken::new(),
            sender,
        )
        .await;
        match receiver.recv().await.unwrap() {
            OperationMessage::Failed { operation, error } => {
                assert_eq!(operation, ActiveOperation::Check);
                assert_eq!(error.code, "metadata_download_failed");
            }
            _ => panic!("expected signature failure"),
        }
        let requests = network.requests();
        assert_eq!(requests.len(), 2);
        assert_eq!(
            requests[0].url.as_str(),
            "http://127.0.0.1:8080/releases?per_page=100&page=1"
        );
        assert_eq!(requests[0].installed_version, "1.2.3");
        assert!(requests[0].system_proxy);
        assert_eq!(requests[0].timeout, METADATA_TIMEOUT);
        assert!(requests[0].range.is_none());
        assert_eq!(clock.sleeps(), vec![METADATA_TIMEOUT, METADATA_TIMEOUT]);
    }

    #[tokio::test]
    async fn metadata_timeout_is_driven_by_the_injected_clock() {
        let network = FakeNetwork::new([FakeReply::Pending, FakeReply::Pending]);
        let clock = FakeClock::new(fixed_time(), vec![METADATA_TIMEOUT]);
        let (sender, mut receiver) = mpsc::channel(4);
        fetch_metadata(
            MetadataInputs {
                clock: Arc::new(clock),
                network: Arc::new(network),
                github_api_url: Url::parse("http://127.0.0.1:8080").unwrap(),
                gitee_api_url: Url::parse("http://127.0.0.1:8080").unwrap(),
                system_proxy: false,
                installed_version: "1.0.0".to_owned(),
                manual: false,
            },
            fresh_eligibility(),
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
            url: inputs.saved_url.clone(),
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
            url: inputs.saved_url.clone(),
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
            url: inputs.saved_url.clone(),
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
        let mut inputs = download_inputs(
            &temporary,
            Arc::new(network.clone()),
            Arc::new(clock.clone()),
            3,
        );
        inputs.package.sha256 =
            "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad".to_owned();
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
    fn github_fixture(version: &str, asset_names: &[&str]) -> Value {
        let mut names: Vec<String> = asset_names.iter().map(|name| (*name).to_owned()).collect();
        if asset_names.contains(&crate::edition::FEED_NAME) {
            names.extend(
                [
                    "online.exe",
                    "online-update.zip",
                    "offline.exe",
                    "offline-update.zip",
                    "portable.zip",
                ]
                .map(|suffix| {
                    format!(
                        "{product}-{version}-windows-x64-{suffix}",
                        product = crate::edition::PRODUCT
                    )
                }),
            );
        }
        json!({"draft":false,"prerelease":false,"tag_name":format!("v{version}_snow-shot"),
            "assets":names.iter().map(|name| json!({"name":name,"browser_download_url":
                format!("{}/releases/download/v{version}_snow-shot/{name}",github::REPOSITORY)})).collect::<Vec<_>>()})
    }

    fn metadata_inputs(network: &FakeNetwork) -> MetadataInputs {
        MetadataInputs {
            clock: Arc::new(FakeClock::new(fixed_time(), Vec::new())),
            network: Arc::new(network.clone()),
            github_api_url: Url::parse(github::API).unwrap(),
            gitee_api_url: Url::parse("https://gitee.com/api/v5/repos/mg-chao/snow-apps/releases")
                .unwrap(),
            system_proxy: false,
            installed_version: "1.0.0".to_owned(),
            manual: true,
        }
    }

    fn fresh_eligibility() -> ReleaseEligibility {
        ReleaseEligibility {
            variant: "portable".to_owned(),
            observed_version: String::new(),
            observed_hash: String::new(),
        }
    }

    fn signed_fixture() -> &'static (Vec<u8>, Vec<u8>) {
        static FIXTURE: std::sync::OnceLock<(Vec<u8>, Vec<u8>)> = std::sync::OnceLock::new();
        FIXTURE.get_or_init(|| {
            let private = rsa::RsaPrivateKey::new(&mut rand::rngs::OsRng, 3072).unwrap();
            (
                crate::contract::tests::sign_payload(
                    &crate::contract::tests::valid_payload(),
                    &private,
                    32,
                ),
                crate::contract::tests::trusted_key(&private, None),
            )
        })
    }

    #[test]
    fn legacy_github_source_migrates_without_reusing_an_unbound_partial() {
        let temporary = TempDir::new().unwrap();
        let path = temporary.path().join("state.json");
        std::fs::write(&path, r#"{"githubSource":true,"observedVersion":"2.0.0","partialHash":"abc","validator":"\"old\""}"#).unwrap();
        let state = read_persisted(&path);
        assert_eq!(state.source, Some(ReleaseSource::GitHub));
        assert!(state.partial_url.is_empty());
        assert!(!state.github_source);
        assert!(
            !serde_json::to_string(&state)
                .unwrap()
                .contains("githubSource")
        );
    }

    #[tokio::test]
    async fn newest_valid_signed_release_skips_an_incomplete_newer_release() {
        let (signed, keys) = signed_fixture();
        let github_list = format!("{}?per_page=100&page=1", github::API);
        let github_manifest = format!(
            "{}/releases/download/v2.0.0_snow-shot/{feed}",
            github::REPOSITORY,
            feed = crate::edition::FEED_NAME
        );
        let releases = serde_json::to_vec(&json!([
            github_fixture("3.0.0", &[]),
            github_fixture("2.0.0", &[crate::edition::FEED_NAME])
        ]))
        .unwrap();
        let network = FakeNetwork::routed([
            (github_list, vec![reply(StatusCode::OK, &releases)]),
            (github_manifest, vec![reply(StatusCode::OK, signed)]),
            (
                "https://gitee.com/api/v5/repos/mg-chao/snow-apps/releases?per_page=100&page=1"
                    .to_owned(),
                vec![FakeReply::Pending],
            ),
        ]);
        let (sender, mut receiver) = mpsc::channel(4);
        fetch_metadata_trusted(
            metadata_inputs(&network),
            fresh_eligibility(),
            CancellationToken::new(),
            sender,
            Some(keys),
        )
        .await;
        assert!(matches!(
            receiver.recv().await,
            Some(OperationMessage::Metadata { release, source: ReleaseSource::GitHub, .. })
                if release.version == "2.0.0"
        ));
    }

    #[tokio::test]
    async fn signed_release_without_an_exact_package_asset_cannot_win() {
        let (signed, keys) = signed_fixture();
        let mut release = github_fixture("2.0.0", &[crate::edition::FEED_NAME]);
        release["assets"].as_array_mut().unwrap().pop();
        let listing = serde_json::to_vec(&json!([release])).unwrap();
        let github_manifest = format!(
            "{}/releases/download/v2.0.0_snow-shot/{feed}",
            github::REPOSITORY,
            feed = crate::edition::FEED_NAME
        );
        let network = FakeNetwork::routed([
            (
                format!("{}?per_page=100&page=1", github::API),
                vec![reply(StatusCode::OK, &listing)],
            ),
            (github_manifest, vec![reply(StatusCode::OK, signed)]),
            (
                "https://gitee.com/api/v5/repos/mg-chao/snow-apps/releases?per_page=100&page=1"
                    .to_owned(),
                vec![reply(StatusCode::SERVICE_UNAVAILABLE, b"")],
            ),
        ]);
        let (sender, mut receiver) = mpsc::channel(4);
        fetch_metadata_trusted(
            metadata_inputs(&network),
            fresh_eligibility(),
            CancellationToken::new(),
            sender,
            Some(keys),
        )
        .await;
        assert!(matches!(
            receiver.recv().await,
            Some(OperationMessage::Failed { .. })
        ));
    }

    #[tokio::test]
    async fn ineligible_first_channel_does_not_cancel_an_acceptable_release() {
        // Hold GitHub until Gitee has delivered its signed envelope. No timing sleeps.
        struct OrderedNetwork {
            inner: FakeNetwork,
            gate: CancellationToken,
            manifest: String,
        }
        impl Network for OrderedNetwork {
            fn get(
                &self,
                request: HttpRequest,
            ) -> Pin<Box<dyn Future<Output = Result<HttpResponse>> + Send>> {
                let inner = self.inner.clone();
                let gate = self.gate.clone();
                let wait = request.url.host_str() == Some("api.github.com");
                let release = request.url.as_str() == self.manifest;
                Box::pin(async move {
                    if wait {
                        gate.cancelled().await;
                    }
                    let response = inner.get(request).await;
                    if release {
                        gate.cancel();
                    }
                    response
                })
            }
        }
        let private = rsa::RsaPrivateKey::new(&mut rand::rngs::OsRng, 3072).unwrap();
        let keys = crate::contract::tests::trusted_key(&private, None);
        let payload = crate::contract::tests::valid_payload();
        let signed = crate::contract::tests::sign_payload(&payload, &private, 32);
        for mutation in [false, true] {
            let mut rejected = payload.clone();
            if mutation {
                rejected["packages"][if crate::edition::MINI { 2 } else { 4 }]["sha256"] =
                    json!("6".repeat(64));
            } else {
                rejected["version"] = json!("1.9.0");
            }
            let version = rejected["version"].as_str().unwrap();
            let tag = format!("v{version}_snow-shot");
            let manifest = format!(
                "https://gitee.com/mg-chao/snow-apps/releases/download/{tag}/{feed}",
                feed = crate::edition::FEED_NAME
            );
            let mut files = github_fixture(version, &[crate::edition::FEED_NAME])["assets"].clone();
            for file in files.as_array_mut().unwrap() {
                file["browser_download_url"] = json!(
                    file["browser_download_url"]
                        .as_str()
                        .unwrap()
                        .replace("github.com", "gitee.com")
                );
            }
            let rejected_signed = crate::contract::tests::sign_payload(&rejected, &private, 32);
            let network = FakeNetwork::routed([
                (format!("{}?per_page=100&page=1", github::API), vec![reply(StatusCode::OK, &serde_json::to_vec(&json!([github_fixture("2.0.0", &[crate::edition::FEED_NAME])] )).unwrap())]),
                (format!("{}/releases/download/v2.0.0_snow-shot/{feed}", github::REPOSITORY, feed = crate::edition::FEED_NAME), vec![reply(StatusCode::OK, &signed)]),
                ("https://gitee.com/api/v5/repos/mg-chao/snow-apps/releases?per_page=100&page=1".to_owned(), vec![reply(StatusCode::OK, &serde_json::to_vec(&json!([{"id":123,"tag_name":tag}])).unwrap())]),
                ("https://gitee.com/api/v5/repos/mg-chao/snow-apps/releases/123/attach_files?per_page=100".to_owned(), vec![reply(StatusCode::OK, &serde_json::to_vec(&files).unwrap())]),
                (manifest.clone(), vec![reply(StatusCode::OK, &rejected_signed)]),
            ]);
            let mut inputs = metadata_inputs(&network);
            inputs.network = Arc::new(OrderedNetwork {
                inner: network,
                gate: CancellationToken::new(),
                manifest,
            });
            let eligibility = ReleaseEligibility {
                variant: "portable".to_owned(),
                observed_version: "2.0.0".to_owned(),
                observed_hash: "5".repeat(64),
            };
            let (sender, mut receiver) = mpsc::channel(4);
            fetch_metadata_trusted(
                inputs,
                eligibility.clone(),
                CancellationToken::new(),
                sender,
                Some(&keys),
            )
            .await;
            match receiver.recv().await.unwrap() {
                OperationMessage::Metadata {
                    release, source, ..
                } => {
                    assert_eq!(source, ReleaseSource::GitHub);
                    eligibility.validate(&release).unwrap();
                }
                _ => panic!("expected the acceptable GitHub release"),
            }
        }
    }

    #[tokio::test]
    async fn first_valid_signed_release_wins_the_race() {
        let (signed, keys) = signed_fixture();
        let github_list = format!("{}?per_page=100&page=1", github::API);
        let gitee_api = "https://gitee.com/api/v5/repos/mg-chao/snow-apps/releases";
        let gitee_list = format!("{gitee_api}?per_page=100&page=1");
        let gitee_assets = format!("{gitee_api}/123/attach_files?per_page=100");
        let gitee_manifest = format!(
            "https://gitee.com/mg-chao/snow-apps/releases/download/v2.0.0_snow-shot/{}",
            crate::edition::FEED_NAME
        );
        let release = serde_json::to_vec(
            &json!([{"id":123,"tag_name":"v2.0.0_snow-shot","prerelease":false}]),
        )
        .unwrap();
        let names = [
            crate::edition::FEED_NAME,
            &format!("{}-2.0.0-windows-x64-online.exe", crate::edition::PRODUCT),
            &format!(
                "{}-2.0.0-windows-x64-online-update.zip",
                crate::edition::PRODUCT
            ),
            &format!("{}-2.0.0-windows-x64-offline.exe", crate::edition::PRODUCT),
            &format!(
                "{}-2.0.0-windows-x64-offline-update.zip",
                crate::edition::PRODUCT
            ),
            &format!("{}-2.0.0-windows-x64-portable.zip", crate::edition::PRODUCT),
        ];
        let assets = serde_json::to_vec(&names.iter().map(|name| json!({"name":name,
            "browser_download_url":format!("https://gitee.com/mg-chao/snow-apps/releases/download/v2.0.0_snow-shot/{name}")})).collect::<Vec<_>>()).unwrap();
        for gitee_wins in [true, false] {
            let github_release = serde_json::to_vec(&json!([github_fixture(
                "2.0.0",
                &[crate::edition::FEED_NAME]
            )]))
            .unwrap();
            let github_manifest = format!(
                "{}/releases/download/v2.0.0_snow-shot/{feed}",
                github::REPOSITORY,
                feed = crate::edition::FEED_NAME
            );
            let network = FakeNetwork::routed([
                (
                    github_list.clone(),
                    vec![if gitee_wins {
                        FakeReply::Pending
                    } else {
                        reply(StatusCode::OK, &github_release)
                    }],
                ),
                (github_manifest, vec![reply(StatusCode::OK, signed)]),
                (
                    gitee_list.clone(),
                    vec![if gitee_wins {
                        reply(StatusCode::OK, &release)
                    } else {
                        FakeReply::Pending
                    }],
                ),
                (gitee_assets.clone(), vec![reply(StatusCode::OK, &assets)]),
                (
                    gitee_manifest.to_owned(),
                    vec![reply(StatusCode::OK, signed)],
                ),
            ]);
            let (sender, mut receiver) = mpsc::channel(4);
            fetch_metadata_trusted(
                metadata_inputs(&network),
                fresh_eligibility(),
                CancellationToken::new(),
                sender,
                Some(keys),
            )
            .await;
            match receiver.recv().await.unwrap() {
                OperationMessage::Metadata {
                    release, source, ..
                } => {
                    assert_eq!(release.version, "2.0.0");
                    assert_eq!(
                        source,
                        if gitee_wins {
                            ReleaseSource::Gitee
                        } else {
                            ReleaseSource::GitHub
                        }
                    );
                }
                _ => panic!("expected authenticated metadata"),
            }
            assert!(
                network
                    .requests()
                    .iter()
                    .any(|request| request.url.as_str() == github_list)
            );
            assert!(
                network
                    .requests()
                    .iter()
                    .any(|request| request.url.as_str() == gitee_list)
            );
        }
    }

    #[tokio::test]
    async fn github_rejects_tag_mismatch_and_unsigned_metadata() {
        let (signed, keys) = signed_fixture();
        for (version, envelope) in [
            ("3.0.0", signed.as_slice()),
            ("2.0.0", b"unsigned".as_slice()),
        ] {
            let github = serde_json::to_vec(&json!([github_fixture(
                version,
                &[crate::edition::FEED_NAME]
            )]))
            .unwrap();
            let network = FakeNetwork::new([
                reply(StatusCode::SERVICE_UNAVAILABLE, b""),
                reply(StatusCode::OK, &github),
                reply(StatusCode::OK, envelope),
            ]);
            let (sender, mut receiver) = mpsc::channel(4);
            fetch_metadata_trusted(
                metadata_inputs(&network),
                fresh_eligibility(),
                CancellationToken::new(),
                sender,
                Some(keys),
            )
            .await;
            assert!(matches!(
                receiver.recv().await,
                Some(OperationMessage::Failed { .. })
            ));
        }
    }

    #[tokio::test]
    async fn discovery_filters_and_bounds_pagination() {
        let mut prerelease = github_fixture("9.0.0", &[]);
        prerelease["prerelease"] = json!(true);
        let mut draft = github_fixture("8.0.0", &[]);
        draft["draft"] = json!(true);
        let page = serde_json::to_vec(&json!([
            prerelease,
            draft,
            github_fixture("2.0.0", &[]),
            github_fixture("3.0.0", &[])
        ]))
        .unwrap();
        let network = FakeNetwork::new([reply(StatusCode::OK, &page)]);
        let found = github_release(&metadata_inputs(&network), None, &CancellationToken::new())
            .await
            .unwrap();
        assert_eq!(found["tag_name"], "v9.0.0_snow-shot");
        let page = serde_json::to_vec(&vec![github_fixture("2.0.0", &[]); 100]).unwrap();
        let network = FakeNetwork::new((0..10).map(|_| reply(StatusCode::OK, &page)));
        assert!(
            github_release(&metadata_inputs(&network), None, &CancellationToken::new())
                .await
                .is_err()
        );
        assert_eq!(network.requests().len(), 10);
    }

    #[tokio::test]
    async fn metadata_cancellation_never_falls_back() {
        let network = FakeNetwork::new([]);
        let cancellation = CancellationToken::new();
        cancellation.cancel();
        let (sender, mut receiver) = mpsc::channel(4);
        fetch_metadata(
            metadata_inputs(&network),
            fresh_eligibility(),
            cancellation,
            sender,
        )
        .await;
        assert!(matches!(
            receiver.recv().await,
            Some(OperationMessage::Cancelled(ActiveOperation::Check))
        ));
        assert!(network.requests().is_empty());
    }

    #[tokio::test]
    async fn failed_selected_package_uses_exact_version_on_other_channel() {
        let temporary = TempDir::new().unwrap();
        let asset = &format!("{}-2.0.0-windows-x64-portable.zip", crate::edition::PRODUCT);
        let gitee_url = format!(
            "https://gitee.com/mg-chao/snow-apps/releases/download/v2.0.0_snow-shot/{asset}"
        );
        let gitee_release = serde_json::to_vec(&json!([
            {"id":456,"tag_name":"v3.0.0_snow-shot"},
            {"id":123,"tag_name":"v2.0.0_snow-shot"}
        ]))
        .unwrap();
        let attachments =
            serde_json::to_vec(&json!([{"name":asset,"browser_download_url":gitee_url}])).unwrap();
        let first_url = "http://127.0.0.1:8080/snow-shot-portable.zip";
        let network = FakeNetwork::routed([
            (
                first_url.to_owned(),
                vec![
                    reply(StatusCode::SERVICE_UNAVAILABLE, b""),
                    reply(StatusCode::SERVICE_UNAVAILABLE, b""),
                    reply(StatusCode::SERVICE_UNAVAILABLE, b""),
                ],
            ),
            (
                "http://127.0.0.1:8080/?per_page=100&page=1".to_owned(),
                vec![reply(StatusCode::OK, &gitee_release)],
            ),
            (
                "http://127.0.0.1:8080/123/attach_files?per_page=100".to_owned(),
                vec![reply(StatusCode::OK, &attachments)],
            ),
            (gitee_url.clone(), vec![reply(StatusCode::OK, b"abc")]),
        ]);
        let mut inputs = download_inputs(
            &temporary,
            Arc::new(network.clone()),
            Arc::new(FakeClock::new(
                fixed_time(),
                vec![Duration::from_secs(2), Duration::from_secs(4)],
            )),
            3,
        );
        inputs.package.path = format!("{}portable.zip", crate::edition::PACKAGE_PREFIX);
        inputs.package.sha256 =
            "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad".to_owned();
        inputs.saved_hash = inputs.package.sha256.clone();
        inputs.saved_url = first_url.to_owned();
        initialize_cache(&inputs);
        std::fs::write(
            cache_path(&inputs.options, format!("{}.part", inputs.package.sha256)),
            b"a",
        )
        .unwrap();
        let (sender, mut receiver) = mpsc::channel(32);
        download_package(inputs, CancellationToken::new(), sender).await;
        let mut resumed_on_gitee = false;
        let mut downloaded_on_gitee = false;
        while let Ok(message) = receiver.try_recv() {
            resumed_on_gitee |= matches!(
                &message,
                OperationMessage::DownloadValidator {
                    source: ReleaseSource::Gitee,
                    ..
                }
            );
            downloaded_on_gitee |= matches!(
                &message,
                OperationMessage::Downloaded {
                    source: ReleaseSource::Gitee,
                    ..
                }
            );
        }
        assert!(resumed_on_gitee && downloaded_on_gitee);
        let requests = network.requests();
        assert_eq!(
            requests[3].url.as_str(),
            "http://127.0.0.1:8080/?per_page=100&page=1"
        );
        assert_eq!(requests[5].url.as_str(), gitee_url);
        assert!(requests[5].range.is_none());
    }
    #[test]
    fn cached_source_survives_restart_only_for_the_verified_identity() {
        let temporary = TempDir::new().unwrap();
        let mut service = ready_service(&temporary, FakeNetwork::new([]));
        let (signed, keys) = signed_fixture();
        let release = verify_release(signed, Some(keys)).unwrap();
        service.persisted.observed_hash =
            release.update_package("portable").unwrap().sha256.clone();
        service.persisted.source = Some(ReleaseSource::GitHub);
        service.persisted.partial_url =
            "https://github.com/mg-chao/snow-apps/releases/download/v2.0.0_snow-shot/package.zip"
                .to_owned();
        write_persisted(&service.options, &service.persisted).unwrap();
        service.persisted = read_persisted(&cache_path(&service.options, "state.json"));
        service.available = None;
        service.restore_verified_release(release.clone());
        assert_eq!(service.persisted.source, Some(ReleaseSource::GitHub));
        assert!(!service.persisted.partial_url.is_empty());
        assert_eq!(service.available.as_ref().unwrap().version, "2.0.0");
        assert_eq!(service.status.state, "Available");
        service.available = None;
        service.persisted.observed_version = "3.0.0".to_owned();
        service.restore_verified_release(release.clone());
        assert!(service.available.is_none());
        service.persisted.observed_version = "2.0.0".to_owned();
        service.persisted.observed_hash = "0".repeat(64);
        service.restore_verified_release(release);
        assert!(service.available.is_none());
        std::fs::write(cache_path(&service.options, "release.json"), b"unsigned").unwrap();
        service.restore_cached_release();
        assert!(service.available.is_none());
    }

    #[tokio::test]
    async fn github_package_hash_failure_never_reports_downloaded() {
        let temporary = TempDir::new().unwrap();
        let asset = &format!("{}-2.0.0-windows-x64-portable.zip", crate::edition::PRODUCT);
        let metadata = serde_json::to_vec(&github_fixture("2.0.0", &[asset])).unwrap();
        let network = FakeNetwork::new([
            reply(StatusCode::OK, &metadata),
            reply(StatusCode::OK, b"bad"),
            reply(StatusCode::OK, b"bad"),
            reply(StatusCode::OK, b"bad"),
        ]);
        let mut inputs = download_inputs(
            &temporary,
            Arc::new(network.clone()),
            Arc::new(FakeClock::new(
                fixed_time(),
                vec![Duration::from_secs(2), Duration::from_secs(4)],
            )),
            3,
        );
        inputs.source = ReleaseSource::GitHub;
        inputs.package.path = format!("{}portable.zip", crate::edition::PACKAGE_PREFIX);
        initialize_cache(&inputs);
        let (sender, mut receiver) = mpsc::channel(32);
        download_package(inputs, CancellationToken::new(), sender).await;
        let mut failed = false;
        while let Ok(message) = receiver.try_recv() {
            assert!(!matches!(message, OperationMessage::Downloaded { .. }));
            if let OperationMessage::Failed { error, .. } = message {
                assert_eq!(error.code, "update_payload_mismatch");
                failed = true;
            }
        }
        assert!(failed);
        assert_eq!(network.requests().len(), 4);
    }
}
