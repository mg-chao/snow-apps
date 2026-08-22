#!/usr/bin/env python3
"""Generate a typed, embedded Qt icon pack from a JSON manifest."""

from __future__ import annotations

import argparse
import hashlib
import json
import re
import sys
import xml.etree.ElementTree as ET
from dataclasses import dataclass
from pathlib import Path
from typing import Any

MODELS = {
    "monochrome": "IconColorModel::Monochrome",
    "twoTone": "IconColorModel::TwoTone",
    "threeTone": "IconColorModel::ThreeTone",
    "fullColor": "IconColorModel::FullColor",
}
FITS = {"contain": "IconFit::Contain", "stretch": "IconFit::Stretch"}
IDENTIFIER = re.compile(r"^[A-Za-z_][A-Za-z0-9_]*$")
CANONICAL = re.compile(r"^[a-z0-9]+(?:-[a-z0-9]+)*$")
FORBIDDEN_ELEMENTS = {"script", "foreignObject", "iframe", "animate", "animateMotion", "animateTransform", "set"}
PLACEHOLDERS = {
    "primary": "__ADQT_SLOT_PRIMARY__",
    "secondary": "__ADQT_SLOT_SECONDARY__",
    "tertiary": "__ADQT_SLOT_TERTIARY__",
}


class ManifestError(ValueError):
    pass


@dataclass(frozen=True)
class Entry:
    variant: str
    symbol: str
    name: str
    model: str
    fit: str
    defaults: dict[str, str]
    allow_data_images: bool
    normalized_svg: str
    source_hash: str


def _local_name(tag: str) -> str:
    return tag.rsplit("}", 1)[-1]


def _cpp_string(value: str) -> str:
    return '"' + value.replace("\\", "\\\\").replace('"', '\\"') + '"'


def _string_view_expression(value: str, index: int) -> str:
    # Keep raw segments below MSVC's individual literal limit. Adjacent literals are folded by the
    # compiler into one read-only array, so the descriptor still points at immutable contiguous
    # storage and no QByteArray construction is emitted.
    chunks: list[str] = []
    current: list[str] = []
    current_bytes = 0
    for character in value:
        character_bytes = len(character.encode("utf-8"))
        if current and current_bytes + character_bytes > 12_000:
            chunks.append("".join(current))
            current = []
            current_bytes = 0
        current.append(character)
        current_bytes += character_bytes
    if current:
        chunks.append("".join(current))

    literals: list[str] = []
    for chunk_index, chunk in enumerate(chunks):
        delimiter = f"ADQT_SVG_{index}" if len(chunks) == 1 else f"ADQT_SVG_{index}_{chunk_index}"
        if f"){delimiter}\"" in chunk:
            raise ManifestError("SVG contains the generator raw-string delimiter")
        literals.append(f'R"{delimiter}({chunk}){delimiter}"')
    return "std::string_view(" + " ".join(literals) + ")"


def _byte_array_expression(value: str, index: int) -> str:
    """Compatibility helper retained for generator unit tests and downstream tooling.

    New generated packs use _string_view_expression; this function deliberately keeps the old
    chunked QByteArray expression available to callers that used the private helper in v2.
    """
    chunks: list[str] = []
    current: list[str] = []
    current_bytes = 0
    for character in value:
        character_bytes = len(character.encode("utf-8"))
        if current and current_bytes + character_bytes > 12_000:
            chunks.append("".join(current))
            current = []
            current_bytes = 0
        current.append(character)
        current_bytes += character_bytes
    if current:
        chunks.append("".join(current))
    literals: list[str] = []
    for chunk_index, chunk in enumerate(chunks):
        delimiter = f"ADQT_SVG_{index}" if len(chunks) == 1 else f"ADQT_SVG_{index}_{chunk_index}"
        literals.append(f'R"{delimiter}({chunk}){delimiter}"')
    if len(literals) == 1:
        return f"QByteArray({literals[0]})"
    return "QByteArray()\n          .append(" + ")\n          .append(".join(literals) + ")"


def _replace_attr(tag: str, attribute: str, placeholder: str) -> tuple[str, bool]:
    expression = re.compile(rf"\b{attribute}\s*=\s*(['\"])([^'\"]*)\1", re.IGNORECASE)
    match = expression.search(tag)
    if not match or match.group(2).lower() == "none":
        return tag, False
    return tag[: match.start()] + f'{attribute}="{placeholder}"' + tag[match.end() :], True


