use crate::error::{Result, UpdateError, require};
use path_clean::PathClean;
use sha2::{Digest, Sha256};
use std::ffi::{OsStr, OsString};
use std::mem::ManuallyDrop;
use std::os::windows::ffi::{OsStrExt, OsStringExt};
use std::path::{Path, PathBuf};
use windows::Win32::Foundation::{
    ERROR_FILE_NOT_FOUND, ERROR_NO_MORE_ITEMS, HLOCAL, VARIANT_FALSE,
};
use windows::Win32::Globalization::{CSTR_EQUAL, CompareStringOrdinal};
use windows::Win32::Security::Authorization::{ConvertSidToStringSidW, ConvertStringSidToSidW};
use windows::Win32::Security::{
    DACL_SECURITY_INFORMATION, LookupAccountNameW, PSID, SID_NAME_USE, SidTypeInvalid,
};
use windows::Win32::System::Com::{
    CLSCTX_INPROC_SERVER, CLSCTX_LOCAL_SERVER, COINIT_APARTMENTTHREADED, CoCreateInstance,
    CoInitializeEx, CoUninitialize, IDispatch, IServiceProvider,
};
use windows::Win32::System::Registry::{
    HKEY, HKEY_LOCAL_MACHINE, HKEY_USERS, KEY_QUERY_VALUE, KEY_READ, KEY_SET_VALUE, KEY_WRITE,
    REG_PROCESS_APPKEY, REG_SZ, RRF_RT_REG_EXPAND_SZ, RRF_RT_REG_SZ, RegCloseKey, RegDeleteValueW,
    RegEnumKeyExW, RegFlushKey, RegGetValueW, RegLoadAppKeyW, RegOpenKeyExW, RegSetValueExW,
};
use windows::Win32::System::TaskScheduler::{
    IAction, IExecAction, ILogonTrigger, IRegisteredTask, ITaskFolder, ITaskService,
    TASK_ACTION_EXEC, TASK_CREATE, TASK_ENUM_HIDDEN, TASK_INSTANCES_IGNORE_NEW,
    TASK_LOGON_INTERACTIVE_TOKEN, TASK_RUNLEVEL_HIGHEST, TASK_TRIGGER_LOGON, TaskScheduler,
};
use windows::Win32::System::Variant::{
    VARIANT, VARIANT_0, VARIANT_0_0, VARIANT_0_0_0, VT_BSTR, VT_I4, VariantClear, VariantInit,
};
use windows::Win32::UI::Shell::{
    IShellBrowser, IShellDispatch2, IShellFolderViewDual, IShellWindows, SID_STopLevelBrowser,
    SVGIO_BACKGROUND, SWC_DESKTOP, SWFO_NEEDDISPATCH, ShellWindows,
};
use windows::Win32::UI::WindowsAndMessaging::SW_SHOWNORMAL;
use windows::core::{BSTR, Interface, PCWSTR, PWSTR};

const STARTUP_FAILURE_CODE: &str = "startup_registration_failed";
const STARTUP_FAILURE_MESSAGE: &str = "Could not update Snow Shot startup registration";

fn update_error(error: impl std::fmt::Display) -> UpdateError {
    UpdateError::new(STARTUP_FAILURE_CODE, STARTUP_FAILURE_MESSAGE).detail(error)
}

fn wide(value: impl AsRef<OsStr>) -> Vec<u16> {
    value
        .as_ref()
        .encode_wide()
        .chain(std::iter::once(0))
        .collect()
}

fn windows_equal(left: impl AsRef<OsStr>, right: impl AsRef<OsStr>) -> bool {
    let left = left.as_ref().encode_wide().collect::<Vec<_>>();
    let right = right.as_ref().encode_wide().collect::<Vec<_>>();
    unsafe { CompareStringOrdinal(&left, &right, true) == CSTR_EQUAL }
}

fn clean_windows_path(path: &Path) -> PathBuf {
    path.clean()
}

fn task_name(executable: &Path, sid: &str) -> String {
    let identity = clean_windows_path(executable)
        .to_string_lossy()
        .replace('\\', "/")
        .to_lowercase();
    let digest = format!("{:x}", Sha256::digest(identity.as_bytes()));
    format!("SnowShot-{sid}-{}", &digest[..24])
}

struct ComApartment(bool);

impl ComApartment {
    fn new() -> Result<Self> {
        unsafe { CoInitializeEx(None, COINIT_APARTMENTTHREADED) }
            .ok()
            .map_err(update_error)?;
        Ok(Self(true))
    }
}

