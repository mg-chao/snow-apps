"""Publish audited local release files directly to Gitee, without GitHub downloads."""

import argparse
import base64
import hashlib
import importlib.util
import json
import os
from pathlib import Path
import re
import subprocess
import sys
import tempfile
import time
from urllib.error import HTTPError
from urllib.parse import urlencode
from urllib.request import urlopen


GITEE_REPOSITORY = "mg-chao/snow-apps"
GITEE_API = f"https://gitee.com/api/v5/repos/{GITEE_REPOSITORY}/releases"
GITEE_GIT = f"https://gitee.com/{GITEE_REPOSITORY}.git"
TAG = re.compile(r"^v(0|[1-9][0-9]*)\.(0|[1-9][0-9]*)\.(0|[1-9][0-9]*)"
                 r"(?:-([0-9A-Za-z-]+(?:\.[0-9A-Za-z-]+)*))?_snow-shot$")


def run(*args: str) -> str:
    return subprocess.check_output(args, text=True).strip()


def api_json(url: str, token: str = ""):
    if token:
        url += ("&" if "?" in url else "?") + urlencode({"access_token": token})
    # Retry only idempotent reads after temporary gateway/service failures.
    # Never log the request URL: authenticated Gitee requests carry a token.
    for attempt in range(4):
        try:
            with urlopen(url, timeout=30) as response:
                return json.load(response)
        except HTTPError as error:
            if error.code == 404:
                return None
            if error.code not in (502, 503, 504) or attempt == 3:
                raise
            print(f"Transient Gitee API read failure (HTTP {error.code}); retrying read",
                  flush=True)
            time.sleep(2 * (attempt + 1))


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


def attachments(release_id: int, token: str = "") -> list[dict]:
    files = api_json(f"{GITEE_API}/{release_id}/attach_files?per_page=100", token)
    if not isinstance(files, list) or len(files) >= 100:
        raise ValueError("Gitee attachment listing is incomplete")
    return files


def existing_release(tag: str, token: str = ""):
    found = None
    for page in range(1, 11):
        releases = api_json(f"{GITEE_API}?per_page=100&page={page}", token)
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
        raise ValueError(f"Gitee asset differs from audited local bytes: {name}")
    output.unlink()


def load_token() -> str:
    token = os.environ.get("GITEE_TOKEN", "")
    if not token and sys.platform == "win32":
        # An already-running Codex/PowerShell process does not inherit later changes
        # made in Windows Environment Variables. Read only this service's setting.
        import winreg
        for root, key in ((winreg.HKEY_CURRENT_USER, "Environment"),
                          (winreg.HKEY_LOCAL_MACHINE,
                           r"SYSTEM\CurrentControlSet\Control\Session Manager\Environment")):
            try:
                with winreg.OpenKey(root, key) as handle:
                    token = winreg.QueryValueEx(handle, "GITEE_TOKEN")[0]
            except FileNotFoundError:
                continue
            if token:
                break
    if not token or not re.fullmatch(r"[A-Za-z0-9._-]+", token):
        raise ValueError("Configure a valid local GITEE_TOKEN with release write access")
    return token


def check_auth(token: str) -> None:
    try:
        account = api_json("https://gitee.com/api/v5/user", token)
    except HTTPError as error:
        raise ValueError(f"Gitee API authentication failed (HTTP {error.code})") from None
    if not isinstance(account, dict) or not account.get("login"):
        raise ValueError("Gitee API authentication returned no account")
    print(f"Authenticated Gitee account: {account['login']}", flush=True)


def local_assets(release: dict) -> dict[str, Path]:
    if release.get("schema") != 1:
        raise ValueError("Unsupported local release manifest schema")
    checked_tag(release.get("tag", ""))
    if not re.fullmatch(r"[a-f0-9]{40}", release.get("sourceCommit", "")):
        raise ValueError("Invalid release source commit")
    if (not isinstance(release.get("title"), str) or not release["title"].strip() or
            not isinstance(release.get("body"), str) or not release["body"].strip()):
        raise ValueError("Local release title and notes are required")
    assets = {}
    names = set()
    if not isinstance(release.get("assets"), list):
        raise ValueError("Local release assets must be a list")
    for descriptor in release["assets"]:
        name = descriptor["name"]
        if not re.fullmatch(r"[A-Za-z0-9][A-Za-z0-9._-]*", name) or name.lower() in names:
            raise ValueError("Unsafe or duplicate local release asset name")
        path = Path(descriptor["path"])
        if (type(descriptor.get("size")) is not int or descriptor["size"] <= 0 or
                not re.fullmatch(r"[a-f0-9]{64}", descriptor.get("sha256", ""))):
            raise ValueError(f"Invalid local release asset digest: {name}")
        if not path.is_absolute() or path.name != name or path.is_symlink() or not path.is_file():
            raise ValueError(f"Invalid local release asset path: {name}")
        check_local_asset(descriptor, path)
        names.add(name.lower())
        assets[name] = path
    if "latest-version.json" not in assets:
        raise ValueError("Local release is missing signed update metadata")
    return assets


