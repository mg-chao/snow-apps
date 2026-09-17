//! Startup-registration migration and removal: Task Scheduler tasks named
//! `SnowShot-<sid>-<hash>` plus every user's auto-start Run value, including
//! signed-out profiles loaded privately for the reconciliation.

use super::registryx::{
    Reconciled, enumerate_subkeys, flush_key, profile_image_path, reconcile_startup_run_value,
};
use super::{hresult_from_win32, wide_str, windows_error, windows_error_from_hresult};
use crate::crypto::sha256;
use crate::errors::{Error, Result};
use crate::paths::{case_fold, clean_path, paths_ci_eq};
use std::path::Path;
use windows::Win32::Foundation::LocalFree;
use windows::Win32::Security::Authorization::{ConvertSidToStringSidW, ConvertStringSidToSidW};
use windows::core::{BSTR, Interface};

use windows::Win32::Security::{
    DACL_SECURITY_INFORMATION, EqualSid, IsValidSid, LookupAccountNameW, SID, SID_NAME_USE,
};
use windows::Win32::System::Com::{
    CLSCTX_INPROC_SERVER, COINIT_APARTMENTTHREADED, CoCreateInstance, CoInitializeEx,
    CoUninitialize,
};
use windows::Win32::System::Registry::{
    HKEY, HKEY_LOCAL_MACHINE, HKEY_USERS, KEY_READ, KEY_WRITE, REG_PROCESS_APPKEY, RegCloseKey,
    RegLoadAppKeyW, RegOpenKeyExW,
};
use windows::Win32::System::TaskScheduler::{
    IActionCollection, IExecAction, IPrincipal, IRegisteredTask, IRegisteredTaskCollection,
    ITaskFolder, ITaskService, TASK_ACTION_EXEC, TASK_CREATE, TASK_INSTANCES_IGNORE_NEW,
    TASK_LOGON_INTERACTIVE_TOKEN, TASK_RUNLEVEL_HIGHEST, TASK_TRIGGER_LOGON, TaskScheduler,
};
use windows::Win32::System::Variant::{VARENUM, VARIANT};

struct Apartment;

impl Apartment {
    fn new() -> Option<Self> {
        unsafe {
            // S_OK and S_FALSE both leave the apartment initialized.
            (CoInitializeEx(None, COINIT_APARTMENTTHREADED).0 >= 0).then_some(Apartment)
        }
    }
}

impl Drop for Apartment {
    fn drop(&mut self) {
        unsafe {
            CoUninitialize();
        }
    }
}

/// A `VT_BSTR` variant owning its BSTR.
struct OwnedBstrVariant {
    variant: VARIANT,
}

impl OwnedBstrVariant {
    fn new(text: &str) -> Self {
        let mut variant = VARIANT::default();
        unsafe {
            let inner = &mut *variant.Anonymous.Anonymous;
            inner.vt = VARENUM(8); // VT_BSTR
            inner.Anonymous.bstrVal = core::mem::ManuallyDrop::new(BSTR::from(text));
        }
        OwnedBstrVariant { variant }
    }

    fn as_variant(&self) -> &VARIANT {
        &self.variant
    }
}

impl Drop for OwnedBstrVariant {
    fn drop(&mut self) {
        unsafe {
            let inner = &mut *self.variant.Anonymous.Anonymous;
            if inner.vt == VARENUM(8) {
                drop(core::mem::ManuallyDrop::take(&mut inner.Anonymous.bstrVal));
            }
        }
    }
}

fn empty_variant() -> VARIANT {
    VARIANT::default()
}

