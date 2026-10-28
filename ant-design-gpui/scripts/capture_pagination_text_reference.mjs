// Copyright (C) 2026 mg-chao
// SPDX-License-Identifier: Apache-2.0
// Supplement surface metrics with original browser line and bidi-run geometry.
import fs from 'node:fs/promises';
import path from 'node:path';
import crypto from 'node:crypto';
import { fileURLToPath, pathToFileURL } from 'node:url';
import { setTimeout as delay } from 'node:timers/promises';
import { browser } from './pagination_browser.mjs';
const root = path.resolve(path.dirname(fileURLToPath(import.meta.url)), '..');
const args = Object.fromEntries(process.argv.slice(2).reduce((out, value, index, all) => {
  if (index % 2 === 0) out.push([value.replace(/^--/, ''), all[index + 1]]); return out;
}, []));
const scale = Number(args.scale ?? 1.5);
if (![1, 1.5, 2].includes(scale)) throw Error('Scale must be 1, 1.5 or 2');
const source = path.join(root, '../build/ant-design-gpui/pagination-reference/browser/index.html');
const output = path.resolve(args.output ?? path.join(root, `../build/ant-design-gpui/pagination-reference/text-${Math.round(scale * 100)}`));
const manifest = JSON.parse(await fs.readFile(path.join(root, 'fixtures/pagination/visual-cases.json'), 'utf8'));
const cases = manifest.filter(scenario => ['all', 'total'].includes(scenario.kind) || scenario.locale);
const b = await browser({ output, scale });
try {
  const results = [];
  for (const scenario of cases) {
    await b.page('Emulation.setDeviceMetricsOverride', { width: scenario.viewport_width ?? 1280,
      height: 420, deviceScaleFactor: scale, mobile: false });
    const url = pathToFileURL(source);
    for (const [key, value] of Object.entries(scenario)) if (key !== 'name') url.searchParams.set(key, String(value));
    await b.page('Page.navigate', { url: url.href });
    for (let index = 0; index < 300; index++) {
      if (await b.evaluate(`location.href === ${JSON.stringify(url.href)} && !!document.querySelector('#visual-metrics')`)) break;
      if (index === 299) throw Error(`Fixture not ready: ${scenario.name}`);
      await delay(20);
    }
    await b.settle();
    const text = await b.evaluate(`(() => {
      const rect = r => ({x:r.x,y:r.y,w:r.width,h:r.height});
      return ['.ant-pagination-total-text','.ant-select-content'].flatMap(selector => {
        const element=document.querySelector(selector);
        if (!element || !element.getBoundingClientRect().width) return [];
        const walker=document.createTreeWalker(element, NodeFilter.SHOW_TEXT), characters=[],rects=[];
        let node;
        while ((node=walker.nextNode())) {
          const fullRange=document.createRange();fullRange.selectNodeContents(node);
          rects.push(...[...fullRange.getClientRects()].filter(r=>r.width>0&&r.height>0).map(rect));
          for (let index=0; index<node.textContent.length; index++) {
            const range=document.createRange();range.setStart(node,index);range.setEnd(node,index+1);
            characters.push({character:node.textContent[index],...rect(range.getBoundingClientRect())});
          }
        }
        const lineTops=[];
        for(const r of rects) if(!lineTops.some(y=>Math.abs(y-r.y)<1))lineTops.push(r.y);
        return [{selector,text:element.textContent,direction:getComputedStyle(element).direction,
          bounds:rect(element.getBoundingClientRect()),line_count:lineTops.length,rects,characters}];
      });
    })()`);
    results.push({ scenario, text });
  }
  const artifact = { renderer: b.renderer, scale,
    original_bundle_sha256: crypto.createHash('sha256').update(await fs.readFile(path.join(path.dirname(source), 'reference.js'))).digest('hex'),
    cases: results };
  await fs.writeFile(path.join(output, 'text-metrics.json'), JSON.stringify(artifact, null, 2) + '\n');
  console.log(`Captured original DOMRange line/bidi evidence for ${results.length} scenarios at ${scale * 100}%`);
} finally { await b.close(); }
