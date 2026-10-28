// Copyright (C) 2026 mg-chao
// SPDX-License-Identifier: Apache-2.0
// Expected values and event traces are observed from the executable original.
import fs from 'node:fs/promises';
import path from 'node:path';
import { fileURLToPath, pathToFileURL } from 'node:url';
import { setTimeout as delay } from 'node:timers/promises';
import { browser } from './pagination_browser.mjs';
const args = Object.fromEntries(process.argv.slice(2).reduce((out, value, index, all) => {
  if (index % 2 === 0) out.push([value.replace(/^--/, ''), all[index + 1]]); return out;
}, []));
const root = path.resolve(path.dirname(fileURLToPath(import.meta.url)), '..');
const built = path.resolve(args.reference ?? path.join(root, '../build/ant-design-gpui/pagination-reference/browser'));
const output = path.resolve(args.output ?? path.join(root, '../build/ant-design-gpui/pagination-reference/behavior'));
const b = await browser({ output, scale: 1.5, edge: args.edge });
try {
  await b.page('Emulation.setDeviceMetricsOverride', { width: 1280, height: 420, deviceScaleFactor: 1.5, mobile: false });
  await b.page('Page.navigate', { url: pathToFileURL(path.join(built, 'index.html')).href });
  for (let i = 0; i < 300; i++) {
    if (await b.evaluate('!!window.paginationHarness && !!document.querySelector("#visual-metrics")')) break;
    if (i === 299) throw Error('Reference did not become ready');
    await delay(20);
  }
  async function reset(scenario) { await b.evaluate(`paginationHarness.reset(${JSON.stringify(scenario)})`); await b.settle(); }
  async function snapshot() {
    const m = await b.evaluate('capturePaginationMetrics()');
    return { active: m.activeText === null ? null : Number(m.activeText), simple: m.simpleValue,
      quick: m.quickValue, events: m.events, items: m.items.map(item => ({
        kind: item.class.includes('jump-prev') && !item.class.includes('item-after') ? 'jump-prev' :
          item.class.includes('jump-next') && !item.class.includes('item-before') ? 'jump-next' :
          item.class.includes('ant-pagination-prev') ? 'prev' : item.class.includes('ant-pagination-next') ? 'next' :
          item.class.includes('ant-pagination-item') ? 'page' : item.class.includes('options') ? 'options' :
          item.class.includes('total-text') ? 'total' : 'simple',
        text: item.text, title: item.title, visible: item.visible, disabled: item.disabled,
      })) };
  }
  const models = [];
  for (const total of [0, 1, 10, 11, 50, 70, 71, 80, 500, 1000, 1_000_000_000_000]) {
    const last = Math.max(1, Math.ceil(total / 10));
    for (const current of [...new Set([0, 1, 2, 3, 4, 5, 6, 7, last - 2, last - 1, last, last + 1].filter(n => n >= 0))]) {
      for (const show_less of [false, true]) {
        const scenario = { total, current, page_size: 10, show_less, show_size_changer: false };
        await reset(scenario);
        models.push({ scenario, observed: await snapshot() });
      }
    }
  }
  for (const total of [0, 1, 10, 11, 50, 51]) {
    for (const hide_single of [false, true]) {
      const scenario = { total, hide_single, current: 1, show_size_changer: 'auto' };
      await reset(scenario); models.push({ scenario, observed: await snapshot() });
    }
  }
  const interactions = [];
  async function click(selector) {
    const found = await b.evaluate(`(() => {const e=document.querySelector(${JSON.stringify(selector)});if(!e)return false;e.click();return true;})()`);
    if (!found) throw Error(`Missing click target ${selector}`);
    await b.settle();
  }
  async function input(selector, value) {
    await b.evaluate(`(() => {const e=document.querySelector(${JSON.stringify(selector)});if(!e)throw Error('Missing input');e.focus();e.select();})()`);
    await b.page('Input.insertText', { text: value }); await b.settle();
  }
  async function pointerClick(selector) {
    const point = await b.evaluate(`(() => {const r=document.querySelector(${JSON.stringify(selector)}).getBoundingClientRect();return {x:r.x+r.width/2,y:r.y+r.height/2};})()`);
    await b.page('Input.dispatchMouseEvent', { type: 'mouseMoved', ...point });
    await b.page('Input.dispatchMouseEvent', { type: 'mousePressed', button: 'left', clickCount: 1, ...point });
    await b.page('Input.dispatchMouseEvent', { type: 'mouseReleased', button: 'left', clickCount: 1, ...point });
    await b.settle();
  }
  async function key(key, code) {
    await b.page('Input.dispatchKeyEvent', { type: 'keyDown', key, code: key, windowsVirtualKeyCode: code });
    await b.page('Input.dispatchKeyEvent', { type: 'keyUp', key, code: key, windowsVirtualKeyCode: code });
    await b.settle();
  }
  async function run(name, scenario, actions) {
    await reset(scenario); const observations = [{ action: 'initial', observed: await snapshot() }];
    for (const [action, work] of actions) { await work(); observations.push({ action, observed: await snapshot() }); }
    interactions.push({ name, scenario, observations });
  }
  await run('normal-clicks-and-jumps', { total: 1000, current: 50 }, [
    ['click-current', () => click('.ant-pagination-item-50')],
    ['next', () => click('.ant-pagination-next')], ['prev', () => click('.ant-pagination-prev')],
    ['jump-next', () => click('.ant-pagination-jump-next')], ['jump-prev', () => click('.ant-pagination-jump-prev')],
    ['first', () => click('.ant-pagination-item-1')], ['disabled-prev', () => click('.ant-pagination-prev')],
  ]);
  await run('disabled-clicks', { total: 1000, current: 50, disabled: true }, [
    ['next', () => click('.ant-pagination-next')], ['page', () => click('.ant-pagination-item-1')],
    ['jump-next', () => click('.ant-pagination-jump-next')],
  ]);
  for (const controlled of ['frozen', 'echo']) await run(`controlled-${controlled}`, { total: 500, current: 6, controlled }, [
    ['next', () => click('.ant-pagination-next')], ['next-again', () => click('.ant-pagination-next')],
  ]);
  for (const value of ['12', '0', '9999', 'abc', '12a', '', '0004']) await run(`quick-enter-${value || 'empty'}`,
    { kind: 'quick', total: 500, current: 6 }, [
      ['input', () => input('.ant-pagination-options-quick-jumper input', value)], ['enter', () => key('Enter', 13)],
    ]);
  await run('quick-blur', { kind: 'quick', total: 500, current: 6 }, [
    ['input-12', () => input('.ant-pagination-options-quick-jumper input', '12')],
    ['blur-outside', () => b.evaluate('document.activeElement.blur()').then(() => b.settle())],
  ]);
  await run('quick-blur-to-page', { kind: 'quick', total: 500, current: 6 }, [
    ['input-12', () => input('.ant-pagination-options-quick-jumper input', '12')],
    ['first', () => pointerClick('.ant-pagination-item-1')],
  ]);
  await run('quick-go', { kind: 'quick-go', total: 500, current: 6 }, [
    ['input-12', () => input('.ant-pagination-options-quick-jumper input', '12')],
    ['blur-outside', () => b.evaluate('document.activeElement.blur()').then(() => b.settle())],
    ['go', () => click('.ant-pagination-options-quick-jumper-button')],
  ]);
  await run('simple-editing', { kind: 'simple', total: 500, current: 6 }, [
    ['input-12', () => input('.ant-pagination-simple-pager input', '12')], ['enter', () => key('Enter', 13)],
    ['up', () => key('ArrowUp', 38)], ['down', () => key('ArrowDown', 40)],
    ['input-overflow', () => input('.ant-pagination-simple-pager input', '999')], ['enter-overflow', () => key('Enter', 13)],
    ['clear', () => input('.ant-pagination-simple-pager input', '')], ['enter-empty', () => key('Enter', 13)],
  ]);
  await run('size-change-clamps-current', { kind: 'changer', total: 500, current: 50, open: true }, [
    ['size-100', () => click('.ant-select-item-option[title="100 / page"]')],
  ]);
  await run('size-change-preserves-current', { kind: 'changer', total: 500, current: 3, open: true }, [
    ['size-20', () => click('.ant-select-item-option[title="20 / page"]')],
  ]);
  const provenance = JSON.parse(await fs.readFile(path.join(built, 'provenance.json'), 'utf8'));
  const result = { provenance: { revision: provenance.revision, version: provenance.version,
    lock_sha256: provenance.lock_sha256, renderer: b.renderer, rc_pagination: '1.4.0' }, models, interactions };
  const destination = path.resolve(args.fixture ?? path.join(root, 'fixtures/pagination/behavior-reference.json'));
  await fs.writeFile(destination, JSON.stringify(result, null, 2) + '\n');
  console.log(`Captured ${models.length} source-rendered models and ${interactions.length} interaction traces into ${destination}`);
} finally { await b.close(); }
