//! Path helpers mirroring the Qt operations the previous implementation
//! relied on: `QDir::cleanPath`, case-insensitive comparison, and long-path
//! prefixes.

use std::path::{Path, PathBuf};

/// Lexical path normalization equivalent to `QDir::cleanPath`: separators
/// become '/', '.' segments collapse, '..' cancels the previous segment,
/// duplicate separators collapse, and drive/UNC roots are preserved.
pub fn clean_path(input: &str) -> String {
    let unified = input.replace('\\', "/");
    let (prefix, rest, absolute) = if let Some(tail) = unified.strip_prefix("//") {
        let mut parts = tail.splitn(3, '/');
        let server = parts.next().unwrap_or_default();
        let share = parts.next().unwrap_or_default();
        let remainder = parts.next().unwrap_or_default();
        (format!("//{server}/{share}"), remainder, true)
    } else if unified.len() >= 2 && unified.as_bytes()[1] == b':' {
        let prefix = unified[..2].to_owned();
        let rest = unified[2..].trim_start_matches('/');
        (prefix, rest, true)
    } else if let Some(tail) = unified.strip_prefix('/') {
        (String::new(), tail, true)
    } else {
        (String::new(), unified.as_str(), false)
    };
    let mut segments: Vec<&str> = Vec::new();
    for segment in rest.split('/') {
        match segment {
            "" | "." => {}
            ".." => {
                if let Some(last) = segments.last() {
                    if *last != ".." {
                        segments.pop();
                    }
                } else if !absolute {
                    segments.push("..");
                }
            }
            other => segments.push(other),
        }
    }
    let joined = segments.join("/");
    if absolute {
        format!("{prefix}/{joined}")
    } else if joined.is_empty() {
        ".".to_owned()
    } else {
        format!("{prefix}{joined}")
    }
}

/// `QFileInfo::absoluteFilePath` equivalent for already-anchored paths.
pub fn absolute_path(path: &Path) -> PathBuf {
    if path.is_absolute() {
        path.to_path_buf()
    } else {
        std::env::current_dir()
            .unwrap_or_else(|_| PathBuf::from("."))
            .join(path)
    }
}

/// QString::toCaseFolded equivalent for the Unicode CaseFolding entries that
/// expand beyond simple lowercasing. Install paths in practice are ASCII or
/// CJK (identity under folding); the full expansion table exists so task-name
/// identity cannot silently diverge from the app's registration.
pub fn case_fold(text: &str) -> String {
    let mut folded = String::with_capacity(text.len());
    for character in text.chars() {
        let expansions: &[&str] = match character {
            '\u{00DF}' => &["ss"],
            '\u{0130}' => &["i", "\u{0307}"],
            '\u{0149}' => &["\u{02BC}", "n"],
            '\u{01F0}' => &["j", "\u{030C}"],
            '\u{0390}' => &["\u{03B9}", "\u{0308}", "\u{0301}"],
            '\u{03B0}' => &["\u{03C5}", "\u{0308}", "\u{0301}"],
            '\u{0587}' => &["\u{0565}", "\u{0582}"],
            '\u{1E96}' => &["h", "\u{0331}"],
            '\u{1E97}' => &["t", "\u{0331}"],
            '\u{1E98}' => &["w"],
            '\u{1E99}' => &["y"],
            '\u{1E9A}' => &["a", "\u{02BE}"],
            '\u{1F50}' => &["\u{03C5}", "\u{0313}"],
            '\u{1F52}' => &["\u{03C5}", "\u{0313}", "\u{0300}"],
            '\u{1F54}' => &["\u{03C5}", "\u{0313}", "\u{0301}"],
            '\u{1F56}' => &["\u{03C5}", "\u{0313}", "\u{0342}"],
            '\u{1FB2}' => &["\u{1F70}", "\u{03B9}"],
            '\u{1FB3}' => &["\u{03B1}", "\u{03B9}"],
            '\u{1FB4}' => &["\u{03AC}", "\u{03B9}"],
            '\u{1FB7}' => &["\u{03B1}", "\u{0342}", "\u{03B9}"],
            '\u{1FC2}' => &["\u{1F72}", "\u{03B9}"],
            '\u{1FC3}' => &["\u{03B7}", "\u{03B9}"],
            '\u{1FC4}' => &["\u{03AE}", "\u{03B9}"],
            '\u{1FC7}' => &["\u{03B7}", "\u{0342}", "\u{03B9}"],
            '\u{1FF2}' => &["\u{1F7A}", "\u{03B9}"],
            '\u{1FF3}' => &["\u{03C9}", "\u{03B9}"],
            '\u{1FF4}' => &["\u{03CE}", "\u{03B9}"],
            '\u{1FF7}' => &["\u{03C9}", "\u{0342}", "\u{03B9}"],
            '\u{FB00}' => &["ff"],
            '\u{FB01}' => &["fi"],
            '\u{FB02}' => &["fl"],
            '\u{FB03}' => &["ffi"],
            '\u{FB04}' => &["ffl"],
            '\u{FB05}' | '\u{FB06}' => &["st"],
            '\u{FB13}' => &["\u{0574}", "\u{0576}"],
            '\u{FB14}' => &["\u{0574}", "\u{0565}"],
            '\u{FB15}' => &["\u{0574}", "\u{056B}"],
            '\u{FB16}' => &["\u{057E}", "\u{0576}"],
            '\u{FB17}' => &["\u{0574}", "\u{056D}"],
            _ => &[],
        };
        if !expansions.is_empty() {
            for expansion in expansions {
                folded.push_str(expansion);
            }
            continue;
        }
        // ᾀ-ᾏ/ᾐ-ᾟ/ᾠ-ᾯ fold to their base vowel followed by a iota adscript.
        if (0x1F80..=0x1FAF).contains(&u32::from(character)) {
            let value = u32::from(character);
            let block = (value - 0x1F80) / 0x10; // 0, 1, or 2
            let offset = (value - 0x1F80) % 8; // upper or lower half of the block
            let base = 0x1F00 + block * 0x20 + offset;
            if let Some(base) = char::from_u32(base) {
                folded.push(base);
                folded.push('\u{03B9}');
                continue;
            }
        }
        for lower in character.to_lowercase() {
            folded.push(lower);
        }
    }
    folded
}

