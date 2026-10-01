//! Dispatch queues that follow the optional application startup policy.
use dispatch2::{DispatchQoS, DispatchQueue, DispatchQueueAttr, DispatchRetained};

pub fn application_queue(label: &str) -> DispatchRetained<DispatchQueue> {
    match snow_core::qos::active_application_qos() {
        Some(qos) => {
            let attr = DispatchQueueAttr::with_qos_class(None, DispatchQoS(qos.native_class()), 0);
            DispatchQueue::new(label, Some(&attr))
        }
        None => DispatchQueue::new(label, None),
    }
}

#[cfg(test)]
mod tests {
    #[test]
    fn native_queue_profiles() {
        for code in 0..5 {
            let status = std::process::Command::new(std::env::current_exe().unwrap())
                .args(["--exact", "qos::tests::native_queue_child", "--nocapture"])
                .env("SNOW_QOS_TEST_PROFILE", code.to_string())
                .status()
                .unwrap();
            assert!(status.success());
        }
    }

    #[test]
    fn native_queue_child() {
        use snow_core::qos::{ApplicationQos, active_application_qos, initialize_application_qos};
        assert_eq!(active_application_qos(), None);
        let Ok(code) = std::env::var("SNOW_QOS_TEST_PROFILE") else {
            return;
        };
        let qos = ApplicationQos::from_code(code.parse().unwrap()).unwrap();
        assert!(initialize_application_qos(qos, None));
        let queue = super::application_queue("app.snow.qos.test");
        let mut relative = 1;
        let class = unsafe { queue.qos_class(&mut relative) };
        assert_eq!(class.0, qos.native_class());
        assert_eq!(relative, 0);
        let (sender, receiver) = std::sync::mpsc::channel();
        queue.exec_async(move || {
            unsafe extern "C" {
                fn qos_class_self() -> u32;
            }
            sender.send(unsafe { qos_class_self() }).unwrap();
        });
        assert_eq!(receiver.recv().unwrap(), qos.native_class());
    }
}
