use std::{
    env, fs,
    path::{Path, PathBuf},
};

use clap::{Args, Parser, Subcommand, ValueEnum};
use rapid_ocr_rs::{
    EngineConfig, LangRec, LoadImage, OcrInput, OcrResult, ProviderPreference, RapidOcrEngine,
    RunOptions,
};

const CHECK_IMG_URL: &str = "https://www.modelscope.cn/models/RapidAI/RapidOCR/resolve/v3.1.0/resources/test_files/ch_en_num.jpg";
const CHECK_EXPECTED: &str = "姝ｅ搧淇冮攢";

fn main() {
    if let Err(err) = run_main() {
        eprintln!("error: {err}");
        std::process::exit(1);
    }
}

fn run_main() -> Result<(), Box<dyn std::error::Error>> {
    let cli = Cli::parse_from(normalize_legacy_args(env::args_os()));
    match cli.command {
        Commands::Run(args) => run_cmd(args),
        Commands::Config(args) => config_cmd(args),
        Commands::Check => check_cmd(),
    }
}

#[derive(Debug, Parser)]
#[command(
    name = "rapidocr",
    about = "Run PaddleOCR ONNX models",
    disable_help_subcommand = true
)]
struct Cli {
    #[command(subcommand)]
    command: Commands,
}

#[derive(Debug, Subcommand)]
enum Commands {
    Run(RunArgs),
    Config(ConfigArgs),
    Check,
}

#[derive(Debug, Args, Clone)]
struct RunArgs {
    #[arg(long = "img-path", alias = "img")]
    img_path: Option<String>,
    #[arg(value_name = "IMG_PATH", conflicts_with = "img_path")]
    img_path_positional: Option<String>,
    #[arg(long = "config")]
    config_path: Option<PathBuf>,
    #[arg(long, value_enum)]
    provider: Option<ProviderCli>,
    #[arg(long, requires = "provider")]
    device_id: Option<usize>,
    #[arg(long, conflicts_with = "allow_provider_fallback")]
    fail_if_provider_unavailable: bool,
    #[arg(long, conflicts_with = "fail_if_provider_unavailable")]
    allow_provider_fallback: bool,
    #[arg(long, value_parser = parse_f32_unit_interval)]
    text_score: Option<f32>,
    #[arg(long = "lang-type", alias = "lang", value_parser = parse_lang)]
    lang_type: Option<LangRec>,
    #[arg(long, value_parser = parse_bool)]
    use_det: Option<bool>,
    #[arg(long, value_parser = parse_bool)]
    use_cls: Option<bool>,
    #[arg(long, value_parser = parse_bool)]
    use_rec: Option<bool>,
    #[arg(long, alias = "word", conflicts_with = "no_return_word_box")]
    return_word_box: bool,
    #[arg(long, conflicts_with = "return_word_box")]
    no_return_word_box: bool,
    #[arg(long, conflicts_with = "no_return_single_char_box")]
    return_single_char_box: bool,
    #[arg(long, conflicts_with = "return_single_char_box")]
    no_return_single_char_box: bool,
    #[arg(long, value_parser = parse_f32_unit_interval)]
    box_thresh: Option<f32>,
    #[arg(long, value_parser = parse_positive_f32)]
    unclip_ratio: Option<f32>,
    #[arg(long, alias = "vis")]
    vis: bool,
    #[arg(long)]
    vis_word: bool,
    #[arg(long, default_value = ".")]
    vis_save_dir: PathBuf,
    #[arg(long, value_enum)]
    output_format: Option<OutputFormat>,
    #[arg(long, conflicts_with = "markdown")]
    json: bool,
    #[arg(long, conflicts_with = "json")]
    markdown: bool,
}

#[derive(Debug, Args)]
struct ConfigArgs {
    #[command(subcommand)]
    command: ConfigCommand,
}

#[derive(Debug, Subcommand)]
enum ConfigCommand {
    Init(ConfigInitArgs),
}

#[derive(Debug, Args)]
struct ConfigInitArgs {
    #[arg(long, default_value = "./default_rapidocr.yaml")]
    output: PathBuf,
}

#[derive(Debug, Clone, Copy, PartialEq, Eq, ValueEnum)]
enum ProviderCli {
    Auto,
    Cpu,
    Cuda,
    Directml,
    Cann,
}

#[derive(Debug, Clone, Copy, PartialEq, Eq, ValueEnum)]
enum OutputFormat {
    Summary,
    Json,
    Markdown,
}

