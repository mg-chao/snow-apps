// Copyright (C) 2026 mg-chao
// SPDX-License-Identifier: Apache-2.0
import fs from 'node:fs/promises';
import path from 'node:path';
import { pathToFileURL, fileURLToPath } from 'node:url';
import { setTimeout as delay } from 'node:timers/promises';
import { browser } from './pagination_browser.mjs';
const args = Object.fromEntries(process.argv.slice(2).reduce((out, value, index, all) => {
  if (index % 2 === 0) out.push([value.replace(/^--/, ''), all[index + 1]]); return out;
}, []));
if (!args.cases || !args.output) throw Error('Required: --cases JSON --output DIRECTORY');
const root = path.resolve(path.dirname(fileURLToPath(import.meta.url)), '..');
const reference = path.resolve(args.reference ?? path.join(root, '../build/ant-design-gpui/pagination-reference/browser'));
const output = path.resolve(args.output), scale = Number(args.scale ?? 1.5);
if (![1, 1.5, 2].includes(scale)) throw Error('Scale must be 1, 1.5 or 2');
const cases = JSON.parse(await fs.readFile(args.cases, 'utf8'));
const b = await browser({ output, scale, edge: args.edge });
try {
  const results = args.resume === 'true' ? JSON.parse(await fs.readFile(path.join(output, 'browser-metrics.json'), 'utf8')).cases : [];
  const completed = new Set(results.map(result => result.scenario.name));
  for (const scenario of cases) {
    if (completed.has(scenario.name)) continue;
    if (!/^[a-zA-Z0-9_-]+$/.test(scenario.name)) throw Error(`Unsafe fixture name: ${scenario.name}`);
    const width = scenario.viewport_width ?? 1280;
    await b.page('Emulation.setDeviceMetricsOverride', { width, height: 420, deviceScaleFactor: scale, mobile: false });
    // A CDP pointer/key can outlive navigation. Each scenario owns its input
    // state, including the absence of hover during keyboard-only focus.
    await b.page('Input.dispatchMouseEvent', { type: 'mouseReleased', x: 0, y: 0, button: 'left', clickCount: 1 });
    await b.page('Input.dispatchMouseEvent', { type: 'mouseMoved', x: 0, y: 0 });
    for (const [key, code, virtualKey] of [[' ', 'Space', 32], ['Enter', 'Enter', 13]]) {
      await b.page('Input.dispatchKeyEvent', { type: 'keyUp', key, code, windowsVirtualKeyCode: virtualKey });
    }
    const url = pathToFileURL(path.join(reference, 'index.html'));
    for (const [key, value] of Object.entries(scenario)) if (key !== 'name') url.searchParams.set(key, String(value));
    await b.page('Page.navigate', { url: url.href });
    for (let i = 0; i < 300; i++) {
      if (await b.evaluate(`location.href === ${JSON.stringify(url.href)} && !!document.querySelector('#visual-metrics')`)) break;
      if (i === 299) throw Error(`Fixture not ready: ${scenario.name}`);
      await delay(20);
    }
    const selectors = { page: '.ant-pagination-item:not(.ant-pagination-item-active)', active: '.ant-pagination-item-active',
      prev: '.ant-pagination-prev:not(.ant-pagination-disabled)', jump: '.ant-pagination-jump-next',
      quick: '.ant-pagination-options-quick-jumper input', simple: '.ant-pagination-simple-pager input', changer: '.ant-select-input',
      go: '.ant-pagination-options-quick-jumper button' };
    if (scenario.focus) {
      await b.page('Input.dispatchKeyEvent', { type: 'keyDown', key: 'Tab', code: 'Tab', windowsVirtualKeyCode: 9 });
      await b.page('Input.dispatchKeyEvent', { type: 'keyUp', key: 'Tab', code: 'Tab', windowsVirtualKeyCode: 9 });
      await b.evaluate(`document.querySelector(${JSON.stringify(selectors[scenario.focus])})?.focus()`);
    }
    if (scenario.hover) {
      const point = await b.evaluate(`(() => { const r=document.querySelector(${JSON.stringify(selectors[scenario.hover])})?.getBoundingClientRect(); return r && {x:r.x+r.width/2,y:r.y+r.height/2}; })()`);
      if (!point) throw Error(`Missing hover target: ${scenario.name}`);
      await b.page('Input.dispatchMouseEvent', { type: 'mouseMoved', ...point });
      if (scenario.pressed) await b.page('Input.dispatchMouseEvent', { type: 'mousePressed', button: 'left', clickCount: 1, ...point });
    }
    if (scenario.held_key) {
      await b.page('Input.dispatchKeyEvent', { type: 'keyDown', key: scenario.held_key === 'space' ? ' ' : 'Enter',
        code: scenario.held_key === 'space' ? 'Space' : 'Enter', windowsVirtualKeyCode: scenario.held_key === 'space' ? 32 : 13 });
    }
    await b.settle();
    const metrics = await b.evaluate('capturePaginationMetrics()');
    if (scenario.name.startsWith('go-')) metrics.native_button_state = await b.evaluate(`(() => {
      const button=document.querySelector('.ant-pagination-options-quick-jumper button');
      return {hover:button.matches(':hover'),active:button.matches(':active'),focus:button.matches(':focus'),
        focus_visible:button.matches(':focus-visible'),disabled:button.disabled};
    })()`);
    if (metrics.scale !== scale) throw Error(`Scale mismatch: ${scenario.name}`);
    const { data } = await b.page('Page.captureScreenshot', { format: 'png', fromSurface: true,
      clip: { x: 0, y: 0, width, height: 420, scale: 1 }, captureBeyondViewport: false });
    await fs.writeFile(path.join(output, `${scenario.name}.png`), Buffer.from(data, 'base64'));
    results.push({ scenario, metrics });
    if (results.length % 20 === 0 || results.length === cases.length) console.log(`Captured ${results.length}/${cases.length}`);
  }
  const order = new Map(cases.map((scenario, index) => [scenario.name, index]));
  results.sort((a, b) => order.get(a.scenario.name) - order.get(b.scenario.name));
  await fs.writeFile(path.join(output, 'browser-metrics.json'), JSON.stringify({ renderer: b.renderer, cases: results }, null, 2) + '\n');
} finally { await b.close(); }
