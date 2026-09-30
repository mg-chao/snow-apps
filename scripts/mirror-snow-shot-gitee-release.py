"""Mirror an already published, locally signed GitHub release to Gitee."""

import argparse
import base64
import hashlib
import json
import os
from pathlib import Path
import re
import subprocess
import tempfile
from urllib.error import HTTPError
from urllib.request import urlopen


GITHUB_REPOSITORY = "mg-chao/snow-apps"
GITEE_REPOSITORY = "mg-chao/snow-apps"
GITEE_API = f"https://gitee.com/api/v5/repos/{GITEE_REPOSITORY}/releases"
GITEE_GIT = f"https://gitee.com/{GITEE_REPOSITORY}.git"
TAG = re.compile(r"^v(0|[1-9][0-9]*)\.(0|[1-9][0-9]*)\.(0|[1-9][0-9]*)"
                 r"(?:-([0-9A-Za-z-]+(?:\.[0-9A-Za-z-]+)*))?_snow-shot$")


def run(*args: str) -> str:
    return subprocess.check_output(args, text=True).strip()


def api_json(url: str):
    try:
        with urlopen(url, timeout=30) as response:
            return json.load(response)
    except HTTPError as error:
        if error.code == 404:
            return None
        raise


def checked_tag(tag: str) -> str:
    match = TAG.fullmatch(tag)
    if not match or (match[4] and any(part.isascii() and part.isdigit() and
                                      len(part) > 1 and part[0] == "0"
                                      for part in match[4].split("."))):
        raise ValueError(f"Invalid Snow Shot release tag: {tag}")
    return tag


def asset_url(file: dict, tag: str, name: str) -> str:
    if file.get("name") != name:
        raise ValueError(f"Unexpected Gitee attachment: {name}")
    url = file.get("browser_download_url", "")
    if url != f"https://gitee.com/{GITEE_REPOSITORY}/releases/download/{tag}/{name}":
        raise ValueError(f"Unsafe Gitee attachment URL: {name}")
    return url


def sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for block in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(block)
    return digest.hexdigest()


def git_with_gitee_auth(token: str, username: str, *args: str) -> str:
    header = base64.b64encode(f"{username}:{token}".encode()).decode()
    environment = dict(os.environ,
                       GIT_CONFIG_COUNT="1",
                       GIT_CONFIG_KEY_0="http.https://gitee.com/.extraheader",
                       GIT_CONFIG_VALUE_0=f"Authorization: Basic {header}")
    return subprocess.check_output(("git", *args), text=True, env=environment).strip()


def sync_tag(tag: str, commit: str, token: str, username: str) -> None:
    refs = git_with_gitee_auth(token, username, "ls-remote", "--tags", GITEE_GIT,
                               f"refs/tags/{tag}", f"refs/tags/{tag}^{{}}")
    hashes = {line.split()[0] for line in refs.splitlines() if line}
    if hashes:
        if commit not in hashes:
            raise ValueError("Gitee tag points to a different source commit")
        return
    git_with_gitee_auth(token, username, "push", GITEE_GIT, f"refs/tags/{tag}:refs/tags/{tag}")
    refs = git_with_gitee_auth(token, username, "ls-remote", "--tags", GITEE_GIT,
                               f"refs/tags/{tag}", f"refs/tags/{tag}^{{}}")
    if commit not in {line.split()[0] for line in refs.splitlines() if line}:
        raise ValueError("Gitee tag verification failed")


def post_form(url: str, fields: dict[str, str], token: str, file: Path | None = None):
    if not re.fullmatch(r"[A-Za-z0-9._-]+", token):
        raise ValueError("Invalid Gitee token syntax")
    command = ["curl", "--fail", "--progress-bar", "--show-error", "--max-time", "3600",
               "--request", "POST", "--config", "-"]
    for key, value in fields.items():
        command.extend(["--form-string", f"{key}={value}"])
    if file is not None:
        command.extend(["--form", f"file=@{file}"])
    command.append(url)
    config = f'form-string = "access_token={token}"\n'
    return json.loads(subprocess.check_output(command, input=config, text=True))


def attachments(release_id: int) -> list[dict]:
    files = api_json(f"{GITEE_API}/{release_id}/attach_files?per_page=100")
    if not isinstance(files, list) or len(files) >= 100:
        raise ValueError("Gitee attachment listing is incomplete")
    return files


