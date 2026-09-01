#!/usr/bin/env python3
"""Synchronize Ant Design SVG icons into the default antd icon pack.

Default source: GitHub ant-design/ant-design-icons archive.
"""

from __future__ import annotations

import argparse
import datetime as _dt
import json
import subprocess
import sys
import tempfile
import urllib.request
import xml.etree.ElementTree as ET
import zipfile
from pathlib import Path
from typing import Dict, List, Optional, Sequence, Tuple

THEMES: Tuple[str, ...] = ("outlined", "filled", "twotone")
PRIMARY_COLORS = {"#333", "#333333", "#000", "#000000"}
SECONDARY_COLORS = {"#e6e6e6", "#d9d9d9", "#d8d8d8"}
TERTIARY_COLORS = {"#f5f5f5", "#f5f5f7"}
SVG_NS = "http://www.w3.org/2000/svg"
ET.register_namespace("", SVG_NS)


def _download_bytes(url: str, timeout: int = 60) -> bytes:
    req = urllib.request.Request(url, headers={"User-Agent": "ant_design_icons_qt_sync/1.0"})
    with urllib.request.urlopen(req, timeout=timeout) as resp:
        return resp.read()


def _resolve_commit_sha(repo: str, ref: str) -> Optional[str]:
    api_url = f"https://api.github.com/repos/{repo}/commits/{ref}"
    try:
        payload = _download_bytes(api_url)
        data = json.loads(payload.decode("utf-8"))
    except Exception:
        return None
    sha = data.get("sha")
    return sha if isinstance(sha, str) and sha else None


def _resolve_default_branch(repo: str) -> Optional[str]:
    api_url = f"https://api.github.com/repos/{repo}"
    try:
        payload = _download_bytes(api_url)
        data = json.loads(payload.decode("utf-8"))
    except Exception:
        return None
    branch = data.get("default_branch")
    return branch if isinstance(branch, str) and branch else None


def _download_archive(repo: str, ref: str, out_zip: Path) -> Tuple[str, str]:
    refs_to_try: List[str] = [ref]
    if ref == "main":
        refs_to_try.append("master")

    default_branch = _resolve_default_branch(repo)
    if default_branch and default_branch not in refs_to_try:
        refs_to_try.append(default_branch)

    candidates = [
        (candidate_ref, f"https://codeload.github.com/{repo}/zip/refs/heads/{candidate_ref}")
        for candidate_ref in refs_to_try
    ]
    candidates += [
        (candidate_ref, f"https://codeload.github.com/{repo}/zip/refs/tags/{candidate_ref}")
        for candidate_ref in refs_to_try
    ]
    candidates += [
        (candidate_ref, f"https://codeload.github.com/{repo}/zip/{candidate_ref}")
        for candidate_ref in refs_to_try
    ]

    last_error: Optional[Exception] = None
    for used_ref, url in candidates:
        try:
            payload = _download_bytes(url)
            out_zip.write_bytes(payload)
            return used_ref, url
        except Exception as exc:
            last_error = exc

    raise RuntimeError(f"Failed to download archive for ref '{ref}': {last_error}")


def _extract_archive(zip_path: Path, dst: Path) -> Path:
    with zipfile.ZipFile(zip_path, "r") as zf:
        zf.extractall(dst)
    roots = [p for p in dst.iterdir() if p.is_dir()]
    if len(roots) != 1:
        raise RuntimeError(f"Expected exactly one extracted root directory, got {len(roots)}.")
    return roots[0]


def _collect_svg_paths(svg_root: Path) -> Dict[str, List[Path]]:
    result: Dict[str, List[Path]] = {}
    for theme in THEMES:
        theme_dir = svg_root / theme
        if not theme_dir.exists() or not theme_dir.is_dir():
            raise RuntimeError(f"Missing svg theme directory: {theme_dir}")
        result[theme] = sorted(theme_dir.glob("*.svg"), key=lambda p: p.name)
    return result

def _local_name(tag: str) -> str:
    return tag.split("}", 1)[-1] if "}" in tag else tag


def _normalize_svg(svg_text: str, theme: str) -> Tuple[str, str]:
    root = ET.fromstring(svg_text)
    root.set("data-adqt-slot", "primary")
    if not root.get("fill"):
        root.set("fill", "currentColor")

    slots_used = {"primary"}

    for elem in root.iter():
        for attr_name in ("fill", "stroke"):
            value = elem.get(attr_name)
            if not value:
                continue
            lower = value.strip().lower()
            if lower in PRIMARY_COLORS:
                elem.set(attr_name, "currentColor")
            elif theme == "twotone" and lower in SECONDARY_COLORS:
                elem.set("data-adqt-slot", "secondary")
                slots_used.add("secondary")
            elif theme == "twotone" and lower in TERTIARY_COLORS:
                elem.set("data-adqt-slot", "tertiary")
                slots_used.add("tertiary")

    text = ET.tostring(root, encoding="unicode")
    text = text.replace(" />", "/>")
    text += "\n"

    if theme == "twotone":
        render_model = "threetone" if "tertiary" in slots_used else "twotone"
    else:
        render_model = "monochrome"
    return text, render_model