def check_local_asset(descriptor: dict, path: Path) -> None:
    if path.stat().st_size != descriptor["size"] or sha256(path) != descriptor["sha256"]:
        raise ValueError(f"Local release asset differs from its audit: {descriptor['name']}")


def prepare_homebrew(release: dict, assets: dict[str, Path], output: Path) -> None:
    """Reproduce the distribution archive from local audited bytes and tagged source."""
    if not any(name.endswith("-macos-arm64.dmg") for name in assets):
        return
    spec = importlib.util.spec_from_file_location(
        "snow_homebrew", Path(__file__).with_name("snow-shot-homebrew.py"))
    homebrew = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(homebrew)
    with tempfile.TemporaryDirectory(prefix="snow-homebrew-local-") as temporary:
        source = Path(temporary)
        for name in ("CMakeLists.txt", "scripts/install-snow-shot-macos.sh",
                     "scripts/prepare-snow-shot-homebrew.sh"):
            path = source / name
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_bytes(subprocess.check_output(("git", "show", f"{release['tag']}:{name}")))
        metadata = {"tag_name": release["tag"], "draft": False,
                    "prerelease": "-" in release["tag"][1:-len("_snow-shot")],
                    "assets": [{"name": name, "digest": "sha256:" + sha256(path)}
                               for name, path in assets.items()]}
        dmg = next(path for name, path in assets.items() if name.endswith("-macos-arm64.dmg"))
        archive, _ = homebrew.package(metadata, source, dmg.parent, output,
                                      source / "absent-cask.rb")
    descriptor = {"name": archive.name, "path": str(archive.resolve()),
                  "size": archive.stat().st_size, "sha256": sha256(archive)}
    old = next((item for item in release["assets"] if item["name"] == archive.name), None)
    if old is not None:
        if old["size"] != descriptor["size"] or old["sha256"] != descriptor["sha256"]:
            raise ValueError("Existing local Homebrew archive differs from tagged source")
    else:
        release["assets"].append(descriptor)


def verify_local_release(release: dict, assets: dict[str, Path], auditor: Path) -> None:
    tag = release["tag"]
    version = tag[1:-len("_snow-shot")]
    envelope = json.loads(assets["latest-version.json"].read_text(encoding="utf-8-sig"))
    payload = json.loads(base64.b64decode(envelope["payload"], validate=True))
    if payload.get("version") != version:
        raise ValueError("Local signed metadata and release tag versions differ")
    expected = {("online", "installer"), ("online", "update"),
                ("offline", "installer"), ("offline", "update"), ("portable", "portable")}
    packages = payload.get("packages", [])
    if len(packages) != 5 or {(p["variant"], p["kind"]) for p in packages} != expected:
        raise ValueError("Signed metadata must describe all five Windows packages")
    for package in packages:
        suffix = ".exe" if package["kind"] == "installer" else (
            "-update.zip" if package["kind"] == "update" else ".zip")
        name = f"snow-shot-{version}-windows-x64-{package['variant']}{suffix}"
        if package["path"] != f"setup/snow-shot_windows-x64-{package['variant']}{suffix}":
            raise ValueError("Unexpected signed package path")
        if (name not in assets or assets[name].stat().st_size != package["size"] or
                sha256(assets[name]) != package["sha256"]):
            raise ValueError(f"Local Windows package differs from signed metadata: {name}")
    for name, path in assets.items():
        if name.endswith(".dmg"):
            checksum = assets.get(name + ".sha256")
            if checksum is None or checksum.read_text().split()[0].lower() != sha256(path):
                raise ValueError(f"Local macOS package checksum differs: {name}")
    if not auditor.is_file():
        raise ValueError("The compiled release auditor is required")
    run(str(auditor), "--verify-release", "--manifest", str(assets["latest-version.json"]))
    commit = run("git", "rev-list", "-n", "1", tag)
    if commit != release["sourceCommit"]:
        raise ValueError("Local release tag differs from audited source commit")


