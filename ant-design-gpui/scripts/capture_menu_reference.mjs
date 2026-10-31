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
if (!args.cases || !args.output) throw Error('Required: --cases JSON --output DIRECTORY');
const scale = Number(args.scale ?? 1.5);
if (![1, 1.5, 2].includes(scale)) throw Error('Scale must be 1, 1.5 or 2');
const output = path.resolve(args.output);
const reference = path.resolve(args.reference ?? '../build/ant-design-gpui/menu-reference/browser');
const cases = JSON.parse(await fs.readFile(args.cases, 'utf8'));
await fs.mkdir(output, { recursive: true });
const profile = path.join(output, `profile-${randomUUID()}`);
const child = spawn(args.edge ?? 'C:/Program Files (x86)/Microsoft/Edge/Application/msedge.exe', [
  '--headless', '--disable-gpu', '--no-first-run', '--hide-scrollbars', '--disable-extensions',
  '--disable-background-networking', '--disable-component-update', '--remote-debugging-port=0',
  `--force-device-scale-factor=${scale}`,
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
  await page('Emulation.setDeviceMetricsOverride', { width: 800, height: 500, deviceScaleFactor: scale, mobile: false });
  const results = [];
  for (const scenario of cases) {
    if (!/^[a-zA-Z0-9_-]+$/.test(scenario.name)) throw Error(`Unsafe fixture name: ${scenario.name}`);
    const url = pathToFileURL(path.join(reference, 'index.html'));
    for (const [key, value] of Object.entries(scenario)) if (key !== 'name') url.searchParams.set(key, String(value));
    await page('Input.dispatchMouseEvent', { type: 'mouseMoved', x: 790, y: 490 });
    await page('Input.dispatchMouseEvent', { type: 'mouseReleased', x: 790, y: 490, button: 'left', clickCount: 1 });
    const navigation = await page('Page.navigate', { url: url.href });
    if (navigation.errorText) throw Error(navigation.errorText);
    if (scenario.hover === true || scenario.pressed) {
      for (let i=0;i<150;i++) {
        const result=await page('Runtime.evaluate',{expression:`location.href === ${JSON.stringify(url.href)} && !!document.querySelector('.ant-menu-root')`,returnByValue:true});
        if(result.result.value)break;
        await delay(10);
      }
      const targetKey=scenario.pressed==='danger'?'delete':'home';
      let position={x:128,y:204};
      if(scenario.pressed){
        const result=await page('Runtime.evaluate',{expression:`(()=>{const item=[...document.querySelectorAll('.ant-menu-root > .ant-menu-item')].find(node=>node.getAttribute('data-menu-id')?.endsWith('-${targetKey}'));if(!item)throw Error('Pressed fixture item missing');const r=item.getBoundingClientRect();return{x:r.x+r.width/2,y:r.y+r.height/2};})()`,returnByValue:true});
        position=result.result.value;
      }
      await page('Input.dispatchMouseEvent', { type: 'mouseMoved', ...position });
      if(scenario.pressed)await page('Input.dispatchMouseEvent', { type: 'mousePressed', ...position, button:'left', clickCount:1 });
    }
    let metrics;
    for (let i = 0; i < 300; i++) {
      const result = await page('Runtime.evaluate', {
        expression: `location.href === ${JSON.stringify(url.href)} ? document.querySelector('#visual-metrics')?.textContent : undefined`,
        returnByValue: true,
      });
      if (result.result.value) { metrics = JSON.parse(result.result.value); break; }
      await delay(20);
    }
    if (!metrics || metrics.scale !== scale) throw Error(`Invalid fixture metrics: ${scenario.name}`);
    // DOM metrics can precede the software compositor's final tiled frame.
    // Settle two frames before copying pixels, then allow the compositor to
    // publish that frame (especially important at fractional device scale).
    await page('Runtime.evaluate', { expression: 'new Promise(resolve => requestAnimationFrame(() => requestAnimationFrame(resolve)))', awaitPromise: true });
    await delay(100);
    const customExpand = await page('Runtime.evaluate', {
      expression: `JSON.stringify([...document.querySelectorAll('.ant-menu-submenu-expand-icon')].filter(element => {
        const css = getComputedStyle(element), bounds = element.getBoundingClientRect();
        return css.display !== 'none' && css.visibility !== 'hidden' && bounds.width > 0 && bounds.height > 0 && !element.closest('[aria-hidden="true"]');
      }).map(element => {
        const bounds = element.getBoundingClientRect();
        return { selector: '.ant-menu-submenu-expand-icon', text: element.textContent,
          customExpand: { x: bounds.x, y: bounds.y, w: bounds.width, h: bounds.height } };
      }))`, returnByValue: true,
    });
    metrics.customExpandSlots = JSON.parse(customExpand.result.value);
    const { data } = await page('Page.captureScreenshot', { format: 'png', fromSurface: true,
      clip: { x: 0, y: 0, width: 800, height: 500, scale: 1 }, captureBeyondViewport: false });
    await fs.writeFile(path.join(output, `${scenario.name}.png`), Buffer.from(data, 'base64'));
    const dom = await page('Runtime.evaluate', { expression: 'document.documentElement.outerHTML', returnByValue: true });
    await fs.writeFile(path.join(output, `${scenario.name}.dom.html`), dom.result.value);
    results.push({ scenario, metrics });
    if (results.length % 20 === 0 || results.length === cases.length) console.log(`Captured ${results.length}/${cases.length}`);
  }
  await fs.writeFile(path.join(output, 'browser-metrics.json'), JSON.stringify({ renderer: version.Browser, cases: results }, null, 2) + '\n');
} finally {
  if (socket?.readyState === WebSocket.OPEN) await command('Browser.close').catch(() => {});
  socket?.close();
  if (child.exitCode === null) child.kill();
  await fs.writeFile(path.join(output, 'browser-stderr.txt'), stderr);
}
