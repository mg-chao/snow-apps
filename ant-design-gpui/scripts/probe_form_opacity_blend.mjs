// Copyright (C) 2026 mg-chao
// SPDX-License-Identifier: Apache-2.0
import fs from 'node:fs/promises';
import path from 'node:path';
import {fileURLToPath} from 'node:url';
import {browser} from './pagination_browser.mjs';
const root=path.resolve(path.dirname(fileURLToPath(import.meta.url)),'..');
const output=path.resolve(root,'../build/ant-design-gpui/form-opacity-blend');
const b=await browser({output,scale:1,edge:'C:/Program Files/Google/Chrome/Application/chrome.exe'});
try{
  await b.page('Emulation.setDeviceMetricsOverride',{width:640,height:700,deviceScaleFactor:1,mobile:false});
  const cases=await b.evaluate(`(()=>{document.body.style.margin='0';const cases=[];for(const background of ['#ffffff','#141414','#e65a5a'])for(const alpha of [.25,.5,.65,1]){const i=cases.length,y=i*56;const parent=document.createElement('div');parent.style.cssText='position:absolute;left:0;top:'+y+'px;width:640px;height:56px;background:'+background;const group=document.createElement('div');group.style.cssText='position:absolute;left:20px;top:10px;width:120px;height:32px;background:#1677ff;opacity:'+alpha;const handle=document.createElement('div');handle.style.cssText='position:absolute;left:80px;top:2px;width:28px;height:28px;background:#fff';group.append(handle);parent.append(group);document.body.append(parent);cases.push({background,alpha,track:[30,y+20],handle:[110,y+20]});}return cases;})()`);
  await b.settle();
  const image=Buffer.from((await b.page('Page.captureScreenshot',{format:'png',fromSurface:true})).data,'base64');
  await fs.writeFile(path.join(output,'blend.png'),image);
  await fs.writeFile(path.join(output,'blend.json'),JSON.stringify({renderer:b.renderer,cases},null,2)+'\n');
  console.log(output);
}finally{await b.close();}