impl Drop for ComApartment {
    fn drop(&mut self) {
        if self.0 {
            unsafe { CoUninitialize() };
        }
    }
}

struct RegistryKey(HKEY);

impl Drop for RegistryKey {
    fn drop(&mut self) {
        if !self.0.is_invalid() {
            let _ = unsafe { RegCloseKey(self.0) };
        }
    }
}

struct OwnedVariant(VARIANT);

impl OwnedVariant {
    fn empty() -> Self {
        Self(unsafe { VariantInit() })
    }

    fn integer(value: i32) -> Self {
        let mut variant = Self::empty();
        variant.0.Anonymous = VARIANT_0 {
            Anonymous: ManuallyDrop::new(VARIANT_0_0 {
                vt: VT_I4,
                Anonymous: VARIANT_0_0_0 { lVal: value },
                ..Default::default()
            }),
        };
        variant
    }

    fn string(value: impl AsRef<str>) -> Self {
        let mut variant = Self::empty();
        variant.0.Anonymous = VARIANT_0 {
            Anonymous: ManuallyDrop::new(VARIANT_0_0 {
                vt: VT_BSTR,
                Anonymous: VARIANT_0_0_0 {
                    bstrVal: ManuallyDrop::new(BSTR::from(value.as_ref())),
                },
                ..Default::default()
            }),
        };
        variant
    }
}

impl std::ops::Deref for OwnedVariant {
    type Target = VARIANT;

    fn deref(&self) -> &Self::Target {
        &self.0
    }
}

impl Drop for OwnedVariant {
    fn drop(&mut self) {
        let _ = unsafe { VariantClear(&mut self.0) };
    }
}

fn sid_to_string(sid: PSID) -> Option<String> {
    let mut text = PWSTR::null();
    unsafe { ConvertSidToStringSidW(sid, &mut text) }.ok()?;
    let value = unsafe { text.to_string() }.ok();
    unsafe {
        windows::Win32::Foundation::LocalFree(Some(HLOCAL(text.0.cast())));
    }
    value
}

fn canonical_account_sid(account_or_sid: &str) -> Option<String> {
    let account = wide(account_or_sid);
    let mut converted = PSID::default();
    if unsafe { ConvertStringSidToSidW(PCWSTR(account.as_ptr()), &mut converted) }.is_ok() {
        let value = sid_to_string(converted);
        unsafe {
            windows::Win32::Foundation::LocalFree(Some(HLOCAL(converted.0)));
        }
        return value;
    }

    let mut sid_size = 0_u32;
    let mut domain_size = 0_u32;
    let mut use_kind: SID_NAME_USE = SidTypeInvalid;
    let _ = unsafe {
        LookupAccountNameW(
            PCWSTR::null(),
            PCWSTR(account.as_ptr()),
            None,
            &mut sid_size,
            None,
            &mut domain_size,
            &mut use_kind,
        )
    };
    if sid_size == 0 {
        return None;
    }
    let mut sid = vec![0_u8; sid_size as usize];
    let mut domain = vec![0_u16; domain_size.max(1) as usize];
    let pointer = PSID(sid.as_mut_ptr().cast());
    unsafe {
        LookupAccountNameW(
            PCWSTR::null(),
            PCWSTR(account.as_ptr()),
            Some(pointer),
            &mut sid_size,
            Some(PWSTR(domain.as_mut_ptr())),
            &mut domain_size,
            &mut use_kind,
        )
    }
    .ok()?;
    sid_to_string(pointer)
}

struct TaskManager {
    service: ITaskService,
    folder: ITaskFolder,
    // Rust drops fields in declaration order, so the apartment must outlive all COM pointers.
    _apartment: ComApartment,
}

struct TaskSnapshot {
    name: String,
    sid: String,
    xml: BSTR,
    security: BSTR,
}

impl TaskManager {
    fn new() -> Result<Self> {
        let apartment = ComApartment::new()?;
        let service: ITaskService =
            unsafe { CoCreateInstance(&TaskScheduler, None, CLSCTX_INPROC_SERVER) }
                .map_err(update_error)?;
        let empty = OwnedVariant::empty();
        unsafe { service.Connect(&empty, &empty, &empty, &empty) }.map_err(update_error)?;
        let folder = unsafe { service.GetFolder(&BSTR::from("\\")) }.map_err(update_error)?;
        Ok(Self {
            service,
            folder,
            _apartment: apartment,
        })
    }

