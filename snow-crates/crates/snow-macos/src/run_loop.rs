//! Utility for command-line native harnesses. GUI hosts should run their normal
//! application loop instead; all synchronous media operations belong on workers.
pub fn drive_until(done: impl Fn() -> bool) {
    assert!(
        objc2::MainThreadMarker::new().is_some(),
        "the host run loop must run on the main thread"
    );
    while !done() {
        unsafe {
            objc2_core_foundation::CFRunLoop::run_in_mode(
                objc2_core_foundation::kCFRunLoopDefaultMode,
                0.02,
                false,
            );
        }
        std::thread::sleep(std::time::Duration::from_millis(1));
    }
}
