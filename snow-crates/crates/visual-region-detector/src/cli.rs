//! Argument parsing without image or filesystem side effects.

use std::{error::Error, path::PathBuf};

#[derive(Debug, PartialEq, Eq)]
pub(crate) enum Command {
    Help,
    Version,
    Detect(Options),
}

#[derive(Debug, PartialEq, Eq)]
pub(crate) struct Options {
    pub(crate) source: PathBuf,
    pub(crate) out_json: Option<PathBuf>,
    pub(crate) preview: Option<PathBuf>,
    pub(crate) crops: Option<PathBuf>,
    pub(crate) no_preview: bool,
}

pub(crate) fn parse(args: impl IntoIterator<Item = String>) -> Result<Command, Box<dyn Error>> {
    let mut args = args.into_iter();
    let mut source = None;
    let mut out_json = None;
    let mut preview = None;
    let mut crops = None;
    let mut no_preview = false;
    while let Some(arg) = args.next() {
        match arg.as_str() {
            "--version" => {
                return Ok(Command::Version);
            }
            "-h" | "--help" => return Ok(Command::Help),
            "--out-json" | "--out-preview" | "--out-crops" => {
                let value = PathBuf::from(
                    args.next()
                        .ok_or_else(|| format!("{arg} requires a path"))?,
                );
                match arg.as_str() {
                    "--out-json" => out_json = Some(value),
                    "--out-preview" => preview = Some(value),
                    _ => crops = Some(value),
                }
            }
            "--no-preview" => no_preview = true,
            _ if arg.starts_with('-') => return Err(format!("unknown option: {arg}").into()),
            _ => {
                if source.is_some() {
                    return Err("expected exactly one source image".into());
                }
                source = Some(PathBuf::from(arg));
            }
        }
    }
    let source = source.ok_or("missing source image; use --help for usage")?;
    Ok(Command::Detect(Options {
        source,
        out_json,
        preview,
        crops,
        no_preview,
    }))
}

#[cfg(test)]
mod tests {
    use super::*;

    fn command(args: &[&str]) -> Result<Command, Box<dyn Error>> {
        parse(args.iter().map(|s| s.to_string()))
    }

    #[test]
    fn help_and_version_need_no_source() {
        assert_eq!(command(&["--help"]).unwrap(), Command::Help);
        assert_eq!(command(&["--version"]).unwrap(), Command::Version);
    }

    #[test]
    fn rejects_ambiguous_and_incomplete_commands() {
        for args in [
            vec![],
            vec!["a.png", "b.png"],
            vec!["--unknown"],
            vec!["a.png", "--out-json"],
            vec!["a.png", "--out-preview"],
            vec!["a.png", "--out-crops"],
        ] {
            assert!(command(&args).is_err(), "{args:?}");
        }
    }

    #[test]
    fn preserves_output_paths_and_preview_override() {
        let Command::Detect(options) = command(&[
            "source image.png",
            "--out-json",
            "out/data.json",
            "--out-preview",
            "out/preview.png",
            "--out-crops",
            "out/crops",
            "--no-preview",
        ])
        .unwrap() else {
            panic!("expected detection command")
        };
        assert_eq!(options.source, PathBuf::from("source image.png"));
        assert_eq!(options.out_json, Some(PathBuf::from("out/data.json")));
        assert_eq!(options.preview, Some(PathBuf::from("out/preview.png")));
        assert_eq!(options.crops, Some(PathBuf::from("out/crops")));
        assert!(options.no_preview);
    }
}
