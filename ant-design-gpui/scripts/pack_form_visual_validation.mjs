// Copyright (C) 2026 mg-chao
// SPDX-License-Identifier: Apache-2.0
// Attest a complete real native/reference matrix without hiding failed gates.
import fs from 'node:fs';
import path from 'node:path';
import crypto from 'node:crypto';
import {fileURLToPath} from 'node:url';
import {spawnSync} from 'node:child_process';

const root=path.resolve(path.dirname(fileURLToPath(import.meta.url)),'..');
const options={};
for(let index=2;index<process.argv.length;index+=2)options[process.argv[index]]=process.argv[index+1];
const resolve=value=>path.resolve(root,value);
const native=resolve(options['--native']??'../build/ant-design-gpui/form-native');
const browser=resolve(options['--browser']??'../build/ant-design-gpui/form-browser');
const output=resolve(options['--output']??'fixtures/form/visual-validation-windows.json');
const read=file=>JSON.parse(fs.readFileSync(file,'utf8'));
const digest=file=>crypto.createHash('sha256').update(fs.readFileSync(file)).digest('hex');
const requireThat=(condition,message)=>{if(!condition)throw Error(message);};
const contextPath=path.join(native,'capture-context.json');
const context=read(contextPath);
requireThat(context.source_snapshot_matches_build===true,'A coherent source-verified build is required; SkipBuild captures cannot be attested.');
requireThat(context.label_geometry_source==='logical-prepaint'&&JSON.stringify(context.features)===JSON.stringify(['test-support']),'Final label geometry must use the explicitly recorded passive inspection feature.');
requireThat(context.pe_stack_reserve_bytes===1048576,'The native executable must use the ordinary Windows 1 MiB main stack.');
requireThat(context.executable_sha256===digest(path.join(native,'form_visual-capture.exe')),'The frozen native executable changed.');
for(const [file,hash]of Object.entries(context.source_hashes))requireThat(digest(resolve(file))===hash,`Source changed after the capture build: ${file}`);
const sourceList=spawnSync('rg',['--files','src'],{cwd:root,encoding:'utf8'});
requireThat(sourceList.status===0,'Cannot enumerate the current native sources.');
for(const file of sourceList.stdout.trim().split(/\r?\n/).filter(file=>file.endsWith('.rs')))
  requireThat(context.source_hashes[file.replaceAll('\\','/')]!==undefined,`Source was added after the capture build: ${file}`);