def _normalize_slots(svg: str, model: str) -> str:
    if model == "fullColor":
        if re.search(r"data-adqt-slot|currentColor", svg, re.IGNORECASE):
            raise ManifestError("fullColor assets cannot contain theme slots")
        return svg

    slot_tag = re.compile(
        r"<([A-Za-z_:][A-Za-z0-9:._-]*)([^>]*)\bdata-adqt-slot\s*=\s*(['\"])(primary|secondary|tertiary)\3([^>]*)>",
        re.IGNORECASE,
    )
    slot_attr = re.compile(r"\s*data-adqt-slot\s*=\s*(['\"])(primary|secondary|tertiary)\1", re.IGNORECASE)

    def replace(match: re.Match[str]) -> str:
        tag = match.group(0)
        placeholder = PLACEHOLDERS[match.group(4).lower()]
        tag = slot_attr.sub("", tag)
        tag, fill_changed = _replace_attr(tag, "fill", placeholder)
        tag, stroke_changed = _replace_attr(tag, "stroke", placeholder)
        changed = fill_changed or stroke_changed
        if re.search("currentColor", tag, re.IGNORECASE):
            tag = re.sub("currentColor", placeholder, tag, flags=re.IGNORECASE)
            changed = True
        if not changed:
            position = tag.rfind(">")
            if tag[position - 1] == "/":
                position -= 1
            tag = tag[:position] + f' fill="{placeholder}"' + tag[position:]
        return tag

    normalized = slot_tag.sub(replace, svg)
    normalized = re.sub("currentColor", PLACEHOLDERS["primary"], normalized, flags=re.IGNORECASE)
    present = {name for name, placeholder in PLACEHOLDERS.items() if placeholder in normalized}
    expected = {
        "monochrome": {"primary"},
        "twoTone": {"primary", "secondary"},
        "threeTone": {"primary", "secondary", "tertiary"},
    }[model]
    if not present and model == "monochrome":
        root = re.search(r"<svg\b[^>]*>", normalized, re.IGNORECASE)
        if root:
            tag = root.group(0)
            position = tag.rfind(">")
            tag = tag[:position] + f' fill="{PLACEHOLDERS["primary"]}"' + tag[position:]
            normalized = normalized[: root.start()] + tag + normalized[root.end() :]
            present = {"primary"}
    if present != expected:
        raise ManifestError(f"declared {model} model requires slots {sorted(expected)}, found {sorted(present)}")
    return normalized


def _validate_svg(path: Path, model: str, allow_data_images: bool) -> str:
    try:
        source = path.read_text(encoding="utf-8-sig").replace("\r\n", "\n").replace("\r", "\n").strip()
    except OSError as error:
        raise ManifestError(f"cannot read SVG {path}: {error}") from error
    try:
        root = ET.fromstring(source)
    except ET.ParseError as error:
        raise ManifestError(f"invalid SVG XML in {path}: {error}") from error
    if _local_name(root.tag) != "svg":
        raise ManifestError(f"{path} does not have an svg root")
    view_box = root.attrib.get("viewBox")
    if not view_box:
        raise ManifestError(f"{path} has no viewBox")
    try:
        values = [float(item) for item in re.split(r"[\s,]+", view_box.strip())]
    except ValueError as error:
        raise ManifestError(f"{path} has an invalid viewBox") from error
    if len(values) != 4 or values[2] <= 0 or values[3] <= 0:
        raise ManifestError(f"{path} must have a positive four-value viewBox")

    image_count = 0
    for element in root.iter():
        local = _local_name(element.tag)
        if local in FORBIDDEN_ELEMENTS:
            raise ManifestError(f"{path} uses unsupported element <{local}>")
        for attribute, value in element.attrib.items():
            attr = _local_name(attribute)
            if attr not in {"href", "src"}:
                continue
            if re.match(r"^(?:https?:)?//", value, re.IGNORECASE):
                raise ManifestError(f"{path} contains an external network reference")
            if local == "image":
                image_count += 1
                if not value.startswith("data:"):
                    raise ManifestError(f"{path} contains a non-data image reference")
    if image_count and (model != "fullColor" or not allow_data_images):
        raise ManifestError(f"{path} embeds data images without fullColor allowEmbeddedDataImages")
    return _normalize_slots(source, model) + "\n"


