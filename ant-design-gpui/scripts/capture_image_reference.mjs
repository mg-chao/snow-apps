// Copyright (C) 2026 mg-chao
// SPDX-License-Identifier: Apache-2.0
// Independent browser reference: one fresh headless browser/profile, actual upstream React.
import fs from 'node:fs/promises';
import path from 'node:path';
import crypto from 'node:crypto';
import { fileURLToPath } from 'node:url';
import { spawn } from 'node:child_process';
import { setTimeout as delay } from 'node:timers/promises';
import { createImageFixtureServer } from './image_fixture_server.mjs';

const args = Object.fromEntries(process.argv.slice(2).reduce((out, value, index, all) => {
  if (index % 2 === 0) out.push([value.replace(/^--/, ''), all[index + 1]]);
  return out;
}, []));
const root = path.resolve(path.dirname(fileURLToPath(import.meta.url)), '..');
const output = path.resolve(args.output ?? path.join(root, '../build/ant-design-gpui/image-browser'));
const reference = path.resolve(args.reference ?? path.join(root, '../build/ant-design-gpui/image-reference/browser'));
const scenes = JSON.parse(await fs.readFile(path.join(root, 'fixtures/image/visual-cases.json'), 'utf8'));
const provenance = JSON.parse(await fs.readFile(path.join(reference, 'provenance.json'), 'utf8'));
const themes = args.theme ? [args.theme] : ['default', 'dark', 'compact', 'dark-compact'];
const directions = args.direction ? [args.direction] : ['ltr', 'rtl'];
const scales = args.scale ? [Number(args.scale)] : [1, 1.5, 2];
if (themes.some(theme => !['default', 'dark', 'compact', 'dark-compact'].includes(theme)) ||
    directions.some(direction => !['ltr', 'rtl'].includes(direction)) || scales.some(scale => ![1, 1.5, 2].includes(scale))) throw Error('Invalid matrix filter');
