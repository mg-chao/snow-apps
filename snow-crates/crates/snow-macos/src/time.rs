use objc2_core_media::CMClock;
use snow_media::time::{ClockDomain, MediaTime};
use std::time::Instant;
pub fn host_time_to_instant(time: MediaTime) -> Option<Instant> {
    let now = Instant::now();
    let host = unsafe { CMClock::host_time_clock().time() };
    let host = MediaTime {
        value: host.value,
        timescale: host.timescale as u32,
        domain: ClockDomain::MacHostTime,
        epoch: host.epoch,
    };
    now.checked_sub(host.duration_since(time)?)
}
