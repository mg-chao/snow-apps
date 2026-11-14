// Copyright (C) 2026 mg-chao
// SPDX-License-Identifier: Apache-2.0
import fs from 'node:fs';
import path from 'node:path';
import { fileURLToPath } from 'node:url';
const root=path.resolve(path.dirname(fileURLToPath(import.meta.url)),'..');
const built=path.resolve(process.argv[2]??path.join(root,'../build/ant-design-gpui/select-slider-reference'));
const captures=path.resolve(process.argv[3]??path.join(built,'visual-150'));
const browser=JSON.parse(fs.readFileSync(path.join(captures,'browser-metrics.json'),'utf8'));
const names=process.argv[4]?new Set(JSON.parse(fs.readFileSync(process.argv[4],'utf8')).map(c=>c.name)):null;
const provenance=JSON.parse(fs.readFileSync(path.join(built,'browser/provenance.json'),'utf8'));
for(const component of ['select','slider']) {
 const fixture={provenance:{revision:provenance.revision,version:provenance.version,lock_sha256:provenance.lock_sha256,renderer:browser.renderer,
   packages:Object.fromEntries(Object.entries(provenance.packages).filter(([name])=>name).map(([name,p])=>[name,{version:p.version,integrity:p.integrity}])),
   inputs:Object.fromEntries(Object.entries(provenance.inputs).filter(([name])=>name.includes('/components/'+component+'/')||name.endsWith('select_slider_reference.tsx')))},
   cases:browser.cases.filter(c=>c.scenario.component===component&&(!names||names.has(c.scenario.name)))};
 fs.writeFileSync(path.join(root,`fixtures/${component}/visual-reference.json`),JSON.stringify(fixture,null,2)+'\n');
 console.log(`Packed ${fixture.cases.length} ${component} browser reference cases`);
}