/// Resolves an account name or SID string into its canonical SID string.
fn account_sid_bytes(account: &str) -> Option<Vec<u8>> {
    let wide = wide_str(account);
    unsafe {
        let mut sid = windows::Win32::Security::PSID(std::ptr::null_mut());
        if ConvertStringSidToSidW(windows::core::PCWSTR(wide.as_ptr()), &mut sid).is_ok() {
            let length = (*sid.0.cast::<SID>()).SubAuthorityCount as usize * 4 + 8;
            let bytes = std::slice::from_raw_parts(sid.0.cast::<u8>(), length).to_vec();
            let _ = LocalFree(Some(windows::Win32::Foundation::HLOCAL(sid.0.cast())));
            return Some(bytes);
        }
        let mut sid_size = 0u32;
        let mut domain_size = 0u32;
        let mut use_kind = SID_NAME_USE(0);
        let _ = LookupAccountNameW(
            None,
            windows::core::PCWSTR(wide.as_ptr()),
            None,
            &mut sid_size,
            None,
            &mut domain_size,
            &mut use_kind,
        );
        if sid_size == 0 {
            return None;
        }
        let mut sid_bytes = vec![0u8; sid_size as usize];
        let mut domain = vec![0u16; domain_size as usize];
        if !LookupAccountNameW(
            None,
            windows::core::PCWSTR(wide.as_ptr()),
            Some(windows::Win32::Security::PSID(
                sid_bytes.as_mut_ptr().cast(),
            )),
            &mut sid_size,
            Some(windows::core::PWSTR(domain.as_mut_ptr())),
            &mut domain_size,
            &mut use_kind,
        )
        .is_ok()
            || !IsValidSid(windows::Win32::Security::PSID(
                sid_bytes.as_mut_ptr().cast(),
            ))
            .as_bool()
        {
            return None;
        }
        let length = (*sid_bytes.as_ptr().cast::<SID>()).SubAuthorityCount as usize * 4 + 8;
        sid_bytes.truncate(length);
        Some(sid_bytes)
    }
}

fn sid_bytes_to_string(bytes: &[u8]) -> Option<String> {
    unsafe {
        let mut text = windows::core::PWSTR::null();
        if !ConvertSidToStringSidW(
            windows::Win32::Security::PSID(bytes.as_ptr() as *mut _),
            &mut text,
        )
        .is_ok()
        {
            return None;
        }
        let string = text.to_string().ok()?;
        let _ = LocalFree(Some(windows::Win32::Foundation::HLOCAL(
            text.as_ptr().cast(),
        )));
        Some(string)
    }
}

pub fn canonical_account_sid(account_or_sid: &str) -> Option<String> {
    sid_bytes_to_string(&account_sid_bytes(account_or_sid)?)
}

pub fn same_account_sid(left: &str, right: &str) -> bool {
    let (Some(first), Some(second)) = (account_sid_bytes(left), account_sid_bytes(right)) else {
        return false;
    };
    unsafe {
        IsValidSid(windows::Win32::Security::PSID(first.as_ptr() as *mut _)).as_bool()
            && IsValidSid(windows::Win32::Security::PSID(second.as_ptr() as *mut _)).as_bool()
            && EqualSid(
                windows::Win32::Security::PSID(first.as_ptr() as *mut _),
                windows::Win32::Security::PSID(second.as_ptr() as *mut _),
            )
            .is_ok()
    }
}

/// The deterministic task name for an installation executable and owner.
fn task_name(executable_display: &str, sid: &str) -> String {
    let identity = case_fold(&clean_path(executable_display));
    let digest = sha256(identity.as_bytes());
    let hex = crate::crypto::hex(&digest);
    format!("SnowShot-{sid}-{}", &hex[..24])
}

struct Scheduler {
    service: ITaskService,
    folder: ITaskFolder,
}

impl Scheduler {
    fn connect() -> Result<Self> {
        unsafe {
            let service: ITaskService =
                CoCreateInstance(&TaskScheduler, None, CLSCTX_INPROC_SERVER)
                    .map_err(|error| windows_error_from_hresult(error.code().0))?;
            service
                .Connect(
                    &empty_variant(),
                    &empty_variant(),
                    &empty_variant(),
                    &empty_variant(),
                )
                .map_err(|error| windows_error_from_hresult(error.code().0))?;
            let folder = service
                .GetFolder(&BSTR::from("\\"))
                .map_err(|error| windows_error_from_hresult(error.code().0))?;
            Ok(Scheduler { service, folder })
        }
    }

    fn get_task(&self, name: &str) -> Result<Option<IRegisteredTask>> {
        unsafe {
            match self.folder.GetTask(&BSTR::from(name)) {
                Ok(task) => Ok(Some(task)),
                Err(error) if error.code().0 == -2147024894 => Ok(None), // HRESULT_FROM_WIN32(ERROR_FILE_NOT_FOUND)
                Err(error) => Err(windows_error_from_hresult(error.code().0)),
            }
        }
    }

    fn delete_task(&self, name: &str) -> Result<()> {
        unsafe {
            self.folder
                .DeleteTask(&BSTR::from(name), 0)
                .map_err(|error| windows_error_from_hresult(error.code().0))
        }
    }

    fn task_xml(&self, task: &IRegisteredTask) -> Result<String> {
        unsafe {
            task.Xml()
                .map(|value| value.to_string())
                .map_err(|error| windows_error_from_hresult(error.code().0))
        }
    }