def _load_manifest(path: Path) -> tuple[dict[str, Any], list[Entry]]:
    try:
        data = json.loads(path.read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError) as error:
        raise ManifestError(f"cannot parse manifest: {error}") from error
    if data.get("schemaVersion") != 1:
        raise ManifestError("schemaVersion must be 1")
    pack = data.get("pack")
    namespace = data.get("cppNamespace")
    raw_entries = data.get("entries")
    if not isinstance(pack, str) or not CANONICAL.fullmatch(pack):
        raise ManifestError("pack must be a canonical lower-case name")
    if not isinstance(namespace, str) or not namespace or any(
        not IDENTIFIER.fullmatch(part) for part in namespace.split("::")
    ):
        raise ManifestError("cppNamespace must contain valid C++ identifiers")
    export_macro = data.get("exportMacro", "")
    if not isinstance(export_macro, str) or (export_macro and not IDENTIFIER.fullmatch(export_macro)):
        raise ManifestError("exportMacro must be empty or a valid C++ identifier")
    if not isinstance(raw_entries, list) or not raw_entries:
        raise ManifestError("entries must be a non-empty array")

    entries: list[Entry] = []
    keys: set[tuple[str, str]] = set()
    symbols: set[tuple[str, str]] = set()
    base = path.parent
    for index, raw in enumerate(raw_entries):
        if not isinstance(raw, dict):
            raise ManifestError(f"entry {index} must be an object")
        variant, symbol, name = raw.get("variant"), raw.get("symbol"), raw.get("name")
        model = raw.get("colorModel")
        fit = raw.get("fit", "contain")
        if not isinstance(variant, str) or not CANONICAL.fullmatch(variant):
            raise ManifestError(f"entry {index} has an invalid variant")
        if not isinstance(symbol, str) or not IDENTIFIER.fullmatch(symbol):
            raise ManifestError(f"entry {index} has an invalid exported symbol")
        if not isinstance(name, str) or not CANONICAL.fullmatch(name):
            raise ManifestError(f"entry {index} has an invalid canonical name")
        if model not in MODELS or fit not in FITS:
            raise ManifestError(f"entry {index} has an unsupported colorModel or fit")
        if (variant, name) in keys:
            raise ManifestError(f"duplicate icon key {variant}/{name}")
        if (variant, symbol) in symbols:
            raise ManifestError(f"duplicate exported symbol {variant}::{symbol}")
        keys.add((variant, name))
        symbols.add((variant, symbol))

        defaults = raw.get("defaultColors", {})
        if not isinstance(defaults, dict) or any(
            key not in PLACEHOLDERS or not isinstance(value, str)
            for key, value in defaults.items()
        ):
            raise ManifestError(f"entry {index} has invalid defaultColors")
        allowed_slots = {"monochrome": 1, "twoTone": 2, "threeTone": 3, "fullColor": 0}[model]
        slot_order = ["primary", "secondary", "tertiary"]
        if any(key in defaults for key in slot_order[allowed_slots:]):
            raise ManifestError(f"entry {index} default colors do not match {model}")
        allow_data = raw.get("allowEmbeddedDataImages", False)
        if not isinstance(allow_data, bool):
            raise ManifestError(f"entry {index} allowEmbeddedDataImages must be boolean")
        source_value = raw.get("source")
        if not isinstance(source_value, str) or not source_value:
            raise ManifestError(f"entry {index} has no source")
        normalized = _validate_svg((base / source_value).resolve(), model, allow_data)
        digest = hashlib.sha256(normalized.encode("utf-8")).hexdigest()
        entries.append(Entry(variant, symbol, name, model, fit, defaults, allow_data, normalized, digest))
    entries.sort(key=lambda item: (item.variant, item.name, item.symbol))
    return data, entries


def _colors_expression(defaults: dict[str, str]) -> str:
    values = [f"std::string_view({_cpp_string(defaults.get(slot_name, ''))})"
              for slot_name in ("primary", "secondary", "tertiary")]
    return "IconStaticColors{" + ", ".join(values) + "}"


def _render_header(data: dict[str, Any], entries: list[Entry], header_name: str) -> str:
    guard = re.sub(r"[^A-Za-z0-9]", "_", f"ADQT_GENERATED_{data['pack']}_{header_name}").upper()
    namespace = data["cppNamespace"]
    export = (data.get("exportMacro", "") + " ") if data.get("exportMacro") else ""
    variants = sorted({entry.variant for entry in entries})
    lines = [
        "// Generated by tools/generate_icon_pack.py. DO NOT EDIT.",
        f"#ifndef {guard}", f"#define {guard}", "", '#include "external_icon_pack.h"', "",
        f"namespace {namespace} {{", "",
        f"{export}const adqt::icons::ExternalIconPack& pack();",
        f"{export}adqt::icons::IconPackRegistrationResult registerWith(adqt::icons::IconRegistry& registry);",
        f"{export}adqt::icons::IconPackRegistrationResult ensureRegistered();", "",
    ]
    for variant in variants:
        lines.append(f"namespace {variant} {{")
        for entry in entries:
            if entry.variant == variant:
                lines.append(f"[[nodiscard]] {export}adqt::icons::IconRef {entry.symbol}(const adqt::icons::IconColors& colors = {{}});")
        lines.extend([f"}}  // namespace {variant}", ""])
    lines.extend([f"}}  // namespace {namespace}", "", f"#endif  // {guard}", ""])
    return "\n".join(lines)