def publish(release: dict, assets: dict[str, Path], token: str, username: str,
            verify_only: bool = False) -> None:
    tag = release["tag"]
    prerelease = "-" in tag[1:-len("_snow-shot")]
    if not verify_only:
        if not token:
            raise ValueError("Configure GITEE_TOKEN with release write access")
        sync_tag(tag, release["sourceCommit"], token, username)
    remote = existing_release(tag, token)
    if remote is None:
        if verify_only:
            raise ValueError("Expected a published Gitee release")
        remote = post_form(GITEE_API, {
            "tag_name": tag,
            "name": release["title"], "body": release["body"],
            "prerelease": str(prerelease).lower(),
            "target_commitish": release["sourceCommit"],
        }, token)
    if remote.get("tag_name") != tag:
        raise ValueError("Gitee release tag differs")
    if remote.get("draft") is True or (isinstance(remote.get("prerelease"), bool) and
                                      remote["prerelease"] != prerelease):
        raise ValueError("Gitee release publication state differs")
    if remote.get("name") != release["title"] or remote.get("body", "").strip() != release["body"].strip():
        raise ValueError("Published Gitee title or notes differ from the local release")
    release_id = remote.get("id")
    if not isinstance(release_id, int):
        raise ValueError("Gitee release has no id")
    with tempfile.TemporaryDirectory(prefix="snow-gitee-release-") as temporary:
        directory = Path(temporary)
        files = attachments(release_id, token)
        names = [file.get("name") for file in files]
        if len(names) != len(set(names)) or set(names) - set(assets):
            raise ValueError("Gitee has duplicate or unexpected release assets")
        # Check all published bytes before making any new upload. A retry may fill
        # missing files, but cannot silently replace an existing version's bytes.
        for file in files:
            name = file["name"]
            print(f"Verifying existing {name}", flush=True)
            verify_attachment(file, tag, name, assets[name], directory)
        if verify_only and set(names) != set(assets):
            raise ValueError("Published Gitee release is missing local assets")
        descriptors = {item["name"]: item for item in release["assets"]}
        for name in sorted(set(assets) - set(names),
                           key=lambda item: (item == "latest-version.json", item)):
            if verify_only:
                raise ValueError("Verification must not upload files")
            check_local_asset(descriptors[name], assets[name])
            print(f"Uploading local {name} ({assets[name].stat().st_size} bytes)", flush=True)
            post_form(f"{GITEE_API}/{release_id}/attach_files", {}, token, assets[name])
            existing = [file for file in attachments(release_id, token) if file.get("name") == name]
            if len(existing) != 1:
                raise ValueError(f"Gitee upload did not produce exactly one asset: {name}")
            print(f"Verifying {name}", flush=True)
            verify_attachment(existing[0], tag, name, assets[name], directory)
            print(f"Verified {name}", flush=True)
        final_files = attachments(release_id, token)
        if (len(final_files) != len(assets) or
                {file.get("name") for file in final_files} != set(assets)):
            raise ValueError("Final Gitee asset listing differs from local release")
        local_assets(release)
    print(f"Published and verified local release {tag} on Gitee")


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--manifest", type=Path)
    parser.add_argument("--auditor", type=Path)
    parser.add_argument("--check-auth", action="store_true")
    parser.add_argument("--verify-only", action="store_true")
    parser.add_argument("--prepare-homebrew", action="store_true",
                        help="Prepare local distribution archive and manifest without publishing")
    arguments = parser.parse_args()
    try:
        if arguments.check_auth:
            check_auth(load_token())
        else:
            if not arguments.manifest or not arguments.auditor:
                raise ValueError("--manifest and --auditor are required")
            release = json.loads(arguments.manifest.read_text(encoding="utf-8-sig"))
            assets = local_assets(release)
            verify_local_release(release, assets, arguments.auditor)
            if arguments.prepare_homebrew:
                prepare_homebrew(release, assets, arguments.manifest.parent / "homebrew-local")
                arguments.manifest.write_text(json.dumps(release, ensure_ascii=False, indent=2) + "\n",
                                               encoding="utf-8")
            else:
                token = ""
                if arguments.verify_only:
                    try:
                        token = load_token()
                    except ValueError:
                        pass  # Public verification remains available without credentials.
                else:
                    token = load_token()
                if token:
                    check_auth(token)
                publish(release, assets, token, os.environ.get("GITEE_USERNAME", "mg-chao"),
                        arguments.verify_only)
    except (ValueError, OSError, KeyError, subprocess.CalledProcessError) as error:
        parser.exit(1, f"Gitee publication failed: {error}\n")