/// Removes the `\\?\` verbatim prefix `std::fs::canonicalize` produces so
/// cleaned paths stay comparable with Qt-style display paths.
pub fn strip_verbatim(path: &std::path::Path) -> String {
    let text = path.to_string_lossy();
    if let Some(rest) = text.strip_prefix(r"\\?\UNC\") {
        format!(r"\\{rest}")
    } else if let Some(rest) = text.strip_prefix(r"\\?\") {
        rest.to_owned()
    } else {
        text.into_owned()
    }
}

/// Case-insensitive equality over cleaned '/'-style paths.
pub fn paths_ci_eq(left: &str, right: &str) -> bool {
    left.to_lowercase() == right.to_lowercase()
}

/// Case-insensitive `haystack.starts_with(prefix)` with a separator boundary,
/// mirroring `QString::startsWith(other + '/')` checks on cleaned paths.
pub fn path_ci_starts_with_dir(haystack: &str, dir: &str) -> bool {
    let (haystack, dir) = (haystack.to_lowercase(), dir.to_lowercase());
    haystack.starts_with(&dir)
        && (haystack.len() == dir.len() || haystack.as_bytes().get(dir.len()) == Some(&b'/'))
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn cleans_like_qdir() {
        assert_eq!(clean_path("C:\\a//b/./c"), "C:/a/b/c");
        assert_eq!(clean_path("C:/a/b/../c"), "C:/a/c");
        assert_eq!(clean_path("/a/../../b"), "/b");
        assert_eq!(clean_path("//server/share/x/../y"), "//server/share/y");
        assert_eq!(clean_path("a/b/"), "a/b");
        assert_eq!(clean_path("E:\\"), "E:/");
        assert_eq!(clean_path("./a"), "a");
    }

    #[test]
    fn case_fold_handles_expansions() {
        assert_eq!(case_fold("Fa\u{00DF}"), "fass");
        assert_eq!(case_fold("STRASSE"), "strasse");
        assert_eq!(case_fold("\u{1F88}"), "\u{1F00}\u{03B9}");
        assert_eq!(case_fold("ABC"), "abc");
        assert_eq!(case_fold("\u{FB01}n"), "fin");
    }

    #[test]
    fn ci_prefix_requires_separator_boundary() {
        assert!(path_ci_starts_with_dir("C:/a/b", "c:/A"));
        assert!(!path_ci_starts_with_dir("C:/ab", "c:/A"));
        assert!(path_ci_starts_with_dir("C:/a", "c:/a"));
    }
}
