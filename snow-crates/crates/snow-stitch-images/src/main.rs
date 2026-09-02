use std::{env, ffi::OsString, fs, path::PathBuf, process::ExitCode};

use anyhow::{Context, Result, bail};
use snow_stitch_images::{MotionEstimatorOptions, StitchAxis, StitchOptions, stitch_files};

const USAGE: &str = "Usage: snow-stitch-images [OPTIONS] <INPUT> <INPUT> [INPUT ...]\n\
\n\
Options:\n\
  -o, --output <PATH>              Output image (default: stitched.png)\n\
      --trace <PATH>               Write detailed trace JSON\n\
      --axis <DIRECTION>           Scroll direction: vertical or horizontal (default: vertical)\n\
      --max-motion-ratio <RATIO>   Maximum motion/view height (default: 0.6)\n\
      --min-confidence <VALUE>     Required confidence in [0, 1] (default: 0.65)\n\
      --tile-size <PIXELS>         Region tile size (default: 32)\n\
      --max-features <COUNT>       Selected motion-feature limit (default: 2500)\n\
      --learning-rate <VALUE>      Temporal learning rate in (0, 1] (default: 0.2)\n\
  -h, --help                       Show this help";

#[derive(Debug, PartialEq)]
struct CliOptions {
    inputs: Vec<PathBuf>,
    output: PathBuf,
    trace: Option<PathBuf>,
    axis: StitchAxis,
    estimator: MotionEstimatorOptions,
}

enum Command {
    Run(CliOptions),
    Help,
}

fn option_value(arguments: &mut impl Iterator<Item = OsString>, name: &str) -> Result<OsString> {
    arguments
        .next()
        .with_context(|| format!("{name} requires a value"))
}

fn parse_f32(value: &OsString, name: &str) -> Result<f32> {
    let text = value
        .to_str()
        .with_context(|| format!("{name} must be valid UTF-8"))?;
    text.parse::<f32>()
        .with_context(|| format!("invalid value for {name}: {text:?}"))
}

fn parse_u32(value: &OsString, name: &str) -> Result<u32> {
    let text = value
        .to_str()
        .with_context(|| format!("{name} must be valid UTF-8"))?;
    text.parse::<u32>()
        .with_context(|| format!("invalid value for {name}: {text:?}"))
}

fn parse_axis(value: &OsString) -> Result<StitchAxis> {
    match value.to_str() {
        Some("vertical") => Ok(StitchAxis::Vertical),
        Some("horizontal") => Ok(StitchAxis::Horizontal),
        Some(value) => {
            bail!("invalid value for --axis: {value:?}; expected vertical or horizontal")
        }
        None => bail!("--axis must be valid UTF-8"),
    }
}