await fs.mkdir(output, { recursive: true });
const fixture = await createImageFixtureServer({ reference });
const profile = path.join(output, `profile-${crypto.randomUUID()}`);
const child = spawn(args.browser ?? 'C:/Program Files/Google/Chrome/Application/chrome.exe', [
  '--headless', '--disable-gpu', '--no-first-run', '--hide-scrollbars', '--disable-extensions',
  '--disable-background-networking', '--disable-component-update', '--remote-debugging-port=0',
  `--user-data-dir=${profile}`, 'about:blank',
], { windowsHide: true, stdio: ['ignore', 'ignore', 'pipe'] });
let stderr = '', launchError, socket, sequence = 0;
child.stderr.on('data', chunk => { stderr = (stderr + chunk).slice(-32768); });
child.on('error', error => { launchError = error; });
const pending = new Map();
async function command(method, params = {}, sessionId) {
  const id = ++sequence;
  return new Promise((resolve, reject) => {
    const timer = setTimeout(() => { pending.delete(id); reject(Error(`Timeout: ${method}`)); }, 30000);
    pending.set(id, { resolve, reject, timer });
    socket.send(JSON.stringify({ id, method, params, ...(sessionId ? { sessionId } : {}) }));
  });
}
let captured = [];
try {
  let port;
  for (let pass = 0; pass < 300; pass++) {
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
    const response = JSON.parse(event.data), request = pending.get(response.id);
    if (!request) return;
    pending.delete(response.id); clearTimeout(request.timer);
    if (response.error) request.reject(Error(JSON.stringify(response.error))); else request.resolve(response.result);
  };
  const { targetId } = await command('Target.createTarget', { url: 'about:blank' });
  const { sessionId } = await command('Target.attachToTarget', { targetId, flatten: true });
  const page = (method, params) => command(method, params, sessionId);
  await page('Page.enable'); await page('Runtime.enable');
  const evaluate = async expression => {
    const result = await page('Runtime.evaluate', { expression, returnByValue: true, awaitPromise: true });
    if (result.exceptionDetails) throw Error(JSON.stringify(result.exceptionDetails));
    return result.result.value;
  };
  async function navigate(scene, theme, direction, scale, size) {
    await page('Emulation.setDeviceMetricsOverride', { width: size.width, height: size.height, deviceScaleFactor: scale, mobile: false });
    const url = new URL(fixture.url);
    for (const [key, value] of Object.entries({ scene, theme, direction })) url.searchParams.set(key, value);
    const navigation = await page('Page.navigate', { url: url.href });
    if (navigation.errorText) throw Error(navigation.errorText);
    for (let pass = 0; pass < 300; pass++) {
      if (await evaluate(`location.href === ${JSON.stringify(url.href)} && !!window.__imageReference && !!window.__imageMounted`)) break;
      if (pass === 299) throw Error(`Reference mount timed out: ${scene}`);
      await delay(20);
    }
    await evaluate('window.__imageReference.ready()');
  }
  const jobs = Object.entries(scenes).filter(([name]) => !args.scene || name === args.scene);
  if (!jobs.length) throw Error(`Unknown Image scene ${args.scene}`);
  const total = jobs.length * themes.length * directions.length * scales.length;
  for (const [scene, specification] of jobs) for (const theme of themes) for (const direction of directions) for (const scale of scales) {
    await navigate(scene, theme, direction, scale, specification);
    for (const action of specification.preview?.actions ?? []) await evaluate(`window.__imageReference.action(${JSON.stringify(action)})`);
    if (specification.preview?.pan) {
      const image = (await evaluate('window.__imageReference.measure()')).preview.image;
      const origin = { x: image.x + image.width / 2, y: image.y + image.height / 2 };
      await page('Input.dispatchMouseEvent', { type: 'mouseMoved', ...origin });
      await page('Input.dispatchMouseEvent', { type: 'mousePressed', ...origin, button: 'left', buttons: 1, clickCount: 1 });
      const target = { x: origin.x + specification.preview.pan[0], y: origin.y + specification.preview.pan[1] };
      await page('Input.dispatchMouseEvent', { type: 'mouseMoved', ...target, button: 'left', buttons: 1 });
      await page('Input.dispatchMouseEvent', { type: 'mouseReleased', ...target, button: 'left', clickCount: 1 });
    }
    if (specification.hover) {
      const cover = (await evaluate('window.__imageReference.measure()')).rows[0].root;
      await page('Input.dispatchMouseEvent', { type: 'mouseMoved', x: cover.x + cover.width / 2, y: cover.y + cover.height / 2 });
    } else await page('Input.dispatchMouseEvent', { type: 'mouseMoved', x: specification.width - 2, y: specification.height - 2 });
    await evaluate('new Promise(resolve => requestAnimationFrame(() => requestAnimationFrame(resolve)))');
    const metrics = await evaluate('window.__imageReference.measure()');
    if (metrics.scale !== scale) throw Error('Incorrect browser device scale');
    const name = `${scene}-${theme}-${direction}-${Math.round(scale * 100)}`;
    await delay(40);
    const { data } = await page('Page.captureScreenshot', { format: 'png', fromSurface: true,
      clip: { x: 0, y: 0, width: specification.width, height: specification.height, scale: 1 }, captureBeyondViewport: false });
    const bytes = Buffer.from(data, 'base64');
    await fs.writeFile(path.join(output, `${name}.png`), bytes);
    captured.push({ scene, theme, direction, scale, width: specification.width, height: specification.height, name, ...metrics,
      sha256: crypto.createHash('sha256').update(bytes).digest('hex') });
    if (captured.length % 24 === 0 || captured.length === total) console.log(`Captured actual upstream Image ${captured.length}/${total}`);
  }
  const behavior = [];
  if (args.behavior === 'true') {
    await navigate('thumbnails', 'default', 'ltr', 1, { width: 800, height: 600 });
    await evaluate(`window.__imageReference.change({isolated:true,src:${JSON.stringify(fixture.url + '/held/portrait.png')}})`);
    await delay(50);
    const pending = await evaluate(`({placeholder:!!document.querySelector('[data-image-loading]'),source:document.querySelector('.ant-image-img')?.getAttribute('src')})`);
    if (!pending.placeholder) throw Error('Original Image loading placeholder did not mount');
    behavior.push({ name: 'pending-load', ...pending });
    await evaluate(`window.__imageReference.change({src:${JSON.stringify(fixture.url + '/images/landscape.png')}})`);
    await evaluate('window.__imageReference.ready()');
    await fixture.release('portrait');
    await delay(50);
    const completed = await evaluate(`({placeholder:!!document.querySelector('[data-image-loading]'),source:document.querySelector('.ant-image-img')?.getAttribute('src'),naturalWidth:document.querySelector('.ant-image-img')?.naturalWidth})`);
    if (completed.placeholder || !completed.source.endsWith('/images/landscape.png') || completed.naturalWidth !== 320) throw Error('Stale image request replaced the current source');
    behavior.push({ name: 'stale-source-completion', ...completed });
    await evaluate(`window.__imageReference.change({src:${JSON.stringify(fixture.url + '/images/fail.png')},fallback:${JSON.stringify(fixture.url + '/images/fallback.png')}})`);
    await evaluate('window.__imageReference.ready()');
    const fallback = await evaluate(`({source:document.querySelector('.ant-image-img')?.getAttribute('src'),naturalWidth:document.querySelector('.ant-image-img')?.naturalWidth,errors:window.__imageReference.trace.filter(event=>event.error).length})`);
    if (!fallback.source.endsWith('/images/fallback.png') || fallback.naturalWidth !== 200 || fallback.errors < 1) throw Error('Upstream fallback behavior capture failed');
    behavior.push({ name: 'fallback-after-error', ...fallback });
    await navigate('preview-landscape', 'default', 'ltr', 1, { width: 800, height: 600 });
    const initial = await evaluate('window.__imageReference.measure().preview.transform');
    for (const action of ['zoomIn', 'rotateRight', 'flipX', 'reset']) {
      await evaluate(`window.__imageReference.action(${JSON.stringify(action)})`);
      behavior.push({ name: action, transform: await evaluate('window.__imageReference.measure().preview.transform') });
    }
    behavior.unshift({ name: 'initial-transform', transform: initial });
  }
  const report = { schema_version: 1, provenance, renderer: version.Browser, captured_utc: new Date().toISOString(), cases: captured, behavior };
  await fs.writeFile(path.join(output, 'browser-metrics.json'), JSON.stringify(report, null, 2) + '\n');
  if (args.fixtures === 'true') {
    if (args.scene || args.theme || args.direction || args.scale) throw Error('Checked-in fixtures require the entire matrix');
    await fs.writeFile(path.join(root, 'fixtures/image/reference.json'), JSON.stringify(report, null, 2) + '\n');
    await fs.writeFile(path.join(root, 'fixtures/image/browser-validation-windows.json'), JSON.stringify({ renderer: version.Browser, captured_utc: report.captured_utc, cases: captured.length,
      themes, directions, scales, behavior, upstream_revision: provenance.revision, image_version: provenance.imageVersion, lock_sha256: provenance.lock_sha256 }, null, 2) + '\n');
  }
} finally {
  if (socket?.readyState === WebSocket.OPEN) await command('Browser.close').catch(() => {});
  socket?.close(); if (child.exitCode === null) child.kill();
  await fixture.close();
  await fs.writeFile(path.join(output, 'browser-stderr.txt'), stderr);
}
