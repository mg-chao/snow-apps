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
const reference = path.resolve(args.reference ?? '../build/ant-design-gpui/spin-reference');
const scenes = JSON.parse(await fs.readFile(new URL('../fixtures/spin/visual-cases.json', import.meta.url), 'utf8'));
const mode=args.mode??'visual';
const cases=[];
if(mode==='metrics'){for(const theme of ['default','dark','compact','dark-compact'])cases.push({name:`reference-${theme}`,scene:'reference',theme,direction:'both',scale:1});}
else for (const scene of Object.keys(scenes))
for (const theme of ['default','dark','compact','dark-compact'])
for (const direction of ['ltr','rtl'])
for (const scale of [1,1.5,2]) cases.push({name:`${scene}-${theme}-${direction}-${scale*100}`,scene,theme,direction,scale});
for(const theme of ['default','dark','compact','dark-compact'])for(const direction of ['ltr','rtl'])for(const phase of [0,200,400,800,1200,1600,2000])cases.push({name:`phase-${theme}-${direction}-${phase}`,scene:'basic',theme,direction,scale:1,phase});
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
  let behavior,transitions;
  for (const scenario of cases) {
    if (!/^[a-zA-Z0-9_-]+$/.test(scenario.name)) throw Error(`Unsafe fixture name: ${scenario.name}`);
    const url = pathToFileURL(path.join(reference, scenario.scene==='reference'?`spin-reference-${scenario.theme}.html`:`spin-visual-${scenario.scene}-${scenario.theme}-${scenario.direction}.html`));
    await page('Emulation.setDeviceMetricsOverride', {width:740,height:1000,deviceScaleFactor:scenario.scale,mobile:false});
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
    if (!metrics || (scenario.scene!=='reference' && metrics.length !== scenes[scenario.scene].length)) throw Error(`Invalid fixture metrics: ${scenario.name}`);
    if(mode==='metrics'&&scenario.theme==='default'&&scenario.scene==='reference'){
      const result=await page('Runtime.evaluate',{expression:'window.__spinReference.behavior().then(JSON.stringify)',returnByValue:true,awaitPromise:true});
      if(result.exceptionDetails)throw Error(JSON.stringify(result.exceptionDetails));
      behavior=JSON.parse(result.result.value);
      const transitionResult=await page('Runtime.evaluate',{expression:'window.__spinReference.transitions().then(JSON.stringify)',returnByValue:true,awaitPromise:true});
      if(transitionResult.exceptionDetails)throw Error(JSON.stringify(transitionResult.exceptionDetails));
      transitions=JSON.parse(transitionResult.result.value);
    }
    if(scenario.phase!==undefined){const sampled=await page('Runtime.evaluate',{expression:`JSON.stringify(window.__spinReference.setPhase(${scenario.phase}))`,returnByValue:true});metrics=JSON.parse(sampled.result.value);}
    if(mode!=='metrics'){
    const { data } = await page('Page.captureScreenshot', { format: 'png', fromSurface: true,
      clip: { x: 0, y: 0, width: 740, height: 1000, scale: 1 }, captureBeyondViewport: false });
    await fs.writeFile(path.join(output, `${scenario.name}.png`), Buffer.from(data, 'base64'));
    const dom = await page('Runtime.evaluate', { expression: 'document.documentElement.outerHTML', returnByValue: true });
    await fs.writeFile(path.join(output, `${scenario.name}.dom.html`), dom.result.value);
    }
    results.push({...scenario,rows:metrics});
    if (results.length % 20 === 0 || results.length === cases.length) console.log(`Captured ${results.length}/${cases.length}`);
  }
  if(mode==='metrics'){
    const referenceCases=results.filter(c=>c.scene==='reference');
    if(referenceCases.length!==4||new Set(referenceCases.map(c=>c.rows.length)).size!==1)throw Error('Incomplete Spin theme measurements');
    for(const entry of referenceCases)for(const row of entry.rows){
      if(row.dots.some(dot=>!(dot.layoutWidth>0&&dot.layoutHeight>0)))throw Error(`Invalid upstream dot dimensions in ${entry.name}/${row.index}`);
      if(row.options.percent>0&&!row.options.indicator&&!row.progress)throw Error(`Missing upstream progress ring in ${entry.name}/${row.index}`);
    }
    if(behavior.delay.find(row=>row.label==='stop-immediately').busy!=='false'||behavior.stopped.pendingTimers!==0||behavior.remainingTimersAfterUnmount!==0)throw Error('Incomplete Spin effect cleanup trace');
    if(transitions.length!==3||!transitions[0].properties.length||!transitions[1].properties.length||!transitions[2].properties.length)throw Error('Missing actual upstream CSS transitions');
    const midpoint=transitions[0].samples.find(row=>row.time===150).holder.opacity;
    if(!(midpoint>0&&midpoint<1))throw Error('Transition clock did not sample an intermediate frame');
  }
  const provenance=JSON.parse(await fs.readFile(path.join(reference,'provenance.json'),'utf8'));
  if(args.fixture)await fs.writeFile(path.resolve(args.fixture),JSON.stringify({provenance:{...provenance,renderer:{name:version.Browser,viewport:[740,1000],scaleFactor:1}},componentDefaults:JSON.parse(await fs.readFile(path.join(reference,'theme-defaults.json'),'utf8')),behavior,transitions,cases:results.filter(c=>c.scene==='reference').map(c=>({theme:c.theme,results:c.rows})),animationCases:results.filter(c=>c.phase!==undefined).map(c=>({theme:c.theme,direction:c.direction,time:c.phase,results:c.rows}))})+'\n');
  await fs.writeFile(path.join(output, 'browser-metrics.json'), JSON.stringify({provenance:JSON.parse(await fs.readFile(path.join(reference,'provenance.json'),'utf8')),renderer:{name:version.Browser,viewport:[740,1000],scales:[1,1.5,2]},cases:results},null,2) + '\n');
} finally {
  if (socket?.readyState === WebSocket.OPEN) await command('Browser.close').catch(() => {});
  socket?.close();
  if (child.exitCode === null) child.kill();
  await fs.writeFile(path.join(output, 'browser-stderr.txt'), stderr);
}
