// Copyright (C) 2026 mg-chao
// SPDX-License-Identifier: Apache-2.0
// Reproducible test renderer. Owns one isolated headless process, never a user browser.
import fs from 'node:fs/promises';
import path from 'node:path';
import { pathToFileURL } from 'node:url';
import { spawn } from 'node:child_process';
import { randomUUID } from 'node:crypto';
import { setTimeout as delay } from 'node:timers/promises';

const args = Object.fromEntries(process.argv.slice(2).reduce((out, value, i, all) => {
  if (i % 2 === 0) out.push([value.replace(/^--/, ''), all[i + 1]]);
  return out;
}, []));
if (!args.output) throw Error('Required: --output DIRECTORY');
const output = path.resolve(args.output);
const reference = path.resolve(args.reference ?? '../build/ant-design-gpui/result-reference');
const scenes = JSON.parse(await fs.readFile(new URL('../fixtures/result/visual-cases.json', import.meta.url), 'utf8'));
const cases=[];
for (const scene of Object.keys(scenes).filter(scene=>!args.scene||scene===args.scene))
for (const theme of ['default','dark','compact','dark-compact'])
for (const direction of ['ltr','rtl'])
for (const scale of [1,1.5,2]) cases.push({name:`${scene}-${theme}-${direction}-${scale*100}`,scene,theme,direction,scale});
if(!cases.length)throw Error('Unknown Result capture scene');
await fs.mkdir(output, { recursive: true });
const profile = path.join(output, `profile-${randomUUID()}`);
const child = spawn(args.browser ?? 'C:/Program Files/Google/Chrome/Application/chrome.exe', [
  '--headless', '--disable-gpu', '--no-first-run', '--hide-scrollbars', '--disable-extensions',
  '--disable-background-networking', '--disable-component-update', '--remote-debugging-port=0',
  '--force-device-scale-factor=1',
  `--user-data-dir=${profile}`, 'about:blank',
], { windowsHide: true, stdio: ['ignore', 'ignore', 'pipe'] });
let stderr = '';
child.stderr.on('data', chunk => { stderr = (stderr + chunk).slice(-32768); });
let launchError;
child.on('error', error => { launchError = error; });
let socket;
let sequence = 0;
const pending = new Map();
async function command(method, params = {}, sessionId) {
  const id = ++sequence;
  return new Promise((resolve, reject) => {
    const timer = setTimeout(() => { pending.delete(id); reject(Error(`Timeout: ${method}`)); }, 30000);
    pending.set(id, { resolve, reject, timer });
    socket.send(JSON.stringify({ id, method, params, ...(sessionId ? { sessionId } : {}) }));
  });
}
try {
  let port;
  for (let i = 0; i < 300; i++) {
    if (launchError) throw launchError;
    if (child.exitCode !== null) throw Error(`Headless browser exited: ${stderr}`);
    try { port = (await fs.readFile(path.join(profile, 'DevToolsActivePort'), 'utf8')).split('\n')[0]; break; } catch {}
    await delay(100);
  }
  if (!port) throw Error(`Browser startup timed out: ${stderr}`);
  const version = await (await fetch(`http://127.0.0.1:${port}/json/version`)).json();
  socket = new WebSocket(version.webSocketDebuggerUrl);
  await new Promise((resolve, reject) => { socket.onopen = resolve; socket.onerror = reject; });
  socket.onmessage = event => {
    const response = JSON.parse(event.data);
    const request = pending.get(response.id);
    if (!request) return;
    pending.delete(response.id); clearTimeout(request.timer);
    if (response.error) request.reject(Error(JSON.stringify(response.error)));
    else request.resolve(response.result);
  };
  const { targetId } = await command('Target.createTarget', { url: 'about:blank' });
  const { sessionId } = await command('Target.attachToTarget', { targetId, flatten: true });
  const page = (method, params) => command(method, params, sessionId);
  await page('Page.enable');
  await page('Runtime.enable');
  const results = [];
  for (const scenario of cases) {
    if (!/^[a-zA-Z0-9_-]+$/.test(scenario.name)) throw Error(`Unsafe fixture name: ${scenario.name}`);
    const url = pathToFileURL(path.join(reference, `result-visual-${scenario.scene}-${scenario.theme}-${scenario.direction}.html`));
    await page('Emulation.setDeviceMetricsOverride', {width:740,height:760,deviceScaleFactor:scenario.scale,mobile:false});
    const navigation = await page('Page.navigate', { url: url.href });
    if (navigation.errorText) throw Error(navigation.errorText);
    let metrics;
    for (let i = 0; i < 300; i++) {
      const result = await page('Runtime.evaluate', {
        expression: `location.href === ${JSON.stringify(url.href)} ? document.querySelector('#reference-metrics')?.textContent : undefined`,
        returnByValue: true,
      });
      if (result.result.value) { metrics = JSON.parse(result.result.value); break; }
      await delay(20);
    }
    if (!metrics || metrics.length !== scenes[scenario.scene].length) throw Error(`Invalid fixture metrics: ${scenario.name}`);
    const { data } = await page('Page.captureScreenshot', { format: 'png', fromSurface: true,
      clip: { x: 0, y: 0, width: 740, height: 760, scale: 1 }, captureBeyondViewport: false });
    await fs.writeFile(path.join(output, `${scenario.name}.png`), Buffer.from(data, 'base64'));
    const dom = await page('Runtime.evaluate', { expression: 'document.documentElement.outerHTML', returnByValue: true });
    await fs.writeFile(path.join(output, `${scenario.name}.dom.html`), dom.result.value);
    results.push({name:scenario.name,scale:scenario.scale,rows:metrics});
    if (results.length % 20 === 0 || results.length === cases.length) console.log(`Captured ${results.length}/${cases.length}`);
  }
  const previous=args.scene?await fs.readFile(path.join(output,'browser-metrics.json'),'utf8').then(JSON.parse).catch(()=>null):null;
  if(previous&&previous.renderer.name!==version.Browser)throw Error('Browser changed since retained captures; run the complete matrix');
  const names=new Set(results.map(result=>result.name));
  const merged=[...(previous?.cases??[]).filter(result=>!names.has(result.name)),...results];
  await fs.writeFile(path.join(output, 'browser-metrics.json'), JSON.stringify({provenance:JSON.parse(await fs.readFile(path.join(reference,'provenance.json'),'utf8')),renderer:{name:version.Browser,viewport:[740,760],scales:[1,1.5,2]},cases:merged},null,2) + '\n');
} finally {
  if (socket?.readyState === WebSocket.OPEN) await command('Browser.close').catch(() => {});
  socket?.close();
  if (child.exitCode === null) child.kill();
  await fs.writeFile(path.join(output, 'browser-stderr.txt'), stderr);
}