def _sync_svg_assets(
    svg_root: Path,
    dst_templates_root: Path,
    dry_run: bool,
) -> Dict[str, List[str]]:
    copied: Dict[str, List[str]] = {theme: [] for theme in THEMES}

    for theme in THEMES:
        src_dir = svg_root / theme
        dst_dir = dst_templates_root / theme
        dst_dir.mkdir(parents=True, exist_ok=True)

        source_files: Dict[str, Path] = {}
        for src_file in sorted(src_dir.glob("*.svg"), key=lambda p: p.name):
            source_files[src_file.name] = src_file

        wanted_names = set(source_files.keys())
        for old_file in dst_dir.glob("*.svg"):
            if old_file.name not in wanted_names and not dry_run:
                old_file.unlink()

        for file_name in sorted(wanted_names):
            src_file = source_files[file_name]
            normalized_svg, _ = _normalize_svg(src_file.read_text(encoding="utf-8"), theme)
            copied[theme].append(Path(file_name).stem)
            if dry_run:
                continue
            (dst_dir / file_name).write_text(normalized_svg, encoding="utf-8", newline="\n")

    return copied


def _write_text(path: Path, content: str, dry_run: bool) -> None:
    if dry_run:
        return
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(content, encoding="utf-8", newline="\n")


def _write_json(path: Path, payload: dict, dry_run: bool) -> None:
    if dry_run:
        return
    path.parent.mkdir(parents=True, exist_ok=True)
    text = json.dumps(payload, ensure_ascii=False, indent=2)
    path.write_text(text + "\n", encoding="utf-8", newline="\n")


def main(argv: Optional[Sequence[str]] = None) -> int:
    parser = argparse.ArgumentParser(description="Sync Ant Design SVG assets into the default antd pack.")
    parser.add_argument("--repo", default="ant-design/ant-design-icons", help="GitHub repo in owner/name format.")
    parser.add_argument("--ref", help="Git ref (default: commit pinned in upstream.lock.json).")
    parser.add_argument("--dry-run", action="store_true", help="Print summary without writing files.")
    args = parser.parse_args(argv)

    root = Path(__file__).resolve().parents[1]
    pkg_root = root / "packages" / "ant_design_icons_qt"
    resources_root = pkg_root / "resources"
    templates_root = resources_root / "templates"

    if not pkg_root.exists():
        raise RuntimeError(f"Package path not found: {pkg_root}")
    custom_icons_root = resources_root / "custom-icons"
    if custom_icons_root.exists() and any(custom_icons_root.rglob("*.svg")):
        raise RuntimeError("Project-owned SVGs are forbidden in the built-in antd inputs.")
    lock_path = resources_root / "upstream.lock.json"
    locked = json.loads(lock_path.read_text(encoding="utf-8")) if lock_path.exists() else {}
    requested_ref = args.ref or locked.get("resolved_commit")
    if not requested_ref:
        raise RuntimeError("No --ref was supplied and upstream.lock.json has no resolved_commit.")

    with tempfile.TemporaryDirectory(prefix="ant-design-icons-sync-") as tmp:
        tmp_path = Path(tmp)
        archive = tmp_path / "upstream.zip"

        used_ref, used_url = _download_archive(args.repo, requested_ref, archive)
        extracted_root = _extract_archive(archive, tmp_path / "src")
        svg_root = extracted_root / "packages" / "icons-svg" / "svg"

        _collect_svg_paths(svg_root)
        collected_names = _sync_svg_assets(svg_root, templates_root, args.dry_run)
        total_count = sum(len(collected_names[theme]) for theme in THEMES)

        lock_payload = {
            "repository": args.repo,
            "ref": requested_ref,
            "resolved_ref": used_ref,
            "archive_url": used_url,
            "resolved_commit": _resolve_commit_sha(args.repo, used_ref),
            "generated_at_utc": _dt.datetime.now(_dt.timezone.utc).replace(microsecond=0).isoformat().replace("+00:00", "Z"),
            "counts": {
                "outlined": len(collected_names["outlined"]),
                "filled": len(collected_names["filled"]),
                "twotone": len(collected_names["twotone"]),
                "total": total_count,
            },
        }
        _write_json(resources_root / "upstream.lock.json", lock_payload, args.dry_run)

        if not args.dry_run:
            subprocess.run([sys.executable, str(root / "tools" / "build_antd_manifest.py")], check=True)
            subprocess.run([
                sys.executable, str(root / "tools" / "generate_icon_pack.py"),
                str(resources_root / "antd.manifest.json"),
                "--header", str(pkg_root / "src" / "antd_icons.h"),
                "--source", str(pkg_root / "src" / "antd_icons.cpp"),
            ], check=True)

    print(
        f"synced outlined={len(collected_names['outlined'])} filled={len(collected_names['filled'])} "
        f"twotone={len(collected_names['twotone'])} total={total_count} dry_run={args.dry_run}"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
