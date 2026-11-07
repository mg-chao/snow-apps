// Copyright (C) 2026 mg-chao
// SPDX-License-Identifier: Apache-2.0
// Freeze genuine upstream CSS animations with WAAPI; never synthesize expected poses.
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
const output = path.resolve(args.output ?? path.join(root, '../build/ant-design-gpui/image-browser-motion'));
const reference = path.resolve(args.reference ?? path.join(root, '../build/ant-design-gpui/image-reference/browser'));
const motion = JSON.parse(await fs.readFile(path.join(root, 'fixtures/image/motion-cases.json'), 'utf8'));
const scenes = JSON.parse(await fs.readFile(path.join(root, 'fixtures/image/visual-cases.json'), 'utf8'));
const provenance = JSON.parse(await fs.readFile(path.join(reference, 'provenance.json'), 'utf8'));
const themes = args.theme ? [args.theme] : motion.themes;
const directions = args.direction ? [args.direction] : motion.directions;
const scales = args.scale ? [Number(args.scale)] : motion.scales;
const jobs = Object.entries(motion.cases).filter(([name]) => !args.case || args.case === name);
if (!jobs.length) throw Error('No selected motion cases');
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
  const results = [];
  const total = jobs.length * themes.length * directions.length * scales.length;
  for (const [name, sample] of jobs) for (const theme of themes) for (const direction of directions) for (const scale of scales) {
    const scene = scenes[sample.scene];
    await page('Emulation.setDeviceMetricsOverride', { width: scene.width, height: scene.height, deviceScaleFactor: scale, mobile: false });
    const url = new URL(fixture.url);
    for (const [key, value] of Object.entries({ scene: sample.scene, motioncase: name, theme, direction })) url.searchParams.set(key, value);
    await page('Page.navigate', { url: url.href });
    for (let pass = 0; pass < 300; pass++) {
      if (await evaluate(`location.href === ${JSON.stringify(url.href)} && !!window.__imageReference && !!window.__imageMounted`)) break;
      if (pass === 299) throw Error(`Motion reference mount timed out: ${name}`);
      await delay(20);
    }
    await evaluate('window.__imageReference.ready()');
    if (sample.transition === 'hover') {
      await evaluate('window.__imageReference.prepareMotion()');
      const button = (await evaluate('window.__imageReference.measure()')).preview.controls.at(-1).bounds;
      await page('Input.dispatchMouseEvent', { type: 'mouseMoved', x: button.x + button.width / 2, y: button.y + button.height / 2 });
      await evaluate('new Promise(resolve=>requestAnimationFrame(()=>requestAnimationFrame(resolve)))');
    }
    const measured = await evaluate(`window.__imageReference.sampleMotion().then(sample=>({...sample,css_animations:document.getAnimations().map(animation=>({type:animation.constructor.name,target:animation.effect?.target?.className,timing:animation.effect?.getTiming(),keyframes:animation.effect?.getKeyframes()}))}))`);
    if (measured.scale !== scale || measured.phase_ms !== sample.phase_ms || !measured.css_animations.length) throw Error('Invalid genuine CSS motion sample');
    await delay(40);
    const { data } = await page('Page.captureScreenshot', { format: 'png', fromSurface: true,
      clip: { x: 0, y: 0, width: scene.width, height: scene.height, scale: 1 }, captureBeyondViewport: false });
    const filename = `${name}-${theme}-${direction}-${Math.round(scale * 100)}.png`;
    const bytes = Buffer.from(data, 'base64');
    await fs.writeFile(path.join(output, filename), bytes);
    results.push({ name, scene: sample.scene, transition: sample.transition, theme, direction, scale,
      filename, sha256: crypto.createHash('sha256').update(bytes).digest('hex'), ...measured });
    if (results.length % 12 === 0 || results.length === total) console.log(`Captured genuine CSS Image motion ${results.length}/${total}`);
  }
  const captureHash = crypto.createHash('sha256').update(await fs.readFile(fileURLToPath(import.meta.url))).digest('hex');
  const report = { schema_version: 1, renderer: version.Browser, captured_utc: new Date().toISOString(), provenance,
    capture_script_sha256: captureHash, measurement: 'Actual upstream CSS animations/transitions paused through WAAPI at declared phase. Computed DOM transforms, opacity, timing and CSS keyframes are original browser values; native implementation values are never inputs.', cases: results };
  await fs.writeFile(path.join(output, 'motion-metrics.json'), JSON.stringify(report, null, 2) + '\n');
  if (args.fixtures === 'true') {
    if (args.theme || args.direction || args.scale || args.case) throw Error('Checked-in motion reference requires the entire input matrix');
    await fs.writeFile(path.join(root, 'fixtures/image/motion-reference.json'), JSON.stringify(report, null, 2) + '\n');
  }
} finally {
  if (socket?.readyState === WebSocket.OPEN) await command('Browser.close').catch(() => {});
  socket?.close(); if (child.exitCode === null) child.kill();
  await fixture.close(); await fs.writeFile(path.join(output, 'browser-stderr.txt'), stderr);
}
