"""Check the documented capability matrix against both actual dispatch catalogs."""
import json
import argparse
from pathlib import Path
import re

ROOT = Path(__file__).resolve().parents[1]
MINI = False
REMOVED_MINI_TOOLS = {
    "snow_shot_screenshot_translate", "snow_shot_translation_catalog",
    "snow_shot_translation_start", "snow_shot_models_list",
    "snow_shot_models_update", "snow_shot_credentials_set",
}


def read(path):
    source = (ROOT / path).read_text(encoding="utf-8")
    if MINI:
        source = re.sub(r'#\[cfg\(not\(feature = "mini"\)\)\]\s*\w+\s*,?', "", source)
        source = re.sub(r"#if SNOW_SHOT_ENABLE_[A-Z_]+\n.*?\n#endif", "", source,
                        flags=re.S)
    return source


def block(source, pattern):
    match = re.search(pattern, source, re.S)
    assert match, f"Dispatch declaration no longer matches {pattern}; update this check explicitly."
    return match.group(1)


def rust_catalog(path):
    declaration = block(read(path), r"const TOOLS:.*?=\s*&\[(.*?)\n\];")
    names = re.findall(r'\(\s*"([a-z0-9_]+)"\s*,', declaration)
    if MINI:
        names = [name for name in names if name not in REMOVED_MINI_TOOLS]
    assert len(names) == len(set(names)), f"Duplicate Rust tool in {path}"
    return set(names)


def enum_members(source, name):
    declaration = block(source, rf"enum(?: class)? {name}\s*\{{(.*?)\}}")
    declaration = re.sub(r"//[^\n]*", "", declaration)
    declaration = re.sub(r"#\[[^\n]*\]\s*", "", declaration)
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
        if MINI and name == "settings_custom_renderers":
            actual -= {"CustomAiModels", "TextTranslationConfigurations"}
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
    if MINI:
        qt_actions -= {"select_cells", "set_cell", "merge_cells", "split_cells", "reset_table", "show_original"}
    rust_actions = {re.sub(r"(?<!^)(?=[A-Z])", "_", action).lower()
                    for action in enum_members(read("rust/snow-shot-mcp/src/schemas.rs"), "RecognitionAction")}
    reviewed = set(coverage["recognition_actions"]["members"])
    assert qt_actions == rust_actions == reviewed, f"Recognition actions: Qt={qt_actions}, Rust={rust_actions}, reviewed={reviewed}"


def check_angle_contracts(matrix):
    """Check angle coverage without accepting or changing other catalog snapshots."""
    coverage = matrix["surface_contracts"]
    qt_types = read("../snow_draw_engine_qt/include/snow_draw_engine_qt/snow_canvas_types.h")
    schemas = read("rust/snow-shot-mcp/src/schemas.rs")
    domains = read("rust/snow-shot-mcp/src/domain_schemas.rs")
    assert "Angle" in enum_members(qt_types, "SnowCanvasTool"), "Qt has no Angle tool"
    assert "Angle" in enum_members(schemas, "CanvasTool"), "Screenshot schema has no Angle tool"
    document_tools = block(domains, r"choices!\(DocumentCanvasTool\s*\{(.*?)\}\);")
    assert re.search(r"\bAngle\s*,", document_tools), "Document schema has no Angle tool"
    assert coverage["canvas_tools"]["members"].get("Angle") == "Angle", "Review the Angle tool mapping"
    qt_sources = enum_members(qt_types, "SnowCanvasStyleToolbarSource")
    reviewed_sources = coverage["canvas_style_sources"]["members"]
    for source in ("DefaultAngle", "SelectedAngle"):
        assert source in qt_sources, f"Qt has no {source} style source"
        assert reviewed_sources.count(source) == 1, f"Review exactly one {source} style source"

    for path in ("include/snow_shot/app/mcp/mcpstylepatch.h", "src/app/mcp/mcpdocumentservice.cpp"):
        assert re.search(r'\{QStringLiteral\("angle"\),\s*SnowCanvasTool::Angle\}', read(path)), \
            f"{path} has no Angle dispatch mapping"
    assert enum_members(qt_types, "SnowCanvasAngleUnit") == {"Degrees", "Radians"}
    assert enum_members(schemas, "AngleUnit") == {"Degrees", "Radians"}
    assert re.search(r'#\[serde\(rename_all = "snake_case"\)\]\s*enum AngleUnit', schemas), \
        "Angle unit protocol values must remain degrees and radians"

    style = block(schemas, r"struct AngleAnnotationStyle\s*\{(.*?)\}")
    fields = {name: re.sub(r"\s", "", kind) for name, kind in
              re.findall(r"^\s*([a-z_]+)\s*:\s*([^\n,]+),", style, re.M)}
    expected = {"stroke": "Option<[u8;4]>", "stroke_width": "Option<AngleStrokeWidth>",
                "unit": "Option<AngleUnit>", "decimal_places": "Option<AngleDecimals>"}
    assert fields == expected, f"Angle style fields changed: {fields}"
    assert re.search(r'#\[serde\(default, deny_unknown_fields\)\]\s*struct AngleAnnotationStyle', schemas), \
        "Angle style must default missing fields and reject unrelated fields"
    for declaration in (r'AngleDecimals,\s*u8,\s*"integer",\s*0,\s*3',
                        r'AngleStrokeWidth,\s*f64,\s*"number",\s*1\.0,\s*72\.0'):
        assert re.search(rf"bounded_style_number!\({declaration}\);", schemas), \
            f"Angle style bounds changed: {declaration}"
    qt_patch = read("include/snow_shot/app/mcp/mcpstylepatch.h")
    allowed = block(qt_patch, r'else if \(target == QStringLiteral\("angle"\)\)\s*allowed = \{(.*?)\};')
    assert set(re.findall(r'QStringLiteral\("([a-z_]+)"\)', allowed)) == set(expected), \
        "Qt and Rust Angle style fields differ"
    units = block(qt_patch, r'target == QStringLiteral\("angle"\)\s*\? QStringList\{(.*?)\}')
    assert re.findall(r'QStringLiteral\("([a-z_]+)"\)', units) == ["degrees", "radians"], \
        "Qt and Rust Angle units differ"

    annotation = block(schemas, r"enum Annotation\s*\{(.*?)\n\}")
    angle = block(annotation, r"\bAngle\s*\{(.*?)\n\s*\},")
    assert re.search(r"points:\s*\[\[f64;\s*2\];\s*3\]", angle), "Angle creation needs three points"
    for name, kind in (("style", "AngleAnnotationStyle"), ("full_turn", "bool")):
        assert re.search(rf"#\[serde\(default\)\]\s*{name}:\s*{kind}", angle), \
            f"Angle creation must allow an omitted {name}"
    assert enum_members(schemas, "AngleTarget") == {"Angle"}, "Angle style target must be specific"
    for source, name, kind in ((schemas, "ToolStyle", "AngleToolStyle"),
                               (schemas, "ScreenshotToolStyleMutation", "Mutation<AngleToolStyle>"),
                               (domains, "DocumentToolStyleMutation", "DocumentMutation<AngleToolStyle>")):
        union = block(source, rf"enum {name}\s*\{{(.*?)\}}")
        assert re.search(rf"\bAngle\({re.escape(kind)}\)", union), f"{name} has no typed Angle style branch"


