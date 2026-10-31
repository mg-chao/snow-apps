// Copyright (C) 2026 mg-chao
// SPDX-License-Identifier: Apache-2.0
// Replace focused reference cases while preserving complete original provenance.
import fs from 'node:fs';
import path from 'node:path';
import crypto from 'node:crypto';
const args=Object.fromEntries(process.argv.slice(2).reduce((a,v,i,all)=>{if(i%2===0)a.push([v.replace(/^--/,''),all[i+1]]);return a;},[]));
if(!args.supplement||!args.id||!/^[a-z0-9-]+$/.test(args.id))throw Error('Require --supplement <directory> and a simple --id');
const base=path.resolve(args.base??'../build/ant-design-gpui/date-time-picker-reference/browser-capture');
const supplement=path.resolve(args.supplement);
const read=(directory,name)=>JSON.parse(fs.readFileSync(path.join(directory,name),'utf8'));
const original=read(base,'browser-metrics.json'),update=read(supplement,'browser-metrics.json');
if(update.probe_overrides)throw Error('Isolated probe overrides must not replace official manifest cases');
for(const key of ['clock','timezone'])if(original[key]!==update[key])throw Error(`Reference ${key} changed`);
for(const key of ['version','revision','rc_picker','scene_sha256','lock_sha256'])if(original.provenance[key]!==update.provenance[key])throw Error(`Reference provenance ${key} changed`);
const oldCases=new Map(original.cases.map(c=>[c.name,c]));
if(oldCases.size!==original.cases.length||new Set(update.cases.map(c=>c.name)).size!==update.cases.length)throw Error('Duplicate cases');
const png=name=>{if(!/^[a-z0-9.-]+$/.test(name))throw Error(`Invalid case name ${name}`);return `${name}.png`;};
for(const c of update.cases){
  if(!oldCases.has(c.name))throw Error(`Supplement case absent from baseline: ${c.name}`);
  if(!fs.existsSync(path.join(supplement,png(c.name))))throw Error(`Missing supplemental PNG ${c.name}`);
}
const backup=path.join(base,`replaced-${args.id}`);
fs.mkdirSync(backup,{recursive:true});
for(const name of ['browser-metrics.json','image-manifest.json']){
  const destination=path.join(backup,name);
  if(fs.existsSync(destination))throw Error(`Capture ID already used: ${args.id}`);
  fs.copyFileSync(path.join(base,name),destination,fs.constants.COPYFILE_EXCL);
}
const captures=original.captures??[{id:'baseline',completed_utc:original.completed_utc,renderer:original.renderer,provenance:original.provenance,
  metrics_sha256:crypto.createHash('sha256').update(fs.readFileSync(path.join(base,'browser-metrics.json'))).digest('hex')}];
if(args['previous-capture-script']){
  const script=fs.readFileSync(path.resolve(args['previous-capture-script']));
  const historicalSettings={script_sha256:crypto.createHash('sha256').update(script).digest('hex'),
    hide_scrollbars:script.toString('utf8').includes("'--hide-scrollbars'"),reconstructed_from_preserved_script:true};
  for(const capture of captures)if(!capture.capture_settings)capture.capture_settings=historicalSettings;
}
if(captures.some(c=>c.id===args.id))throw Error(`Duplicate capture ID ${args.id}`);
captures.push({id:args.id,completed_utc:update.completed_utc,renderer:update.renderer,provenance:update.provenance,cases:update.cases.map(c=>c.name),
  capture_settings:update.capture_settings,
  metrics_sha256:crypto.createHash('sha256').update(fs.readFileSync(path.join(supplement,'browser-metrics.json'))).digest('hex')});
for(const c of update.cases){
  fs.copyFileSync(path.join(base,png(c.name)),path.join(backup,png(c.name)),fs.constants.COPYFILE_EXCL);
  fs.copyFileSync(path.join(supplement,png(c.name)),path.join(base,png(c.name)));
  oldCases.set(c.name,{...c,capture_id:args.id});
}
const result={...original,completed_utc:update.completed_utc,captures,cases:original.cases.map(c=>oldCases.get(c.name))};
fs.writeFileSync(path.join(base,'browser-metrics.json'),JSON.stringify(result,null,2)+'\n');
const images=result.cases.map(c=>({file:png(c.name),sha256:crypto.createHash('sha256').update(fs.readFileSync(path.join(base,png(c.name)))).digest('hex')}));
fs.writeFileSync(path.join(base,'image-manifest.json'),JSON.stringify(images,null,2)+'\n');
console.log(`Merged ${update.cases.length} focused reference cases with preserved baseline and supplemental provenance`);
