//! Strict ZIP extraction against the signed inventory: every entry must be
//! expected, regular, and hash-identical to its descriptor.

use crate::contract::{UpdateFile, UpdatePackage, safe_relative_path, verify_file};
use crate::errors::{Error, Result, require};
use crate::platform::fsx;
use std::collections::HashMap;
use std::io::{Read, Write};
use std::path::Path;
use zip::ZipArchive;

const REGULAR_MODE: u32 = 0o100000;

pub fn extract(archive: &Path, destination: &Path, package: &UpdatePackage) -> Result<()> {
    let file =
        std::fs::File::open(archive).map_err(|_| Error::fixed("Could not open update archive"))?;
    let mut reader = ZipArchive::new(std::io::BufReader::new(file))
        .map_err(|_| Error::fixed("Could not open update archive"))?;
    let mut expected: HashMap<String, UpdateFile> = package
        .files
        .iter()
        .cloned()
        .map(|file| (file.path.clone(), file))
        .collect();
    for index in 0..reader.len() {
        let mut entry = reader
            .by_index(index)
            .map_err(|_| Error::fixed("Invalid update archive entry"))?;
        let name = entry.name().to_owned();
        let mode = entry.unix_mode().unwrap_or(0) & 0o170000;
        let descriptor = expected.get(&name).cloned();
        require(
            descriptor.is_some()
                && safe_relative_path(&name)
                && (mode == 0 || mode == REGULAR_MODE)
                && !entry.is_dir()
                && entry.size() == descriptor.as_ref().map_or(0, |file| file.size as u64),
            "Unexpected, duplicate, or unsafe update archive entry",
        )?;
        let descriptor = descriptor.expect("presence verified above");
        expected.remove(&name);
        let output = destination.join(&name);
        if let Some(parent) = output.parent() {
            fsx::create_dir_all(parent)
                .map_err(|_| Error::fixed("Could not create update staging directory"))?;
        }
        let mut file = std::fs::File::options()
            .write(true)
            .create_new(true)
            .open(&output)
            .map_err(|_| Error::fixed("Could not extract update archive entry"))?;
        let mut total: u64 = 0;
        let mut buffer = [0u8; 65536];
        loop {
            let count = entry
                .read(&mut buffer)
                .map_err(|_| Error::fixed("Corrupt update archive entry"))?;
            if count == 0 {
                break;
            }
            total += count as u64;
            if total > descriptor.size as u64 {
                return Err(Error::fixed("Could not write update archive entry"));
            }
            file.write_all(&buffer[..count])
                .map_err(|_| Error::fixed("Could not write update archive entry"))?;
        }
        drop(file);
        // The zip crate validates the CRC32 trailer at end-of-stream; a
        // mismatch surfaces through the final signed hash check below.
        verify_file(&output, descriptor.size, &descriptor.sha256)
            .map_err(|_| Error::fixed("Corrupt update archive checksum"))?;
    }
    require(expected.is_empty(), "Incomplete update archive")?;
    Ok(())
}
