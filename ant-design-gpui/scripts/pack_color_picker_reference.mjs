// Copyright (C) 2026 mg-chao
// SPDX-License-Identifier: Apache-2.0
// Preserve compact, independently measured baseline geometry for offline tests.
import fs from 'node:fs';
import path from 'node:path';
import crypto from 'node:crypto';
import {fileURLToPath} from 'node:url';

const args=Object.fromEntries(process.argv.slice(2).reduce((a,v,i,all)=>{if(i%2===0)a.push([v.replace(/^--/,''),all[i+1]]);return a;},[]));
const input=path.resolve(args.input??'../build/ant-design-gpui/color-picker-browser/browser-metrics.json');
const output=path.resolve(args.output??'fixtures/color_picker/style-reference.json');
const capture=JSON.parse(fs.readFileSync(input,'utf8'));
const scenePath=path.resolve(path.dirname(fileURLToPath(import.meta.url)),'../fixtures/color_picker/visual-cases.json');
const sceneCount=Object.keys(JSON.parse(fs.readFileSync(scenePath,'utf8'))).length;
const rect=value=>value==null?null:{...Object.fromEntries(['x','y','width','height','background','borderColor','borderRadius'].map(key=>[key,value[key]])),...(value.kind===undefined?{}:{kind:value.kind,index:value.index})};
const thumb=value=>value?{...rect(value),borderWidth:value.borderWidth}:null;
const cases=capture.cases.filter(c=>c.scale===1).map(c=>({scene:c.scene,theme:c.theme,direction:c.direction,
  tokens:c.tokens,rows:c.rows.map(r=>({options:r.options,trigger:rect(r.trigger),swatch:rect(r.swatch),
    content:r.swatch?thumb(c.elements.find(e=>e.className.split(' ').includes('ant-color-picker-color-block-inner')&&e.x>=r.swatch.x&&e.y>=r.swatch.y&&e.x<r.swatch.x+r.swatch.width&&e.y<r.swatch.y+r.swatch.height)):null})),
  controls:{paletteThumb:thumb(c.elements.find(e=>e.className==='ant-color-picker-handler')),
    railThumbs:c.elements.filter(e=>e.className.split(' ').includes('ant-color-picker-slider-handle')).map(e=>({width:e.width,height:e.height,outerWidth:parseFloat(e.after.width),outerHeight:parseFloat(e.after.height),borderWidth:parseFloat(e.after.borderWidth)}))},
  geometry:Object.fromEntries(Object.entries(c.geometry).map(([key,value])=>[key,Array.isArray(value)?value.map(rect):rect(value)]))}));
const expected=sceneCount*8;
if(cases.length!==expected||new Set(cases.map(c=>`${c.scene}-${c.theme}-${c.direction}`)).size!==expected)throw Error(`Expected all${sceneCount} scenes ×4 themes ×2 directions at100% (${expected} cases)`);
const digest=file=>crypto.createHash('sha256').update(fs.readFileSync(file)).digest('hex');
const sourceHashes=Object.fromEntries(Object.entries(capture.provenance.inputs).filter(([file])=>file.replaceAll('\\','/').includes('/color-picker/')));
fs.mkdirSync(path.dirname(output),{recursive:true});
fs.writeFileSync(output,JSON.stringify({schema_version:1,upstream:{version:capture.provenance.version,revision:capture.provenance.revision,rc_color_picker:capture.provenance.rc_color_picker},renderer:capture.renderer,
  generator_sha256:digest(fileURLToPath(import.meta.url)),capture_sha256:digest(input),lock_sha256:capture.provenance.lock_sha256,sources:sourceHashes,cases},null,2)+'\n');
console.log(`Packed ${cases.length} independently measured ColorPicker baselines`);
