//! Encoder observations enabled only in benchmark builds.
use std::collections::BTreeMap;
use std::time::{Duration, Instant};

#[derive(Clone, Debug, Default, PartialEq, Eq)]
pub struct EncoderTimings {
    pub stages: BTreeMap<&'static str, Vec<Duration>>,
    pub first_packet: Option<Duration>,
    pub submitted_pts: Vec<i64>,
    pub encoded_pts: Vec<i64>,
    pub copied_bytes: u64,
    started: Option<Instant>,
}

impl EncoderTimings {
    pub(crate) fn begin(&mut self) {
        self.started = Some(Instant::now());
    }

    pub(crate) fn record(&mut self, name: &'static str, since: Instant) {
        self.stages.entry(name).or_default().push(since.elapsed());
    }

    pub(crate) fn packet(&mut self) {
        if self.first_packet.is_none() {
            self.first_packet = self.started.map(|started| started.elapsed());
        }
    }
}
