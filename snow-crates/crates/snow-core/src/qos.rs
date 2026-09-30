//! Optional, launch-scoped scheduling policy for application-owned work.
//! Library consumers that do not initialize it retain the system's scheduling policy.

use std::sync::OnceLock;

#[derive(Clone, Copy, Debug, Eq, PartialEq)]
#[repr(u32)]
pub enum ApplicationQos {
    Responsive = 0,
    UserInitiated = 1,
    Default = 2,
    Utility = 3,
    Background = 4,
}

impl ApplicationQos {
    pub fn from_code(code: u32) -> Option<Self> {
        match code {
            0 => Some(Self::Responsive),
            1 => Some(Self::UserInitiated),
            2 => Some(Self::Default),
            3 => Some(Self::Utility),
            4 => Some(Self::Background),
            _ => None,
        }
    }

    /// Darwin QoS class; also usable when constructing native dispatch queues.
    pub fn native_class(self) -> u32 {
        match self {
            Self::Responsive => 0x21,
            Self::UserInitiated => 0x19,
            Self::Default => 0x15,
            Self::Utility => 0x11,
            Self::Background => 0x09,
        }
    }
}

pub type ErrorHandler = extern "C" fn(i32);

struct Policy {
    qos: ApplicationQos,
    report_error: Option<ErrorHandler>,
}

static ACTIVE: OnceLock<Policy> = OnceLock::new();

pub fn active_application_qos() -> Option<ApplicationQos> {
    ACTIVE.get().map(|policy| policy.qos)
}

/// Initialize once at launch. A different subsequent preference requires a restart.
pub fn initialize_application_qos(qos: ApplicationQos, report_error: Option<ErrorHandler>) -> bool {
    if let Some(policy) = ACTIVE.get() {
        return policy.qos == qos;
    }
    if ACTIVE.set(Policy { qos, report_error }).is_err() {
        return active_application_qos() == Some(qos);
    }
    #[cfg(target_os = "macos")]
    {
        // Image/stitching algorithms use Rayon both through dedicated pools and
        // the global pool. Install its policy before any application work starts.
        if rayon::ThreadPoolBuilder::new()
            .start_handler(|_| apply_current_thread())
            .build_global()
            .is_err()
        {
            report(-3);
        }
    }
    true
}

fn report(code: i32) {
    if let Some(handler) = ACTIVE.get().and_then(|policy| policy.report_error) {
        handler(code);
    } else {
        eprintln!("Unable to apply application QoS: {code}");
    }
}

/// Call at worker entry or at a reusable worker's work boundary.
pub fn apply_current_thread() {
    let _ = apply_current_thread_result();
}

pub fn apply_current_thread_result() -> i32 {
    let code = try_apply_current_thread();
    if code != 0 {
        #[cfg(target_os = "macos")]
        {
            use std::cell::Cell;
            thread_local! { static REPORTED: Cell<bool> = const { Cell::new(false) }; }
            REPORTED.with(|reported| {
                if !reported.replace(true) {
                    report(code);
                }
            });
        }
        #[cfg(not(target_os = "macos"))]
        report(code);
    }
    code
}

