// Copyright (C) 2026 mg-chao
// SPDX-License-Identifier: Apache-2.0
// Preserve the auxiliary Switch matrix, including every backend discrepancy.
import fs from 'node:fs';
import path from 'node:path';
import crypto from 'node:crypto';
import {fileURLToPath} from 'node:url';
import {spawnSync} from 'node:child_process';
const root=path.resolve(path.dirname(fileURLToPath(import.meta.url)),'..');
const options={};for(let i=2;i<process.argv.length;i+=2)options[process.argv[i]]=process.argv[i+1];
const resolve=value=>path.resolve(root,value);
const native=resolve(options['--native']??'../build/ant-design-gpui/form-opacity-native-final');
const build=resolve(options['--build']??'../build/ant-design-gpui/form-native-final');
const browser=resolve('../build/ant-design-gpui/form-opacity-browser');
const digest=file=>crypto.createHash('sha256').update(fs.readFileSync(file)).digest('hex');
const read=file=>JSON.parse(fs.readFileSync(file,'utf8'));
const ensure=(condition,message)=>{if(!condition)throw Error(message);};
const context=read(path.join(build,'capture-context.json'));
ensure(context.source_snapshot_matches_build&&context.pe_stack_reserve_bytes===1048576,'A verified ordinary-stack native build is required.');
ensure(context.executable_sha256===digest(path.join(build,'form_visual-capture.exe')),'The frozen executable changed.');
for(const [file,sha]of Object.entries(context.source_hashes))ensure(digest(resolve(file))===sha,`Source changed: ${file}`);
const source=read(path.join(browser,'browser-metrics.json'));
const measured=read(path.join(native,'native-metrics.json'));
ensure(source.cases.length===12&&measured.length===12,'The complete independent opacity matrix requires 12 real captures.');
ensure(measured.every(sample=>sample.capture_pid!==sample.foreground_pid&&sample.gpu),'A hidden native GPU capture is required.');
const reportPath=path.join(native,'comparison.json');fs.rmSync(reportPath,{force:true});
const compared=spawnSync('python',[resolve('scripts/compare_form_visual.py'),'--browser',browser,'--native',native,'--output',reportPath],{cwd:root,encoding:'utf8'});
ensure(compared.status===0||compared.status===1,'Auxiliary comparison could not run.');
const report=read(reportPath);
ensure(report.comparisons===12&&(compared.status===0)===report.passed,'Auxiliary report is incomplete.');
const result={schema_version:1,passed:report.passed,
  scope:'Auxiliary real original/native Switch compositions over six uniform backdrops, independent of the primary Form matrix. Every strict failure is retained.',
  native_executable_sha256:context.executable_sha256,gpu:measured[0].gpu,
  geometry_passed:report.cases.filter(c=>c.geometry_pass).length,paint_passed:report.cases.filter(c=>c.paint_pass).length,
  actual_captures:12,gates:report.gates,failed_cases:report.failures,
  limitation:'Floating-point GPU composition and Chromium software RGBA8 interpolation can differ by two RGB bytes in solid interiors at some authored opacities. Antialiasing coverage differences remain reported separately. No opacity-specific corrections or threshold changes are applied.',
  cases:report.cases.map(c=>({name:c.name,geometry_pass:c.geometry_pass,paint_pass:c.paint_pass,
    flat_interior_max_error:c.flat_interior_max_error,flat_interior_error_pixels:c.flat_interior_error_pixels,
    coverage_error_pixels:c.coverage_error_pixels,reference_sha256:c.reference_sha256,native_sha256:c.native_sha256})),
  evidence_sha256:{comparison:digest(reportPath),source_metrics:digest(path.join(browser,'browser-metrics.json')),
    native_metrics:digest(path.join(native,'native-metrics.json')),build_context:digest(path.join(build,'capture-context.json')),
    software_backend_probe:digest(resolve('fixtures/form/opacity-backend-reference.json')),
    manifest:digest(resolve('fixtures/form/switch-opacity-cases.json'))}};
const output=resolve('fixtures/form/switch-opacity-validation-windows.json');
fs.writeFileSync(output,JSON.stringify(result,null,2)+'\n');
console.log(JSON.stringify({output,passed:result.passed,captures:12,geometry_passed:result.geometry_passed,
  paint_passed:result.paint_passed,failed_cases:result.failed_cases.length}));