def main():
    global MINI
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--mini", action="store_true", help="Check the compiled Mini contract")
    parser.add_argument("--angle-only", action="store_true",
                        help="Check only Angle entries and schemas in both full and Mini contracts")
    parser.add_argument("--execution-reports", nargs="+", type=Path,
                        help="Require every tool and resource to appear in passing real-IPC driver reports")
    args = parser.parse_args()
    if args.angle_only:
        if args.mini or args.execution_reports:
            parser.error("--angle-only checks both variants and cannot be combined with other modes")
        for MINI in (False, True):
            matrix = json.loads(read("mcp-capabilities-mini.json" if MINI else "mcp-capabilities.json"))
            check_angle_contracts(matrix)
        print("MCP Angle capability agreement passed for full and Mini catalogs, styles and creation schemas.")
        return
    MINI = args.mini
    declarations = {
        "screenshot": ("src/app/mcp/screenshotmcpsession.cpp", r"const QStringList tools\s*=\s*\{(.*?)\};"),
        "application": ("src/app/mcp/mcpapplicationservice.cpp", r"QStringList McpApplicationService::methods\(\).*?return\s*\{(.*?)\};"),
        "documents_jobs": ("src/app/mcp/mcpdocumentservice.cpp", r"const QStringList kTools\s*\{(.*?)\};"),
        "recording_pinned": ("src/app/mcp/mcpmediaservice.cpp", r"const QStringList methods\s*\{(.*?)\};"),
    }
    matrix = json.loads(read("mcp-capabilities-mini.json" if MINI else "mcp-capabilities.json"))
    all_qt = set()
    for domain, (path, pattern) in declarations.items():
        qt_names = set(re.findall(r'QStringLiteral\("([a-z0-9_]+)"\)', block(read(path), pattern)))
        documented = set(matrix["domains"][domain]["tools"])
        assert qt_names == documented, f"{domain}: Qt-only={qt_names-documented}; matrix-only={documented-qt_names}"
        assert not all_qt & qt_names, f"Ambiguous dispatch ownership: {all_qt & qt_names}"
        all_qt |= qt_names
    screenshot = rust_catalog("rust/snow-shot-mcp/src/server.rs")
    rust = screenshot | rust_catalog("rust/snow-shot-mcp/src/domain_schemas.rs")
    assert rust == all_qt, f"Rust-only={rust-all_qt}; Qt-only={all_qt-rust}"
    fixtures = json.loads(read("tests/mcp_contract_fixtures.json"))["fixtures"]
    if MINI:
        fixtures = [case for case in fixtures if case["name"] not in REMOVED_MINI_TOOLS]
    assert {case["name"] for case in fixtures} == screenshot
    assert len(fixtures) == (27 if MINI else 28)
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
    print(f"MCP capability agreement passed: {len(rust)} tools in {len(declarations)} domains, {len(fixtures)} screenshot input contracts and {len(matrix['surface_contracts'])} reviewed UI catalogs.")


if __name__ == "__main__":
    main()