pub fn try_apply_current_thread() -> i32 {
    #[cfg(target_os = "macos")]
    {
        use std::cell::Cell;
        thread_local! { static ATTEMPTED: Cell<Option<i32>> = const { Cell::new(None) }; }
        unsafe extern "C" {
            fn pthread_set_qos_class_self_np(class: u32, relative_priority: i32) -> i32;
        }
        let Some(qos) = active_application_qos() else {
            return 0;
        };
        ATTEMPTED.with(|attempted| {
            if let Some(code) = attempted.get() {
                return code;
            }
            // SAFETY: the enum maps exclusively to supported Darwin classes and
            // zero is a valid relative priority. Only the calling thread changes.
            let code = unsafe { pthread_set_qos_class_self_np(qos.native_class(), 0) };
            attempted.set(Some(code));
            code
        })
    }
    #[cfg(not(target_os = "macos"))]
    {
        0
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn codes_match_native_classes() {
        let classes = [0x21, 0x19, 0x15, 0x11, 0x09];
        for (code, class) in classes.into_iter().enumerate() {
            assert_eq!(
                ApplicationQos::from_code(code as u32)
                    .unwrap()
                    .native_class(),
                class
            );
        }
        assert_eq!(ApplicationQos::from_code(5), None);
        assert_eq!(ApplicationQos::from_code(u32::MAX), None);
    }

    #[cfg(target_os = "macos")]
    #[test]
    fn native_profiles() {
        for code in 0..5 {
            let status = std::process::Command::new(std::env::current_exe().unwrap())
                .args(["--exact", "qos::tests::native_child", "--nocapture"])
                .env("SNOW_QOS_TEST_PROFILE", code.to_string())
                .status()
                .unwrap();
            assert!(status.success(), "native profile {code}");
        }
    }

    #[cfg(target_os = "macos")]
    #[test]
    fn native_child() {
        unsafe extern "C" {
            fn qos_class_self() -> u32;
        }
        assert_eq!(active_application_qos(), None);
        let before = unsafe { qos_class_self() };
        apply_current_thread();
        assert_eq!(unsafe { qos_class_self() }, before);
        let Ok(code) = std::env::var("SNOW_QOS_TEST_PROFILE") else {
            return;
        };
        let qos = ApplicationQos::from_code(code.parse().unwrap()).unwrap();
        assert!(initialize_application_qos(qos, None));
        assert_eq!(try_apply_current_thread(), 0);
        assert_eq!(unsafe { qos_class_self() }, qos.native_class());
        let other = ApplicationQos::from_code((qos as u32 + 1) % 5).unwrap();
        assert!(!initialize_application_qos(other, None));
        assert_eq!(active_application_qos(), Some(qos));
        assert_eq!(
            std::thread::spawn(|| {
                apply_current_thread();
                unsafe { qos_class_self() }
            })
            .join()
            .unwrap(),
            qos.native_class()
        );
        let (sender, receiver) = std::sync::mpsc::channel();
        rayon::spawn(move || sender.send(unsafe { qos_class_self() }).unwrap());
        assert_eq!(receiver.recv().unwrap(), qos.native_class());
        let pool = rayon::ThreadPoolBuilder::new()
            .num_threads(2)
            .start_handler(|_| apply_current_thread())
            .build()
            .unwrap();
        let (sender, receiver) = std::sync::mpsc::channel();
        pool.spawn(move || sender.send(unsafe { qos_class_self() }).unwrap());
        assert_eq!(receiver.recv().unwrap(), qos.native_class());
    }

    #[cfg(target_os = "macos")]
    #[test]
    fn native_failure() {
        let status = std::process::Command::new(std::env::current_exe().unwrap())
            .args(["--exact", "qos::tests::native_failure_child", "--nocapture"])
            .env("SNOW_QOS_TEST_FAILURE", "1")
            .status()
            .unwrap();
        assert!(status.success());
    }

    #[cfg(target_os = "macos")]
    #[test]
    fn native_failure_child() {
        if std::env::var("SNOW_QOS_TEST_FAILURE").is_err() {
            return;
        }
        use std::sync::atomic::{AtomicUsize, Ordering};
        static ERRORS: AtomicUsize = AtomicUsize::new(0);
        extern "C" fn on_error(_: i32) {
            ERRORS.fetch_add(1, Ordering::Relaxed);
        }
        #[repr(C)]
        struct Schedule {
            priority: i32,
        }
        unsafe extern "C" {
            fn pthread_self() -> *mut std::ffi::c_void;
            fn pthread_setschedparam(
                thread: *mut std::ffi::c_void,
                policy: i32,
                param: *const Schedule,
            ) -> i32;
        }
        // Darwin permanently opts a thread out of QoS after a traditional
        // scheduling call. Exercise failure only inside this disposable process.
        assert_eq!(
            unsafe { pthread_setschedparam(pthread_self(), 1, &Schedule { priority: 0 }) },
            0
        );
        assert!(initialize_application_qos(
            ApplicationQos::Utility,
            Some(on_error)
        ));
        let code = apply_current_thread_result();
        assert_ne!(code, 0);
        assert_eq!(apply_current_thread_result(), code);
        assert_eq!(ERRORS.load(Ordering::Relaxed), 1);
        assert_eq!(active_application_qos(), Some(ApplicationQos::Utility));
    }
}
