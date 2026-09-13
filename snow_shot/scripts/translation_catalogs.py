#!/usr/bin/env python3
"""Deterministically split/merge Qt TS catalogs; lupdate owns message extraction."""

import argparse
from collections import Counter
import copy
import json
from pathlib import Path
import re
import xml.etree.ElementTree as ET


LOCALES = ("en_US", "zh_CN", "zh_TW")
DEFAULT_CATALOG_DIR = Path(__file__).resolve().parents[1] / "i18n"


def read_modules(directory):
    def unique_object(pairs):
        result = {}
        for key, value in pairs:
            if key in result:
                raise ValueError(f"Duplicate module: {key}")
            result[key] = value
        return result

    modules = json.loads(
        (directory / "modules.json").read_text(encoding="utf-8"),
        object_pairs_hook=unique_object,
    )
    owners = {}
    if not isinstance(modules, dict) or not modules:
        raise ValueError("modules.json must contain a nonempty module-to-context mapping")
    for module, contexts in modules.items():
        if not re.fullmatch(r"[a-z][a-z0-9-]*", module):
            raise ValueError(f"Invalid module name: {module!r}")
        if not isinstance(contexts, list):
            raise ValueError(f"Contexts for {module} must be a list")
        for context in contexts:
            if not isinstance(context, str) or not context:
                raise ValueError(f"Invalid context in {module}")
            if context in owners:
                raise ValueError(f"Duplicate context assignment: {context}")
            owners[context] = module
    return modules, owners


def message_key(context, message):
    # Numerus is metadata, not a separate text-based Qt lookup key.
    return context, message.findtext("source", ""), message.findtext("comment", "")


def read_catalog(path, locale):
    root = ET.parse(path).getroot()
    if root.tag != "TS" or root.get("language") != locale:
        raise ValueError(f"{path}: expected a TS catalog for {locale}")
    if root.get("sourcelanguage") != "en_US" or root.get("version") != "2.1":
        raise ValueError(f"{path}: expected TS 2.1 with source language en_US")
    validate_keys(root, str(path))
    return root


def validate_keys(root, label):
    keys, ids, contexts = set(), set(), set()
    for context in root.findall("context"):
        name = context.findtext("name")
        if not name or name in contexts:
            raise ValueError(f"{label}: missing or duplicate context {name!r}")
        contexts.add(name)
        for message in context.findall("message"):
            key = message_key(name, message)
            identifier = message.get("id")
            if key in keys or (identifier and identifier in ids):
                raise ValueError(f"{label}: duplicate translation key {key!r}")
            keys.add(key)
            if identifier:
                ids.add(identifier)
            if message.find("source") is None or message.find("translation") is None:
                raise ValueError(f"{label}: missing source or translation for {key!r}")


def empty_catalog(locale):
    return ET.Element("TS", version="2.1", language=locale, sourcelanguage="en_US")


def canonical_bytes(root):
    root = copy.deepcopy(root)
    contexts = root.findall("context")
    for context in contexts:
        messages = context.findall("message")
        for message in messages:
            for location in message.findall("location"):
                message.remove(location)
            context.remove(message)
        context.extend(sorted(messages, key=lambda m: message_key("", m)))
        root.remove(context)
    # Sort contexts independently of manifest and source-code order.
    root.extend(sorted(contexts, key=lambda c: c.findtext("name", "")))
    return serialize(root)


def serialize(root):
    # Only indent structural containers. Source/translation text can contain
    # significant whitespace and mixed content such as Qt's <byte> elements.
    def indent(element, depth=0):
        if element.tag not in ("TS", "context", "message"):
            return
        if len(element):
            element.text = "\n" + "    " * (depth + 1)
            for child in element:
                indent(child, depth + 1)
                child.tail = "\n" + "    " * (depth + 1)
            element[-1].tail = "\n" + "    " * depth

    indent(root)
    return (b'<?xml version="1.0" encoding="utf-8"?>\n<!DOCTYPE TS>\n'
            + ET.tostring(root, encoding="utf-8") + b"\n")


def write_if_changed(path, content):
    if path.exists() and path.read_bytes() == content:
        return
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_bytes(content)


def validate_parity(catalogs):
    reference = None
    for locale, root in catalogs.items():
        keys = {
            (*message_key(c.findtext("name"), m), m.get("numerus"), m.get("id"))
            for c in root.findall("context") for m in c.findall("message")
        }
        if reference is None:
            reference = keys
        elif keys != reference:
            raise ValueError(f"{locale}: message keys differ from en_US")


