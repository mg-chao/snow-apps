"""Check the documented capability matrix against both actual dispatch catalogs."""
import json
import argparse
from pathlib import Path
import re

ROOT = Path(__file__).resolve().parents[1]


def read(path):
    return (ROOT / path).read_text(encoding="utf-8")


def block(source, pattern):
    match = re.search(pattern, source, re.S)
    assert match, f"Dispatch declaration no longer matches {pattern}; update this check explicitly."
    return match.group(1)


def rust_catalog(path):
    declaration = block(read(path), r"const TOOLS:.*?=\s*&\[(.*?)\n\];")
    names = re.findall(r'\(\s*"([a-z0-9_]+)"\s*,', declaration)
    assert len(names) == len(set(names)), f"Duplicate Rust tool in {path}"
    return set(names)


def enum_members(source, name):
    declaration = block(source, rf"enum(?: class)? {name}\s*\{{(.*?)\}}")
    declaration = re.sub(r"//[^\n]*", "", declaration)
    return {item.split("=")[0].strip() for item in declaration.split(",") if item.strip()}


def check_surface_contracts(matrix):
    """Snapshots require explicit coverage review whenever a UI catalog grows.

    These checks prove catalog/adapter agreement, not native behavior. Runtime
    settings roundtrips and native media checks remain separate test gates.
    """
    sources = {
        "settings_field_kinds": ("include/snow_shot/presentation/settings/settingsregistry.h", "SettingsFieldKind"),
        "settings_action_bindings": ("include/snow_shot/presentation/settings/settingscatalog.h", "SettingsActionBinding"),
        "settings_custom_renderers": ("include/snow_shot/presentation/settings/settingscatalog.h", "SettingsCustomRenderer"),
        "settings_shortcut_adjustments": ("include/snow_shot/presentation/settings/settingscatalog.h", "SettingsShortcutAdjustment"),
        "canvas_tools": ("../snow_draw_engine_qt/include/snow_draw_engine_qt/snow_canvas_types.h", "SnowCanvasTool"),
        "canvas_style_sources": ("../snow_draw_engine_qt/include/snow_draw_engine_qt/snow_canvas_types.h", "SnowCanvasStyleToolbarSource"),
    }
    coverage = matrix["surface_contracts"]
    for name, (path, enum) in sources.items():
        actual = enum_members(read(path), enum)
        reviewed = set(coverage[name]["members"])
        assert actual == reviewed, f"{name}: review new={actual-reviewed}, removed={reviewed-actual}"
    rust_tools = enum_members(read("rust/snow-shot-mcp/src/schemas.rs"), "CanvasTool")
    for canvas, public in coverage["canvas_tools"]["members"].items():
        assert public in rust_tools, f"{canvas} has no public tool choice: {public}"
    runtime = read("src/presentation/recording/screenrecordingcontroller.cpp")
    booleans = block(runtime, r"const QStringList booleans\s*\{(.*?)\};")
    colors = block(runtime, r"const QStringList colors\s*\{(.*?)\};")
    enums = block(runtime, r"const QHash<QString, QStringList> enums\s*\{(.*?)\};")
    integers = block(runtime, r"const QHash<QString, QPair<int, int>> integers\s*\{(.*?)\};")
    qt_options = set(re.findall(r'QStringLiteral\("([a-z_]+)"\)', booleans + colors))
    qt_options.update(re.findall(r'\{QStringLiteral\("([a-z_]+)"\),\s*\{', enums + integers))
    qt_options.add("path")
    rust = block(read("rust/snow-shot-mcp/src/domain_schemas.rs"), r"input!\(RecordingOptions\s*\{(.*?)\}\);")
    rust_options = set(re.findall(r"([a-z_]+):\s*Option<", rust))
    reviewed = set(coverage["recording_options"]["members"])
    assert qt_options == rust_options == reviewed, f"Recording options: Qt={qt_options}, Rust={rust_options}, reviewed={reviewed}"
    recognition = block(read("src/presentation/ocr/screenshotrecognitionsessioncontroller.cpp"),
                        r"bool ScreenshotRecognitionSessionController::editWorkflow\(.*?\{(.*?)\nvoid ScreenshotRecognitionSessionController::cancelWorkflow")
    qt_actions = set(re.findall(r'action == QStringLiteral\("([a-z_]+)"\)', recognition))
    rust_actions = {re.sub(r"(?<!^)(?=[A-Z])", "_", action).lower()
                    for action in enum_members(read("rust/snow-shot-mcp/src/schemas.rs"), "RecognitionAction")}
    reviewed = set(coverage["recognition_actions"]["members"])
    assert qt_actions == rust_actions == reviewed, f"Recognition actions: Qt={qt_actions}, Rust={rust_actions}, reviewed={reviewed}"


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--execution-reports", nargs="+", type=Path,
                        help="Require every tool and resource to appear in passing real-IPC driver reports")
    args = parser.parse_args()
    declarations = {
        "legacy_screenshot": ("src/app/mcp/screenshotmcpsession.cpp", r"const QStringList tools\s*=\s*\{(.*?)\};"),
        "application": ("src/app/mcp/mcpapplicationservice.cpp", r"QStringList McpApplicationService::methods\(\).*?return\s*\{(.*?)\};"),
        "documents_jobs": ("src/app/mcp/mcpdocumentservice.cpp", r"const QStringList kTools\s*\{(.*?)\};"),
        "recording_pinned": ("src/app/mcp/mcpmediaservice.cpp", r"const QStringList methods\s*\{(.*?)\};"),
    }
    matrix = json.loads(read("mcp-capabilities.json"))
    all_qt = set()
    for domain, (path, pattern) in declarations.items():
        qt_names = set(re.findall(r'QStringLiteral\("([a-z0-9_]+)"\)', block(read(path), pattern)))
        documented = set(matrix["domains"][domain]["tools"])
        assert qt_names == documented, f"{domain}: Qt-only={qt_names-documented}; matrix-only={documented-qt_names}"
        assert not all_qt & qt_names, f"Ambiguous dispatch ownership: {all_qt & qt_names}"
        all_qt |= qt_names
    legacy = rust_catalog("rust/snow-shot-mcp/src/server.rs")
    rust = legacy | rust_catalog("rust/snow-shot-mcp/src/domain_schemas.rs")
    assert rust == all_qt, f"Rust-only={rust-all_qt}; Qt-only={all_qt-rust}"
    fixtures = json.loads(read("tests/mcp_contract_fixtures.json"))["fixtures"]
    assert {case["name"] for case in fixtures} == legacy and len(fixtures) == 28
    check_surface_contracts(matrix)
    if args.execution_reports:
        reports = [json.loads(path.read_text(encoding="utf-8")) for path in args.execution_reports]
        executed = set().union(*(set(report["executed_tools"]) for report in reports))
        assert rust <= executed, f"Tools missing real-IPC execution: {sorted(rust-executed)}"
        read_uris = set().union(*(set(report["read_resources"]) for report in reports))
        discovery = read("rust/snow-shot-mcp/src/discovery.rs")
        catalogs = discovery[discovery.index("fn resources()"):discovery.index("fn resource_request(")]
        declared = set(re.findall(r'"(snow-shot://[^\"]+)"', catalogs))
        for uri in declared:
            parts = re.split(r"(\{[^}]+\})", uri)
            pattern = "^" + "".join("[^/]+" if part.startswith("{") else re.escape(part) for part in parts) + "$"
            assert any(re.fullmatch(pattern, actual) for actual in read_uris), f"Resource missing real-IPC read: {uri}"
        print(f"MCP real-IPC execution coverage passed: {len(rust)} tools and {len(declared)} resource entries/templates.")
    print(f"MCP capability agreement passed: {len(rust)} tools in {len(declarations)} domains, 28 legacy input contracts and {len(matrix['surface_contracts'])} reviewed UI catalogs.")


if __name__ == "__main__":
    main()
