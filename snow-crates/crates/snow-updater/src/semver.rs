//! Strict SemVer 2.0.0 subset matching `updatecontract.cpp`: no leading
//! zeros, length capped at 128 characters, build metadata ignored for
//! ordering, and numeric identifiers compared by value.

use crate::errors::{Error, Result, require};

const MAX_VERSION_LENGTH: usize = 128;

struct Version<'a> {
    major: &'a str,
    minor: &'a str,
    patch: &'a str,
    prerelease: Option<&'a str>,
}

fn numeric_identifier(text: &str) -> bool {
    let mut chars = text.chars();
    match chars.next() {
        Some('0') => chars.next().is_none(),
        Some(c) if c.is_ascii_digit() => chars.all(|c| c.is_ascii_digit()),
        _ => false,
    }
}

fn alphanumeric_identifier(text: &str) -> bool {
    // Either a plain numeric identifier without leading zeros, or an
    // identifier containing at least one letter or hyphen (which may start
    // with a zero, matching the reference pattern).
    if text.is_empty() || !text.chars().all(|c| c.is_ascii_alphanumeric() || c == '-') {
        return false;
    }
    numeric_identifier(text) || text.chars().any(|c| c.is_ascii_alphabetic() || c == '-')
}

fn build_metadata(text: &str) -> bool {
    !text.is_empty()
        && text.split('.').all(|part| {
            !part.is_empty() && part.chars().all(|c| c.is_ascii_alphanumeric() || c == '-')
        })
}

fn split_version(version: &str) -> Result<Version<'_>> {
    require(
        version.len() <= MAX_VERSION_LENGTH,
        "Invalid semantic version",
    )?;
    let (core_and_pre, _build) = match version.split_once('+') {
        Some((left, right)) => {
            require(build_metadata(right), "Invalid semantic version")?;
            (left, Some(right))
        }
        None => (version, None),
    };
    let (core, prerelease) = match core_and_pre.split_once('-') {
        Some((left, right)) => {
            // A trailing '-' yields an empty prerelease, which is invalid.
            require(
                right.split('.').all(alphanumeric_identifier),
                "Invalid semantic version",
            )?;
            (left, Some(right))
        }
        None => (core_and_pre, None),
    };
    let numbers: Vec<&str> = core.split('.').collect();
    require(numbers.len() == 3, "Invalid semantic version")?;
    for number in &numbers {
        require(numeric_identifier(number), "Invalid semantic version")?;
    }
    Ok(Version {
        major: numbers[0],
        minor: numbers[1],
        patch: numbers[2],
        prerelease,
    })
}

fn numeric_compare(a: &str, b: &str) -> std::cmp::Ordering {
    a.len().cmp(&b.len()).then_with(|| a.cmp(b))
}

fn is_numeric(value: &str) -> bool {
    !value.is_empty() && value.chars().all(|c| c.is_ascii_digit())
}

/// Compares two versions; fails with the catalog diagnostic when either is
/// malformed. Returns the same convention as `str::cmp`.
pub fn compare_versions(first: &str, second: &str) -> Result<std::cmp::Ordering> {
    let a = split_version(first).map_err(|_| Error::fixed("Invalid semantic version"))?;
    let b = split_version(second).map_err(|_| Error::fixed("Invalid semantic version"))?;
    for (x, y) in [
        (&a.major, &b.major),
        (&a.minor, &b.minor),
        (&a.patch, &b.patch),
    ] {
        let result = numeric_compare(x, y);
        if result != std::cmp::Ordering::Equal {
            return Ok(result);
        }
    }
    let (ap, bp) = (a.prerelease, b.prerelease);
    let ordering = match (ap, bp) {
        (None, None) => std::cmp::Ordering::Equal,
        (None, Some(_)) => std::cmp::Ordering::Greater,
        (Some(_), None) => std::cmp::Ordering::Less,
        (Some(x), Some(y)) => {
            let xp: Vec<&str> = x.split('.').collect();
            let yp: Vec<&str> = y.split('.').collect();
            for (xi, yi) in xp.iter().zip(yp.iter()) {
                let (xn, yn) = (is_numeric(xi), is_numeric(yi));
                let result = match (xn, yn) {
                    (true, true) => numeric_compare(xi, yi),
                    (true, false) => std::cmp::Ordering::Less,
                    (false, true) => std::cmp::Ordering::Greater,
                    (false, false) => xi.cmp(yi),
                };
                if result != std::cmp::Ordering::Equal {
                    return Ok(result);
                }
            }
            xp.len().cmp(&yp.len())
        }
    };
    Ok(ordering)
}

/// Validates a version without comparing it against anything.
pub fn validate_version(version: &str) -> Result<()> {
    split_version(version).map(|_| ())
}

#[cfg(test)]
mod tests {
    use super::*;
    use std::cmp::Ordering;

    fn cmp(a: &str, b: &str) -> Ordering {
        compare_versions(a, b).expect("valid versions")
    }

    #[test]
    fn semver_precedence() {
        let ordered = [
            "1.0.0-alpha",
            "1.0.0-alpha.1",
            "1.0.0-alpha.beta",
            "1.0.0-beta.2",
            "1.0.0-beta.10",
            "1.0.0-rc.1",
            "1.0.0",
            "1.0.1",
        ];
        for pair in ordered.windows(2) {
            assert_eq!(
                cmp(pair[0], pair[1]),
                Ordering::Less,
                "{} < {}",
                pair[0],
                pair[1]
            );
        }
    }

    #[test]
    fn build_metadata_is_ignored() {
        assert_eq!(cmp("1.0.0+build.1", "1.0.0+build.2"), Ordering::Equal);
    }

    #[test]
    fn malformed_versions_are_rejected() {
        for value in [
            "1.0",
            "01.0.0",
            "1.0.0-beta.01",
            "1.0.0-",
            "1.0.0_foo",
            "",
            "1.0.0+",
        ] {
            assert!(
                compare_versions(value, "1.0.0").is_err(),
                "{value} must be rejected"
            );
        }
        let long = format!("1.0.0-{}", "a".repeat(200));
        assert!(compare_versions(&long, "1.0.0").is_err());
    }

    #[test]
    fn numeric_and_alphanumeric_prerelease_identifiers() {
        assert_eq!(cmp("1.0.0-2", "1.0.0-10"), Ordering::Less);
        assert_eq!(cmp("1.0.0-alpha", "1.0.0-beta"), Ordering::Less);
        assert_eq!(cmp("1.0.0-1", "1.0.0-alpha"), Ordering::Less);
        assert_eq!(cmp("1.0.0-alpha", "1.0.0-alpha.1"), Ordering::Less);
        assert_eq!(cmp("2.0.0", "10.0.0"), Ordering::Less);
    }
}