def merged_catalogs(directory):
    modules, owners = read_modules(directory)
    expected = {
        directory / module / f"snow_shot_{module}_{locale}.ts"
        for module in modules for locale in LOCALES
    }
    actual = set(directory.rglob("*.ts"))
    if actual != expected:
        raise ValueError(f"Catalog files differ from modules.json; "
                         f"missing={sorted(expected - actual)}, extra={sorted(actual - expected)}")
    catalogs = {}
    for locale in LOCALES:
        merged = empty_catalog(locale)
        for module in modules:
            path = directory / module / f"snow_shot_{module}_{locale}.ts"
            root = read_catalog(path, locale)
            if module == next(iter(modules)):
                merged.attrib = dict(root.attrib)
            elif root.attrib != merged.attrib:
                raise ValueError(f"{path}: TS metadata differs between modules")
            for context in root.findall("context"):
                name = context.findtext("name")
                if owners.get(name) != module:
                    raise ValueError(f"{path}: {name} belongs to {owners.get(name)!r}; "
                                     "assign new contexts in modules.json")
            merged.extend(root)
        validate_keys(merged, locale)
        catalogs[locale] = merged
    validate_parity(catalogs)
    return catalogs


def merge(directory, output):
    catalogs = merged_catalogs(directory)
    for locale, root in catalogs.items():
        write_if_changed(output / f"snow_shot_{locale}.ts", canonical_bytes(root))


def split(directory, source):
    modules, owners = read_modules(directory)
    catalogs = {locale: read_catalog(source / f"snow_shot_{locale}.ts", locale)
                for locale in LOCALES}
    validate_parity(catalogs)
    outputs = {}
    for locale, root in catalogs.items():
        fragments = {module: empty_catalog(locale) for module in modules}
        for fragment in fragments.values():
            fragment.attrib = dict(root.attrib)
        # Root metadata (for example TS dependencies) has one owner, while
        # context and message metadata travels with its original context.
        fragments[next(iter(modules))].extend(c for c in root if c.tag != "context")
        for context in root.findall("context"):
            name = context.findtext("name")
            if name not in owners:
                raise ValueError(f"Unassigned context {name!r}; add it to i18n/modules.json "
                                 "and run snow_shot_update_translations again")
            fragments[owners[name]].append(context)
        for module, fragment in fragments.items():
            outputs[directory / module / f"snow_shot_{module}_{locale}.ts"] = canonical_bytes(fragment)
    # Validate all locales and assignments before changing any tracked catalog.
    for path, content in outputs.items():
        write_if_changed(path, content)


def check(directory):
    catalogs = merged_catalogs(directory)
    for locale, root in catalogs.items():
        for context in root.findall("context"):
            for message in context.findall("message"):
                translation = message.find("translation")
                if translation.get("type") in ("unfinished", "vanished", "obsolete"):
                    raise ValueError(f"{locale}: incomplete or obsolete translation in "
                                     f"{context.findtext('name')}: {message.findtext('source')}")
                forms = translation.findall(".//numerusform") or [translation]
                placeholders = Counter(re.findall(r"%L?(?:[1-9][0-9]?|n)",
                                                  message.findtext("source", "")))
                for form in forms:
                    text = "".join(form.itertext())
                    if not text.strip() or Counter(re.findall(
                            r"%L?(?:[1-9][0-9]?|n)", text)) != placeholders:
                        raise ValueError(f"{locale}: empty translation or changed placeholders "
                                         f"for {message.findtext('source')}")
                language_name = message_key(context.findtext("name"), message) == (
                    "LanguageCatalog", "Language name", "")
                if locale == "en_US" and message.get("numerus") != "yes" and not language_name:
                    source = message.find("source")
                    if (source.text, [ET.tostring(c) for c in source]) != (
                            translation.text, [ET.tostring(c) for c in translation]):
                        raise ValueError(f"en_US must be identity: {message.findtext('source')}")
    for path in directory.rglob("*.ts"):
        if path.read_bytes() != canonical_bytes(ET.parse(path).getroot()):
            raise ValueError(f"{path}: noncanonical catalog; run snow_shot_update_translations")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("command", choices=("merge", "split", "check"))
    parser.add_argument("--catalog-dir", type=Path, default=DEFAULT_CATALOG_DIR)
    parser.add_argument("--output-dir", type=Path)
    parser.add_argument("--input-dir", type=Path)
    args = parser.parse_args()
    try:
        if args.command == "merge":
            if args.output_dir is None:
                parser.error("merge requires --output-dir")
            merge(args.catalog_dir, args.output_dir)
        elif args.command == "split":
            if args.input_dir is None:
                parser.error("split requires --input-dir")
            split(args.catalog_dir, args.input_dir)
        else:
            check(args.catalog_dir)
    except (ValueError, OSError, ET.ParseError) as error:
        parser.exit(1, f"Translation catalogs: {error}\n")


if __name__ == "__main__":
    main()
