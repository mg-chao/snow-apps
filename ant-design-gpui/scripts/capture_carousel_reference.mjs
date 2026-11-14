// Copyright (C) 2026 mg-chao
// SPDX-License-Identifier: Apache-2.0
// Owns an isolated hidden headless browser; never attaches to a user's browser.
import fs from 'node:fs/promises';
import path from 'node:path';
import {fileURLToPath,pathToFileURL} from 'node:url';
import {spawn} from 'node:child_process';
import {randomUUID,createHash} from 'node:crypto';
import {setTimeout as delay} from 'node:timers/promises';

const args=Object.fromEntries(process.argv.slice(2).reduce((out,v,i,all)=>{if(i%2===0)out.push([v.replace(/^--/,''),all[i+1]]);return out;},[]));
const root=path.resolve(path.dirname(fileURLToPath(import.meta.url)),'..');
const output=path.resolve(args.output??path.join(root,'../build/ant-design-gpui/carousel-browser-current'));
const reference=path.resolve(args.reference??path.join(root,'../build/ant-design-gpui/carousel-reference-current'));
const scenes=JSON.parse(await fs.readFile(path.join(root,'fixtures/carousel/visual-cases.json'),'utf8'));
const scales=args.scale?[Number(args.scale)]:[1,1.5,2];
if(scales.some(s=>![1,1.5,2].includes(s)))throw Error('Supported scales are 1, 1.5 and 2');
const themes=args.theme?[args.theme]:['default','dark','compact','dark-compact'];
const directions=args.direction?[args.direction]:['ltr','rtl'];
await fs.mkdir(output,{recursive:true});
const child=spawn(args.browser??'C:/Program Files/Google/Chrome/Application/chrome.exe',[
  '--headless','--disable-gpu','--no-first-run','--hide-scrollbars','--disable-extensions','--disable-background-networking','--disable-component-update',
  '--remote-debugging-port=0',`--user-data-dir=${path.join(output,`profile-${randomUUID()}`)}`,'about:blank'
],{windowsHide:true,stdio:['ignore','ignore','pipe']});
let stderr='',launchError,socket,sequence=0;
child.stderr.on('data',chunk=>{stderr=(stderr+chunk).slice(-32768);});
child.on('error',error=>{launchError=error;});
const pending=new Map();
const command=(method,params={},sessionId)=>new Promise((resolve,reject)=>{
  const id=++sequence;const timer=setTimeout(()=>{pending.delete(id);reject(Error(`Timeout: ${method}`));},30000);
  pending.set(id,{resolve,reject,timer});socket.send(JSON.stringify({id,method,params,...(sessionId?{sessionId}:{})}));
});
try{
  let match;
  for(let i=0;i<300;i++){
    if(launchError)throw launchError;if(child.exitCode!==null)throw Error(`Browser exited: ${stderr}`);
    match=stderr.match(/DevTools listening on (ws:\/\/[^\s]+)/);if(match)break;await delay(50);
  }
  if(!match)throw Error(`Browser startup timed out: ${stderr}`);
  socket=new WebSocket(match[1]);await new Promise((resolve,reject)=>{socket.onopen=resolve;socket.onerror=reject;});
  socket.onmessage=event=>{const response=JSON.parse(event.data);const request=pending.get(response.id);if(!request)return;pending.delete(response.id);clearTimeout(request.timer);response.error?request.reject(Error(JSON.stringify(response.error))):request.resolve(response.result);};
  const renderer=await command('Browser.getVersion');
  const {targetId}=await command('Target.createTarget',{url:'about:blank'});
  const {sessionId}=await command('Target.attachToTarget',{targetId,flatten:true});
  const page=(method,params)=>command(method,params,sessionId);
  const evaluate=async expression=>{
    const result=await page('Runtime.evaluate',{expression,returnByValue:true,awaitPromise:true});
    if(result.exceptionDetails)throw Error(JSON.stringify(result.exceptionDetails));return result.result.value;
  };
  await page('Page.enable');await page('Runtime.enable');
  const records=[];let componentDefaults;
  for(const scene of Object.keys(scenes)){
    if(args.scene&&args.scene!==scene)continue;
    const dynamic=scene.startsWith('motion')||scene==='progress';
    const phases=scene.startsWith('motion')?[0,125,250,375,500]:scene==='progress'?[0,750,1500,2250,3000]:[0];
    for(const theme of themes)for(const direction of directions)for(const scale of scales)for(const phase of phases){
      await page('Emulation.setDeviceMetricsOverride',{width:520,height:1000,deviceScaleFactor:scale,mobile:false});
      await page('Input.dispatchMouseEvent',{type:'mouseMoved',x:4,y:4});
      const url=pathToFileURL(path.join(reference,'index.html'));
      Object.entries({scene,theme,direction,phase:dynamic?phase:1000}).forEach(([k,v])=>url.searchParams.set(k,String(v)));
      const name=`${scene}-${theme}-${direction}-${scale*100}${dynamic?`-${phase}`:''}`;
      // A unique URL also distinguishes repeated static scenes at another DPI,
      // so readiness can never come from the preceding document during commit.
      url.searchParams.set('capture',name);
      await page('Page.navigate',{url:url.href});
      let ready;
      for(let i=0;i<300;i++){
        ready=await evaluate(`location.href===${JSON.stringify(url.href)}&&(window.__carouselReference?.ready?true:document.body.dataset.error||false)`);
        if(ready)break;await delay(20);
      }
      if(ready!==true)throw Error(`Reference not ready: ${name}: ${ready}`);
      if(scene==='hover-prev'||scene==='hover-dot'){
        const rect=await evaluate(`window.__carouselReference.measure().rows[0].${scene==='hover-prev'?'arrows[0]':'dots[1]'}`);
        await page('Input.dispatchMouseEvent',{type:'mouseMoved',x:rect.x+rect.width/2,y:rect.y+rect.height/2});
      }else if(scene==='focus-next')await evaluate(`document.querySelector('.slick-next').focus()`);
      // Hover/focus creates a native CSS transition on the following frame.
      await evaluate('new Promise(resolve=>requestAnimationFrame(()=>requestAnimationFrame(resolve)))');
      const metrics=await evaluate(`window.__carouselReference.setPhase(${dynamic?phase:1000})`);
      if(metrics.scale!==scale)throw Error(`DPI mismatch: ${name}`);
      await evaluate('new Promise(resolve=>requestAnimationFrame(()=>requestAnimationFrame(resolve)))');
      const {data}=await page('Page.captureScreenshot',{format:'png',fromSurface:true,clip:{x:0,y:0,width:520,height:1000,scale:1},captureBeyondViewport:false});
      await fs.writeFile(path.join(output,`${name}.png`),Buffer.from(data,'base64'));
      records.push({name,phase,...metrics});
      componentDefaults??=await evaluate('window.__carouselReference.componentDefaults');
      if(records.length%24===0)console.log(`Captured ${records.length} original Carousel frames`);
    }
  }
  if(!records.length)throw Error('No scenes selected');
  const provenance=JSON.parse(await fs.readFile(path.join(reference,'provenance.json'),'utf8'));
  const generator=path.join(root,'scripts/capture_carousel_reference.mjs');
  provenance.capture_sha256=createHash('sha256').update(await fs.readFile(generator)).digest('hex');
  await fs.writeFile(path.join(output,'browser-metrics.json'),JSON.stringify({renderer,provenance,cases:records},null,2)+'\n');
  await fs.writeFile(path.join(root,'fixtures/carousel/reference.json'),JSON.stringify({schemaVersion:1,provenance,componentDefaults},null,2)+'\n');
  console.log(`Captured ${records.length} original Carousel frames: ${output}`);
}finally{
  if(socket?.readyState===WebSocket.OPEN)await command('Browser.close').catch(()=>{});
  socket?.close();if(child.exitCode===null)child.kill();await fs.writeFile(path.join(output,'browser-stderr.txt'),stderr);
}