    fn validate(&self, task: &IRegisteredTask, executable: &Path, sid: &str) -> Result<()> {
        let definition = unsafe { task.Definition() }.map_err(update_error)?;
        let principal = unsafe { definition.Principal() }.map_err(update_error)?;
        let mut owner = BSTR::new();
        unsafe { principal.UserId(&mut owner) }.map_err(update_error)?;
        let owner = canonical_account_sid(&owner.to_string()).unwrap_or_default();

        let actions = unsafe { definition.Actions() }.map_err(update_error)?;
        let mut count = 0;
        unsafe { actions.Count(&mut count) }.map_err(update_error)?;
        require(
            owner == sid && count == 1,
            "startup_task_owner_mismatch",
            "Startup task ownership mismatch",
        )?;

        let action: IAction = unsafe { actions.get_Item(1) }.map_err(update_error)?;
        let action: IExecAction = action.cast().map_err(update_error)?;
        let mut target = BSTR::new();
        let mut arguments = BSTR::new();
        unsafe {
            action.Path(&mut target).map_err(update_error)?;
            action.Arguments(&mut arguments).map_err(update_error)?;
        }
        require(
            windows_equal(
                target.to_string(),
                clean_windows_path(executable).as_os_str(),
            ) && arguments == "--autostart",
            "startup_task_target_mismatch",
            "Startup task target mismatch",
        )
    }

    fn destination_absent(&self, name: &str) -> Result<()> {
        match unsafe { self.folder.GetTask(&BSTR::from(name)) } {
            Ok(_) => Err(UpdateError::new(
                "startup_task_destination_exists",
                "The destination already has a startup task",
            )),
            Err(error)
                if error.code() == windows::core::HRESULT::from_win32(ERROR_FILE_NOT_FOUND.0) =>
            {
                Ok(())
            }
            Err(error) => Err(update_error(error)),
        }
    }

    fn create(&self, executable: &Path, sid: &str, name: &str) -> Result<()> {
        let definition = unsafe { self.service.NewTask(0) }.map_err(update_error)?;
        let principal = unsafe { definition.Principal() }.map_err(update_error)?;
        unsafe {
            principal
                .SetUserId(&BSTR::from(sid))
                .map_err(update_error)?;
            principal
                .SetLogonType(TASK_LOGON_INTERACTIVE_TOKEN)
                .map_err(update_error)?;
            principal
                .SetRunLevel(TASK_RUNLEVEL_HIGHEST)
                .map_err(update_error)?;
        }

        let settings = unsafe { definition.Settings() }.map_err(update_error)?;
        unsafe {
            settings
                .SetDisallowStartIfOnBatteries(VARIANT_FALSE)
                .map_err(update_error)?;
            settings
                .SetStopIfGoingOnBatteries(VARIANT_FALSE)
                .map_err(update_error)?;
            settings
                .SetExecutionTimeLimit(&BSTR::from("PT0S"))
                .map_err(update_error)?;
            settings
                .SetMultipleInstances(TASK_INSTANCES_IGNORE_NEW)
                .map_err(update_error)?;
        }

        let triggers = unsafe { definition.Triggers() }.map_err(update_error)?;
        let trigger = unsafe { triggers.Create(TASK_TRIGGER_LOGON) }.map_err(update_error)?;
        let trigger: ILogonTrigger = trigger.cast().map_err(update_error)?;
        unsafe { trigger.SetUserId(&BSTR::from(sid)) }.map_err(update_error)?;

        let actions = unsafe { definition.Actions() }.map_err(update_error)?;
        let action = unsafe { actions.Create(TASK_ACTION_EXEC) }.map_err(update_error)?;
        let action: IExecAction = action.cast().map_err(update_error)?;
        let executable = clean_windows_path(executable);
        let working_directory = executable.parent().unwrap_or_else(|| Path::new("."));
        unsafe {
            action
                .SetPath(&BSTR::from(executable.to_string_lossy().as_ref()))
                .map_err(update_error)?;
            action
                .SetArguments(&BSTR::from("--autostart"))
                .map_err(update_error)?;
            action
                .SetWorkingDirectory(&BSTR::from(working_directory.to_string_lossy().as_ref()))
                .map_err(update_error)?;
        }

        let user = OwnedVariant::string(sid);
        let password = OwnedVariant::empty();
        let sddl = OwnedVariant::string(format!("D:P(A;;GA;;;SY)(A;;GA;;;BA)(A;;GRGX;;;{sid})"));
        unsafe {
            self.folder.RegisterTaskDefinition(
                &BSTR::from(name),
                &definition,
                TASK_CREATE.0,
                &user,
                &password,
                TASK_LOGON_INTERACTIVE_TOKEN,
                &sddl,
            )
        }
        .map_err(update_error)?;
        Ok(())
    }

