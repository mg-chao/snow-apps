use std::fmt;

#[derive(Clone, Copy, Debug, PartialEq, Eq, Hash)]
pub(crate) struct MonitorKey {
    pub(crate) adapter_luid: u64,
    pub(crate) output_id: u64,
}

impl MonitorKey {
    pub(crate) fn from_device_name(adapter_luid: u64, device_name: &str) -> Self {
        Self {
            adapter_luid,
            output_id: fnv1a_64(device_name.as_bytes()),
        }
    }
}

fn fnv1a_64(bytes: &[u8]) -> u64 {
    const OFFSET_BASIS: u64 = 0xcbf2_9ce4_8422_2325;
    const PRIME: u64 = 0x0000_0001_0000_01b3;

    let mut hash = OFFSET_BASIS;
    for byte in bytes {
        hash ^= u64::from(*byte);
        hash = hash.wrapping_mul(PRIME);
    }
    hash
}

#[derive(Clone, Debug)]
pub struct MonitorId {
    pub(crate) key: MonitorKey,

    pub(crate) handle: isize,

    name: String,

    is_primary: bool,
}

impl MonitorId {
    pub fn from_parts(
        adapter_luid: u64,
        output_id: u64,
        raw_handle: isize,
        name: impl Into<String>,
        is_primary: bool,
    ) -> Self {
        Self {
            key: MonitorKey {
                adapter_luid,
                output_id,
            },
            handle: raw_handle,
            name: name.into(),
            is_primary,
        }
    }

    pub fn from_name(raw_handle: isize, name: impl Into<String>, is_primary: bool) -> Self {
        let name = name.into();
        Self {
            key: MonitorKey::from_device_name(0, &name),
            handle: raw_handle,
            name,
            is_primary,
        }
    }

    pub(crate) fn raw_handle(&self) -> isize {
        self.handle
    }

    pub fn name(&self) -> &str {
        &self.name
    }

    pub fn is_primary(&self) -> bool {
        self.is_primary
    }

    pub fn stable_id(&self) -> String {
        format!("{:016x}-{:016x}", self.key.adapter_luid, self.key.output_id)
    }

    pub(crate) fn key(&self) -> MonitorKey {
        self.key
    }
}

impl fmt::Display for MonitorId {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        write!(f, "{}", self.name)
    }
}

pub fn enumerate_monitors() -> crate::error::CaptureResult<Vec<MonitorId>> {
    crate::system::CaptureSystem::builder()
        .build()?
        .enumerate_monitors()
}

pub fn primary_monitor() -> crate::error::CaptureResult<MonitorId> {
    crate::system::CaptureSystem::builder()
        .build()?
        .primary_monitor()
}

// Native handles, names and primary status are mutable observations of a display.
// Identity remains stable across reconfiguration and refreshed enumeration.
impl PartialEq for MonitorId {
    fn eq(&self, other: &Self) -> bool {
        self.key == other.key
    }
}
impl Eq for MonitorId {}
impl std::hash::Hash for MonitorId {
    fn hash<H: std::hash::Hasher>(&self, state: &mut H) {
        self.key.hash(state);
    }
}
#[cfg(test)]
mod tests {
    use super::*;
    #[test]
    fn display_identity_excludes_mutable_enumeration_metadata() {
        let first = MonitorId::from_parts(1, 2, 3, "first", true);
        let refreshed = MonitorId::from_parts(1, 2, 4, "renamed", false);
        assert_eq!(first, refreshed);
        assert_eq!(first.stable_id(), refreshed.stable_id());
        assert_eq!(std::collections::HashSet::from([first, refreshed]).len(), 1);
        assert_ne!(
            MonitorId::from_parts(1, 2, 3, "same", false),
            MonitorId::from_parts(1, 3, 3, "same", false)
        );
    }
}
