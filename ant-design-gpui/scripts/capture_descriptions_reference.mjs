// Copyright (C) 2026 mg-chao
// SPDX-License-Identifier: Apache-2.0
// Dedicated test browser/profile and a loopback server; never attaches to user tabs.
import fs from 'node:fs/promises';
import path from 'node:path';
import http from 'node:http';
import {spawn} from 'node:child_process';
import {randomUUID} from 'node:crypto';
import {setTimeout as delay} from 'node:timers/promises';

const args = Object.fromEntries(process.argv.slice(2).reduce((all, value, index, values) => {
  if (index % 2 === 0) all.push([value.replace(/^--/, ''), values[index + 1]]);
  return all;
}, []));
const output = path.resolve(args.output ?? '../build/ant-design-gpui/descriptions-browser');
const reference = path.resolve(args.reference ?? '../build/ant-design-gpui/descriptions-reference');
const scenes = JSON.parse(await fs.readFile(new URL('../fixtures/descriptions/visual-cases.json', import.meta.url), 'utf8'));
const cases = [];
for (const scene of Object.keys(scenes)) for (const theme of ['default', 'dark', 'compact', 'dark-compact']) {
  for (const direction of ['ltr', 'rtl']) for (const scale of [1, 1.5, 2]) {
    if (args.scene && args.scene !== scene || args.theme && args.theme !== theme
      || args.direction && args.direction !== direction || args.scale && Number(args.scale) !== scale) continue;
    cases.push({scene,theme,direction,scale,name:`${scene}-${theme}-${direction}-${scale * 100}`});
  }
}
if (!cases.length) throw Error('No scenarios selected');
await fs.mkdir(output, {recursive:true});
const server = http.createServer(async (request,response) => {
  const name = new URL(request.url, 'http://localhost').pathname;
  if (!['/', '/index.html', '/reference.js'].includes(name)) {response.writeHead(404).end();return;}
  try {
    const bytes = await fs.readFile(path.join(reference, name === '/' ? 'index.html' : name.slice(1)));
    response.writeHead(200, {'Content-Type': name.endsWith('.js') ? 'text/javascript' : 'text/html', 'Cache-Control':'no-store'}).end(bytes);
  } catch {response.writeHead(500).end();}
});
await new Promise(resolve => server.listen(0, '127.0.0.1', resolve));
const address = `http://127.0.0.1:${server.address().port}/`;
const profile = path.join(output, `profile-${randomUUID()}`);
const child = spawn(args.browser ?? 'C:/Program Files/Google/Chrome/Application/chrome.exe', [
  '--headless','--no-first-run','--hide-scrollbars','--disable-extensions',
  '--disable-background-networking','--disable-component-update','--remote-debugging-port=0',
  `--user-data-dir=${profile}`,'about:blank',
], {windowsHide:true,stdio:['ignore','ignore','pipe']});
let stderr = '', error, socket, sequence = 0;
const pending = new Map();
child.stderr.on('data', data => stderr = (stderr + data).slice(-32768));
child.on('error', value => error = value);
function command(method, params = {}, sessionId) {
  const id = ++sequence;
  return new Promise((resolve,reject) => {
    const timer = setTimeout(() => {pending.delete(id);reject(Error(`Timeout: ${method}`));}, 30000);
    pending.set(id,{resolve,reject,timer});
    socket.send(JSON.stringify({id,method,params,...sessionId ? {sessionId} : {}}));
  });
}
try {
  let port;
  for (let i = 0; i < 300; i++) {
    if (error) throw error;
    if (child.exitCode !== null) throw Error(stderr);
    try {port = (await fs.readFile(path.join(profile,'DevToolsActivePort'),'utf8')).split('\n')[0];break;} catch {}
    await delay(100);
  }
  if (!port) throw Error('Browser startup timeout');
  const renderer = await (await fetch(`http://127.0.0.1:${port}/json/version`)).json();
  socket = new WebSocket(renderer.webSocketDebuggerUrl);
  await new Promise((resolve,reject) => {socket.onopen=resolve;socket.onerror=reject;});
  socket.onmessage = event => {
    const response=JSON.parse(event.data),request=pending.get(response.id);
    if (!request) return;
    pending.delete(response.id);clearTimeout(request.timer);
    response.error ? request.reject(Error(JSON.stringify(response.error))) : request.resolve(response.result);
  };
  const {targetId} = await command('Target.createTarget',{url:'about:blank'});
  const {sessionId} = await command('Target.attachToTarget',{targetId,flatten:true});
  const page = (method,params) => command(method,params,sessionId);
  await page('Page.enable');await page('Runtime.enable');
  const results = [], intrinsicProbes = [];
  for (const scenario of cases) {
    const url = new URL(address);
    for (const key of ['scene','theme','direction']) url.searchParams.set(key,scenario[key]);
    const {width,height} = scenes[scenario.scene].viewport;
    await page('Emulation.setDeviceMetricsOverride',{width,height,deviceScaleFactor:scenario.scale,mobile:false});
    const navigation=await page('Page.navigate',{url:url.href});
    if (navigation.errorText) throw Error(navigation.errorText);
    let metrics;
    for (let i = 0; i < 300; i++) {
      const result = await page('Runtime.evaluate',{expression:`location.href===${JSON.stringify(url.href)}?document.querySelector('#visual-ready')?.textContent:undefined`,returnByValue:true});
      if (result.result.value) {metrics=JSON.parse(result.result.value);break;}
      await delay(20);
    }
    if (!metrics || metrics.components.length !== scenes[scenario.scene].components.length) throw Error(`Incomplete ${scenario.name}`);
    if (args.intrinsic === 'true') {
      const probe = await page('Runtime.evaluate', {returnByValue:true,expression:`(() => {
        const rect = element => {const r=element.getBoundingClientRect();return {x:r.x,y:r.y,width:r.width,height:r.height};};
        return [...document.querySelectorAll('.descriptions-case')].map(wrapper => {
          const root=wrapper.querySelector('.ant-descriptions');
          const table=root.querySelector(':scope > .ant-descriptions-view > table');
          const saved=table.getAttribute('style');
          const measure=() => ({table:rect(table),rows:[...table.rows].map(row => ({bounds:rect(row),cells:[...row.cells].map(cell => ({bounds:rect(cell),span:cell.colSpan,text:cell.textContent}))}))});
          const normal=measure();table.style.tableLayout='auto';const automatic=measure();
          table.style.minWidth='0';table.style.width='min-content';const minimum=measure();
          table.style.width='max-content';const preferred=measure();
          if(saved===null)table.removeAttribute('style');else table.setAttribute('style',saved);
          return {id:wrapper.getAttribute('data-id'),normal,automatic,minimum,preferred,restored:measure()};
        });
      })()`});
      if (probe.exceptionDetails) throw Error(JSON.stringify(probe.exceptionDetails));
      intrinsicProbes.push({...scenario,components:probe.result.value});
    }
    const {data} = await page('Page.captureScreenshot',{format:'png',fromSurface:true,clip:{x:0,y:0,width,height,scale:1}});
    await fs.writeFile(path.join(output,`${scenario.name}.png`),Buffer.from(data,'base64'));
    results.push({...scenario,...metrics});
    if (results.length % 12 === 0 || results.length === cases.length) console.log(`Captured ${results.length}/${cases.length}`);
  }
  const provenance = JSON.parse(await fs.readFile(path.join(reference,'provenance.json'),'utf8'));
  if (intrinsicProbes.length) await fs.writeFile(path.join(output,'intrinsic-probes.json'),JSON.stringify({provenance,renderer,cases:intrinsicProbes},null,2)+'\n');
  await fs.writeFile(path.join(output,'browser-metrics.json'),JSON.stringify({schema_version:1,provenance,renderer,cases:results},null,2)+'\n');
  if (args.fixtures === 'true') {
    const selected = results.filter(c => c.scale === 1 && (c.theme === 'default' && c.direction === 'ltr'
      || c.scene === 'sizes-bordered' && c.direction === 'ltr' || c.scene === 'horizontal' && c.direction === 'rtl'));
    await fs.writeFile(new URL('../fixtures/descriptions/reference.json',import.meta.url),JSON.stringify({
      schema_version:1,provenance,renderer,cases:selected,
    },null,2)+'\n');
  }
} finally {
  if (socket?.readyState === WebSocket.OPEN) await command('Browser.close').catch(() => {});
  socket?.close();if (child.exitCode === null) child.kill();
  await new Promise(resolve => server.close(resolve));
  await fs.writeFile(path.join(output,'browser-stderr.txt'),stderr);
}
