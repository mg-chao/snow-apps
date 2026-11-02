// Copyright (C) 2026 mg-chao
// SPDX-License-Identifier: Apache-2.0
// Isolated original-source browser; screenshots remain in the ignored build tree.
import fs from 'node:fs/promises';
import path from 'node:path';
import {fileURLToPath,pathToFileURL} from 'node:url';
import crypto from 'node:crypto';
import {setTimeout as delay} from 'node:timers/promises';
import {browser} from './pagination_browser.mjs';

const args = Object.fromEntries(process.argv.slice(2).reduce((all,value,index,values)=>{
  if(index%2===0)all.push([value.replace(/^--/,''),values[index+1]]);return all;
},[]));
const root = path.resolve(path.dirname(fileURLToPath(import.meta.url)),'..');
const output = path.resolve(args.output ?? path.join(root,'../build/ant-design-gpui/form-browser'));
const reference = path.resolve(args.reference ?? path.join(root,'../build/ant-design-gpui/form-reference'));
const scenes = JSON.parse(await fs.readFile(path.resolve(args.cases??path.join(root,'fixtures/form/visual-cases.json')),'utf8'));
const digest = bytes=>crypto.createHash('sha256').update(bytes).digest('hex');
const selected = [];
for (const scene of Object.keys(scenes)) for (const theme of ['default','dark','compact','dark-compact'])
  for (const direction of ['ltr','rtl']) for (const scale of [1,1.5,2]) {
    if(args.scene&&args.scene!==scene||args.theme&&args.theme!==theme||args.direction&&args.direction!==direction
      ||args.scale&&Number(args.scale)!==scale)continue;
    selected.push({scene,theme,direction,scale,name:`${scene}-${theme}-${direction}-${scale*100}`});
  }
if(!selected.length)throw Error('No Form scenarios selected');
const results=[];
let renderer;
for(const scale of [...new Set(selected.map(scenario=>scenario.scale))]){
  // Native Chromium DPI controls CSS border snapping. CDP raster emulation on
  // a force-1 process reports the requested DPR but retains force-1 CSS borders.
  const b=await browser({output,scale,edge:args.browser??'C:/Program Files/Google/Chrome/Application/chrome.exe'});
  try {
    if(renderer&&renderer!==b.renderer)throw Error('Browser version changed during the matrix');
    renderer=b.renderer;
    const launchDpr=await b.evaluate('devicePixelRatio');
    if(launchDpr!==scale)throw Error(`Native browser launch DPI differs: ${launchDpr} vs ${scale}`);
    for(const scenario of selected.filter(scenario=>scenario.scale===scale)) {
    const url=pathToFileURL(path.join(reference,'index.html'));
    for(const key of ['scene','theme','direction'])url.searchParams.set(key,scenario[key]);
    const {width,height}=scenes[scenario.scene].viewport;
    await b.page('Emulation.setDeviceMetricsOverride',{width,height,deviceScaleFactor:scenario.scale,mobile:false});
    const navigation=await b.page('Page.navigate',{url:url.href});
    if(navigation.errorText)throw Error(navigation.errorText);
    let metrics;
    for(let i=0;i<300;i++) {
      const content=await b.evaluate(`location.href===${JSON.stringify(url.href)}?document.querySelector('#visual-ready')?.textContent:undefined`);
      if(content){metrics=JSON.parse(content);break;}await delay(20);
    }
    if(!metrics||metrics.forms.length!==scenes[scenario.scene].forms.length)throw Error(`Incomplete ${scenario.name}`);
    if(metrics.scale!==scenario.scale)throw Error(`Wrong actual device scale ${metrics.scale}`);
    const {data}=await b.page('Page.captureScreenshot',{format:'png',fromSurface:true,clip:{x:0,y:0,width,height,scale:1}});
    const bytes=Buffer.from(data,'base64');await fs.writeFile(path.join(output,`${scenario.name}.png`),bytes);
    results.push({...scenario,...metrics,browser_launch_scale:scale,browser_launch_device_pixel_ratio:launchDpr,png_sha256:digest(bytes)});
    if(results.length%24===0||results.length===selected.length)console.log(`Captured ${results.length}/${selected.length} Form scenarios`);
    }
  }finally{await b.close();}
}
  const provenance={...JSON.parse(await fs.readFile(path.join(reference,'provenance.json'),'utf8')),
    capture_generator_sha256:digest(await fs.readFile(fileURLToPath(import.meta.url))),
    dpi_mode:'one native --force-device-scale-factor process per actual device scale; matching CDP viewport override'};
  if(args.append==='true'){
    const old=JSON.parse(await fs.readFile(path.join(output,'browser-metrics.json'),'utf8'));
    if(JSON.stringify(old.provenance)!==JSON.stringify(provenance)||old.renderer!==renderer)throw Error('Cannot append captures from another source bundle or browser');
    const names=new Set(results.map(c=>c.name));results.unshift(...old.cases.filter(c=>!names.has(c.name)));
  }
  const result={schema_version:1,provenance,renderer,cases:results};
  await fs.writeFile(path.join(output,'browser-metrics.json'),JSON.stringify(result,null,2)+'\n');
  if(args.fixtures==='true') {
    const compact=results.filter(c=>c.scale===1&&(c.theme==='default'&&c.direction==='ltr'
      ||['sizes','statuses','wrapped'].includes(c.scene)));
    await fs.writeFile(path.join(root,'fixtures/form/visual-reference.json'),JSON.stringify({
      schema_version:1,provenance,renderer,full_capture:{cases:results.length,metrics_sha256:digest(JSON.stringify(result,null,2)+'\n')},
      cases:compact,
    },null,2)+'\n');
  }
