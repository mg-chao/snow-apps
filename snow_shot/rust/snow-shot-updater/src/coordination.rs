#[cfg(not(windows))]
use crate::error::{Result, UpdateError};

#[cfg(windows)]
mod windows_coordination {
    use crate::contract::{compare_versions, verify_release_file};
    use crate::error::{Result, UpdateError, io_error, require};
    use crate::{fsutil, platform, transaction};
    use std::ffi::c_void;
    use std::fs::{self, OpenOptions};
    use std::io::Write;
    use std::os::windows::io::AsRawHandle;
    use std::os::windows::process::CommandExt;
    use std::path::{Path, PathBuf};
    use std::process::{Command, Stdio};
    use std::time::{Duration, Instant, SystemTime};
    use tempfile::Builder;
    use tokio::io::{AsyncRead, AsyncReadExt, AsyncWrite, AsyncWriteExt};
    use tokio::net::windows::named_pipe::{
        ClientOptions, NamedPipeClient, NamedPipeServer, ServerOptions,
    };
    use uuid::Uuid;
    use windows::Win32::Foundation::{CloseHandle, HANDLE, HLOCAL, LocalFree};
    use windows::Win32::Security::Authorization::{
        ConvertSidToStringSidW, ConvertStringSecurityDescriptorToSecurityDescriptorW,
        SDDL_REVISION_1,
    };
    use windows::Win32::Security::{
        GetTokenInformation, PSECURITY_DESCRIPTOR, SECURITY_ATTRIBUTES, TOKEN_QUERY, TOKEN_USER,
        TokenUser,
    };
    use windows::Win32::System::Pipes::{GetNamedPipeClientProcessId, GetNamedPipeServerProcessId};
    use windows::Win32::System::Threading::{GetCurrentProcess, OpenProcessToken};
    use windows::core::{BOOL, PCWSTR, PWSTR};

    const CONNECT_TIMEOUT: Duration = Duration::from_secs(10);
    const WRITE_TIMEOUT: Duration = Duration::from_secs(5);
    const ACK_TIMEOUT: Duration = Duration::from_secs(30);
    const PARENT_EXIT_TIMEOUT: Duration = Duration::from_secs(45);
    const PRE_HANDOFF_TIMEOUT: Duration = Duration::from_secs(180);
    const TRANSACTION_TIMEOUT: Duration = Duration::from_secs(15 * 60);
    const MAX_CONTROL_LINE: usize = 4096;
    const DETACHED_PROCESS: u32 = 0x0000_0008;
    const CREATE_NO_WINDOW: u32 = 0x0800_0000;

    struct OwnedHandle(HANDLE);

    impl Drop for OwnedHandle {
        fn drop(&mut self) {
            if !self.0.is_invalid() {
                let _ = unsafe { CloseHandle(self.0) };
            }
        }
    }

    struct LocalAllocation(*mut c_void);

    impl Drop for LocalAllocation {
        fn drop(&mut self) {
            if !self.0.is_null() {
                unsafe {
                    LocalFree(Some(HLOCAL(self.0)));
                }
            }
        }
    }

    fn option(args: &[String], name: &str) -> Result<String> {
        args.iter()
            .position(|argument| argument == name)
            .and_then(|index| args.get(index + 1))
            .cloned()
            .ok_or_else(|| UpdateError::new("missing_updater_argument", "Missing updater argument"))
    }

    fn path_option(args: &[String], name: &str) -> Result<PathBuf> {
        Ok(PathBuf::from(option(args, name)?))
    }

    fn numeric_option(args: &[String], name: &str) -> Result<u32> {
        option(args, name)?.parse().map_err(|_| {
            UpdateError::new(
                "invalid_updater_argument",
                "Invalid updater command argument",
            )
        })
    }

    fn validate_arguments(
        args: &[String],
        mode: &str,
        option_names: &[&str],
        flag_names: &[&str],
        required_options: &[&str],
    ) -> Result<()> {
        require(
            args.first().is_some_and(|argument| argument == mode),
            "invalid_updater_argument",
            "Invalid updater command argument",
        )?;
        let mut seen = std::collections::HashSet::new();
        let mut index = 1;
        while index < args.len() {
            let name = args[index].as_str();
            require(
                seen.insert(name.to_owned()),
                "invalid_updater_argument",
                "Invalid updater command argument",
            )?;
            if option_names.contains(&name) {
                let value = args.get(index + 1);
                require(
                    value.is_some_and(|value| !value.is_empty() && !value.starts_with("--")),
                    "invalid_updater_argument",
                    "Invalid updater command argument",
                )?;
                index += 2;
            } else {
                require(
                    flag_names.contains(&name),
                    "invalid_updater_argument",
                    "Invalid updater command argument",
                )?;
                index += 1;
            }
        }
        for required in required_options {
            require(
                seen.contains(*required),
                "missing_updater_argument",
                "Missing updater argument",
            )?;
        }
        Ok(())
    }