    fn restore(&self, snapshot: &TaskSnapshot) -> Result<()> {
        let user = OwnedVariant::string(&snapshot.sid);
        let password = OwnedVariant::empty();
        let security = OwnedVariant::string(snapshot.security.to_string());
        unsafe {
            self.folder.RegisterTask(
                &BSTR::from(&snapshot.name),
                &snapshot.xml,
                TASK_CREATE.0,
                &user,
                &password,
                TASK_LOGON_INTERACTIVE_TOKEN,
                &security,
            )
        }
        .map_err(update_error)?;
        Ok(())
    }

    fn update_tasks(&self, executable: &Path, replacement: Option<&Path>) -> Result<()> {
        let tasks = unsafe { self.folder.GetTasks(TASK_ENUM_HIDDEN.0) }.map_err(update_error)?;
        let count = unsafe { tasks.Count() }.map_err(update_error)?;
        for index in (1..=count).rev() {
            let task = match unsafe { tasks.get_Item(&OwnedVariant::integer(index)) } {
                Ok(task) => task,
                Err(_) => continue,
            };
            let candidate = match unsafe { task.Name() } {
                Ok(name) => name.to_string(),
                Err(_) => continue,
            };
            if !candidate.starts_with("SnowShot-") {
                continue;
            }
            let definition = match unsafe { task.Definition() } {
                Ok(definition) => definition,
                Err(_) => continue,
            };
            let principal = match unsafe { definition.Principal() } {
                Ok(principal) => principal,
                Err(_) => continue,
            };
            let mut owner = BSTR::new();
            if unsafe { principal.UserId(&mut owner) }.is_err() {
                continue;
            }
            let Some(sid) = canonical_account_sid(&owner.to_string()) else {
                continue;
            };
            if candidate != task_name(executable, &sid) {
                continue;
            }
            self.validate(&task, executable, &sid)?;
            let snapshot = TaskSnapshot {
                name: candidate.clone(),
                sid: sid.clone(),
                xml: unsafe { task.Xml() }.map_err(update_error)?,
                security: unsafe { task.GetSecurityDescriptor(DACL_SECURITY_INFORMATION.0 as i32) }
                    .map_err(update_error)?,
            };
            let destination = replacement.map(|path| path.join("bin/snow_shot.exe"));
            let destination_name = destination.as_deref().map(|path| task_name(path, &sid));
            if let Some(name) = destination_name.as_deref() {
                self.destination_absent(name)?;
            }
            unsafe { self.folder.DeleteTask(&BSTR::from(&candidate), 0) }.map_err(update_error)?;
            if let (Some(path), Some(name)) = (destination.as_deref(), destination_name.as_deref())
                && let Err(error) = self.create(path, &sid, name)
            {
                self.restore(&snapshot).map_err(|rollback| {
                    UpdateError::new(
                        "startup_task_rollback_failed",
                        "Could not restore the previous startup task",
                    )
                    .detail(format!("{error}; {rollback}"))
                })?;
                return Err(error);
            }
        }
        Ok(())
    }
}

fn startup_command(root: &Path) -> String {
    format!(
        "\"{}\" --autostart",
        root.join("bin/snow_shot.exe").display()
    )
}

fn string_from_registry(buffer: &[u16], bytes: u32) -> Option<OsString> {
    if bytes < 2 || bytes as usize > size_of_val(buffer) {
        return None;
    }
    let length = bytes as usize / size_of::<u16>();
    let length = length.saturating_sub(1);
    Some(OsString::from_wide(&buffer[..length]))
}

