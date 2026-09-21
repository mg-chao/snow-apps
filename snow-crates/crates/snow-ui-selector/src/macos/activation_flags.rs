//! Activation flags differ across providers; leases remember the flag actually changed.
use super::activation::Outcome;
use crate::StopReason;

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub(super) enum Flag {
    Manual,
    Enhanced,
}
impl Flag {
    pub(super) fn name(self) -> &'static str {
        match self {
            Self::Manual => "AXManualAccessibility",
            Self::Enhanced => "AXEnhancedUserInterface",
        }
    }
}
#[derive(Clone, Copy, Debug)]
pub(super) struct ActivationState {
    pub outcome: Outcome,
    pub flag: Option<Flag>,
}
impl Default for ActivationState {
    fn default() -> Self {
        Self {
            outcome: Outcome::Unsupported,
            flag: None,
        }
    }
}
pub(super) trait Flags {
    fn read(&self, flag: Flag) -> Result<Option<bool>, StopReason>;
    fn writable(&self, flag: Flag) -> Result<bool, StopReason>;
    fn write(&self, flag: Flag, enabled: bool) -> Result<(), StopReason>;
    // A write can take effect even when its reply or verification fails.
    fn rollback(&self, flag: Flag);
}
pub(super) fn enable(flags: &impl Flags) -> Result<ActivationState, StopReason> {
    for flag in [Flag::Manual, Flag::Enhanced] {
        match flags.read(flag)? {
            Some(true) => {
                return Ok(ActivationState {
                    outcome: Outcome::PreEnabled,
                    flag: Some(flag),
                });
            }
            Some(false) if flags.writable(flag)? => {
                if let Err(reason) = flags.write(flag, true) {
                    flags.rollback(flag);
                    return Err(reason);
                }
                return Ok(ActivationState {
                    outcome: Outcome::Owned,
                    flag: Some(flag),
                });
            }
            _ => {}
        }
    }
    Ok(ActivationState::default())
}

/// Firefox applies Enhanced writes but replies NotImplemented. Trust readback,
/// never the error alone, and do not hide unrelated provider failures.
pub(super) fn verify_write(
    flag: Flag,
    enabled: bool,
    code: i32,
    read: impl FnOnce() -> Result<Option<bool>, StopReason>,
    status: impl FnOnce(i32) -> Result<(), StopReason>,
) -> Result<(), StopReason> {
    if flag == Flag::Enhanced && code == accessibility_sys::kAXErrorNotImplemented {
        return if read()? == Some(enabled) {
            Ok(())
        } else {
            Err(StopReason::ProviderFailure)
        };
    }
    status(code)
}

#[cfg(test)]
mod tests {
    use super::*;
    use std::cell::RefCell;
    struct Fake {
        values: RefCell<[Option<bool>; 2]>,
        writes: RefCell<Vec<(Flag, bool)>>,
        fail_after_write: bool,
    }
    impl Flags for Fake {
        fn read(&self, flag: Flag) -> Result<Option<bool>, StopReason> {
            Ok(self.values.borrow()[flag as usize])
        }
        fn writable(&self, _: Flag) -> Result<bool, StopReason> {
            Ok(true)
        }
        fn write(&self, flag: Flag, enabled: bool) -> Result<(), StopReason> {
            self.values.borrow_mut()[flag as usize] = Some(enabled);
            self.writes.borrow_mut().push((flag, enabled));
            if self.fail_after_write {
                Err(StopReason::BudgetExhausted)
            } else {
                Ok(())
            }
        }
        fn rollback(&self, flag: Flag) {
            self.values.borrow_mut()[flag as usize] = Some(false);
            self.writes.borrow_mut().push((flag, false));
        }
    }
    #[test]
    fn chooses_supported_flag_and_preserves_preenabled_providers() {
        for (values, expected, writes) in [
            (
                [Some(false), Some(false)],
                Outcome::Owned,
                vec![(Flag::Manual, true)],
            ),
            (
                [None, Some(false)],
                Outcome::Owned,
                vec![(Flag::Enhanced, true)],
            ),
            ([None, Some(true)], Outcome::PreEnabled, vec![]),
            ([Some(true), Some(false)], Outcome::PreEnabled, vec![]),
            ([None, None], Outcome::Unsupported, vec![]),
        ] {
            let fake = Fake {
                values: RefCell::new(values),
                writes: RefCell::new(vec![]),
                fail_after_write: false,
            };
            let state = enable(&fake).unwrap();
            assert_eq!(state.outcome, expected);
            assert_eq!(*fake.writes.borrow(), writes);
            if let Some((flag, _)) = writes.first() {
                assert_eq!(state.flag, Some(*flag));
            }
        }
    }
    #[test]
    fn failed_verification_rolls_back_the_changed_flag() {
        let fake = Fake {
            values: RefCell::new([None, Some(false)]),
            writes: RefCell::new(vec![]),
            fail_after_write: true,
        };
        assert_eq!(enable(&fake).unwrap_err(), StopReason::BudgetExhausted);
        assert_eq!(*fake.values.borrow(), [None, Some(false)]);
        assert_eq!(
            *fake.writes.borrow(),
            [(Flag::Enhanced, true), (Flag::Enhanced, false)]
        );
    }
    #[test]
    fn firefox_error_requires_matching_readback_for_enable_and_restore() {
        for enabled in [false, true] {
            for actual in [None, Some(false), Some(true)] {
                let result = verify_write(
                    Flag::Enhanced,
                    enabled,
                    accessibility_sys::kAXErrorNotImplemented,
                    || Ok(actual),
                    |_| panic!(),
                );
                assert_eq!(result.is_ok(), actual == Some(enabled));
            }
        }
        assert_eq!(
            verify_write(
                Flag::Enhanced,
                true,
                accessibility_sys::kAXErrorNotImplemented,
                || Err(StopReason::BudgetExhausted),
                |_| panic!()
            ),
            Err(StopReason::BudgetExhausted)
        );
        for (flag, code) in [
            (Flag::Manual, accessibility_sys::kAXErrorNotImplemented),
            (Flag::Enhanced, accessibility_sys::kAXErrorCannotComplete),
        ] {
            assert_eq!(
                verify_write(
                    flag,
                    true,
                    code,
                    || panic!(),
                    |_| Err(StopReason::ProviderFailure)
                ),
                Err(StopReason::ProviderFailure)
            );
        }
    }
}
