// Copyright (C) 2026 mg-chao
// SPDX-License-Identifier: Apache-2.0
// Compact evidence index, based only on completed independent source checks.
import fs from 'node:fs';
import path from 'node:path';
import crypto from 'node:crypto';
import {fileURLToPath} from 'node:url';
import {execFileSync} from 'node:child_process';
const root=path.resolve(path.dirname(fileURLToPath(import.meta.url)),'..');
const digest=file=>crypto.createHash('sha256').update(fs.readFileSync(file)).digest('hex');
const auditPath=path.join(root,'../build/ant-design-gpui/form-browser/audit.json');
const audit=JSON.parse(fs.readFileSync(auditPath,'utf8'));
const visualPath=path.join(root,'fixtures/form/visual-reference.json');
const visual=JSON.parse(fs.readFileSync(visualPath,'utf8'));
const behaviorPath=path.join(root,'fixtures/form/behavior-reference.json');
const behavior=JSON.parse(fs.readFileSync(behaviorPath,'utf8'));
const cases=JSON.parse(fs.readFileSync(path.join(root,'fixtures/form/visual-cases.json'),'utf8'));
if(!audit.passed||!audit.native_browser_dpi_verified||audit.cases!==Object.keys(cases).length*24||audit.records.some(record=>record.outside_foreground_pixels!==0))throw Error('Incomplete source audit');
if(audit.metrics_sha256!==digest(path.join(root,'../build/ant-design-gpui/form-browser/browser-metrics.json'))
  ||audit.metrics_sha256!==visual.full_capture.metrics_sha256)throw Error('Source audit is stale for the current capture metrics');
const negative=JSON.parse(execFileSync('python',[path.join(root,'scripts/test_form_visual_comparator.py')],{cwd:root,encoding:'utf8'}));
if(!negative.synthetic_baseline||negative.negative_controls_passed!==6||negative.coverage_boundary_controls_passed!==2)throw Error('Comparator negative controls failed');
const packages=JSON.parse(fs.readFileSync(path.join(root,'fixtures/form/reference-packages.json'),'utf8'));
const inheritancePath=path.join(root,'fixtures/form/inheritance-reference.json');
const inheritance=JSON.parse(fs.readFileSync(inheritancePath,'utf8'));
const dpiPath=path.join(root,'fixtures/form/dpi-reference.json');
const textareaPath=path.join(root,'fixtures/form/textarea-resize-reference.json');
const opacityPath=path.join(root,'fixtures/form/switch-opacity-reference.json');
const opacityBackendPath=path.join(root,'fixtures/form/opacity-backend-reference.json');
const textarea=JSON.parse(fs.readFileSync(textareaPath,'utf8'));
const opacity=JSON.parse(fs.readFileSync(opacityPath,'utf8'));
const inheritancePackages=JSON.parse(fs.readFileSync(path.join(root,'fixtures/form/inheritance-packages.json'),'utf8'));
for(const info of [...packages.packages,...inheritancePackages.packages])for(const file of info.files)if(digest(path.join(root,'fixtures/form/licenses',file.name))!==file.sha256)throw Error(`Changed license ${file.name}`);
const result={schema_version:1,scope:'Original-source reference evidence and comparator self-checks; native visual parity is reported separately.',
  upstream:{revision:visual.provenance.revision,version:visual.provenance.version,rc_form:visual.provenance.rc_form},
  renderer:audit.renderer,matrix:{scenes:Object.keys(cases).length,themes:['default','dark','compact','dark-compact'],directions:['ltr','rtl'],
    scales:[1,1.5,2],actual_captures:audit.cases,compact_metric_cases:visual.cases.length},
  original_source_audit:{passed:audit.passed,native_browser_dpi_verified:audit.native_browser_dpi_verified===true,verified_input_hashes:audit.source_files_checked,verified_screenshot_hashes:audit.records.length,
    raster_outside_forms:0,fractional_viewport_edge_cases:audit.records.filter(record=>record.fractional_viewport_edge).length,audit_sha256:digest(auditPath)},
  behavior:{independent_traces:behavior.traces.length,sha256:digest(behaviorPath)},
  inheritance:{configurations:inheritance.scenarios.length,controls:inheritance.scenarios[0].controls.length,
    label_pointer_actions:inheritance.label_clicks.length-1,sha256:digest(inheritancePath)},
  supplemental_source:{native_browser_dpi_proof_sha256:digest(dpiPath),
    required_mark_typography_sha256:digest(path.join(root,'fixtures/form/required-mark-reference.json')),
    textarea_resize:{real_pointer_traces:textarea.cases.length,sha256:digest(textareaPath)},
    switch_opacity:{actual_captures:opacity.captures,actual_pixel_probes:opacity.cases.length,sha256:digest(opacityPath),
      software_compositor_model_sha256:digest(opacityBackendPath)}},
  licenses:{loaded_packages:packages.packages.length,inheritance_loaded_packages:inheritancePackages.packages.length,
    unique_loaded_packages:new Set([...packages.packages,...inheritancePackages.packages].map(info=>info.name+'@'+info.version)).size,
    all_recorded_license_hashes_verified:true},
  comparator_negative_controls:negative,visual_fixture_sha256:digest(visualPath)};
fs.writeFileSync(path.join(root,'fixtures/form/reference-validation.json'),JSON.stringify(result,null,2)+'\n');
console.log(JSON.stringify(result));