fn run_cmd(cli: RunArgs) -> Result<(), Box<dyn std::error::Error>> {
    let img_path = cli
        .img_path
        .or(cli.img_path_positional)
        .ok_or("missing --img-path")?;

    if cli.device_id.is_some() && cli.provider.is_none() {
        return Err("--device-id requires --provider".into());
    }

    let mut cfg = if let Some(path) = &cli.config_path {
        EngineConfig::from_yaml_file(path)?
    } else {
        EngineConfig::default()
    };

    if let Some(lang) = cli.lang_type {
        cfg.rec.model.lang = lang;
    }
    if let Some(provider) = cli.provider {
        let preference = match provider {
            ProviderCli::Auto => ProviderPreference::Auto {
                device_id: cli.device_id.unwrap_or(0),
            },
            ProviderCli::Cpu => ProviderPreference::Cpu,
            ProviderCli::Cuda => ProviderPreference::Cuda {
                device_id: cli.device_id.unwrap_or(0),
            },
            ProviderCli::Directml => ProviderPreference::DirectMl {
                device_id: cli.device_id.unwrap_or(0),
            },
            ProviderCli::Cann => ProviderPreference::Cann {
                device_id: cli.device_id.unwrap_or(0),
            },
        };
        for runtime in [
            &mut cfg.det.runtime,
            &mut cfg.cls.runtime,
            &mut cfg.rec.runtime,
        ] {
            runtime.provider_preference = preference;
        }
    }

    let provider_strictness = if cli.fail_if_provider_unavailable {
        Some(true)
    } else if cli.allow_provider_fallback {
        Some(false)
    } else {
        None
    };
    if let Some(strict_provider) = provider_strictness {
        for runtime in [
            &mut cfg.det.runtime,
            &mut cfg.cls.runtime,
            &mut cfg.rec.runtime,
        ] {
            runtime.fail_if_provider_unavailable = strict_provider;
        }
    }

    let mut engine = RapidOcrEngine::new(cfg)?;
    let input = parse_input(&img_path);
    let run_opts = RunOptions {
        use_det: cli.use_det,
        use_cls: cli.use_cls,
        use_rec: cli.use_rec,
        return_word_box: resolve_flag_pair(cli.return_word_box, cli.no_return_word_box),
        return_single_char_box: resolve_flag_pair(
            cli.return_single_char_box,
            cli.no_return_single_char_box,
        ),
        text_score: cli.text_score,
        box_thresh: cli.box_thresh,
        unclip_ratio: cli.unclip_ratio,
    };

    let use_word_boxes = cli.vis_word || run_opts.return_word_box.unwrap_or(false);
    let out = engine.run(input.clone(), run_opts)?;
    let output_format = resolve_output_format(cli.output_format, cli.json, cli.markdown)?;
    match output_format {
        OutputFormat::Summary => print_result_summary(&out),
        OutputFormat::Json => {
            let doc = out.to_json()?;
            println!("{}", serde_json::to_string_pretty(&doc)?);
        }
        OutputFormat::Markdown => {
            println!("{}", out.to_markdown()?);
        }
    }

    let vis_enabled = cli.vis || cli.vis_word;
    if vis_enabled {
        let loader = LoadImage;
        let image = loader.load(input)?;
        if let Some(vis_img) = out.visualize(&image, use_word_boxes) {
            fs::create_dir_all(&cli.vis_save_dir)?;
            let stem = infer_stem(&img_path);
            let suffix = if use_word_boxes {
                "_vis_single.png"
            } else {
                "_vis.png"
            };
            let save_path = cli.vis_save_dir.join(format!("{stem}{suffix}"));
            vis_img.save(&save_path)?;
            println!("The vis result has saved in {}", save_path.display());
        } else {
            println!("No visualization generated for current output mode.");
        }
    }

    Ok(())
}

fn config_cmd(args: ConfigArgs) -> Result<(), Box<dyn std::error::Error>> {
    match args.command {
        ConfigCommand::Init(init) => {
            let content = serde_yaml::to_string(&EngineConfig::default())?;
            fs::write(&init.output, content)?;
            println!("The config file has saved in {}", init.output.display());
            Ok(())
        }
    }
}

fn check_cmd() -> Result<(), Box<dyn std::error::Error>> {
    let mut engine = RapidOcrEngine::new(EngineConfig::default())?;
    let out = engine.run(parse_input(CHECK_IMG_URL), RunOptions::default())?;
    let text = first_text(&out).ok_or("check failed: empty OCR output")?;
    if text != CHECK_EXPECTED {
        return Err(format!(
            "The installation is incorrect! expected `{CHECK_EXPECTED}`, got `{text}`"
        )
        .into());
    }
    println!("Success! rapidocr is installed correctly!");
    Ok(())
}

