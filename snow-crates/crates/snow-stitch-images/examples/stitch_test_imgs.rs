use std::{
    env,
    ffi::OsString,
    fs,
    path::{Path, PathBuf},
    process::ExitCode,
};

use anyhow::{Context, Result, bail};
use snow_stitch_images::{StitchOptions, stitch_files};

const DEFAULT_INPUT_DIRECTORY: &str = "test-imgs";
const DEFAULT_OUTPUT_DIRECTORY: &str = "artifacts/stitch-test-imgs";

#[derive(Debug, PartialEq, Eq)]
struct ImageGroup {
    directory: PathBuf,
    images: Vec<PathBuf>,
}

fn usage() -> &'static str {
    "Usage: cargo run --example stitch_test_imgs -- [INPUT_DIRECTORY] [OUTPUT_DIRECTORY]"
}

fn is_supported_image(path: &Path) -> bool {
    matches!(
        path.extension()
            .and_then(|extension| extension.to_str())
            .map(str::to_ascii_lowercase)
            .as_deref(),
        Some("png" | "jpg" | "jpeg" | "bmp" | "gif" | "ico" | "tif" | "tiff" | "webp")
    )
}

fn read_directory(path: &Path) -> Result<Vec<fs::DirEntry>> {
    let entries = fs::read_dir(path)
        .with_context(|| format!("could not read input directory {}", path.display()))?
        .collect::<std::io::Result<Vec<_>>>()
        .with_context(|| format!("could not enumerate input directory {}", path.display()))?;
    Ok(entries)
}

fn sort_by_file_name(paths: &mut [PathBuf]) {
    paths.sort_by(|left, right| left.file_name().cmp(&right.file_name()));
}

fn find_image_groups(root: &Path) -> Result<Vec<ImageGroup>> {
    if !root.is_dir() {
        bail!("input directory does not exist: {}", root.display());
    }

    let mut directories = vec![root.to_path_buf()];
    let mut groups = Vec::new();
    while let Some(directory) = directories.pop() {
        let mut child_directories = Vec::new();
        let mut images = Vec::new();
        for entry in read_directory(&directory)? {
            let path = entry.path();
            let file_type = entry
                .file_type()
                .with_context(|| format!("could not inspect {}", path.display()))?;
            if file_type.is_dir() {
                child_directories.push(path);
            } else if file_type.is_file() && is_supported_image(&path) {
                images.push(path);
            }
        }

        sort_by_file_name(&mut images);
        if !images.is_empty() {
            groups.push(ImageGroup { directory, images });
        }

        sort_by_file_name(&mut child_directories);
        directories.extend(child_directories.into_iter().rev());
    }

    if groups.is_empty() {
        bail!("no supported image files found below {}", root.display());
    }
    Ok(groups)
}

fn output_path(output_root: &Path, input_root: &Path, group: &ImageGroup) -> Result<PathBuf> {
    let relative_directory = group.directory.strip_prefix(input_root).with_context(|| {
        format!(
            "image directory {} is not below input directory {}",
            group.directory.display(),
            input_root.display()
        )
    })?;
    if relative_directory.as_os_str().is_empty() {
        return Ok(output_root.join("stitched.png"));
    }
    let mut output = output_root.join(relative_directory);
    let directory_name = output
        .file_name()
        .context("image group directory unexpectedly has no name")?;
    let mut output_name = directory_name.to_os_string();
    output_name.push(".png");
    output.set_file_name(output_name);
    Ok(output)
}

fn parse_arguments(arguments: impl IntoIterator<Item = OsString>) -> Result<(PathBuf, PathBuf)> {
    let arguments = arguments.into_iter().collect::<Vec<_>>();
    if arguments.len() > 2 {
        bail!("too many arguments\n\n{}", usage());
    }
    let input = arguments
        .first()
        .map(PathBuf::from)
        .unwrap_or_else(|| PathBuf::from(DEFAULT_INPUT_DIRECTORY));
    let output = arguments
        .get(1)
        .map(PathBuf::from)
        .unwrap_or_else(|| PathBuf::from(DEFAULT_OUTPUT_DIRECTORY));
    Ok((input, output))
}