def existing_release(tag: str):
    found = None
    for page in range(1, 11):
        releases = api_json(f"{GITEE_API}?per_page=100&page={page}")
        if not isinstance(releases, list) or len(releases) > 100:
            raise ValueError("Gitee release listing is invalid")
        for release in releases:
            if release.get("tag_name") == tag:
                if found is not None:
                    raise ValueError("Duplicate Gitee release tag")
                found = release
        if len(releases) < 100:
            return found
    raise ValueError("Gitee release listing exceeds ten pages")


def verify_attachment(file: dict, tag: str, name: str, expected: Path, directory: Path) -> None:
    url = asset_url(file, tag, name)
    output = directory / f"verify-{name}"
    run("curl", "--fail", "--silent", "--show-error", "--location", "--proto", "=https",
        "--proto-redir", "=https", "--max-time", "3600", "--output", str(output), url)
    if output.stat().st_size != expected.stat().st_size or sha256(output) != sha256(expected):
        raise ValueError(f"Gitee asset differs from GitHub: {name}")
    output.unlink()


def mirror(tag: str, token: str, username: str) -> None:
    checked_tag(tag)
    if not token:
        raise ValueError("Configure GITEE_TOKEN with release write access")
    release = json.loads(run("gh", "api", f"repos/{GITHUB_REPOSITORY}/releases/tags/{tag}"))
    if release.get("draft") or release.get("tag_name") != tag:
        raise ValueError("Expected a published GitHub release for the requested tag")
    source_names = [asset["name"] for asset in release.get("assets", [])]
    if len(source_names) != len(set(source_names)) or "latest-version.json" not in source_names:
        raise ValueError("GitHub release has missing or duplicate signed metadata")
    commit = run("git", "rev-list", "-n", "1", tag)
    if commit != run("git", "rev-parse", "HEAD"):
        raise ValueError("Workflow checkout does not match the release tag")
    sync_tag(tag, commit, token, username)
    remote = existing_release(tag)
    if remote is None:
        remote = post_form(GITEE_API, {
            "tag_name": tag,
            "name": release.get("name") or f"Snow Shot {tag}",
            "body": release.get("body") or "Snow Shot release",
            "prerelease": str(bool(release.get("prerelease"))).lower(),
            "target_commitish": commit,
        }, token)
    if remote.get("tag_name") != tag:
        raise ValueError("Gitee release tag differs")
    if remote.get("draft") is True or (isinstance(remote.get("prerelease"), bool) and
                                      remote["prerelease"] != bool(release.get("prerelease"))):
        raise ValueError("Gitee release publication state differs")
    release_id = remote.get("id")
    if not isinstance(release_id, int):
        raise ValueError("Gitee release has no id")
    with tempfile.TemporaryDirectory(prefix="snow-gitee-release-") as temporary:
        directory = Path(temporary)
        print(f"Downloading {len(source_names)} published GitHub assets", flush=True)
        run("gh", "release", "download", tag, "--repo", GITHUB_REPOSITORY,
            "--dir", str(directory))
        if {path.name for path in directory.iterdir()} != set(source_names):
            raise ValueError("Downloaded GitHub release assets differ from the published listing")
        for name in sorted(source_names, key=lambda item: item == "latest-version.json"):
            existing = [file for file in attachments(release_id) if file.get("name") == name]
            if len(existing) > 1:
                raise ValueError(f"Duplicate Gitee asset: {name}")
            if not existing:
                print(f"Uploading {name} ({(directory / name).stat().st_size} bytes)", flush=True)
                post_form(f"{GITEE_API}/{release_id}/attach_files", {}, token, directory / name)
                existing = [file for file in attachments(release_id) if file.get("name") == name]
            if len(existing) != 1:
                raise ValueError(f"Gitee upload did not produce exactly one asset: {name}")
            print(f"Verifying {name}", flush=True)
            verify_attachment(existing[0], tag, name, directory / name, directory)
            print(f"Verified {name}", flush=True)
    print(f"Mirrored and verified {tag} on Gitee")


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--tag", required=True)
    arguments = parser.parse_args()
    mirror(arguments.tag, os.environ.get("GITEE_TOKEN", ""),
           os.environ.get("GITEE_USERNAME", "mg-chao"))
