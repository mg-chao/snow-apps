//! A real main-thread host is required for TIS. No capture/input permission,
//! event injection, visible window, or change to the user's layout is needed.
#[cfg(not(target_os = "macos"))]
fn main() {}

#[cfg(target_os = "macos")]
fn main() {
    use snow_macos::text::{keyboard_label, prepare_keyboard_layout};
    use std::time::{Duration, Instant};

    // Exercise worker initialization with an active host loop, as recording does.
    let worker = std::thread::spawn(|| {
        prepare_keyboard_layout();
        assert!(
            keyboard_label(0, 40, 0).is_some(),
            "first worker key must resolve"
        );
    });
    let deadline = Instant::now() + Duration::from_secs(10);
    snow_macos::run_loop::drive_until(|| worker.is_finished() || Instant::now() >= deadline);
    assert!(worker.is_finished(), "layout preparation timed out");
    worker.join().unwrap();
    // Repeated main-thread initialization, as preview sessions are recreated.
    prepare_keyboard_layout();
    let cases: Vec<_> = [0, 1, 6, 18, 24, 27, 33, 42]
        .into_iter()
        .flat_map(|key| {
            [0, 1 << 17, 1 << 19, 1 << 16, (1 << 20) | (1 << 18)]
                .into_iter()
                .map(move |modifiers| (key, modifiers))
        })
        .collect();
    let expected: Vec<_> = cases
        .iter()
        .map(|&(key, modifiers)| keyboard_label(key, 40, modifiers))
        .collect();
    assert!(
        expected.iter().any(Option::is_some),
        "active layout must resolve printable keys"
    );
    let (done, received) = std::sync::mpsc::channel();
    let worker = std::thread::spawn(move || {
        // Lookup and reinitialization must not depend on a pumping UI thread:
        // preview shutdown joins its worker on that thread.
        prepare_keyboard_layout();
        for _ in 0..3 {
            let actual: Vec<_> = cases
                .iter()
                .map(|&(key, modifiers)| keyboard_label(key, 40, modifiers))
                .collect();
            assert_eq!(actual, expected);
        }
        done.send(()).unwrap();
    });
    received
        .recv_timeout(Duration::from_secs(10))
        .expect("keyboard worker blocked on the main queue");
    worker.join().unwrap();
}