requireThat(context.capture_script_sha256===digest(resolve('scripts/capture-form-native.ps1')),'The native capture script changed.');
const inspectionPath=resolve('fixtures/form/instrumentation-validation-windows.json');
const inspection=read(inspectionPath);
requireThat(inspection.passed&&inspection.cases===19&&inspection.instrumented_executable_sha256===context.executable_sha256,'Passive inspection pixel equivalence is missing or stale for this executable.');
for(const record of inspection.records)requireThat(record.instrumented_png_sha256===digest(path.join(native,`${record.name}.png`)),`Passive inspection image proof is stale: ${record.name}`);
const manifest=read(resolve('fixtures/form/visual-cases.json'));
const source=read(path.join(browser,'browser-metrics.json'));
const measured=read(path.join(native,'native-metrics.json'));
const sceneNames=Object.keys(manifest);
const count=sceneNames.length*4*2*3;
requireThat(source.cases.length===count&&measured.length===count,'Both captures must contain the complete scene/theme/direction/DPI matrix.');
requireThat(measured.every(sample=>sample.capture_pid!==sample.foreground_pid),'The hidden capture became the foreground application.');
requireThat(measured.every(sample=>sample.label_geometry_source==='logical-prepaint'),'Label measurements must use actual native logical prepaint bounds.');
requireThat(measured[0].gpu&&measured.every(sample=>JSON.stringify(sample.gpu)===JSON.stringify(measured[0].gpu)),'Native GPU identity is missing or changed during capture.');
const audit=read(path.join(browser,'audit.json'));
requireThat(audit.passed&&audit.native_browser_dpi_verified&&audit.cases===count&&audit.metrics_sha256===digest(path.join(browser,'browser-metrics.json')),'Original-source audit is missing, failed or stale.');
const reportPath=path.join(native,'comparison.json');
fs.rmSync(reportPath,{force:true});
const comparison=spawnSync('python',[resolve('scripts/compare_form_visual.py'),'--browser',browser,'--native',native,'--output',reportPath],{cwd:root,encoding:'utf8'});
requireThat(comparison.status===0||comparison.status===1,`Comparator could not run: ${comparison.stderr}`);
const report=read(reportPath);
requireThat((comparison.status===0)===report.passed,'Comparator exit status and report disagree.');
requireThat(report.comparisons===count&&report.cases.length===count,'Comparator did not verify the complete matrix.');
requireThat(report.gates.geometry_logical_px===1&&report.gates.solid_channel_error_bytes===1,'Comparator thresholds changed.');
const negative=spawnSync('python',[resolve('scripts/test_form_visual_comparator.py')],{cwd:root,encoding:'utf8'});
requireThat(negative.status===0,`Comparator negative controls failed: ${negative.stderr}`);
const controls=JSON.parse(negative.stdout);
requireThat(controls.synthetic_baseline&&controls.negative_controls_passed===6&&controls.coverage_boundary_controls_passed===2,'Comparator negative controls are incomplete.');
const summaries=sceneNames.map(scene=>{
  const names=new Set(source.cases.filter(sample=>sample.scene===scene).map(sample=>sample.name));
  const cases=report.cases.filter(sample=>names.has(sample.name));
  return {scene,captures:cases.length,geometry_passed:cases.filter(sample=>sample.geometry_pass).length,
    paint_passed:cases.filter(sample=>sample.paint_pass).length,
    maximum_flat_channel_error:Math.max(...cases.map(sample=>sample.flat_interior_max_error)),
    maximum_flat_error_pixels:Math.max(...cases.map(sample=>sample.flat_interior_error_pixels)),
    maximum_coverage_error_pixels:Math.max(...cases.map(sample=>sample.coverage_error_pixels))};
});
const result={schema_version:1,passed:report.passed,
  scope:'Actual GPUI Form renders compared with the pinned original Ant Design Form. All failures are retained. Glyph rasterization is excluded from solid-paint gates; raw image differences remain in the complete report.',
  upstream:{revision:source.provenance.revision,version:source.provenance.version,rc_form:source.provenance.rc_form},
  original_renderer:source.renderer,native:{os:context.os,architecture:context.architecture,pe_stack_reserve_bytes:context.pe_stack_reserve_bytes,
    features:context.features,label_geometry_source:context.label_geometry_source,
    gpu:measured[0].gpu,
    executable_sha256:context.executable_sha256,source_snapshot_matches_build:true,source_files_verified:Object.keys(context.source_hashes).length,
    hidden_capture_never_foreground:true,started_utc:context.started_utc},
  matrix:{scenes:sceneNames.length,themes:['default','dark','compact','dark-compact'],directions:['ltr','rtl'],scales:[1,1.5,2],actual_captures:count},
  gates:report.gates,geometry_passed:report.cases.filter(sample=>sample.geometry_pass).length,
  paint_passed:report.cases.filter(sample=>sample.paint_pass).length,failed_cases:report.failures,scenes:summaries,
  comparator_negative_controls:controls,
  passive_inspection:{pixel_equivalence_captures:inspection.cases,sha256:digest(inspectionPath)},
  evidence_sha256:{original_metrics:digest(path.join(browser,'browser-metrics.json')),original_audit:digest(path.join(browser,'audit.json')),
    native_metrics:digest(path.join(native,'native-metrics.json')),native_context:digest(contextPath),comparison:digest(reportPath),
    comparator:digest(resolve('scripts/compare_form_visual.py')),native_capture:digest(resolve('examples/form_visual.rs'))}};
fs.writeFileSync(output,JSON.stringify(result,null,2)+'\n');
console.log(JSON.stringify({output:path.relative(root,output),passed:result.passed,captures:count,geometry_passed:result.geometry_passed,
  paint_passed:result.paint_passed,failed_cases:result.failed_cases.length}));