fn run() -> Result<()> {
    let (input_root, output_root) = parse_arguments(env::args_os().skip(1))?;
    for group in find_image_groups(&input_root)? {
        let output = output_path(&output_root, &input_root, &group)?;
        let parent = output
            .parent()
            .context("output path unexpectedly has no parent directory")?;
        fs::create_dir_all(parent)
            .with_context(|| format!("could not create output directory {}", parent.display()))?;

        let result = stitch_files(&group.images, StitchOptions::default()).with_context(|| {
            format!(
                "could not stitch {} image(s) from {}",
                group.images.len(),
                group.directory.display()
            )
        })?;
        result
            .image
            .encode(&output)
            .with_context(|| format!("could not write stitched image {}", output.display()))?;
        println!(
            "stitched {} image(s): {}",
            group.images.len(),
            output.display()
        );
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
    use tempfile::tempdir;

    #[test]
    fn discovers_groups_recursively_and_sorts_images() {
        let temporary = tempdir().unwrap();
        let root = temporary.path();
        fs::create_dir(root.join("second")).unwrap();
        fs::create_dir(root.join("first")).unwrap();
        fs::write(root.join("first/002.png"), []).unwrap();
        fs::write(root.join("first/001.JPG"), []).unwrap();
        fs::write(root.join("second/image.webp"), []).unwrap();
        fs::write(root.join("second/notes.txt"), []).unwrap();

        let groups = find_image_groups(root).unwrap();

        assert_eq!(groups.len(), 2);
        assert_eq!(groups[0].directory, root.join("first"));
        assert_eq!(
            groups[0].images,
            vec![root.join("first/001.JPG"), root.join("first/002.png")]
        );
        assert_eq!(groups[1].directory, root.join("second"));
        assert_eq!(groups[1].images, vec![root.join("second/image.webp")]);
    }

    #[test]
    fn discovers_images_directly_in_the_input_directory() {
        let temporary = tempdir().unwrap();
        let root = temporary.path();
        fs::write(root.join("frame_000002.png"), []).unwrap();
        fs::write(root.join("frame_000001.png"), []).unwrap();

        let groups = find_image_groups(root).unwrap();

        assert_eq!(groups.len(), 1);
        assert_eq!(groups[0].directory, root);
        assert_eq!(
            groups[0].images,
            vec![root.join("frame_000001.png"), root.join("frame_000002.png")]
        );
        assert_eq!(
            output_path(Path::new("artifacts"), root, &groups[0]).unwrap(),
            PathBuf::from("artifacts/stitched.png")
        );
    }

    #[test]
    fn preserves_the_group_directory_in_its_output_path() {
        let input_root = Path::new("test-imgs");
        let group = ImageGroup {
            directory: PathBuf::from("test-imgs/scroll-1"),
            images: vec![],
        };

        assert_eq!(
            output_path(Path::new("artifacts"), input_root, &group).unwrap(),
            PathBuf::from("artifacts/scroll-1.png")
        );

        let dotted_group = ImageGroup {
            directory: PathBuf::from("test-imgs/scroll-2"),
            images: vec![],
        };
        assert_eq!(
            output_path(Path::new("artifacts"), input_root, &dotted_group).unwrap(),
            PathBuf::from("artifacts/scroll-2.png")
        );
    }

    #[test]
    fn parses_default_and_custom_directories() {
        assert_eq!(
            parse_arguments(Vec::<OsString>::new()).unwrap(),
            (
                PathBuf::from(DEFAULT_INPUT_DIRECTORY),
                PathBuf::from(DEFAULT_OUTPUT_DIRECTORY)
            )
        );
        assert_eq!(
            parse_arguments([OsString::from("input"), OsString::from("output")]).unwrap(),
            (PathBuf::from("input"), PathBuf::from("output"))
        );
    }
}
