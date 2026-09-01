#!/usr/bin/env python3
"""Reject legacy and bypass icon paths in repository source files."""

from __future__ import annotations

import re
import subprocess
from dataclasses import dataclass
from pathlib import Path


CPP_SUFFIXES = {".c", ".cc", ".cpp", ".cxx", ".h", ".hh", ".hpp"}
SCANNED_SUFFIXES = CPP_SUFFIXES | {".py", ".qrc"}
CORE_RENDERER = Path("ant_design_qt/packages/adqt_icon_core/src/icon_renderer.cpp")


@dataclass(frozen=True)
class Rule:
    name: str
    pattern: re.Pattern[str]
    suffixes: set[str]


RULES = (
    Rule(
        "retired icon API",
        re.compile(r"\b(?:IconTheme|IconRenderModel|IconColorOverrides|makeIconRef|registerIcon)\b"),
        CPP_SUFFIXES | {".py"},
    ),
    Rule(
        "direct SVG resource path",
        re.compile(r"[\"']:/[^\"'\r\n]*\.svg[\"']", re.IGNORECASE),
        CPP_SUFFIXES,
    ),
    Rule(
        "direct Qt SVG file construction",
        re.compile(r"\bQ(?:File|Icon|Image|Pixmap)\s*\([^)]{0,300}\.svg", re.IGNORECASE | re.DOTALL),
        CPP_SUFFIXES,
    ),
    Rule(
        "QIcon SVG addFile bypass",
        re.compile(r"\.addFile\s*\([^)]{0,300}\.svg", re.IGNORECASE | re.DOTALL),
        CPP_SUFFIXES,
    ),
    Rule(
        "obsolete icon resource initializer",
        re.compile(r"\bQ_(?:INIT|CLEANUP)_RESOURCE\s*\([^)]*(?:icon|cursor)", re.IGNORECASE),
        CPP_SUFFIXES,
    ),
    Rule(
        "procedural serial-toolbar glyph rasterizer",
        re.compile(r"\bserialToolbarIconPixmap\b"),
        CPP_SUFFIXES,
    ),
    Rule(
        "SVG declared in a qrc",
        re.compile(r"<file\b[^>]*>[^<]*\.svg\s*</file>", re.IGNORECASE),
        {".qrc"},
    ),
)


def repository_files(root: Path) -> list[Path]:
    result = subprocess.run(
        ["git", "ls-files", "--cached", "--others", "--exclude-standard", "-z"],
        cwd=root,
        check=True,
        capture_output=True,
    )
    files = []
    for encoded in result.stdout.split(b"\0"):
        if not encoded:
            continue
        relative = Path(encoded.decode("utf-8"))
        path = root / relative
        if path.is_file() and path.suffix.lower() in SCANNED_SUFFIXES:
            files.append(relative)
    return sorted(files, key=lambda path: path.as_posix())


def line_number(text: str, offset: int) -> int:
    return text.count("\n", 0, offset) + 1


def main() -> int:
    root = Path(__file__).resolve().parents[2]
    script_path = Path(__file__).resolve().relative_to(root)
    violations: list[tuple[str, int, str]] = []

    for relative in repository_files(root):
        if relative == script_path:
            continue
        text = (root / relative).read_text(encoding="utf-8", errors="replace")
        suffix = relative.suffix.lower()

        if suffix in CPP_SUFFIXES and "QSvgRenderer" in text and relative != CORE_RENDERER:
            violations.append((relative.as_posix(), line_number(text, text.index("QSvgRenderer")),
                               "QSvgRenderer is restricted to the icon core"))

        for rule in RULES:
            if suffix not in rule.suffixes:
                continue
            match = rule.pattern.search(text)
            if match:
                violations.append((relative.as_posix(), line_number(text, match.start()), rule.name))

    custom_root = root / "ant_design_qt/packages/ant_design_icons_qt/resources/custom-icons"
    for svg in sorted(custom_root.rglob("*.svg")) if custom_root.exists() else []:
        relative = svg.relative_to(root).as_posix()
        violations.append((relative, 1, "project-owned SVG in the built-in antd input tree"))

    if violations:
        for path, line, message in sorted(violations):
            print(f"{path}:{line}: {message}")
        return 1

    print("repository icon usage check passed")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