    fn task_security(&self, task: &IRegisteredTask) -> Result<String> {
        unsafe {
            task.GetSecurityDescriptor(DACL_SECURITY_INFORMATION.0 as i32)
                .map(|value| value.to_string())
                .map_err(|error| windows_error_from_hresult(error.code().0))
        }
    }

    /// Verifies a discovered task really belongs to this installation.
    fn validate(&self, task: &IRegisteredTask, executable_display: &str, sid: &str) -> Result<()> {
        unsafe {
            let definition = task
                .Definition()
                .map_err(|error| windows_error_from_hresult(error.code().0))?;
            let principal: IPrincipal = definition
                .Principal()
                .map_err(|error| windows_error_from_hresult(error.code().0))?;
            let mut owner = BSTR::new();
            principal
                .UserId(&mut owner)
                .map_err(|error| windows_error_from_hresult(error.code().0))?;
            let owner = owner.to_string();
            // Task Scheduler returns a SAM/UPN name even when the task XML
            // stores a SID.
            if !same_account_sid(&owner, sid) {
                return Err(Error::raw("Startup task ownership mismatch"));
            }
            let actions: IActionCollection = definition
                .Actions()
                .map_err(|error| windows_error_from_hresult(error.code().0))?;
            let mut count = 0i32;
            actions
                .Count(&mut count)
                .map_err(|error| windows_error_from_hresult(error.code().0))?;
            if count != 1 {
                return Err(Error::raw("Startup task ownership mismatch"));
            }
            let action = actions
                .get_Item(1)
                .map_err(|error| windows_error_from_hresult(error.code().0))?;
            let exec: IExecAction = action
                .cast()
                .map_err(|_| Error::raw("Startup task target mismatch"))?;
            let mut path = BSTR::new();
            exec.Path(&mut path)
                .map_err(|error| windows_error_from_hresult(error.code().0))?;
            let target = clean_path(&path.to_string());
            let mut arguments = BSTR::new();
            exec.Arguments(&mut arguments)
                .map_err(|error| windows_error_from_hresult(error.code().0))?;
            let arguments = arguments.to_string();
            if !paths_ci_eq(&target, &clean_path(executable_display)) || arguments != "--autostart"
            {
                return Err(Error::raw("Startup task target mismatch"));
            }
            Ok(())
        }
    }

    fn restore_xml(&self, name: &str, xml: &str, sid: &str, security: &str) -> Result<()> {
        let user = OwnedBstrVariant::new(sid);
        let sddl = OwnedBstrVariant::new(security);
        unsafe {
            self.folder
                .RegisterTask(
                    &BSTR::from(name),
                    &BSTR::from(xml),
                    TASK_CREATE.0,
                    user.as_variant(),
                    &empty_variant(),
                    TASK_LOGON_INTERACTIVE_TOKEN,
                    sddl.as_variant(),
                )
                .map_err(|error| windows_error_from_hresult(error.code().0))?;
        }
        Ok(())
    }