def _render_source(data: dict[str, Any], entries: list[Entry], header_name: str) -> str:
    namespace = data["cppNamespace"]
    header_include = data.get("headerInclude", header_name)
    pack_hash_input = "".join(f"{e.variant}\0{e.name}\0{e.source_hash}\n" for e in entries)
    pack_hash = hashlib.sha256(pack_hash_input.encode("utf-8")).hexdigest()
    lines = [
        "// Generated by tools/generate_icon_pack.py. DO NOT EDIT.",
        f'#include "{header_include}"', "", "#include <cstddef>", "#include <string_view>", "",
        f"namespace {namespace} {{", "", "namespace {", "",
        "using adqt::icons::IconColorModel;",
        "using adqt::icons::IconDescriptor;",
        "using adqt::icons::IconFit;",
        "using adqt::icons::IconPack;",
        "using adqt::icons::IconStaticColors;",
        "",
        "constexpr IconDescriptor kEntries[] = {",
    ]
    for index, entry in enumerate(entries):
        lines.extend([
            "  {",
            f"    std::string_view({_cpp_string(data['pack'])}), std::string_view({_cpp_string(entry.variant)}), std::string_view({_cpp_string(entry.name)}),",
            f"    {_string_view_expression(entry.normalized_svg, index)}, std::string_view({_cpp_string(entry.source_hash)}),",
            f"    {MODELS[entry.model]}, {FITS[entry.fit]}, {_colors_expression(entry.defaults)}, {'true' if entry.allow_data_images else 'false'}",
            "  },",
        ])
    lines.extend([
        "};", "",
        f"constexpr IconPack kStaticPack{{std::string_view({_cpp_string(data['pack'])}), std::string_view({_cpp_string(data.get('source', 'project-owned manifest'))}), std::string_view({_cpp_string(pack_hash)}), kEntries, sizeof(kEntries) / sizeof(kEntries[0])}};",
        "",
        "}  // namespace", "",
        "const adqt::icons::ExternalIconPack& pack() {",
        "  static const adqt::icons::ExternalIconPack value(kStaticPack);",
        "  return value;", "}", "",
        "adqt::icons::IconPackRegistrationResult registerWith(adqt::icons::IconRegistry& registry) {",
        "  return pack().registerWith(registry);", "}", "",
        "adqt::icons::IconPackRegistrationResult ensureRegistered() {",
        "  return pack().ensureRegistered();", "}", "",
    ])
    current_variant = None
    for index, entry in enumerate(entries):
        if current_variant != entry.variant:
            if current_variant is not None:
                lines.extend([f"}}  // namespace {current_variant}", ""])
            current_variant = entry.variant
            lines.extend([f"namespace {current_variant} {{", ""])
        lines.extend([
            f"adqt::icons::IconRef {entry.symbol}(const adqt::icons::IconColors& colors) {{",
            f"  return pack().icon({index}, colors);",
            "}", "",
        ])
    if current_variant is not None:
        lines.extend([f"}}  // namespace {current_variant}", ""])
    lines.extend([f"}}  // namespace {namespace}", ""])
    return "\n".join(lines)


def _check_or_write(path: Path, expected: str, check: bool) -> bool:
    if check:
        try:
            actual = path.read_text(encoding="utf-8")
        except OSError:
            actual = ""
        if actual != expected:
            print(f"stale generated output: {path}", file=sys.stderr)
            return False
        return True
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(expected, encoding="utf-8", newline="\n")
    return True


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("manifest", type=Path)
    parser.add_argument("--header", type=Path, required=True)
    parser.add_argument("--source", type=Path, required=True)
    parser.add_argument("--check", action="store_true")
    args = parser.parse_args()
    try:
        data, entries = _load_manifest(args.manifest.resolve())
        header = _render_header(data, entries, args.header.name)
        source = _render_source(data, entries, args.header.name)
    except ManifestError as error:
        print(f"icon manifest error: {error}", file=sys.stderr)
        return 2
    valid = _check_or_write(args.header, header, args.check)
    valid = _check_or_write(args.source, source, args.check) and valid
    return 0 if valid else 1


if __name__ == "__main__":
    raise SystemExit(main())
