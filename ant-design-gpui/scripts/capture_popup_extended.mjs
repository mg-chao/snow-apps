// Copyright (C) 2026 mg-chao
// SPDX-License-Identifier: Apache-2.0
// Isolated headless browser; CDP fixes the actual viewport, including edge cases.
import fs from 'node:fs';
import path from 'node:path';
import {spawn} from 'node:child_process';
import {fileURLToPath,pathToFileURL} from 'node:url';
const crate=path.resolve(path.dirname(fileURLToPath(import.meta.url)),'..');
const build=path.resolve(crate,'../build/ant-design-gpui');
const native=path.resolve(process.argv[2]??path.join(build,'popup-native-extended'));
const source=path.join(build,'popup-extended-reference');
const output=path.join(build,'popup-extended-browser');
const profile=path.join(output,'profile-'+Date.now());
fs.mkdirSync(profile,{recursive:true});
const delay=ms=>new Promise(r=>setTimeout(r,ms));
class Client {
 constructor(url){
  this.ws=new WebSocket(url);this.next=0;this.pending=new Map();this.events=new Map();
  this.ready=new Promise((resolve,reject)=>{this.ws.addEventListener('open',resolve,{once:true});this.ws.addEventListener('error',reject,{once:true});});
  this.ws.addEventListener('message',event=>{
   const data=JSON.parse(event.data);
   if(data.id){const request=this.pending.get(data.id);if(!request)return;this.pending.delete(data.id);clearTimeout(request.timer);data.error?request.reject(Error(JSON.stringify(data.error))):request.resolve(data.result);}
   else {const fn=this.events.get(data.method);if(fn){this.events.delete(data.method);fn(data.params);}}
  });
 }
 async send(method,params={}){await this.ready;const id=++this.next;return new Promise((resolve,reject)=>{const timer=setTimeout(()=>{this.pending.delete(id);reject(Error('CDP timeout: '+method));},20000);this.pending.set(id,{resolve,reject,timer});this.ws.send(JSON.stringify({id,method,params}));});}
 event(method){return new Promise(resolve=>this.events.set(method,resolve));}
 close(){this.ws.close();}
}
const browser=spawn('C:/Program Files (x86)/Microsoft/Edge/Application/msedge.exe',[
 '--headless','--disable-gpu','--no-first-run','--hide-scrollbars','--disable-extensions',
 '--disable-background-networking','--disable-component-update','--remote-debugging-port=0',
 '--user-data-dir='+profile,'about:blank'
],{stdio:'ignore',windowsHide:true});
let page,control;
try {
 const portFile=path.join(profile,'DevToolsActivePort');
 for(let i=0;!fs.existsSync(portFile)&&i<200;i++)await delay(100);
 const [port,ws]=fs.readFileSync(portFile,'utf8').trim().split(/\r?\n/);
 control=new Client('ws://127.0.0.1:'+port+ws);await control.ready;
 const version=await control.send('Browser.getVersion');
 const target=await control.send('Target.createTarget',{url:'about:blank'});
 const targets=await (await fetch('http://127.0.0.1:'+port+'/json/list')).json();
 page=new Client(targets.find(t=>t.id===target.targetId).webSocketDebuggerUrl);await page.ready;
 await page.send('Page.enable');
 const inputs=JSON.parse(fs.readFileSync(path.join(native,'native-metrics.json'),'utf8'));
 const cases=[];
 for(const input of inputs){
  await page.send('Emulation.setDeviceMetricsOverride',{width:800,height:500,deviceScaleFactor:input.scale,mobile:false});
  const loaded=page.event('Page.loadEventFired');
  await page.send('Page.navigate',{url:pathToFileURL(path.join(source,input.name+'.html')).href});await loaded;
  const measured=await page.send('Runtime.evaluate',{expression:`new Promise((resolve,reject)=>{let n=0;const poll=()=>{if(window.popupMetrics)resolve(window.popupMetrics);else if(++n>200)reject(Error('No popup metrics'));else setTimeout(poll,25);};poll();})`,awaitPromise:true,returnByValue:true,timeout:10000});
  if(measured.exceptionDetails)throw Error(JSON.stringify(measured.exceptionDetails));
  const metric=measured.result.value;
  if(metric.viewport[0]!==800||metric.viewport[1]!==500)throw Error('Unexpected actual viewport '+metric.viewport);
  const shot=await page.send('Page.captureScreenshot',{format:'png',captureBeyondViewport:false,fromSurface:true});
  fs.writeFileSync(path.join(output,input.name+'.png'),Buffer.from(shot.data,'base64'));
  cases.push({...metric,scale:input.scale,anchor:input.anchor});
  if(cases.length%24===0)console.log('Captured '+cases.length+' extended browser cases');
 }
 fs.writeFileSync(path.join(output,'browser-metrics.json'),JSON.stringify({renderer:version.product,cases},null,2)+'\n');
 console.log('Captured '+cases.length+' extended browser cases');
 const loaded=page.event('Page.loadEventFired');
 await page.send('Page.navigate',{url:pathToFileURL(path.join(source,'motion.html')).href});await loaded;
 const motion=await page.send('Runtime.evaluate',{expression:`new Promise((resolve,reject)=>{let n=0;const poll=()=>{if(window.popupMotion)resolve(window.popupMotion);else if(++n>200)reject(Error('No motion metrics'));else setTimeout(poll,25);};poll();})`,awaitPromise:true,returnByValue:true,timeout:10000});
 if(motion.exceptionDetails)throw Error(JSON.stringify(motion.exceptionDetails));
 fs.writeFileSync(path.join(crate,'fixtures/popup/motion.json'),JSON.stringify({renderer:version.product,provenance:JSON.parse(fs.readFileSync(path.join(source,'provenance.json'),'utf8')),motion:motion.result.value},null,2)+'\n');
 console.log('Captured original CSS unique-background interpolation');
}finally{
 page?.close();
 if(control){try{await control.send('Browser.close');}catch{}control.close();}
 browser.kill();
}