fn normalize_legacy_args<I, S>(args: I) -> Vec<std::ffi::OsString>
where
    I: IntoIterator<Item = S>,
    S: Into<std::ffi::OsString>,
{
    args.into_iter()
        .map(Into::into)
        .map(|arg| {
            if arg == "-img" {
                "--img-path".into()
            } else {
                arg
            }
        })
        .collect()
}

fn parse_input(value: &str) -> OcrInput {
    if value.starts_with("http://") || value.starts_with("https://") {
        OcrInput::Url(value.to_string())
    } else {
        OcrInput::Path(PathBuf::from(value))
    }
}

fn parse_bool(value: &str) -> Result<bool, String> {
    match value.to_ascii_lowercase().as_str() {
        "1" | "true" | "yes" => Ok(true),
        "0" | "false" | "no" => Ok(false),
        _ => Err(format!("invalid bool: `{value}`")),
    }
}

fn parse_f32_unit_interval(value: &str) -> Result<f32, String> {
    let parsed = value
        .parse::<f32>()
        .map_err(|_| format!("invalid float value `{value}`"))?;
    if !(0.0..=1.0).contains(&parsed) {
        return Err(format!("value must be in range [0, 1], got {parsed}"));
    }
    Ok(parsed)
}

fn parse_positive_f32(value: &str) -> Result<f32, String> {
    let parsed = value
        .parse::<f32>()
        .map_err(|_| format!("invalid float value `{value}`"))?;
    if parsed <= 0.0 {
        return Err(format!("value must be > 0, got {parsed}"));
    }
    Ok(parsed)
}

fn parse_lang(value: &str) -> Result<LangRec, String> {
    Ok(match value.to_ascii_lowercase().as_str() {
        "ch" => LangRec::Ch,
        "ch_doc" => LangRec::ChDoc,
        "en" => LangRec::En,
        "arabic" => LangRec::Arabic,
        "chinese_cht" => LangRec::ChineseCht,
        "cyrillic" => LangRec::Cyrillic,
        "devanagari" => LangRec::Devanagari,
        "japan" => LangRec::Japan,
        "korean" => LangRec::Korean,
        "ka" => LangRec::Ka,
        "latin" => LangRec::Latin,
        "ta" => LangRec::Ta,
        "te" => LangRec::Te,
        "eslav" => LangRec::Eslav,
        "th" => LangRec::Th,
        "el" => LangRec::El,
        _ => return Err(format!("unsupported --lang-type value `{value}`")),
    })
}

fn resolve_flag_pair(enable: bool, disable: bool) -> Option<bool> {
    if enable {
        Some(true)
    } else if disable {
        Some(false)
    } else {
        None
    }
}

fn resolve_output_format(
    explicit: Option<OutputFormat>,
    json: bool,
    markdown: bool,
) -> Result<OutputFormat, Box<dyn std::error::Error>> {
    let mut format = explicit.unwrap_or(OutputFormat::Summary);
    if json {
        if format != OutputFormat::Summary && format != OutputFormat::Json {
            return Err(
                "output format flags conflict; choose exactly one of summary/json/markdown".into(),
            );
        }
        format = OutputFormat::Json;
    }
    if markdown {
        if format != OutputFormat::Summary && format != OutputFormat::Markdown {
            return Err(
                "output format flags conflict; choose exactly one of summary/json/markdown".into(),
            );
        }
        format = OutputFormat::Markdown;
    }
    Ok(format)
}

fn first_text(out: &OcrResult) -> Option<&str> {
    match out {
        OcrResult::Full(v) => v.txts.first().map(String::as_str),
        OcrResult::Rec(v) => v.txts.first().map(String::as_str),
        _ => None,
    }
}

