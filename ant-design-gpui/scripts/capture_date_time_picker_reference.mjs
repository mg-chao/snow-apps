// Copyright (C) 2026 mg-chao
// SPDX-License-Identifier: Apache-2.0
// Dedicated hidden Chromium; never attaches to a user's browser session.
import fs from 'node:fs/promises';
import path from 'node:path';
import {pathToFileURL} from 'node:url';
import {spawn} from 'node:child_process';
import {randomUUID,createHash} from 'node:crypto';
import {setTimeout as delay} from 'node:timers/promises';

const args=Object.fromEntries(process.argv.slice(2).reduce((a,v,i,all)=>{if(i%2===0)a.push([v.replace(/^--/,''),all[i+1]]);return a;},[]));
const output=path.resolve(args.output??'../build/ant-design-gpui/date-time-picker-reference/browser-capture');
const reference=path.resolve(args.reference??'../build/ant-design-gpui/date-time-picker-reference/browser');
const manifest=JSON.parse(await fs.readFile(new URL('../fixtures/date_time_picker/visual-cases.json',import.meta.url),'utf8'));
const captureScriptSHA=createHash('sha256').update(await fs.readFile(new URL(import.meta.url))).digest('hex');
const cases=[];
for(const scene of Object.keys(manifest.scenes))for(const theme of manifest.themes)for(const direction of manifest.directions)for(const scale of manifest.scales){
  if(args.scene&&!args.scene.split(',').includes(scene)||args.theme&&args.theme!==theme||args.direction&&args.direction!==direction||args.scale&&Number(args.scale)!==scale)continue;
  cases.push({scene,theme,direction,scale,name:`${scene}-${theme}-${direction}-${scale*100}`});
}
if(!cases.length)throw Error('No scenarios selected');
await fs.mkdir(output,{recursive:true});
const profile=path.join(output,`profile-${randomUUID()}`);
const child=spawn(args.browser??'C:/Program Files/Google/Chrome/Application/chrome.exe',['--headless','--no-first-run','--disable-extensions','--disable-background-networking','--disable-component-update','--remote-debugging-port=0',`--user-data-dir=${profile}`,'about:blank'],{windowsHide:true,stdio:['ignore','ignore','pipe']});
let stderr='',error,socket,sequence=0;const pending=new Map();
child.stderr.on('data',data=>stderr=(stderr+data).slice(-32768));child.on('error',e=>error=e);
function command(method,params={},sessionId){const id=++sequence;return new Promise((resolve,reject)=>{const timer=setTimeout(()=>{pending.delete(id);reject(Error(`Timeout: ${method}`));},30000);pending.set(id,{resolve,reject,timer});socket.send(JSON.stringify({id,method,params,...sessionId?{sessionId}:{}}));});}
try{
  let port;
  for(let i=0;i<300;i++){if(error)throw error;if(child.exitCode!==null)throw Error(stderr);
    try{port=(await fs.readFile(path.join(profile,'DevToolsActivePort'),'utf8')).split('\n')[0];break;}catch{}await delay(100);}
  if(!port)throw Error('Browser startup timeout');
  const renderer=await(await fetch(`http://127.0.0.1:${port}/json/version`)).json();
  socket=new WebSocket(renderer.webSocketDebuggerUrl);await new Promise((resolve,reject)=>{socket.onopen=resolve;socket.onerror=reject;});
  socket.onmessage=event=>{const response=JSON.parse(event.data),request=pending.get(response.id);if(!request)return;pending.delete(response.id);clearTimeout(request.timer);response.error?request.reject(Error(JSON.stringify(response.error))):request.resolve(response.result);};
  const {targetId}=await command('Target.createTarget',{url:'about:blank'});
  const {sessionId}=await command('Target.attachToTarget',{targetId,flatten:true});
  const page=(method,params)=>command(method,params,sessionId);
  await page('Page.enable');await page('Runtime.enable');await page('Emulation.setTimezoneOverride',{timezoneId:manifest.timezone});
  const results=[],images=[];
  for(const scenario of cases){
    const url=pathToFileURL(path.join(reference,'index.html'));for(const key of ['scene','theme','direction'])url.searchParams.set(key,scenario[key]);
    if(args['millisecond-step'])url.searchParams.set('millisecondStep',args['millisecond-step']);
    if(args['empty-multiple']==='true')url.searchParams.set('emptyMultiple','true');
    if(args['trigger-width'])url.searchParams.set('triggerWidth',args['trigger-width']);
    await page('Emulation.setDeviceMetricsOverride',{width:manifest.canvas.width,height:manifest.canvas.height,deviceScaleFactor:scenario.scale,mobile:false});
    const navigation=await page('Page.navigate',{url:url.href});if(navigation.errorText)throw Error(navigation.errorText);
    await page('Input.dispatchMouseEvent',{type:'mouseMoved',x:0,y:0});
    let metrics;
    for(let i=0;i<500;i++){
      const result=await page('Runtime.evaluate',{expression:`location.href===${JSON.stringify(url.href)}?document.querySelector('#visual-ready')?.textContent:undefined`,returnByValue:true});
      if(result.exceptionDetails)throw Error(JSON.stringify(result.exceptionDetails));
      if(result.result.value){metrics=JSON.parse(result.result.value);break;}await delay(20);
    }
    if(!metrics?.geometry?.trigger)throw Error(`Missing trigger: ${scenario.name}`);
    const sample=manifest.scenes[scenario.scene];
    if(sample.open&&!metrics.geometry.popup)throw Error(`Missing open panel: ${scenario.name}`);
    if(sample.hoverDate||sample.hoverTrigger){
      const r=sample.hoverTrigger?metrics.geometry.trigger:metrics.geometry.cells.find(c=>c.title===sample.hoverDate&&c.inView)?.rect;
      if(!r)throw Error(`Missing hover target ${scenario.name}`);
      await page('Input.dispatchMouseEvent',{type:'mouseMoved',x:r.x+r.width/2,y:r.y+r.height/2});
      await delay(100);
    }
    // Time-column scroll alignment and focus/hover states are allowed to settle.
    await delay(160);
    const current=await page('Runtime.evaluate',{expression:'window.captureDateTimePickerMetrics()',returnByValue:true});
    if(current.exceptionDetails)throw Error(JSON.stringify(current.exceptionDetails));metrics=current.result.value;
    const {data}=await page('Page.captureScreenshot',{format:'png',fromSurface:true,clip:{x:0,y:0,width:manifest.canvas.width,height:manifest.canvas.height,scale:1}});
    const png=Buffer.from(data,'base64'),file=`${scenario.name}.png`;await fs.writeFile(path.join(output,file),png);
    images.push({file,sha256:createHash('sha256').update(png).digest('hex')});results.push({...scenario,...metrics});
    if(results.length%12===0||results.length===cases.length)console.log(`Captured Date/time picker ${results.length}/${cases.length}`);
  }
  await fs.writeFile(path.join(output,'browser-metrics.json'),JSON.stringify({schema_version:1,completed_utc:new Date().toISOString(),clock:manifest.clock,timezone:manifest.timezone,
    capture_settings:{script_sha256:captureScriptSHA,hide_scrollbars:false},
    probe_overrides:args['millisecond-step']||args['empty-multiple']==='true'||args['trigger-width']?
      {millisecond_step:args['millisecond-step']?Number(args['millisecond-step']):undefined,
       empty_multiple:args['empty-multiple']==='true'||undefined,trigger_width:args['trigger-width']?Number(args['trigger-width']):undefined}:undefined,
    provenance:JSON.parse(await fs.readFile(path.join(reference,'provenance.json'),'utf8')),renderer,cases:results},null,2)+'\n');
  await fs.writeFile(path.join(output,'image-manifest.json'),JSON.stringify(images,null,2)+'\n');
}finally{
  if(socket?.readyState===WebSocket.OPEN)await command('Browser.close').catch(()=>{});socket?.close();if(child.exitCode===null)child.kill();
  await fs.writeFile(path.join(output,'browser-stderr.txt'),stderr);
}