    fn create(&self, name: &str, executable_display: &str, sid: &str) -> Result<()> {
        use windows::Win32::System::TaskScheduler::{
            ITaskSettings, ITriggerCollection, TASK_LOGON_INTERACTIVE_TOKEN,
        };
        unsafe {
            let definition = self
                .service
                .NewTask(0)
                .map_err(|error| windows_error_from_hresult(error.code().0))?;
            let principal: IPrincipal = definition
                .Principal()
                .map_err(|error| windows_error_from_hresult(error.code().0))?;
            principal
                .SetUserId(&BSTR::from(sid))
                .map_err(|error| windows_error_from_hresult(error.code().0))?;
            principal
                .SetLogonType(TASK_LOGON_INTERACTIVE_TOKEN)
                .map_err(|error| windows_error_from_hresult(error.code().0))?;
            principal
                .SetRunLevel(TASK_RUNLEVEL_HIGHEST)
                .map_err(|error| windows_error_from_hresult(error.code().0))?;
            let settings: ITaskSettings = definition
                .Settings()
                .map_err(|error| windows_error_from_hresult(error.code().0))?;
            settings
                .SetDisallowStartIfOnBatteries(windows::Win32::Foundation::VARIANT_BOOL(0))
                .map_err(|error| windows_error_from_hresult(error.code().0))?;
            settings
                .SetStopIfGoingOnBatteries(windows::Win32::Foundation::VARIANT_BOOL(0))
                .map_err(|error| windows_error_from_hresult(error.code().0))?;
            settings
                .SetExecutionTimeLimit(&BSTR::from("PT0S"))
                .map_err(|error| windows_error_from_hresult(error.code().0))?;
            settings
                .SetMultipleInstances(TASK_INSTANCES_IGNORE_NEW)
                .map_err(|error| windows_error_from_hresult(error.code().0))?;
            let triggers: ITriggerCollection = definition
                .Triggers()
                .map_err(|error| windows_error_from_hresult(error.code().0))?;
            let trigger = triggers
                .Create(TASK_TRIGGER_LOGON)
                .map_err(|error| windows_error_from_hresult(error.code().0))?;
            let logon: windows::Win32::System::TaskScheduler::ILogonTrigger = trigger
                .cast()
                .map_err(|error| windows_error_from_hresult(error.code().0))?;
            logon
                .SetUserId(&BSTR::from(sid))
                .map_err(|error| windows_error_from_hresult(error.code().0))?;
            let actions: IActionCollection = definition
                .Actions()
                .map_err(|error| windows_error_from_hresult(error.code().0))?;
            let action = actions
                .Create(TASK_ACTION_EXEC)
                .map_err(|error| windows_error_from_hresult(error.code().0))?;
            let exec: IExecAction = action
                .cast()
                .map_err(|error| windows_error_from_hresult(error.code().0))?;
            exec.SetPath(&BSTR::from(executable_display.replace('/', "\\")))
                .map_err(|error| windows_error_from_hresult(error.code().0))?;
            exec.SetArguments(&BSTR::from("--autostart"))
                .map_err(|error| windows_error_from_hresult(error.code().0))?;
            let working_directory = Path::new(executable_display)
                .parent()
                .unwrap_or(Path::new("."))
                .to_string_lossy()
                .replace('/', "\\");
            exec.SetWorkingDirectory(&BSTR::from(working_directory))
                .map_err(|error| windows_error_from_hresult(error.code().0))?;
            let user = OwnedBstrVariant::new(sid);
            let sddl =
                OwnedBstrVariant::new(&format!("D:P(A;;GA;;;SY)(A;;GA;;;BA)(A;;GRGX;;;{sid})"));
            self.folder
                .RegisterTaskDefinition(
                    &BSTR::from(name),
                    &definition,
                    TASK_CREATE.0,
                    user.as_variant(),
                    &empty_variant(),
                    TASK_LOGON_INTERACTIVE_TOKEN,
                    sddl.as_variant(),
                )
                .map_err(|error| windows_error_from_hresult(error.code().0))?;
        }
        Ok(())
    }
}

fn native_command(executable_display: &str) -> String {
    format!("\"{}\" --autostart", executable_display.replace('/', "\\"))
}

