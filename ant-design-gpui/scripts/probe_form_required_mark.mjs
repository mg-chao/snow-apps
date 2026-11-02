// Copyright (C) 2026 mg-chao
// SPDX-License-Identifier: Apache-2.0
import fs from 'node:fs/promises';
import path from 'node:path';
import {fileURLToPath,pathToFileURL} from 'node:url';
import {setTimeout as delay} from 'node:timers/promises';
import {browser} from './pagination_browser.mjs';
const root=path.resolve(path.dirname(fileURLToPath(import.meta.url)),'..');
const b=await browser({output:path.resolve(root,'../build/ant-design-gpui/form-required-mark'),scale:1,
  edge:'C:/Program Files/Google/Chrome/Application/chrome.exe'});
try{
  await b.page('Emulation.setDeviceMetricsOverride',{width:900,height:900,deviceScaleFactor:1,mobile:false});
  await b.page('Page.navigate',{url:pathToFileURL(path.resolve(root,'../build/ant-design-gpui/form-reference/index.html')).href});
  for(let i=0;i<300;i++){if(await b.evaluate('!!document.querySelector("#visual-ready")'))break;if(i===299)throw Error('Required marker not ready');await delay(20);}
  await b.page('DOM.enable');await b.page('CSS.enable');
  const {root:document}=await b.page('DOM.getDocument',{depth:-1,pierce:true});
  const {nodeId}=await b.page('DOM.querySelector',{nodeId:document.nodeId,selector:'label'});
  const {node}=await b.page('DOM.describeNode',{nodeId,depth:1});
  const platformFonts={};for(const pseudo of node.pseudoElements??[])platformFonts[pseudo.pseudoType]=await b.page('CSS.getPlatformFontsForNode',{nodeId:pseudo.nodeId});
  const observed=await b.evaluate(`(()=>{const l=document.querySelector('label'),p=getComputedStyle(l,'::before'),c=document.createElement('canvas').getContext('2d');return {pseudo:{fontFamily:p.fontFamily,fontSize:p.fontSize,fontWeight:p.fontWeight},fonts:['SimSun, sans-serif','Arial','Segoe UI','Microsoft YaHei UI','sans-serif'].map(f=>{c.font='14px '+f;return {font:f,advance:c.measureText('*').width};})};})()`);
  await fs.writeFile(path.join(root,'fixtures/form/required-mark-reference.json'),JSON.stringify({renderer:b.renderer,observed,platform_fonts:platformFonts},null,2)+'\n');
  console.log(JSON.stringify({observed,platform_fonts:platformFonts}));
}finally{await b.close();}
