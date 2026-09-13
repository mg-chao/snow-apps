//! Encoder observations enabled only in benchmark builds.
use std::collections::BTreeMap;
use std::time::{Duration, Instant};

#[derive(Clone, Debug, Default, PartialEq, Eq)]
pub struct EncoderTimings {
    pub stages: BTreeMap<&'static str, Vec<Duration>>,
    pub first_packet: Option<Duration>,
    pub submitted_pts: Vec<i64>,
    pub encoded_pts: Vec<i64>,
    pub admissions: Vec<(i64, Instant)>,
    pub submissions: Vec<(i64, Instant)>,
    pub packets: Vec<(i64, Instant)>,
    pub copied_bytes: u64,
    pub cpu_conversions: u64,
    pub gpu_surface_submissions: u64,
    started: Option<Instant>,
}

impl EncoderTimings {
    pub(crate) fn begin(&mut self) {
        self.started = Some(Instant::now());
    }

    pub(crate) fn record(&mut self, name: &'static str, since: Instant) {
        self.stages.entry(name).or_default().push(since.elapsed());
    }

    pub(crate) fn packet(&mut self, pts: Option<i64>) {
        if let Some(pts) = pts {
            self.packets.push((pts, Instant::now()));
        }
        if self.first_packet.is_none() {
            self.first_packet = self.started.map(|started| started.elapsed());
        }
    }
}