fn parse_arguments(arguments: impl IntoIterator<Item = OsString>) -> Result<Command> {
    let mut arguments = arguments.into_iter();
    let mut output = PathBuf::from("stitched.png");
    let mut trace = None;
    let mut axis = StitchAxis::Vertical;
    let mut estimator = MotionEstimatorOptions::default();
    let mut inputs = Vec::new();
    let mut positional_only = false;

    while let Some(argument) = arguments.next() {
        if positional_only {
            inputs.push(PathBuf::from(argument));
            continue;
        }
        let text = argument.to_string_lossy();
        match text.as_ref() {
            "--" => positional_only = true,
            "-h" | "--help" => return Ok(Command::Help),
            "-o" | "--output" => {
                output = PathBuf::from(option_value(&mut arguments, "--output")?);
            }
            "--trace" => {
                trace = Some(PathBuf::from(option_value(&mut arguments, "--trace")?));
            }
            "--axis" => {
                axis = parse_axis(&option_value(&mut arguments, "--axis")?)?;
            }
            "--max-motion-ratio" => {
                estimator.max_motion_ratio = parse_f32(
                    &option_value(&mut arguments, "--max-motion-ratio")?,
                    "--max-motion-ratio",
                )?;
            }
            "--min-confidence" => {
                estimator.min_confidence = parse_f32(
                    &option_value(&mut arguments, "--min-confidence")?,
                    "--min-confidence",
                )?;
            }
            "--tile-size" => {
                estimator.tile_size =
                    parse_u32(&option_value(&mut arguments, "--tile-size")?, "--tile-size")?;
            }
            "--max-features" => {
                estimator.max_features = parse_u32(
                    &option_value(&mut arguments, "--max-features")?,
                    "--max-features",
                )?;
            }
            "--learning-rate" => {
                estimator.temporal_learning_rate = parse_f32(
                    &option_value(&mut arguments, "--learning-rate")?,
                    "--learning-rate",
                )?;
            }
            _ if text.starts_with("--output=") => {
                output = PathBuf::from(&text["--output=".len()..]);
            }
            _ if text.starts_with("--trace=") => {
                trace = Some(PathBuf::from(&text["--trace=".len()..]));
            }
            _ if text.starts_with("--axis=") => {
                axis = parse_axis(&OsString::from(&text["--axis=".len()..]))?;
            }
            _ if text.starts_with("--max-motion-ratio=") => {
                estimator.max_motion_ratio = parse_f32(
                    &OsString::from(&text["--max-motion-ratio=".len()..]),
                    "--max-motion-ratio",
                )?;
            }
            _ if text.starts_with("--min-confidence=") => {
                estimator.min_confidence = parse_f32(
                    &OsString::from(&text["--min-confidence=".len()..]),
                    "--min-confidence",
                )?;
            }
            _ if text.starts_with("--tile-size=") => {
                estimator.tile_size = parse_u32(
                    &OsString::from(&text["--tile-size=".len()..]),
                    "--tile-size",
                )?;
            }
            _ if text.starts_with("--max-features=") => {
                estimator.max_features = parse_u32(
                    &OsString::from(&text["--max-features=".len()..]),
                    "--max-features",
                )?;
            }
            _ if text.starts_with("--learning-rate=") => {
                estimator.temporal_learning_rate = parse_f32(
                    &OsString::from(&text["--learning-rate=".len()..]),
                    "--learning-rate",
                )?;
            }
            _ if text.starts_with('-') => bail!("unknown option {text:?}\n\n{USAGE}"),
            _ => inputs.push(PathBuf::from(argument)),
        }
    }

    if inputs.len() < 2 {
        bail!("at least two input images are required\n\n{USAGE}");
    }
    Ok(Command::Run(CliOptions {
        inputs,
        output,
        trace,
        axis,
        estimator,
    }))
}

fn run() -> Result<()> {
    let command = parse_arguments(env::args_os().skip(1))?;
    let Command::Run(cli) = command else {
        println!("{USAGE}");
        return Ok(());
    };
    let result = stitch_files(
        &cli.inputs,
        StitchOptions {
            axis: cli.axis,
            estimator: cli.estimator,
            record_decisions: cli.trace.is_some(),
        },
    )?;
    result.image.encode(&cli.output)?;
    if let Some(trace_path) = cli.trace {
        let json = serde_json::to_vec_pretty(&result.decisions)?;
        fs::write(&trace_path, json)
            .with_context(|| format!("could not write trace JSON {}", trace_path.display()))?;
    }
    Ok(())
}

fn main() -> ExitCode {
    match run() {
        Ok(()) => ExitCode::SUCCESS,
        Err(error) => {
            eprintln!("error: {error:#}");
            ExitCode::FAILURE
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    fn args(values: &[&str]) -> Vec<OsString> {
        values.iter().map(OsString::from).collect()
    }

    #[test]
    fn parses_all_options_and_inputs() {
        let Command::Run(parsed) = parse_arguments(args(&[
            "--output",
            "out.png",
            "--trace=trace.json",
            "--axis=horizontal",
            "--max-motion-ratio",
            "0.4",
            "--min-confidence=0.7",
            "--tile-size",
            "48",
            "--max-features=3000",
            "--learning-rate=0.1",
            "one.png",
            "two.png",
        ]))
        .unwrap() else {
            panic!("expected run command");
        };
        assert_eq!(parsed.output, PathBuf::from("out.png"));
        assert_eq!(parsed.trace, Some(PathBuf::from("trace.json")));
        assert_eq!(parsed.axis, StitchAxis::Horizontal);
        assert_eq!(parsed.estimator.max_motion_ratio, 0.4);
        assert_eq!(parsed.estimator.min_confidence, 0.7);
        assert_eq!(parsed.estimator.tile_size, 48);
        assert_eq!(parsed.estimator.max_features, 3000);
        assert_eq!(parsed.estimator.temporal_learning_rate, 0.1);
        assert_eq!(parsed.inputs.len(), 2);
    }

    #[test]
    fn requires_two_inputs() {
        assert!(parse_arguments(args(&["only.png"])).is_err());
    }

    #[test]
    fn supports_positional_separator() {
        let Command::Run(parsed) = parse_arguments(args(&["--", "-one.png", "-two.png"])).unwrap()
        else {
            panic!("expected run command");
        };
        assert_eq!(
            parsed.inputs,
            vec![PathBuf::from("-one.png"), PathBuf::from("-two.png")]
        );
    }
}
