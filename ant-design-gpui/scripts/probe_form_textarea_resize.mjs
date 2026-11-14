// Copyright (C) 2026 mg-chao
// SPDX-License-Identifier: Apache-2.0
// Genuine Chromium native resize gestures on the pinned original TextArea.
import fs from 'node:fs/promises';
import path from 'node:path';
import crypto from 'node:crypto';
import {createRequire} from 'node:module';
import {fileURLToPath,pathToFileURL} from 'node:url';
import {setTimeout as delay} from 'node:timers/promises';
import {execFileSync} from 'node:child_process';
import {browser} from './pagination_browser.mjs';
const root=path.resolve(path.dirname(fileURLToPath(import.meta.url)),'..');
const output=path.join(root,'../build/ant-design-gpui/form-textarea-resize');
const dependencies=path.join(root,'../build/ant-design-gpui/modal-reference/browser');
const require=createRequire(path.join(dependencies,'package.json'));
const {build}=require('esbuild');
const digest=bytes=>crypto.createHash('sha256').update(bytes).digest('hex');
const revision=execFileSync('git',['-C','E:/ant-design','rev-parse','HEAD'],{encoding:'utf8'}).trim();
if(revision!=='f8f5595ab5e891fff6a2a06c5b3b98af643d008e')throw Error('Unexpected original TextArea revision');
await fs.mkdir(output,{recursive:true});await fs.writeFile(path.join(output,'version.js'),'export default "6.6.7";\n');
const source=`import React from 'react';import{createRoot}from'react-dom/client';import{flushSync}from'react-dom';
import Input from 'antd-reference/input';import ConfigProvider from 'antd-reference/config-provider';import theme from 'antd-reference/theme';
const root=createRoot(document.getElementById('root'));
window.setup=(c)=>{flushSync(()=>root.render(<ConfigProvider direction={c.direction} theme={{algorithm:c.theme==='dark'?theme.darkAlgorithm:theme.defaultAlgorithm,token:{motion:false}}}>
<div style={{padding:20,width:600,height:500,boxSizing:'border-box',direction:c.cssDirection??c.direction,background:c.theme==='dark'?'#141414':'#ffffff'}}>
<Input.TextArea id='resize-field' key={JSON.stringify(c)} defaultValue='Resize me' rows={3} disabled={c.state==='disabled'} readOnly={c.state==='readonly'} autoSize={c.state==='autosize'?{minRows:3,maxRows:6}:undefined} style={{width:300}}/>
<Input id='resize-sibling' defaultValue='Sibling focus' style={{width:200,display:'block',marginTop:40}}/>
</div></ConfigProvider>));document.activeElement?.blur();if(c.focus){const e=document.getElementById(c.focus==='sibling'?'resize-sibling':'resize-field');e.focus();e.setSelectionRange(2,5);}};
window.probe=()=>{const e=document.querySelector('textarea'),r=e.getBoundingClientRect(),s=getComputedStyle(e),a=document.activeElement;return{x:r.x,y:r.y,width:r.width,height:r.height,resize:s.resize,direction:s.direction,minHeight:s.minHeight,maxWidth:s.maxWidth,disabled:e.disabled,readonly:e.readOnly,selection:{start:e.selectionStart,end:e.selectionEnd},active:{id:a.id,tag:a.tagName,selectionStart:a.selectionStart,selectionEnd:a.selectionEnd}};};`;
const built=await build({stdin:{contents:source,loader:'tsx',resolveDir:root,sourcefile:'original-textarea-resize-probe.tsx'},
  outfile:path.join(output,'reference.js'),bundle:true,metafile:true,format:'iife',platform:'browser',nodePaths:[path.join(dependencies,'node_modules')],
  alias:{'antd-reference':'E:/ant-design/components'},define:{'process.env.NODE_ENV':'"production"'},
  plugins:[{name:'version',setup(builder){builder.onResolve({filter:/^\.\/version$/},info=>info.resolveDir.replaceAll('\\','/').endsWith('/components/version')?{path:path.join(output,'version.js')}:undefined);}}]});
await fs.writeFile(path.join(output,'index.html'),'<!doctype html><html><head><meta charset="UTF-8"></head><body style="margin:0"><div id="root"></div><script src="reference.js"></script></body></html>');
const b=await browser({output,scale:1,edge:'C:/Program Files/Google/Chrome/Application/chrome.exe'});
try{
  await b.page('Emulation.setDeviceMetricsOverride',{width:600,height:500,deviceScaleFactor:1,mobile:false});
  await b.page('Page.navigate',{url:pathToFileURL(path.join(output,'index.html')).href});
  for(let i=0;i<300;i++){if(await b.evaluate('!!window.setup'))break;if(i===299)throw Error('Resize probe not ready');await delay(20);}
  const cases=[];
  const configurations=[];
  for(const theme of ['default','dark'])for(const direction of ['ltr','rtl'])for(const state of ['enabled','disabled','readonly','autosize'])configurations.push({theme,direction,state});
  for(const direction of ['ltr','rtl'])for(const focus of ['sibling','textarea'])configurations.push({theme:'default',direction,state:'enabled',focus});
  configurations.push({theme:'default',direction:'rtl',cssDirection:'ltr',state:'enabled'});
  for(const config of configurations){
    const {theme,direction,state}=config,name=`${theme}-${direction}-${state}${config.focus?'-focus-'+config.focus:''}${config.cssDirection?'-css-'+config.cssDirection:''}`;
    await b.evaluate(`setup(${JSON.stringify(config)})`);await b.settle();
    const before=await b.evaluate('probe()');
    const image=Buffer.from((await b.page('Page.captureScreenshot',{format:'png',fromSurface:true})).data,'base64');
    await fs.writeFile(path.join(output,`${name}.png`),image);
    const rtl=before.direction==='rtl';
    const x=rtl?before.x+3:before.x+before.width-3,y=before.y+before.height-3;
    await b.page('Input.dispatchMouseEvent',{type:'mouseMoved',x,y});
    await b.page('Input.dispatchMouseEvent',{type:'mousePressed',button:'left',clickCount:1,x,y});
    const afterDown=await b.evaluate('probe()');
    for(let step=1;step<=6;step++)await b.page('Input.dispatchMouseEvent',{type:'mouseMoved',button:'left',buttons:1,
      x:x+(rtl?-1:1)*step*8,y:y+step*6});
    await b.page('Input.dispatchMouseEvent',{type:'mouseReleased',button:'left',clickCount:1,x:x+(rtl?-48:48),y:y+36});await b.settle();
    cases.push({config,before,afterDown,gesture:{grip_side:rtl?'left':'right',start:{x,y},delta:{x:rtl?-48:48,y:36}},
      after:await b.evaluate('probe()'),png_sha256:digest(image)});
  }
  const inputs={};for(const file of Object.keys(built.metafile.inputs))try{inputs[file.replaceAll('\\','/')]=digest(await fs.readFile(file));}catch{}
  const result={schema_version:1,scope:'Actual original Input.TextArea native Chromium grip drags; screenshot filenames match theme-direction-state under ignored build/form-textarea-resize.',
    upstream:{revision,version:'6.6.7'},renderer:b.renderer,generator_sha256:digest(await fs.readFile(fileURLToPath(import.meta.url))),inputs,cases};
  await fs.writeFile(path.join(root,'fixtures/form/textarea-resize-reference.json'),JSON.stringify(result,null,2)+'\n');
  console.log(JSON.stringify(cases.map(c=>({config:c.config,resize:c.before.resize,before:c.before.height,after:c.after.height,width_before:c.before.width,width_after:c.after.width}))));
}finally{await b.close();}
