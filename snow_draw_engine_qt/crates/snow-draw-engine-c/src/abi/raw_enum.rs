/// A `#[repr(C)]` ABI enum whose raw C discriminants are decoded through
/// [`SnowRawEnum::from_raw`], which [`snow_c_enum!`] generates from the
/// variant list itself.
pub(crate) trait SnowRawEnum: Copy {
    /// Decodes a raw C discriminant, returning `None` for values outside the
    /// declared variant set so they can be rejected before any typed read of
    /// C memory.
    fn from_raw(raw: i32) -> Option<Self>;
}

/// Declares a C ABI enum together with its raw-discriminant decoding.
///
/// The variant list is the single source of truth for both the type and the
/// values accepted at the ABI boundary: appending a variant automatically
/// extends `from_raw`, so validation can never drift behind the enum the way
/// hand-maintained numeric bounds did when `SmartErase` was added.
macro_rules! snow_c_enum {
    (
        $(#[$enum_meta:meta])*
        $vis:vis enum $name:ident {
            $( $(#[$variant_meta:meta])* $variant:ident = $discriminant:literal ),+ $(,)?
        }
    ) => {
        $(#[$enum_meta])*
        #[repr(C)]
        $vis enum $name {
            $( $(#[$variant_meta])* $variant = $discriminant, )+
        }

        impl $crate::abi::raw_enum::SnowRawEnum for $name {
            fn from_raw(raw: i32) -> Option<Self> {
                $(
                    if raw == $name::$variant as i32 {
                        return Some($name::$variant);
                    }
                )+
                None
            }
        }
    };
}

pub(crate) use snow_c_enum;

#[cfg(test)]
mod tests {
    use super::*;

    snow_c_enum! {
        #[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
        pub enum Sample {
            #[default]
            First = 0,
            Second = 1,
            Third = 2,
        }
    }

    snow_c_enum! {
        #[derive(Clone, Copy, Debug, PartialEq, Eq)]
        pub enum Sparse {
            Zero = 0,
            FortyTwo = 42,
        }
    }

    #[test]
    fn from_raw_accepts_exactly_the_declared_discriminants() {
        assert_eq!(Sample::from_raw(0), Some(Sample::First));
        assert_eq!(Sample::from_raw(1), Some(Sample::Second));
        assert_eq!(Sample::from_raw(2), Some(Sample::Third));
        assert_eq!(Sample::from_raw(-1), None);
        assert_eq!(Sample::from_raw(3), None);
        assert_eq!(Sample::from_raw(i32::MAX), None);
    }

    #[test]
    fn from_raw_rejects_gaps_between_sparse_discriminants() {
        assert_eq!(Sparse::from_raw(0), Some(Sparse::Zero));
        assert_eq!(Sparse::from_raw(42), Some(Sparse::FortyTwo));
        assert_eq!(Sparse::from_raw(1), None);
        assert_eq!(Sparse::from_raw(41), None);
    }
}
