// Copyright (C) 2026 mg-chao
// SPDX-License-Identifier: Apache-2.0
// Preserve successful, independently measured evidence. Incomplete checks fail closed.
import fs from 'node:fs';
import path from 'node:path';
import crypto from 'node:crypto';
import { execFileSync } from 'node:child_process';
import { fileURLToPath } from 'node:url';
const root = path.resolve(path.dirname(fileURLToPath(import.meta.url)), '..');
const args = Object.fromEntries(process.argv.slice(2).reduce((out, value, index, all) => {
  if (index % 2 === 0) out.push([value.replace(/^--/, ''), all[index + 1]]); return out;
}, []));
const native = path.resolve(args.native ?? path.join(root, '../build/ant-design-gpui/pagination-native'));
const performance = path.resolve(args.performance ?? path.join(root, '../build/windows-msvc-performance/ant-design-gpui/pagination-results.jsonl'));
const output = path.resolve(args.output ?? path.join(root, 'fixtures/pagination/verification.json'));
const read = file => JSON.parse(fs.readFileSync(file, 'utf8').replace(/^\uFEFF/, ''));
const digest = file => crypto.createHash('sha256').update(fs.readFileSync(file)).digest('hex');
const manifest = read(path.join(root, 'fixtures/pagination/visual-cases.json'));
if (new Set(manifest.map(scenario => scenario.name)).size !== manifest.length) throw Error('Visual scenarios are not unique');
const reference = read(path.join(root, 'fixtures/pagination/visual-reference.json'));
const behavior = read(path.join(root, 'fixtures/pagination/behavior-reference.json'));
const rasterAuditPath = path.join(root, '../build/ant-design-gpui/pagination-reference/raster-integrity.json');
const rasterAudit = read(rasterAuditPath);
if (rasterAudit.cases !== manifest.length * 3 || rasterAudit.corrupt_cases !== 0) {
  throw Error('Original PNG integrity audit must cover every scale with zero unexpected foreground cases');
}
const reports = ['100', '150', '200'].map(label => {
  const file = path.join(native, `final-comparison-${label}.json`);
  const report = read(file);
  if (report.cases !== manifest.length || report.failed_surface_checks || report.passed_surface_checks !== report.surface_checks ||
      report.text_layout_checks !== 68 || report.failed_text_layout_checks || report.passed_text_layout_checks !== 68 ||
      report.bidi_checks !== 124 || report.failed_bidi_checks || report.passed_bidi_checks !== 124 ||
      report.custom_label_line_checks !== 16 || report.failed_custom_label_line_checks || report.passed_custom_label_line_checks !== 16 ||
      report.native_form_label_visibility_checks !== 72 || report.failed_native_form_label_visibility_checks || report.passed_native_form_label_visibility_checks !== 72 ||
      report.native_form_translucency_checks !== 24 || report.failed_native_form_translucency_checks || report.passed_native_form_translucency_checks !== 24) {
    throw Error(`Cannot package incomplete visual evidence at ${label}%: ${report.failed_surface_checks} surface, ${report.failed_text_layout_checks} total-line, ${report.failed_bidi_checks} bidi failures`);
  }
  return { scale: Number(label) / 100, cases: report.cases, surface_checks: report.surface_checks,
    passed_surface_checks: report.passed_surface_checks, failed_surface_checks: report.failed_surface_checks,
    total_label_line_checks: report.text_layout_checks, passed_total_label_line_checks: report.passed_text_layout_checks,
    bidi_shaping_checks: report.bidi_checks, passed_bidi_shaping_checks: report.passed_bidi_checks,
    custom_label_line_checks: report.custom_label_line_checks, passed_custom_label_line_checks: report.passed_custom_label_line_checks,
    native_form_label_visibility_checks: report.native_form_label_visibility_checks,
    passed_native_form_label_visibility_checks: report.passed_native_form_label_visibility_checks,
    native_form_translucency_checks: report.native_form_translucency_checks,
    passed_native_form_translucency_checks: report.passed_native_form_translucency_checks,
    mean_per_case_pixel_channel_error: report.results.reduce((sum, result) => sum + result.pixel_mean_absolute_channel_error, 0) / report.results.length,
    max_case_pixel_channel_error: Math.max(...report.results.map(result => result.pixel_mean_absolute_channel_error)),
    mean_per_case_foreground_channel_error: report.results.reduce((sum, result) => sum + result.foreground_mean_absolute_channel_error, 0) / report.results.length,
    excluded_from_surface_pass: report.excluded_from_surface_pass, report_sha256: digest(file) };
});
const results = fs.readFileSync(performance, 'utf8').replace(/^\uFEFF/, '').trim().split(/\r?\n/).map(line => JSON.parse(line));
if (results.length !== 14 || new Set(results.map(result => result.name)).size !== 14 || results.some(result =>
  result.name.endsWith('/builder') ? result.allocations !== 0 || result.allocated_bytes !== 0 :
  (typeof result.max_items_per_render_sequence !== 'number' || typeof result.max_render_sequences_per_operation !== 'number' ||
   result.max_items_per_render_sequence > 11 || result.max_render_sequences_per_operation > 1))) {
  throw Error('Performance evidence must contain all 14 successful bounded-render measurements');
}
const sourceFiles = [
  ...fs.readdirSync(path.join(root, 'src/pagination')).filter(file => file.endsWith('.rs')).map(file => `src/pagination/${file}`),
  'src/config.rs', 'src/locale.rs', 'src/lib.rs', 'Cargo.toml', 'Cargo.lock',
  'examples/pagination_visual.rs', 'benches/pagination.rs', 'scripts/compare_pagination_visual.py',
  'scripts/capture_pagination_text_reference.mjs', 'scripts/pack_pagination_text_reference.mjs',
  'scripts/audit_pagination_reference.py',
  'scripts/probe_pagination_go_paint.mjs', 'scripts/pack_pagination_native_form.mjs',
];
const summary = {
  validated_at_utc: new Date().toISOString(),
  validation_date: args.date ?? new Intl.DateTimeFormat('en-CA', { timeZone: 'Asia/Shanghai', year: 'numeric', month: '2-digit', day: '2-digit' }).format(new Date()),
  validation_timezone: 'Asia/Shanghai', platform: process.platform, architecture: process.arch,
  toolchain: { rustc: execFileSync('rustc', ['-Vv'], { encoding: 'utf8' }).trim(),
    cargo: execFileSync('cargo', ['--version'], { encoding: 'utf8' }).trim(), node: process.version,
    python: execFileSync('python', ['--version'], { encoding: 'utf8' }).trim() },
  reference: { version: reference.provenance.version, revision: reference.provenance.revision,
    renderer: reference.provenance.renderer, rc_pagination: '1.4.0',
    lock_sha256: reference.provenance.lock_sha256, visual_fixture_sha256: digest(path.join(root, 'fixtures/pagination/visual-reference.json')),
    raster_integrity: { cases: rasterAudit.cases, unexpected_foreground_cases: rasterAudit.corrupt_cases, report_sha256: digest(rasterAuditPath) },
    native_form_paint_probes: { actual_backdrop_samples: 30, computed_cursor_states: 12,
      fixture_sha256: digest(path.join(root, 'fixtures/pagination/native-form-reference.json')) },
    capture_repairs: read(path.join(root, 'fixtures/pagination/capture-repairs.json')).repairs },
  behavior: { browser_rendered_models: behavior.models.length, browser_interaction_traces: behavior.interactions.length,
    fixture_sha256: digest(path.join(root, 'fixtures/pagination/behavior-reference.json')) },
  visual: { native_cases: reports.reduce((sum, report) => sum + report.cases, 0),
    total_surface_checks: reports.reduce((sum, report) => sum + report.surface_checks, 0),
    total_label_line_checks: reports.reduce((sum, report) => sum + report.total_label_line_checks, 0),
    bidi_shaping_checks: reports.reduce((sum, report) => sum + report.bidi_shaping_checks, 0),
    custom_label_line_checks: reports.reduce((sum, report) => sum + report.custom_label_line_checks, 0),
    native_form_label_visibility_checks: reports.reduce((sum, report) => sum + report.native_form_label_visibility_checks, 0),
    native_form_translucency_checks: reports.reduce((sum, report) => sum + report.native_form_translucency_checks, 0),
    original_text_fixture_sha256: digest(path.join(root, 'fixtures/pagination/text-reference.json')),
    failed_surface_checks: 0, budgets: { geometry_logical_px: 1, rgb_rounding_bytes: 1, alpha_rounding: .001 },
    scales: reports, native_binary_sha256: digest(path.join(root, '../build/ant-design-gpui/debug/examples/pagination_visual.exe')) },
  performance: { profile: 'Release', preset: 'windows-msvc-performance', scope: 'Only Pagination benchmark',
    measurements: 'CPU/allocation distributions using GPUI deterministic offscreen text; GPU presentation latency is not measured',
    idle_animation_frames: 0, results_sha256: digest(performance), results },
  sources_sha256: Object.fromEntries(sourceFiles.map(file => [file, digest(path.join(root, file))])),
};
fs.writeFileSync(output, JSON.stringify(summary, null, 2) + '\n');
console.log(`Packaged ${summary.visual.native_cases} native visual cases, ${summary.visual.total_surface_checks} passing surface checks, and ${results.length} Release measurements`);