    fn validate_operation_shape(args: &[String], mode: &str, internal: bool) -> Result<()> {
        let recovery = args.iter().any(|argument| argument == "--recovery");
        let mut required = vec!["--target", "--parent", "--result"];
        if internal {
            required.extend(["--pipe", "--coordinator"]);
        }
        if !recovery {
            required.extend(["--manifest", "--archive"]);
        }
        validate_arguments(
            args,
            mode,
            &[
                "--target",
                "--parent",
                "--service-parent",
                "--pipe",
                "--manifest",
                "--archive",
                "--result",
                "--coordinator",
            ],
            &["--recovery", "--preapproved", "--service-coordinator"],
            &required,
        )?;
        require(
            !args.iter().any(|argument| argument == "--preapproved")
                || !args
                    .iter()
                    .any(|argument| argument == "--service-coordinator"),
            "invalid_updater_argument",
            "Invalid updater command argument",
        )?;
        if args
            .iter()
            .any(|argument| argument == "--service-coordinator")
        {
            option(args, "--service-parent")?;
        }
        Ok(())
    }

    fn narrow_coordinator_path(path: &Path) -> bool {
        let Some(parent) = path.parent() else {
            return false;
        };
        let parent_name = parent
            .file_name()
            .and_then(|name| name.to_str())
            .unwrap_or_default();
        path.is_absolute()
            && path.file_name().and_then(|name| name.to_str()) == Some("coordinator.exe")
            && parent_name.starts_with("snow-shot-updater-")
            && parent_name.len() == "snow-shot-updater-".len() + 6
            && parent
                .parent()
                .is_some_and(|base| platform::path_eq(base, &std::env::temp_dir()))
            && !platform::path_has_reparse(parent)
            && !platform::path_has_reparse(path)
    }

    fn validate_coordinator_context(args: &[String]) -> Result<PathBuf> {
        let coordinator = path_option(args, "--coordinator")?;
        require(
            narrow_coordinator_path(&coordinator),
            "coordinator_identity_invalid",
            "The update coordinator identity could not be verified",
        )
        .map_err(|error| {
            error.detail(format!(
                "coordinator path is outside the narrow temporary layout: {}",
                coordinator.display()
            ))
        })?;
        let current = std::env::current_exe().map_err(|error| {
            io_error(
                "coordinator_identity_invalid",
                "The update coordinator identity could not be verified",
                error,
            )
        })?;
        require(
            fsutil::sha256_file(&coordinator)? == fsutil::sha256_file(&current)?,
            "coordinator_identity_invalid",
            "The update coordinator identity could not be verified",
        )
        .map_err(|error| error.detail("coordinator and executing image digests differ"))?;
        Ok(coordinator)
    }

    fn validate_installed_bootstrap(args: &[String], root: &Path) -> Result<PathBuf> {
        let coordinator = validate_coordinator_context(args)?;
        let current = std::env::current_exe().map_err(|error| {
            io_error(
                "coordinator_identity_invalid",
                "The update coordinator identity could not be verified",
                error,
            )
        })?;
        let installed = root.join("bin/snow-shot-updater.exe");
        require(
            platform::path_eq(&current, &installed),
            "coordinator_identity_invalid",
            "The update coordinator identity could not be verified",
        )
        .map_err(|error| {
            error.detail(format!(
                "bootstrap image {} does not match installed image {}",
                current.display(),
                installed.display()
            ))
        })?;
        Ok(coordinator)
    }

    fn validate_worker_copy(args: &[String], root: &Path) -> Result<PathBuf> {
        let coordinator = validate_coordinator_context(args)?;
        let current = std::env::current_exe().map_err(|error| {
            io_error(
                "coordinator_identity_invalid",
                "The update coordinator identity could not be verified",
                error,
            )
        })?;
        let parent = current.parent().unwrap_or_else(|| Path::new(""));
        let name = current
            .file_name()
            .and_then(|name| name.to_str())
            .unwrap_or_default();
        require(
            platform::path_eq(parent, &root.join(transaction::UPDATE_WORK))
                && name.starts_with("worker-")
                && name.ends_with(".exe")
                && name.len() == "worker-".len() + 32 + ".exe".len()
                && name[7..39].bytes().all(|byte| byte.is_ascii_hexdigit())
                && !platform::path_has_reparse(parent)
                && !platform::path_has_reparse(&current),
            "coordinator_identity_invalid",
            "The update coordinator identity could not be verified",
        )?;
        Ok(coordinator)
    }

