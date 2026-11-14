// Copyright (C) 2026 mg-chao
// SPDX-License-Identifier: Apache-2.0
// Distinguish Chromium's real DPI border snapping from emulated raster scaling.
import fs from 'node:fs/promises';
import path from 'node:path';
import {fileURLToPath} from 'node:url';
import {browser} from './pagination_browser.mjs';
const root=path.resolve(path.dirname(fileURLToPath(import.meta.url)),'..');
const output=path.join(root,'../build/ant-design-gpui/form-dpi-probe');
const cases=[];
for(const scale of [1,1.5,2])for(const mode of ['native-force','emulated-only']){
  const launchScale=mode==='native-force'?scale:1;
  const b=await browser({output,scale:launchScale,edge:'C:/Program Files/Google/Chrome/Application/chrome.exe'});
  try{
    const launchDevicePixelRatio=await b.evaluate('devicePixelRatio');
    await b.page('Emulation.setDeviceMetricsOverride',{width:400,height:150,deviceScaleFactor:scale,mobile:false});
    await b.evaluate(`document.body.innerHTML='<input id="probe" style="font:14px/22px Arial;box-sizing:border-box;border:1px solid #d9d9d9;padding:4px 11px;width:200px">'`);
    const metrics=await b.evaluate(`(()=>{const e=document.getElementById('probe'),s=getComputedStyle(e),r=e.getBoundingClientRect();return {devicePixelRatio,borderTop:s.borderTopWidth,borderBottom:s.borderBottomWidth,height:r.height,width:r.width,lineHeight:s.lineHeight,padding:s.paddingTop};})()`);
    cases.push({mode,scale,launch_scale:launchScale,launch_device_pixel_ratio:launchDevicePixelRatio,renderer:b.renderer,metrics});
  }finally{await b.close();}
}
await fs.writeFile(path.join(root,'fixtures/form/dpi-reference.json'),JSON.stringify({schema_version:1,
  scope:'Independently executed minimal native Chromium DPI versus CDP emulated raster scale. Equal devicePixelRatio does not imply equal CSS border snapping.',cases},null,2)+'\n');
console.log(JSON.stringify(cases));
