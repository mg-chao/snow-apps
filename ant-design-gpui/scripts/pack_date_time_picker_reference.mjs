// Copyright (C) 2026 mg-chao
// SPDX-License-Identifier: Apache-2.0
// Compact independent 100% baselines for offline native tests.
import fs from 'node:fs';
import path from 'node:path';
import crypto from 'node:crypto';
const args=Object.fromEntries(process.argv.slice(2).reduce((a,v,i,all)=>{if(i%2===0)a.push([v.replace(/^--/,''),all[i+1]]);return a;},[]));
const input=path.resolve(args.input??'../build/ant-design-gpui/date-time-picker-reference/browser-capture/browser-metrics.json');
const output=path.resolve(args.output??'fixtures/date_time_picker/style-reference.json');
const data=JSON.parse(fs.readFileSync(input,'utf8'));
const cases=data.cases.filter(c=>c.scale===1);
if(!cases.length)throw Error('No 100% reference cases');
const names=new Set();for(const c of cases){if(names.has(c.name))throw Error(`Duplicate ${c.name}`);names.add(c.name);}
fs.mkdirSync(path.dirname(output),{recursive:true});
const rect=value=>value==null?null:Object.fromEntries(['x','y','width','height','color','background','borderColor','borderWidth','borderRadius','value','scrollTop','visibility','opacity','auxiliary'].filter(key=>value[key]!==undefined).map(key=>[key,value[key]]));
const box=value=>value==null?null:Object.fromEntries(['x','y','width','height'].map(key=>[key,value[key]]));
const compactGeometry=geometry=>Object.fromEntries(Object.entries(geometry).map(([key,value])=>[key,
  key==='tags'?value.map(tag=>({key:tag.key,label:tag.label,rect:rect(tag.rect),content:rect(tag.content),remove:rect(tag.remove)})):
  key==='footerActions'?value.map(action=>({key:action.key,label:action.label,rect:rect(action.rect)})):
  key==='scrollbars'?value.map(bar=>({column:bar.column,track:box(bar.track),side:bar.side,scrollTop:bar.scrollTop,scrollHeight:bar.scrollHeight,clientHeight:bar.clientHeight})):
  key==='cells'||key==='timeCells'?value.map(cell=>({...Object.fromEntries(['key','text','selected','today','disabled','inView','inRange','rangeStart','rangeEnd','rangeHover','weekSelected','weekRangeStart','weekRangeEnd','weekRangeHover','weekHover','column','value'].filter(k=>cell[k]!==undefined).map(k=>[k,cell[k]])),rect:box(cell.rect),inner:box(cell.inner),
    ...(cell.selected||cell.today||cell.disabled||cell.inRange||cell.rangeStart||cell.rangeEnd?{color:cell.inner?.color,background:cell.inner?.background,rangeBackground:cell.before?.background,todayBorder:cell.today?cell.innerBefore?.borderColor:undefined}:{})})):
  Array.isArray(value)?value.map(rect):rect(value)]));
fs.writeFileSync(output,JSON.stringify({schema_version:1,clock:data.clock,timezone:data.timezone,upstream:{version:data.provenance.version,
  revision:data.provenance.revision,rc_picker:data.provenance.rc_picker},scene_sha256:data.provenance.scene_sha256,
  capture_sha256:crypto.createHash('sha256').update(fs.readFileSync(input)).digest('hex'),captures:data.captures,renderer:data.renderer,
  cases:cases.map(({name,scene,theme,direction,options,tokens,geometry})=>({name,scene,theme,direction,options,tokens,geometry:compactGeometry(geometry)}))},null,0)+'\n');
console.log(`Packed ${cases.length} independent date/time picker reference cases`);