/// Core of `--migrate-startup` (replacement set) and `--uninstall` (none).
fn update_installation_startup(root: &Path, replacement_root: Option<&Path>) -> Result<()> {
    let Some(_apartment) = Apartment::new() else {
        return Err(Error::raw("Windows error 0x80004015")); // COINIT failure as a raw diagnostic
    };
    let scheduler = Scheduler::connect()?;
    let executable_display = clean_path(&root.join("bin/snow_shot.exe").to_string_lossy());
    let replacement_display = replacement_root
        .map(|replacement| clean_path(&replacement.join("bin/snow_shot.exe").to_string_lossy()));
    unsafe {
        let collection: IRegisteredTaskCollection = scheduler
            .folder
            .GetTasks(1 /* TASK_ENUM_HIDDEN */)
            .map_err(|error| windows_error_from_hresult(error.code().0))?;
        let count = collection
            .Count()
            .map_err(|error| windows_error_from_hresult(error.code().0))?;
        for index in (1..=count).rev() {
            let index_variant = {
                let mut variant = VARIANT::default();
                let inner = &mut *variant.Anonymous.Anonymous;
                inner.vt = VARENUM(3); // VT_I4
                inner.Anonymous.lVal = index;
                variant
            };
            let Ok(task) = collection.get_Item(&index_variant) else {
                // A task that cannot be inspected (for example a third-party
                // task with a restrictive descriptor) is not a verifiable
                // registration; skipping it must not abort the operation.
                continue;
            };
            let candidate: std::result::Result<String, ()> =
                (|| Ok(task.Name().map_err(|_| ())?.to_string()))();
            let Ok(candidate) = candidate else {
                continue;
            };
            let owner = (|| -> Option<String> {
                let definition = task.Definition().ok()?;
                let principal: IPrincipal = definition.Principal().ok()?;
                let mut user = BSTR::new();
                principal.UserId(&mut user).ok()?;
                Some(user.to_string())
            })();
            let Some(owner) = owner else { continue };
            if !candidate.starts_with("SnowShot-") {
                continue;
            }
            let Some(sid) = canonical_account_sid(&owner) else {
                continue;
            };
            if candidate != task_name(&executable_display, &sid) {
                continue;
            }
            scheduler.validate(&task, &executable_display, &sid)?;
            let previous_xml = scheduler.task_xml(&task)?;
            let previous_security = scheduler.task_security(&task)?;
            if let Some(replacement_display) = &replacement_display {
                let destination = task_name(replacement_display, &sid);
                if scheduler.get_task(&destination)?.is_some() {
                    return Err(Error::raw("The destination already has a startup task"));
                }
            }
            scheduler.delete_task(&candidate)?;
            if let Some(replacement_display) = &replacement_display {
                let destination = task_name(replacement_display, &sid);
                if let Err(error) = scheduler.create(&destination, replacement_display, &sid) {
                    scheduler.restore_xml(&candidate, &previous_xml, &sid, &previous_security)?;
                    return Err(error);
                }
            }
        }
    }
    // Reconcile every user's auto-start value through one privilege-aware
    // pass: missing or inaccessible Run keys are skipped, and only
    // registrations matching this installation are removed or rewritten.
    let startup_command = native_command(&executable_display);
    let migrated_command = replacement_display
        .as_deref()
        .map(native_command)
        .unwrap_or_default();
    for sid in enumerate_subkeys(HKEY_USERS, None) {
        reconcile_startup_run_value(
            HKEY_USERS,
            &format!("{sid}\\Software\\Microsoft\\Windows\\CurrentVersion\\Run"),
            &startup_command,
            if migrated_command.is_empty() {
                None
            } else {
                Some(&migrated_command)
            },
        )?;
    }
    // Users who are signed out have no HKEY_USERS hive. Open their
    // application hive privately rather than mounting it globally or writing
    // the installer's HKCU.
    unsafe {
        let mut profiles = HKEY::default();
        let opened = RegOpenKeyExW(
            HKEY_LOCAL_MACHINE,
            windows::core::PCWSTR(
                wide_str("SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion\\ProfileList").as_ptr(),
            ),
            None,
            KEY_READ,
            &mut profiles,
        );
        if opened.0 != 0 {
            return Err(windows_error(hresult_from_win32(opened.0)));
        }
        for sid in enumerate_subkeys(profiles, None) {
            if !sid.starts_with("S-1-5-21-") && !sid.starts_with("S-1-12-1-") {
                continue;
            }
            if super::registryx::hive_loaded(&sid) {
                continue;
            }
            let Some(directory) = profile_image_path(profiles, &sid) else {
                continue;
            };
            let hive_path = Path::new(&directory).join("NTUSER.DAT");
            if !super::fsx::file_exists(&hive_path) {
                continue;
            }
            let mut hive = HKEY::default();
            // A profile hive that is in use or denies loading cannot be
            // verified; skipping it must not abort the operation.
            if RegLoadAppKeyW(
                windows::core::PCWSTR(super::wide(hive_path.as_os_str()).as_ptr()),
                &mut hive,
                (KEY_READ | KEY_WRITE).0,
                REG_PROCESS_APPKEY,
                None,
            )
            .0 != 0
            {
                continue;
            }
            let reconciled = reconcile_startup_run_value(
                hive,
                "Software\\Microsoft\\Windows\\CurrentVersion\\Run",
                &startup_command,
                if migrated_command.is_empty() {
                    None
                } else {
                    Some(&migrated_command)
                },
            );
            if reconciled
                .as_ref()
                .is_ok_and(|result| matches!(result, Reconciled::Applied))
            {
                let _ = flush_key(hive);
            }
            let _ = RegCloseKey(hive);
            reconciled?;
        }
        let _ = RegCloseKey(profiles);
    }
    Ok(())
}

pub fn remove_installation_startup(root: &Path) -> Result<()> {
    update_installation_startup(root, None)
}

pub fn migrate_installation_startup(previous_root: &Path, root: &Path) -> Result<()> {
    if paths_ci_eq(
        &clean_path(&previous_root.to_string_lossy()),
        &clean_path(&root.to_string_lossy()),
    ) {
        return Ok(());
    }
    update_installation_startup(previous_root, Some(root))
}
