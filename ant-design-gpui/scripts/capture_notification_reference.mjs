// Copyright (C) 2026 mg-chao
// SPDX-License-Identifier: Apache-2.0
// Reuse the repository's isolated headless-browser transport for notification fixtures.
import fs from 'node:fs';
import path from 'node:path';
import {fileURLToPath} from 'node:url';
import {execFileSync} from 'node:child_process';
const args=Object.fromEntries(process.argv.slice(2).reduce((out,v,i,all)=>{if(i%2===0)out.push([v.replace(/^--/,''),all[i+1]]);return out;},[]));
const root=path.resolve(path.dirname(fileURLToPath(import.meta.url)),'..');
const output=path.resolve(args.output??path.join(root,'../build/ant-design-gpui/notification-browser'));
const reference=path.resolve(args.reference??path.join(root,'../build/ant-design-gpui/notification-reference'));
const scenes=JSON.parse(fs.readFileSync(path.join(root,'fixtures/notification/visual-cases.json'),'utf8'));
fs.mkdirSync(output,{recursive:true});
const records=[];let renderer;
for(const scale of args.scale?[Number(args.scale)]:[1,1.5,2]){
  const cases=[];
  for(const [scene,sample] of Object.entries(scenes)){
    if(args.scene&&args.scene!==scene)continue;
    for(const theme of args.theme?[args.theme]:['default','dark','compact','dark-compact'])
    for(const direction of args.direction?[args.direction]:['ltr','rtl'])
      cases.push({name:`${scene}-${theme}-${direction}-${scale*100}`,scene,theme,direction,viewportWidth:sample.width??740,viewportHeight:sample.height??1000});
  }
  const file=path.join(output,`cases-${scale*100}.json`);fs.writeFileSync(file,JSON.stringify(cases));
  const directory=path.join(output,String(scale*100));
  execFileSync(process.execPath,[path.join(root,'scripts/capture_modal_reference.mjs'),'--cases',file,'--output',directory,'--reference',reference,'--scale',String(scale)],{stdio:'inherit',windowsHide:true});
  const captured=JSON.parse(fs.readFileSync(path.join(directory,'browser-metrics.json'),'utf8'));renderer=captured.renderer;
  for(const {scenario,metrics} of captured.cases){records.push({name:scenario.name,...metrics});
    fs.copyFileSync(path.join(directory,`${scenario.name}.png`),path.join(output,`${scenario.name}.png`));}
}
fs.writeFileSync(path.join(output,'browser-metrics.json'),JSON.stringify({renderer,provenance:JSON.parse(fs.readFileSync(path.join(reference,'provenance.json'),'utf8')),cases:records},null,2)+'\n');
