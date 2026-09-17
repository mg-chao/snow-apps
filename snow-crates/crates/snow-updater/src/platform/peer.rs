//! Pipe-peer authentication: the connecting process must resolve to the
//! expected executable path, or to a byte-identical verified copy.

use super::pipes::PipeStream;
use crate::crypto::sha256_file;
use crate::paths::{clean_path, paths_ci_eq};
use std::path::Path;

/// Port of `verifyLocalPeer`. `server_peer` checks the server's process id
/// (used by client sockets); otherwise the client's.
pub fn verify_local_peer(
    stream: &PipeStream,
    server_peer: bool,
    expected_executable: &Path,
    allow_verified_copy: bool,
    expected_pid: u32,
    expected_digest: Option<&str>,
) -> bool {
    let pid = if server_peer {
        stream.server_process_id()
    } else {
        stream.client_process_id()
    };
    let Some(pid) = pid else { return false };
    if expected_pid != 0 && pid != expected_pid {
        return false;
    }
    let Some(path) = super::processx::process_image(pid) else {
        return false;
    };
    // An elevated worker may belong to the alternate administrator used for
    // UAC; executable validation is required in either direction.
    let actual = clean_path(&path.to_string_lossy());
    let expected = clean_path(&expected_executable.to_string_lossy());
    if paths_ci_eq(&actual, &expected) {
        return true;
    }
    if !allow_verified_copy {
        return false;
    }
    let Ok(actual_digest) = sha256_file(&path) else {
        return false;
    };
    match expected_digest {
        Some(digest) => actual_digest == digest,
        None => sha256_file(expected_executable).is_ok_and(|digest| digest == actual_digest),
    }
}
