// Copyright (C) 2026 mg-chao
// SPDX-License-Identifier: Apache-2.0
// Developer-only real-time CDP trace in a fresh, hidden headless browser.
import fs from 'node:fs';
import path from 'node:path';
import crypto from 'node:crypto';
import { spawn, execFileSync } from 'node:child_process';
import { fileURLToPath, pathToFileURL } from 'node:url';

const options=Object.fromEntries(process.argv.slice(2).reduce((pairs,value,index,args)=>{
  if(index%2===0)pairs.push([value.replace(/^--/,''),args[index+1]]);return pairs;
},[]));
const crate=path.resolve(path.dirname(fileURLToPath(import.meta.url)),'..');
const reference=path.resolve(options.reference??path.join(crate,'../build/ant-design-gpui/alert-reference'));
const output=path.resolve(options.output??path.join(crate,'fixtures/alert/lifecycle.json'));
const browser=options.browser??'C:/Program Files/Google/Chrome/Application/chrome.exe';
const profile=path.join(reference,'lifecycle',`realtime-${crypto.randomUUID()}`);
fs.mkdirSync(profile,{recursive:true});
const page=pathToFileURL(path.join(reference,'alert-lifecycle.html')).href+'?mode='+(options.mode??'description');
const child=spawn(browser,['--headless','--disable-gpu','--no-first-run','--hide-scrollbars',
 '--disable-extensions','--disable-background-networking','--disable-component-update',
 '--disable-background-timer-throttling','--disable-renderer-backgrounding',
 '--remote-debugging-port=0',`--user-data-dir=${profile}`,page],{windowsHide:true});
let socket;
try{
 const pause=ms=>new Promise(resolve=>setTimeout(resolve,ms));
 let port;
 for(let attempt=0;attempt<100;attempt++){
  const file=path.join(profile,'DevToolsActivePort');
  if(fs.existsSync(file)){port=Number(fs.readFileSync(file,'utf8').split('\n')[0]);break;}
  await pause(50);
 }
 if(!port)throw Error('Headless browser did not expose its isolated debugging endpoint');
 const targets=await(await fetch(`http://127.0.0.1:${port}/json/list`)).json();
 const target=targets.find(target=>target.type==='page'&&target.url===page);
 if(!target)throw Error('Lifecycle page target was not created');
 socket=new WebSocket(target.webSocketDebuggerUrl);
 await new Promise((resolve,reject)=>{socket.addEventListener('open',resolve,{once:true});socket.addEventListener('error',reject,{once:true});});
 let nextId=1;const pending=new Map();
 socket.addEventListener('message',event=>{const message=JSON.parse(event.data);if(message.id&&pending.has(message.id)){pending.get(message.id)(message);pending.delete(message.id);}});
 const call=(method,params={})=>new Promise((resolve,reject)=>{
  const id=nextId++;const timeout=setTimeout(()=>{pending.delete(id);reject(Error(`CDP ${method} timed out`));},5000);
  pending.set(id,result=>{clearTimeout(timeout);result.error?reject(Error(result.error.message)):resolve(result.result);});
  socket.send(JSON.stringify({id,method,params}));
 });
 let trace;
 for(let attempt=0;attempt<100;attempt++){
  const result=await call('Runtime.evaluate',{expression:'document.querySelector("#lifecycle-metrics")?.textContent',returnByValue:true});
  if(result.result.value){trace=JSON.parse(result.result.value);break;}
  await pause(50);
 }
 if(!trace)throw Error('Actual upstream Alert did not finish its real-time trace');
 if(trace.error)throw Error(trace.error);
 if(!trace.events.some(event=>event.kind==='afterClose'))throw Error('Upstream Alert did not complete close motion');
 if(!trace.samples.some(sample=>Number(sample.opacity)>0&&Number(sample.opacity)<1))throw Error('No intermediate close animation frame was observed');
 const version=await call('Browser.getVersion');
 const rounding=await call('Runtime.evaluate',{expression:'(()=>{const n=document.createElement("div");n.style.cssText="height:85.94px;box-sizing:border-box";document.body.append(n);const r={boundsHeight:n.getBoundingClientRect().height,offsetHeight:n.offsetHeight};n.remove();return r;})()',returnByValue:true});
 const result={provenance:JSON.parse(fs.readFileSync(path.join(reference,'provenance.json'),'utf8')),
  renderer:{name:version.product,revision:version.revision,userAgent:version.userAgent,scale:1,offsetHeightRounding:rounding.result.value},
  capture:{path:'scripts/capture_alert_lifecycle.mjs',sha256:crypto.createHash('sha256').update(fs.readFileSync(fileURLToPath(import.meta.url))).digest('hex')},trace};
 fs.mkdirSync(path.dirname(output),{recursive:true});fs.writeFileSync(output,JSON.stringify(result,null,2)+'\n');
 console.log(`Captured actual upstream mounted Alert close: ${trace.samples.length} samples; ${trace.events.map(event=>`${event.kind}@${event.time.toFixed(1)}ms`).join(', ')}`);
 await call('Browser.close').catch(()=>{});
}finally{
 socket?.close();
 if(child.exitCode===null&&child.pid){
  if(process.platform==='win32'){try{execFileSync('taskkill',['/PID',String(child.pid),'/T','/F'],{windowsHide:true,stdio:'ignore'});}catch{}}
  else child.kill();
 }
}
