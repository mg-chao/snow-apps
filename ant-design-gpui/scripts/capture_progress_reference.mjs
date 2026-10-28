// Copyright (C) 2026 mg-chao
// SPDX-License-Identifier: Apache-2.0
// Owns a dedicated hidden browser and profile; does not attach to user sessions.
import fs from 'node:fs/promises';
import path from 'node:path';
import {pathToFileURL} from 'node:url';
import {spawn} from 'node:child_process';
import {randomUUID} from 'node:crypto';
import {setTimeout as delay} from 'node:timers/promises';

const args=Object.fromEntries(process.argv.slice(2).reduce((a,v,i,all)=>{if(i%2===0)a.push([v.replace(/^--/,''),all[i+1]]);return a;},[]));
const output=path.resolve(args.output??'../build/ant-design-gpui/progress-browser-current');
const reference=path.resolve(args.reference??'../build/ant-design-gpui/progress-reference-current/browser');
const scenes=JSON.parse(await fs.readFile(new URL('../fixtures/progress/visual-cases.json',import.meta.url),'utf8'));
const cases=[];
for(const scene of Object.keys(scenes))for(const theme of ['default','dark','compact','dark-compact'])for(const direction of ['ltr','rtl'])for(const scale of [1,1.5,2]){
  if(args.scene&&args.scene!==scene||args.theme&&args.theme!==theme||args.direction&&args.direction!==direction||args.scale&&Number(args.scale)!==scale)continue;
  cases.push({scene,theme,direction,scale,name:`${scene}-${theme}-${direction}-${scale*100}`});
}
if(!cases.length)throw Error('No scenarios selected');
await fs.mkdir(output,{recursive:true});
const profile=path.join(output,`profile-${randomUUID()}`);
const child=spawn(args.browser??'C:/Program Files/Google/Chrome/Application/chrome.exe',['--headless','--no-first-run','--hide-scrollbars','--disable-extensions','--disable-background-networking','--disable-component-update','--remote-debugging-port=0',`--user-data-dir=${profile}`,'about:blank'],{windowsHide:true,stdio:['ignore','ignore','pipe']});
let stderr='',error,socket,sequence=0;const pending=new Map();
child.stderr.on('data',data=>stderr=(stderr+data).slice(-32768));child.on('error',e=>error=e);
function command(method,params={},sessionId){const id=++sequence;return new Promise((resolve,reject)=>{const timer=setTimeout(()=>{pending.delete(id);reject(Error(`Timeout: ${method}`));},30000);pending.set(id,{resolve,reject,timer});socket.send(JSON.stringify({id,method,params,...sessionId?{sessionId}:{}}));});}
try{
  let port;
  for(let i=0;i<300;i++){
    if(error)throw error;if(child.exitCode!==null)throw Error(stderr);
    try{port=(await fs.readFile(path.join(profile,'DevToolsActivePort'),'utf8')).split('\n')[0];break;}catch{}
    await delay(100);
  }
  if(!port)throw Error('Browser startup timeout');
  const version=await(await fetch(`http://127.0.0.1:${port}/json/version`)).json();
  socket=new WebSocket(version.webSocketDebuggerUrl);
  await new Promise((resolve,reject)=>{socket.onopen=resolve;socket.onerror=reject;});
  socket.onmessage=event=>{const response=JSON.parse(event.data),request=pending.get(response.id);if(!request)return;pending.delete(response.id);clearTimeout(request.timer);response.error?request.reject(Error(JSON.stringify(response.error))):request.resolve(response.result);};
  const {targetId}=await command('Target.createTarget',{url:'about:blank'});
  const {sessionId}=await command('Target.attachToTarget',{targetId,flatten:true});
  const page=(method,params)=>command(method,params,sessionId);
  await page('Page.enable');await page('Runtime.enable');const results=[];
  for(const scenario of cases){
    const url=pathToFileURL(path.join(reference,'index.html'));for(const key of ['scene','theme','direction'])url.searchParams.set(key,scenario[key]);
    await page('Emulation.setDeviceMetricsOverride',{width:740,height:1000,deviceScaleFactor:scenario.scale,mobile:false});
    const navigation=await page('Page.navigate',{url:url.href});if(navigation.errorText)throw Error(navigation.errorText);
    let metrics;
    for(let i=0;i<300;i++){
      const result=await page('Runtime.evaluate',{expression:`location.href===${JSON.stringify(url.href)}?document.querySelector('#visual-ready')?.textContent:undefined`,returnByValue:true});
      if(result.result.value){metrics=JSON.parse(result.result.value);break;}await delay(20);
    }
    if(!metrics||metrics.rows.length!==scenes[scenario.scene].length)throw Error(`Incomplete ${scenario.name}`);
    const {data}=await page('Page.captureScreenshot',{format:'png',fromSurface:true,clip:{x:0,y:0,width:740,height:1000,scale:1}});
    await fs.writeFile(path.join(output,`${scenario.name}.png`),Buffer.from(data,'base64'));
    results.push({...scenario,...metrics});if(results.length%12===0||results.length===cases.length)console.log(`Captured ${results.length}/${cases.length}`);
  }
  if(args.motion){
    const probes=[];
    for(const mode of ['line','circle','steps','active']){
      const result=await page('Runtime.evaluate',{expression:`window.probeProgressMotion(${JSON.stringify(mode)})`,returnByValue:true,awaitPromise:true});
      if(result.exceptionDetails)throw Error(JSON.stringify(result.exceptionDetails));
      probes.push(result.result.value);
    }
    await fs.writeFile(path.join(output,'motion-probes.json'),JSON.stringify({provenance:JSON.parse(await fs.readFile(path.join(reference,'provenance.json'),'utf8')),renderer:version,probes},null,2)+'\n');
  }
  await fs.writeFile(path.join(output,'browser-metrics.json'),JSON.stringify({schema_version:1,provenance:JSON.parse(await fs.readFile(path.join(reference,'provenance.json'),'utf8')),renderer:version,cases:results},null,2)+'\n');
}finally{
  if(socket?.readyState===WebSocket.OPEN)await command('Browser.close').catch(()=>{});socket?.close();if(child.exitCode===null)child.kill();
  await fs.writeFile(path.join(output,'browser-stderr.txt'),stderr);
}