    fn replace_mode(args: &[String], mode: &str) -> Vec<String> {
        let mut result = args.to_vec();
        if result.is_empty() {
            result.push(mode.to_owned());
        } else {
            result[0] = mode.to_owned();
        }
        result
    }

    fn append_option(args: &mut Vec<String>, name: &str, value: impl Into<String>) {
        args.push(name.to_owned());
        args.push(value.into());
    }

    fn spawn_detached(
        executable: &Path,
        args: &[String],
        working_directory: Option<&Path>,
    ) -> Result<()> {
        let mut command = Command::new(executable);
        command
            .args(args)
            .stdin(Stdio::null())
            .stdout(Stdio::null())
            .stderr(Stdio::null())
            .creation_flags(DETACHED_PROCESS | CREATE_NO_WINDOW);
        if let Some(directory) = working_directory {
            command.current_dir(directory);
        }
        command.spawn().map_err(|error| {
            io_error(
                "updater_process_launch_failed",
                "Could not launch updater coordinator",
                error,
            )
        })?;
        Ok(())
    }

    fn current_user_sid() -> Result<String> {
        let mut token = HANDLE::default();
        unsafe { OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &mut token) }.map_err(
            |error| {
                io_error(
                    "coordinator_create_failed",
                    "Could not create updater coordinator",
                    error,
                )
            },
        )?;
        let token = OwnedHandle(token);
        let mut length = 0_u32;
        let _ = unsafe { GetTokenInformation(token.0, TokenUser, None, 0, &mut length) };
        require(
            length >= size_of::<TOKEN_USER>() as u32,
            "coordinator_create_failed",
            "Could not create updater coordinator",
        )?;
        let mut buffer = vec![0_u8; length as usize];
        unsafe {
            GetTokenInformation(
                token.0,
                TokenUser,
                Some(buffer.as_mut_ptr().cast()),
                length,
                &mut length,
            )
        }
        .map_err(|error| {
            io_error(
                "coordinator_create_failed",
                "Could not create updater coordinator",
                error,
            )
        })?;
        let user = unsafe { &*(buffer.as_ptr().cast::<TOKEN_USER>()) };
        let mut sid = PWSTR::null();
        unsafe { ConvertSidToStringSidW(user.User.Sid, &mut sid) }.map_err(|error| {
            io_error(
                "coordinator_create_failed",
                "Could not create updater coordinator",
                error,
            )
        })?;
        let allocation = LocalAllocation(sid.0.cast());
        let result = unsafe { sid.to_string() }.map_err(|error| {
            io_error(
                "coordinator_create_failed",
                "Could not create updater coordinator",
                error,
            )
        });
        drop(allocation);
        result
    }

    fn wide(value: &str) -> Vec<u16> {
        value.encode_utf16().chain(std::iter::once(0)).collect()
    }

    fn secure_server(name: &str) -> Result<NamedPipeServer> {
        let sid = current_user_sid()?;
        let sddl = format!("D:P(A;;GA;;;SY)(A;;GA;;;BA)(A;;GA;;;{sid})S:(ML;;NW;;;ME)");
        let sddl = wide(&sddl);
        let mut descriptor = PSECURITY_DESCRIPTOR::default();
        unsafe {
            ConvertStringSecurityDescriptorToSecurityDescriptorW(
                PCWSTR(sddl.as_ptr()),
                SDDL_REVISION_1,
                &mut descriptor,
                None,
            )
        }
        .map_err(|error| {
            io_error(
                "coordinator_create_failed",
                "Could not create updater coordinator",
                error,
            )
        })?;
        let descriptor_guard = LocalAllocation(descriptor.0);
        let mut attributes = SECURITY_ATTRIBUTES {
            nLength: size_of::<SECURITY_ATTRIBUTES>() as u32,
            lpSecurityDescriptor: descriptor.0,
            bInheritHandle: BOOL(0),
        };
        let server = unsafe {
            ServerOptions::new()
                .first_pipe_instance(true)
                .reject_remote_clients(true)
                .in_buffer_size(MAX_CONTROL_LINE as u32)
                .out_buffer_size(MAX_CONTROL_LINE as u32)
                .create_with_security_attributes_raw(
                    name,
                    (&mut attributes as *mut SECURITY_ATTRIBUTES).cast(),
                )
        }
        .map_err(|error| {
            io_error(
                "coordinator_create_failed",
                "Could not create updater coordinator",
                error,
            )
        });
        drop(descriptor_guard);
        server
    }

    fn pipe_path(name: &str) -> String {
        format!(r"\\.\pipe\{name}")
    }

    async fn connect_client(name: &str) -> Result<NamedPipeClient> {
        let path = pipe_path(name);
        let deadline = Instant::now() + CONNECT_TIMEOUT;
        loop {
            match ClientOptions::new().open(&path) {
                Ok(client) => return Ok(client),
                Err(error) if Instant::now() < deadline => {
                    tracing::debug!(?error, "waiting for updater coordinator pipe");
                    tokio::time::sleep(Duration::from_millis(25)).await;
                }
                Err(error) => {
                    return Err(io_error(
                        "coordinator_connect_failed",
                        "Could not contact the update coordinator",
                        error,
                    ));
                }
            }
        }
    }

    fn report_worker_failure(args: &[String], error: &UpdateError) {
        let Ok(pipe) = option(args, "--pipe") else {
            return;
        };
        let path = pipe_path(&format!("{pipe}-worker"));
        let deadline = Instant::now() + CONNECT_TIMEOUT;
        loop {
            match OpenOptions::new().read(true).write(true).open(&path) {
                Ok(mut stream) => {
                    let message = format!("failed:{}\n", error.message);
                    let _ = stream.write_all(message.as_bytes());
                    let _ = stream.flush();
                    return;
                }
                Err(_) if Instant::now() < deadline => {
                    std::thread::sleep(Duration::from_millis(25));
                }
                Err(_) => return,
            }
        }
    }

    async fn report_worker_failure_async(args: &[String], error: &UpdateError) {
        let Ok(pipe) = option(args, "--pipe") else {
            return;
        };
        let pipe = format!("{pipe}-worker");
        if let Ok(mut coordinator) = connect_client(&pipe).await {
            let _ = write_line(&mut coordinator, &format!("failed:{}", error.message)).await;
        }
    }

    async fn write_line<T: AsyncWrite + Unpin>(stream: &mut T, line: &str) -> Result<()> {
        require(
            line.len() <= MAX_CONTROL_LINE,
            "invalid_updater_argument",
            "Invalid updater command argument",
        )?;
        let mut frame = line.as_bytes().to_vec();
        frame.push(b'\n');
        tokio::time::timeout(WRITE_TIMEOUT, async {
            stream.write_all(&frame).await?;
            stream.flush().await
        })
        .await
        .map_err(|_| UpdateError::new("coordinator_write_failed", "Could not send updater status"))?
        .map_err(|error| {
            io_error(
                "coordinator_write_failed",
                "Could not send updater status",
                error,
            )
        })
    }

    async fn read_line<T: AsyncRead + Unpin>(stream: &mut T, timeout: Duration) -> Result<String> {
        tokio::time::timeout(timeout, async {
            let mut bytes = Vec::new();
            loop {
                let byte = stream.read_u8().await.map_err(|error| {
                    io_error(
                        "coordinator_ack_failed",
                        "The update handoff was not acknowledged",
                        error,
                    )
                })?;
                if byte == b'\n' {
                    break;
                }
                require(
                    bytes.len() < MAX_CONTROL_LINE,
                    "coordinator_ack_failed",
                    "The update handoff was not acknowledged",
                )?;
                if byte != b'\r' {
                    bytes.push(byte);
                }
            }
            String::from_utf8(bytes).map_err(|error| {
                UpdateError::new(
                    "coordinator_ack_failed",
                    "The update handoff was not acknowledged",
                )
                .detail(error)
            })
        })
        .await
        .map_err(|_| {
            UpdateError::new(
                "coordinator_ack_failed",
                "The update handoff was not acknowledged",
            )
        })?
    }

    async fn exchange_line<T: AsyncRead + AsyncWrite + Unpin>(
        stream: &mut T,
        line: &str,
        timeout: Duration,
    ) -> Result<String> {
        write_line(stream, line).await?;
        read_line(stream, timeout).await
    }

    fn pipe_handle(raw: *mut c_void) -> HANDLE {
        HANDLE(raw)
    }

    fn peer_pid(raw: *mut c_void, server_peer: bool) -> Result<u32> {
        let mut pid = 0_u32;
        let result = unsafe {
            if server_peer {
                GetNamedPipeServerProcessId(pipe_handle(raw), &mut pid)
            } else {
                GetNamedPipeClientProcessId(pipe_handle(raw), &mut pid)
            }
        };
        result.map_err(|error| {
            io_error(
                "coordinator_identity_invalid",
                "The update coordinator identity could not be verified",
                error,
            )
        })?;
        Ok(pid)
    }

    fn verify_peer(
        raw: *mut c_void,
        server_peer: bool,
        expected_path: &Path,
        expected_pid: Option<u32>,
        allowed_copy_root: Option<&Path>,
        expected_digest: Option<&str>,
    ) -> Result<()> {
        let pid = peer_pid(raw, server_peer)?;
        if let Some(expected) = expected_pid {
            require(
                pid == expected,
                "coordinator_identity_invalid",
                "The update coordinator identity could not be verified",
            )?;
        }
        let actual = platform::process_path_for_pid(pid)?;
        if platform::path_eq(&actual, expected_path) {
            return Ok(());
        }
        let Some(copy_root) = allowed_copy_root else {
            return Err(UpdateError::new(
                "coordinator_identity_invalid",
                "The update coordinator identity could not be verified",
            ));
        };
        let actual_parent = actual.parent().unwrap_or(Path::new(""));
        let name = actual
            .file_name()
            .and_then(|name| name.to_str())
            .unwrap_or_default();
        require(
            platform::path_eq(actual_parent, copy_root)
                && name.starts_with("worker-")
                && name.ends_with(".exe")
                && name.len() == "worker-".len() + 32 + ".exe".len()
                && name[7..39].bytes().all(|byte| byte.is_ascii_hexdigit()),
            "coordinator_identity_invalid",
            "The update coordinator identity could not be verified",
        )?;
        let digest = fsutil::sha256_file(&actual)?;
        require(
            expected_digest == Some(digest.as_str()),
            "coordinator_identity_invalid",
            "The update coordinator identity could not be verified",
        )
    }

    fn verify_coordinator_copy(raw: *mut c_void) -> Result<()> {
        let pid = peer_pid(raw, false)?;
        let actual = platform::process_path_for_pid(pid)?;
        let parent = actual.parent().unwrap_or(Path::new(""));
        let parent_name = parent
            .file_name()
            .and_then(|name| name.to_str())
            .unwrap_or_default();
        require(
            actual.file_name().and_then(|name| name.to_str()) == Some("coordinator.exe")
                && parent_name.starts_with("snow-shot-updater-")
                && parent_name.len() == "snow-shot-updater-".len() + 6
                && !platform::path_has_reparse(parent),
            "coordinator_identity_invalid",
            "The update coordinator identity could not be verified",
        )?;
        let current = std::env::current_exe().map_err(|error| {
            io_error(
                "coordinator_identity_invalid",
                "The update coordinator identity could not be verified",
                error,
            )
        })?;
        require(
            fsutil::sha256_file(&actual)? == fsutil::sha256_file(&current)?,
            "coordinator_identity_invalid",
            "The update coordinator identity could not be verified",
        )
    }

    fn write_result(path: &Path, value: &str) -> Result<()> {
        fsutil::write_atomic(path, value.as_bytes())
    }

    fn relaunch(root: &Path) -> Result<()> {
        spawn_detached(
            &root.join("bin/snow_shot.exe"),
            &["--show-main-window".to_owned()],
            Some(root),
        )
    }

    fn prune_coordinators() {
        let Ok(entries) = fs::read_dir(std::env::temp_dir()) else {
            return;
        };
        let now = SystemTime::now();
        for entry in entries.flatten() {
            let name = entry.file_name().to_string_lossy().to_string();
            if !name.starts_with("snow-shot-updater-")
                || name.len() != "snow-shot-updater-".len() + 6
            {
                continue;
            }
            let Ok(metadata) = entry.metadata() else {
                continue;
            };
            if !metadata.is_dir()
                || metadata.file_type().is_symlink()
                || now
                    .duration_since(metadata.modified().unwrap_or(now))
                    .unwrap_or_default()
                    <= Duration::from_secs(24 * 60 * 60)
                || platform::path_has_reparse(&entry.path())
            {
                continue;
            }
            let executable = entry.path().join("coordinator.exe");
            if fs::symlink_metadata(&executable)
                .is_ok_and(|value| value.is_file() && !value.file_type().is_symlink())
                && fs::remove_file(&executable).is_ok()
            {
                let _ = fs::remove_dir(entry.path());
            }
        }
    }

    pub fn launch(args: &[String]) -> Result<i32> {
        validate_operation_shape(args, "--launch", false)?;
        prune_coordinators();
        let directory = Builder::new()
            .prefix("snow-shot-updater-")
            .tempdir()
            .map_err(|error| {
                io_error(
                    "coordinator_directory_failed",
                    "Could not create updater coordinator directory",
                    error,
                )
            })?;
        let executable = directory.path().join("coordinator.exe");
        let current = std::env::current_exe().map_err(|error| {
            io_error(
                "coordinator_copy_failed",
                "Could not copy updater coordinator",
                error,
            )
        })?;
        fs::copy(&current, &executable).map_err(|error| {
            io_error(
                "coordinator_copy_failed",
                "Could not copy updater coordinator",
                error,
            )
        })?;
        let mut broker_args = replace_mode(args, "--broker");
        if !broker_args.iter().any(|argument| argument == "--pipe") {
            append_option(
                &mut broker_args,
                "--pipe",
                format!("snow-shot-update-{}", Uuid::new_v4().simple()),
            );
            broker_args.push("--preapproved".to_owned());
        }
        append_option(
            &mut broker_args,
            "--coordinator",
            executable.to_string_lossy().to_string(),
        );
        spawn_detached(&executable, &broker_args, Some(directory.path()))?;
        let _ = directory.keep();
        Ok(0)
    }

    pub struct ServiceHandoff {
        connection: NamedPipeServer,
    }

    impl ServiceHandoff {
        pub async fn decide(mut self, proceed: bool) -> Result<()> {
            write_line(&mut self.connection, if proceed { "go" } else { "cancel" }).await
        }
    }

    pub async fn prepare_service_handoff(mut args: Vec<String>) -> Result<ServiceHandoff> {
        let pipe = format!("snow-shot-service-{}", Uuid::new_v4().simple());
        let mut server = secure_server(&pipe_path(&pipe))?;
        append_option(&mut args, "--pipe", pipe);
        args.push("--service-coordinator".to_owned());
        launch(&args)?;
        tokio::time::timeout(PRE_HANDOFF_TIMEOUT, server.connect())
            .await
            .map_err(|_| UpdateError::new("updater_timeout", "The update helper timed out"))?
            .map_err(|error| {
                io_error(
                    "coordinator_connect_failed",
                    "Could not contact the update coordinator",
                    error,
                )
            })?;
        verify_coordinator_copy(server.as_raw_handle())?;
        let status = read_line(&mut server, PRE_HANDOFF_TIMEOUT).await?;
        if let Some(message) = status.strip_prefix("failed:") {
            return Err(UpdateError::from_message(message));
        }
        require(
            status == "ready",
            "coordinator_ack_failed",
            "The update handoff was not acknowledged",
        )?;
        Ok(ServiceHandoff { connection: server })
    }

    pub async fn broker(args: &[String]) -> Result<i32> {
        validate_operation_shape(args, "--broker", true)?;
        let root = path_option(args, "--target")?;
        transaction::validate_root(&root)?;
        let pipe = option(args, "--pipe")?;
        let worker_pipe = format!("{pipe}-worker");
        let mut server = secure_server(&pipe_path(&worker_pipe))?;
        let coordinator = validate_coordinator_context(args)?;
        let current = std::env::current_exe().map_err(|error| {
            io_error(
                "coordinator_identity_invalid",
                "The update coordinator identity could not be verified",
                error,
            )
        })?;
        require(
            platform::path_eq(&current, &coordinator),
            "coordinator_identity_invalid",
            "The update coordinator identity could not be verified",
        )?;
        let digest = fsutil::sha256_file(&coordinator)?;
        let installed = root.join("bin/snow-shot-updater.exe");
        spawn_detached(&installed, &replace_mode(args, "--bootstrap"), Some(&root))?;

        let preapproved = args.iter().any(|argument| argument == "--preapproved");
        let mut application = if preapproved {
            None
        } else {
            let application = connect_client(&pipe).await?;
            if args
                .iter()
                .any(|argument| argument == "--service-coordinator")
            {
                verify_peer(
                    application.as_raw_handle(),
                    true,
                    &root.join("bin/snow-shot-updater.exe"),
                    Some(numeric_option(args, "--service-parent")?),
                    None,
                    None,
                )?;
            } else {
                verify_peer(
                    application.as_raw_handle(),
                    true,
                    &root.join("bin/snow_shot.exe"),
                    Some(numeric_option(args, "--parent")?),
                    None,
                    None,
                )?;
            }
            Some(application)
        };

        tokio::time::timeout(PRE_HANDOFF_TIMEOUT, server.connect())
            .await
            .map_err(|_| UpdateError::new("updater_timeout", "The update helper timed out"))?
            .map_err(|error| {
                io_error(
                    "coordinator_connect_failed",
                    "Could not contact the update coordinator",
                    error,
                )
            })?;
        verify_peer(
            server.as_raw_handle(),
            false,
            &installed,
            None,
            Some(&root.join(transaction::UPDATE_WORK)),
            Some(&digest),
        )?;
        let worker_status = read_line(&mut server, PRE_HANDOFF_TIMEOUT).await?;
        if worker_status != "ready" {
            let status = if worker_status.starts_with("failed:") {
                worker_status
            } else {
                "failed:The update handoff was not acknowledged".to_owned()
            };
            write_result(&path_option(args, "--result")?, &status)?;
            if let Some(application) = application.as_mut() {
                let _ = write_line(application, &status).await;
            }
            return Ok(1);
        }

        let answer = if preapproved {
            "go".to_owned()
        } else {
            exchange_line(
                application.as_mut().expect("verified application pipe"),
                "ready",
                ACK_TIMEOUT,
            )
            .await?
        };
        write_line(&mut server, &answer).await?;
        if answer != "go" {
            let status = "failed:Application cancelled the update handoff";
            write_result(&path_option(args, "--result")?, status)?;
            return Ok(1);
        }
        let status = match read_line(&mut server, TRANSACTION_TIMEOUT).await {
            Ok(status) => status,
            Err(error) => {
                let status = format!("failed:{}", error.message);
                write_result(&path_option(args, "--result")?, &status)?;
                return Ok(1);
            }
        };
        write_line(&mut server, "done").await?;
        write_result(&path_option(args, "--result")?, &status)?;
        if !transaction::transaction_pending(&root) {
            relaunch(&root)?;
        }
        Ok(i32::from(status != "success"))
    }

    fn writable(root: &Path) -> bool {
        Builder::new()
            .prefix(".snow-shot-permission-")
            .tempfile_in(root)
            .is_ok()
    }

    fn bootstrap_inner(args: &[String], elevated: bool) -> Result<i32> {
        validate_operation_shape(
            args,
            if elevated {
                "--elevated"
            } else {
                "--bootstrap"
            },
            true,
        )?;
        let root = path_option(args, "--target")?;
        transaction::validate_root(&root)?;
        transaction::installation_record(&root)?;
        transaction::validate_target_path(&root, "bin/snow-shot-updater.exe")?;
        validate_installed_bootstrap(args, &root)?;
        if elevated {
            require(
                platform::registered_target_matches(&root),
                "elevation_target_invalid",
                "Elevation requires a registered Snow Shot installation",
            )?;
        }
        if !writable(&root) {
            require(
                !elevated && platform::registered_target_matches(&root),
                "elevation_target_invalid",
                "Elevation requires a registered Snow Shot installation",
            )?;
            let launched = platform::launch_elevated(
                &std::env::current_exe().map_err(|error| {
                    io_error(
                        "elevation_unavailable",
                        "Update permission was declined or could not be obtained",
                        error,
                    )
                })?,
                &replace_mode(args, "--elevated"),
            )?;
            require(
                launched,
                "elevation_declined",
                "Update permission was declined or could not be obtained",
            )?;
            return Ok(0);
        }
        fs::create_dir_all(root.join(transaction::UPDATE_WORK)).map_err(|error| {
            io_error(
                "worker_directory_failed",
                "Could not create update worker directory",
                error,
            )
        })?;
        transaction::prune_update_work(&root)?;
        let worker = root
            .join(transaction::UPDATE_WORK)
            .join(format!("worker-{}.exe", Uuid::new_v4().simple()));
        let current = std::env::current_exe().map_err(|error| {
            io_error(
                "worker_launch_failed",
                "Could not launch update worker",
                error,
            )
        })?;
        fs::copy(current, &worker).map_err(|error| {
            io_error(
                "worker_launch_failed",
                "Could not launch update worker",
                error,
            )
        })?;
        spawn_detached(&worker, &replace_mode(args, "--worker"), Some(&root))?;
        Ok(0)
    }

    pub fn bootstrap(args: &[String], elevated: bool) -> Result<i32> {
        match bootstrap_inner(args, elevated) {
            Ok(code) => Ok(code),
            Err(error) => {
                report_worker_failure(args, &error);
                Err(error)
            }
        }
    }

    pub async fn worker(args: &[String]) -> Result<i32> {
        let prepared = (|| {
            validate_operation_shape(args, "--worker", true)?;
            let root = path_option(args, "--target")?;
            transaction::validate_root(&root)?;
            validate_worker_copy(args, &root)?;
            let parent = numeric_option(args, "--parent")?;
            platform::validate_parent_process(parent, &root.join("bin/snow_shot.exe"))?;
            let recovery = args.iter().any(|argument| argument == "--recovery");
            let mut staged_archive = None;
            let release = if recovery {
                None
            } else {
                let release = verify_release_file(&path_option(args, "--manifest")?)?;
                let record = transaction::installation_record(&root)?;
                require(
                    compare_versions(&release.version, &record.version)?.is_gt(),
                    "release_not_newer",
                    "The release is not newer than this installation",
                )?;
                let package = release.update_package(&record.variant)?;
                let input = root
                    .join(transaction::UPDATE_WORK)
                    .join(format!("input-{}.zip", Uuid::new_v4().simple()));
                fs::copy(path_option(args, "--archive")?, &input).map_err(|error| {
                    io_error(
                        "update_stage_failed",
                        "Could not stage the verified update package",
                        error,
                    )
                })?;
                fsutil::verify_file(&input, package.size, &package.sha256)?;
                staged_archive = Some(input);
                Some(release)
            };
            Ok((root, parent, recovery, staged_archive, release))
        })();
        let (root, parent, recovery, mut staged_archive, release) = match prepared {
            Ok(prepared) => prepared,
            Err(error) => {
                report_worker_failure_async(args, &error).await;
                return Err(error);
            }
        };

        let pipe = format!("{}-worker", option(args, "--pipe")?);
        let mut coordinator = connect_client(&pipe).await?;
        verify_peer(
            coordinator.as_raw_handle(),
            true,
            &path_option(args, "--coordinator")?,
            None,
            None,
            None,
        )?;
        let decision = exchange_line(&mut coordinator, "ready", ACK_TIMEOUT).await?;
        if decision != "go" {
            if let Some(path) = staged_archive.take() {
                let _ = fs::remove_file(path);
            }
            return Ok(1);
        }
        let status = (|| {
            platform::wait_for_process(parent, PARENT_EXIT_TIMEOUT)?;
            if let Ok(service_pid) = option(args, "--service-parent").and_then(|value| {
                value.parse().map_err(|_| {
                    UpdateError::new(
                        "invalid_updater_argument",
                        "Invalid updater command argument",
                    )
                })
            }) {
                platform::wait_for_process(service_pid, PARENT_EXIT_TIMEOUT)?;
            }
            if recovery {
                transaction::recover_transaction(&root)
            } else {
                transaction::apply_transaction(
                    &root,
                    staged_archive.as_ref().expect("staged archive"),
                    release.as_ref().expect("verified release"),
                    transaction::TransactionHooks::default(),
                )
            }
        })();
        if let Some(path) = staged_archive {
            let _ = fs::remove_file(path);
        }
        let text = if let Err(error) = status {
            format!("failed:{}", error.message)
        } else {
            "success".to_owned()
        };
        require(
            exchange_line(&mut coordinator, &text, ACK_TIMEOUT).await? == "done",
            "coordinator_ack_failed",
            "The update handoff was not acknowledged",
        )?;
        Ok(i32::from(text != "success"))
    }

    #[cfg(test)]
    mod tests {
        use super::*;

        #[test]
        fn generated_coordinator_path_is_accepted() {
            let directory = Builder::new()
                .prefix("snow-shot-updater-")
                .tempdir()
                .expect("temporary coordinator directory");
            let coordinator = directory.path().join("coordinator.exe");
            assert!(narrow_coordinator_path(&coordinator));
        }
    }
}

#[cfg(windows)]
pub use windows_coordination::{
    ServiceHandoff, bootstrap, broker, launch, prepare_service_handoff, worker,
};

#[cfg(not(windows))]
pub struct ServiceHandoff;

#[cfg(not(windows))]
impl ServiceHandoff {
    pub async fn decide(self, _proceed: bool) -> Result<()> {
        launch(&[]).map(|_| ())
    }
}

#[cfg(not(windows))]
pub async fn prepare_service_handoff(_args: Vec<String>) -> Result<ServiceHandoff> {
    launch(&[]).map(|_| ServiceHandoff)
}

#[cfg(not(windows))]
pub fn launch(_args: &[String]) -> Result<i32> {
    Err(UpdateError::new(
        "elevation_unavailable",
        "Elevation is unavailable on this platform",
    ))
}

#[cfg(not(windows))]
pub async fn broker(_args: &[String]) -> Result<i32> {
    launch(&[])
}

#[cfg(not(windows))]
pub fn bootstrap(_args: &[String], _elevated: bool) -> Result<i32> {
    launch(&[])
}

#[cfg(not(windows))]
pub async fn worker(_args: &[String]) -> Result<i32> {
    launch(&[])
}