fn print_result_summary(out: &OcrResult) {
    match out {
        OcrResult::Empty => println!("No text detected."),
        OcrResult::Det(v) => {
            println!("det_boxes: {}", v.boxes.len());
            println!(
                "timing_ms(det): {:.3}",
                v.timings.det_ms.unwrap_or_default()
            );
            if let Some(e2e_ms) = v.timings.e2e_ms {
                println!("timing_ms(e2e): {:.3}", e2e_ms);
            }
        }
        OcrResult::Cls(v) => {
            println!("cls_count: {}", v.cls_res.len());
            for (idx, (label, score)) in v.cls_res.iter().enumerate() {
                println!("{idx}: {label} ({score:.5})");
            }
            println!(
                "timing_ms(cls): {:.3}",
                v.timings.cls_ms.unwrap_or_default()
            );
            if let Some(e2e_ms) = v.timings.e2e_ms {
                println!("timing_ms(e2e): {:.3}", e2e_ms);
            }
        }
        OcrResult::Rec(v) => {
            println!("line_count: {}", v.txts.len());
            for (idx, (text, score)) in v.txts.iter().zip(v.scores.iter()).enumerate() {
                println!("{idx}: '{text}' ({score:.5})");
            }
            println!(
                "timing_ms(rec): {:.3}",
                v.timings.rec_ms.unwrap_or_default()
            );
            if let Some(e2e_ms) = v.timings.e2e_ms {
                println!("timing_ms(e2e): {:.3}", e2e_ms);
            }
        }
        OcrResult::Full(v) => {
            println!("line_count: {}", v.txts.len());
            println!("det_boxes: {}", v.boxes.len());
            for (idx, (text, score)) in v.txts.iter().zip(v.scores.iter()).enumerate() {
                println!("{idx}: '{text}' ({score:.5})");
            }
            println!(
                "timing_ms(det/cls/rec/total): {:.3}/{:.3}/{:.3}/{:.3}",
                v.timings.det_ms.unwrap_or_default(),
                v.timings.cls_ms.unwrap_or_default(),
                v.timings.rec_ms.unwrap_or_default(),
                v.timings.total_ms
            );
            if let Some(e2e_ms) = v.timings.e2e_ms {
                println!("timing_ms(e2e): {:.3}", e2e_ms);
            }
        }
    }
}

fn infer_stem(img_path: &str) -> String {
    if img_path.starts_with("http://") || img_path.starts_with("https://") {
        let last = img_path.rsplit('/').next().unwrap_or("rapidocr");
        Path::new(last)
            .file_stem()
            .and_then(|s| s.to_str())
            .unwrap_or("rapidocr")
            .to_string()
    } else {
        Path::new(img_path)
            .file_stem()
            .and_then(|s| s.to_str())
            .unwrap_or("rapidocr")
            .to_string()
    }
}

#[cfg(test)]
mod tests {
    use super::{Cli, Commands, ProviderCli, normalize_legacy_args};
    use clap::Parser;

    fn parse_cli(input: &[&str]) -> Result<Cli, clap::Error> {
        let mut args = vec!["rapidocr".to_string()];
        args.extend(input.iter().map(|v| (*v).to_string()));
        Cli::try_parse_from(normalize_legacy_args(args))
    }

    #[test]
    fn parse_run_cli_provider_and_device_id() {
        let cli = parse_cli(&[
            "run",
            "--img-path",
            "test.png",
            "--provider",
            "directml",
            "--device-id",
            "2",
        ])
        .expect("cli parse should pass");
        let Commands::Run(run) = cli.command else {
            panic!("expected run command");
        };
        assert_eq!(run.provider, Some(ProviderCli::Directml));
        assert_eq!(run.device_id, Some(2));
    }

    #[test]
    fn parse_run_cli_rejects_device_id_without_provider() {
        let err = parse_cli(&["run", "--img-path", "test.png", "--device-id", "1"])
            .expect_err("must reject dangling --device-id");
        assert!(err.to_string().contains("--device-id"));
    }

    #[test]
    fn parse_run_cli_provider_strictness_toggle() {
        let strict = parse_cli(&[
            "run",
            "--img-path",
            "test.png",
            "--fail-if-provider-unavailable",
        ])
        .expect("cli parse should pass");
        let Commands::Run(run) = strict.command else {
            panic!("expected run command");
        };
        assert!(run.fail_if_provider_unavailable);

        let fallback = parse_cli(&["run", "--img-path", "test.png", "--allow-provider-fallback"])
            .expect("cli parse should pass");
        let Commands::Run(run) = fallback.command else {
            panic!("expected run command");
        };
        assert!(run.allow_provider_fallback);
    }

    #[test]
    fn parse_run_cli_rejects_removed_config_format_flag() {
        let err = parse_cli(&[
            "run",
            "--img-path",
            "test.png",
            "--config-format",
            "rapidocr",
        ])
        .expect_err("removed flag should be rejected");
        assert!(
            err.to_string()
                .contains("unexpected argument '--config-format'")
        );
    }

    #[test]
    fn parse_run_cli_accepts_legacy_single_dash_img_alias() {
        let cli = parse_cli(&["run", "-img", "test.png"]).expect("legacy alias should parse");
        let Commands::Run(run) = cli.command else {
            panic!("expected run command");
        };
        assert_eq!(run.img_path.as_deref(), Some("test.png"));
    }
}