fn reconcile_run_value(
    base: HKEY,
    subkey: &OsStr,
    expected: &str,
    replacement: Option<&str>,
) -> Result<bool> {
    let subkey = wide(subkey);
    let mut read = HKEY::default();
    if unsafe {
        RegOpenKeyExW(
            base,
            PCWSTR(subkey.as_ptr()),
            None,
            KEY_QUERY_VALUE,
            &mut read,
        )
    } != windows::Win32::Foundation::ERROR_SUCCESS
    {
        return Ok(false);
    }
    let read = RegistryKey(read);
    let name = wide("SnowShot");
    let mut command = vec![0_u16; 32_768];
    let mut bytes = (command.len() * size_of::<u16>()) as u32;
    let status = unsafe {
        RegGetValueW(
            read.0,
            PCWSTR::null(),
            PCWSTR(name.as_ptr()),
            RRF_RT_REG_SZ,
            None,
            Some(command.as_mut_ptr().cast()),
            Some(&mut bytes),
        )
    };
    let Some(command) = (status == windows::Win32::Foundation::ERROR_SUCCESS)
        .then(|| string_from_registry(&command, bytes))
        .flatten()
    else {
        return Ok(false);
    };
    if !windows_equal(&command, expected) {
        return Ok(false);
    }
    drop(read);

    let mut write = HKEY::default();
    let status = unsafe {
        RegOpenKeyExW(
            base,
            PCWSTR(subkey.as_ptr()),
            None,
            KEY_SET_VALUE,
            &mut write,
        )
    };
    if status == ERROR_FILE_NOT_FOUND {
        return Ok(false);
    }
    require(
        status == windows::Win32::Foundation::ERROR_SUCCESS,
        STARTUP_FAILURE_CODE,
        STARTUP_FAILURE_MESSAGE,
    )?;
    let write = RegistryKey(write);
    let status = if let Some(replacement) = replacement {
        let value = wide(replacement);
        let data = unsafe {
            std::slice::from_raw_parts(value.as_ptr().cast::<u8>(), value.len() * size_of::<u16>())
        };
        unsafe { RegSetValueExW(write.0, PCWSTR(name.as_ptr()), None, REG_SZ, Some(data)) }
    } else {
        unsafe { RegDeleteValueW(write.0, PCWSTR(name.as_ptr())) }
    };
    require(
        status == windows::Win32::Foundation::ERROR_SUCCESS,
        STARTUP_FAILURE_CODE,
        STARTUP_FAILURE_MESSAGE,
    )?;
    Ok(true)
}

fn enumerate_key_names(key: HKEY) -> Result<Vec<OsString>> {
    let mut names = Vec::new();
    for index in 0.. {
        let mut name = vec![0_u16; 256];
        let mut length = name.len() as u32;
        let status = unsafe {
            RegEnumKeyExW(
                key,
                index,
                Some(PWSTR(name.as_mut_ptr())),
                &mut length,
                None,
                None,
                None,
                None,
            )
        };
        if status == ERROR_NO_MORE_ITEMS {
            break;
        }
        require(
            status == windows::Win32::Foundation::ERROR_SUCCESS,
            STARTUP_FAILURE_CODE,
            STARTUP_FAILURE_MESSAGE,
        )?;
        names.push(OsString::from_wide(&name[..length as usize]));
    }
    Ok(names)
}

