//! Launching the application on the interactive desktop from an elevated
//! context, through the shell's own dispatch (the NSIS finish-page path).

use std::path::Path;
use windows::Win32::System::Com::{
    CLSCTX_LOCAL_SERVER, COINIT_APARTMENTTHREADED, CoCreateInstance, CoInitializeEx, CoUninitialize,
};
use windows::Win32::System::Variant::VARIANT;
use windows::Win32::UI::Shell::{
    IShellBrowser, IShellDispatch2, IShellFolderViewDual, IShellWindows, SID_STopLevelBrowser,
    SVGIO_BACKGROUND, SWC_DESKTOP, SWFO_NEEDDISPATCH, ShellWindows,
};
use windows::Win32::UI::WindowsAndMessaging::SW_SHOWNORMAL;
use windows::core::{BSTR, Interface};

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

pub fn launch_on_interactive_desktop(executable: &Path) -> bool {
    let Some(_apartment) = Apartment::new() else {
        return false;
    };
    let windows: IShellWindows =
        match unsafe { CoCreateInstance(&ShellWindows, None, CLSCTX_LOCAL_SERVER) } {
            Ok(windows) => windows,
            Err(_) => return false,
        };
    launch_from_shell_windows(&windows, executable)
}

fn launch_from_shell_windows(windows: &IShellWindows, executable: &Path) -> bool {
    let empty = VARIANT::default();
    let mut handle = 0i32;
    let Ok(desktop) = (unsafe {
        windows.FindWindowSW(&empty, &empty, SWC_DESKTOP, &mut handle, SWFO_NEEDDISPATCH)
    }) else {
        return false;
    };
    let Ok(provider) = desktop.cast::<windows::Win32::System::Com::IServiceProvider>() else {
        return false;
    };
    let Ok(browser) = (unsafe { provider.QueryService::<IShellBrowser>(&SID_STopLevelBrowser) })
    else {
        return false;
    };
    let Ok(view) = (unsafe { browser.QueryActiveShellView() }) else {
        return false;
    };
    let Ok(background) =
        (unsafe { view.GetItemObject::<windows::Win32::System::Com::IDispatch>(SVGIO_BACKGROUND) })
    else {
        return false;
    };
    let Ok(folder) = background.cast::<IShellFolderViewDual>() else {
        return false;
    };
    let Ok(application) = (unsafe { folder.Application() }) else {
        return false;
    };
    let Ok(shell) = application.cast::<IShellDispatch2>() else {
        return false;
    };
    let directory = executable.parent().unwrap_or(Path::new("."));
    let file = BSTR::from(executable.to_string_lossy().into_owned());
    let arguments = OwnedBstrVariant::new("--show-main-window");
    let dir = OwnedBstrVariant::new(&directory.to_string_lossy());
    let operation = OwnedBstrVariant::new("open");
    let show = i4_variant(SW_SHOWNORMAL.0);
    let launched = unsafe {
        shell.ShellExecute(
            &file,
            arguments.as_variant(),
            dir.as_variant(),
            operation.as_variant(),
            &show,
        )
    };
    launched.is_ok()
}

/// A `VT_BSTR` variant owning its BSTR so nothing leaks across COM calls.
struct OwnedBstrVariant {
    variant: VARIANT,
}

impl OwnedBstrVariant {
    fn new(text: &str) -> Self {
        let mut variant = VARIANT::default();
        unsafe {
            let inner = &mut *variant.Anonymous.Anonymous;
            inner.vt = windows::Win32::System::Variant::VARENUM(8); // VT_BSTR
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
            if inner.vt == windows::Win32::System::Variant::VARENUM(8) {
                drop(core::mem::ManuallyDrop::take(&mut inner.Anonymous.bstrVal));
            }
        }
    }
}

fn i4_variant(value: i32) -> VARIANT {
    let mut variant = VARIANT::default();
    unsafe {
        let inner = &mut *variant.Anonymous.Anonymous;
        inner.vt = windows::Win32::System::Variant::VARENUM(3); // VT_I4
        inner.Anonymous.lVal = value;
    }
    variant
}