fn update_run_keys(root: &Path, replacement: Option<&Path>) -> Result<()> {
    let expected = startup_command(root);
    let replacement_command = replacement.map(startup_command);
    let replacement_command = replacement_command.as_deref();
    let run_suffix = OsStr::new("Software\\Microsoft\\Windows\\CurrentVersion\\Run");

    for sid in enumerate_key_names(HKEY_USERS)? {
        let subkey = PathBuf::from(&sid).join(run_suffix);
        reconcile_run_value(
            HKEY_USERS,
            subkey.as_os_str(),
            &expected,
            replacement_command,
        )?;
    }

    let profiles_path = wide("SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion\\ProfileList");
    let mut profiles = HKEY::default();
    let status = unsafe {
        RegOpenKeyExW(
            HKEY_LOCAL_MACHINE,
            PCWSTR(profiles_path.as_ptr()),
            None,
            KEY_READ,
            &mut profiles,
        )
    };
    require(
        status == windows::Win32::Foundation::ERROR_SUCCESS,
        STARTUP_FAILURE_CODE,
        STARTUP_FAILURE_MESSAGE,
    )?;
    let profiles = RegistryKey(profiles);
    for sid in enumerate_key_names(profiles.0)? {
        let sid_text = sid.to_string_lossy();
        if !sid_text.starts_with("S-1-5-21-") && !sid_text.starts_with("S-1-12-1-") {
            continue;
        }
        let sid_wide = wide(&sid);
        let mut loaded = HKEY::default();
        if unsafe {
            RegOpenKeyExW(
                HKEY_USERS,
                PCWSTR(sid_wide.as_ptr()),
                None,
                KEY_READ,
                &mut loaded,
            )
        } == windows::Win32::Foundation::ERROR_SUCCESS
        {
            drop(RegistryKey(loaded));
            continue;
        }

        let value_name = wide("ProfileImagePath");
        let mut directory = vec![0_u16; 32_768];
        let mut bytes = (directory.len() * size_of::<u16>()) as u32;
        let status = unsafe {
            RegGetValueW(
                profiles.0,
                PCWSTR(sid_wide.as_ptr()),
                PCWSTR(value_name.as_ptr()),
                RRF_RT_REG_SZ | RRF_RT_REG_EXPAND_SZ,
                None,
                Some(directory.as_mut_ptr().cast()),
                Some(&mut bytes),
            )
        };
        require(
            status == windows::Win32::Foundation::ERROR_SUCCESS,
            STARTUP_FAILURE_CODE,
            STARTUP_FAILURE_MESSAGE,
        )?;
        let Some(profile) = string_from_registry(&directory, bytes) else {
            return Err(UpdateError::new(
                STARTUP_FAILURE_CODE,
                STARTUP_FAILURE_MESSAGE,
            ));
        };
        let hive_path = PathBuf::from(profile).join("NTUSER.DAT");
        if !hive_path.is_file() {
            continue;
        }
        let hive_path = wide(hive_path.as_os_str());
        let mut hive = HKEY::default();
        if unsafe {
            RegLoadAppKeyW(
                PCWSTR(hive_path.as_ptr()),
                &mut hive,
                (KEY_READ | KEY_WRITE).0,
                REG_PROCESS_APPKEY,
                None,
            )
        } != windows::Win32::Foundation::ERROR_SUCCESS
        {
            continue;
        }
        let hive = RegistryKey(hive);
        if reconcile_run_value(hive.0, run_suffix, &expected, replacement_command)? {
            let status = unsafe { RegFlushKey(hive.0) };
            require(
                status == windows::Win32::Foundation::ERROR_SUCCESS,
                STARTUP_FAILURE_CODE,
                STARTUP_FAILURE_MESSAGE,
            )?;
        }
    }
    Ok(())
}

fn update_installation_startup(root: &Path, replacement: Option<&Path>) -> Result<()> {
    let executable = root.join("bin/snow_shot.exe");
    TaskManager::new()?.update_tasks(&executable, replacement)?;
    update_run_keys(root, replacement)
}

pub(super) fn remove_installation_startup(root: &Path) -> Result<()> {
    update_installation_startup(root, None)
}

pub(super) fn migrate_installation_startup(previous: &Path, root: &Path) -> Result<()> {
    if windows_equal(
        clean_windows_path(previous).as_os_str(),
        clean_windows_path(root).as_os_str(),
    ) {
        return Ok(());
    }
    update_installation_startup(previous, Some(root))
}

pub(super) fn launch_on_interactive_desktop(executable: &Path) -> Result<bool> {
    let apartment = match ComApartment::new() {
        Ok(apartment) => apartment,
        Err(_) => return Ok(false),
    };
    let launched = (|| -> windows::core::Result<()> {
        let windows: IShellWindows =
            unsafe { CoCreateInstance(&ShellWindows, None, CLSCTX_LOCAL_SERVER) }?;
        let empty = OwnedVariant::empty();
        let mut handle = 0;
        let desktop: IDispatch = unsafe {
            windows.FindWindowSW(
                &*empty,
                &*empty,
                SWC_DESKTOP,
                &mut handle,
                SWFO_NEEDDISPATCH,
            )
        }?;
        let provider: IServiceProvider = desktop.cast()?;
        let browser: IShellBrowser = unsafe { provider.QueryService(&SID_STopLevelBrowser) }?;
        let view = unsafe { browser.QueryActiveShellView() }?;
        let background: IDispatch = unsafe { view.GetItemObject(SVGIO_BACKGROUND) }?;
        let folder: IShellFolderViewDual = background.cast()?;
        let application = unsafe { folder.Application() }?;
        let shell: IShellDispatch2 = application.cast()?;

        let path = clean_windows_path(executable);
        let directory = path.parent().unwrap_or_else(|| Path::new("."));
        let arguments = OwnedVariant::string("--show-main-window");
        let directory = OwnedVariant::string(directory.to_string_lossy());
        let operation = OwnedVariant::string("open");
        let show = OwnedVariant::integer(SW_SHOWNORMAL.0);
        unsafe {
            shell.ShellExecute(
                &BSTR::from(path.to_string_lossy().as_ref()),
                &arguments,
                &directory,
                &operation,
                &show,
            )
        }
    })()
    .is_ok();
    drop(apartment);
    Ok(launched)
}
