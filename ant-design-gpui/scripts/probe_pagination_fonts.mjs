// Copyright (C) 2026 mg-chao
// SPDX-License-Identifier: Apache-2.0
// Diagnose source/native text metrics without browser-derived layout constants.
import fs from 'node:fs/promises';
import path from 'node:path';
import { fileURLToPath, pathToFileURL } from 'node:url';
import { setTimeout as delay } from 'node:timers/promises';
import { browser } from './pagination_browser.mjs';
const root = path.resolve(path.dirname(fileURLToPath(import.meta.url)), '..');
const output = path.resolve(process.argv[2] ?? path.join(root, '../build/ant-design-gpui/pagination-reference/font-probe'));
const reference = path.join(root, '../build/ant-design-gpui/pagination-reference/browser/index.html');
const b = await browser({ output });
try {
  await b.page('Emulation.setDeviceMetricsOverride', { width: 1280, height: 420, deviceScaleFactor: 1.5, mobile: false });
  await b.page('DOM.enable'); await b.page('CSS.enable');
  const samples = [];
  for (const kind of ['quick', 'custom']) {
    const url = pathToFileURL(reference); url.searchParams.set('kind', kind);
    await b.page('Page.navigate', { url: url.href });
    for (let i = 0; i < 300; i++) {
      if (await b.evaluate('!!document.querySelector("#visual-metrics")')) break;
      await delay(20);
    }
    const selector = kind === 'quick' ? '.ant-pagination-options-quick-jumper' : '.ant-pagination-prev a';
    const { root: document } = await b.page('DOM.getDocument');
    const { nodeId } = await b.page('DOM.querySelector', { nodeId: document.nodeId, selector });
    const fonts = await b.page('CSS.getPlatformFontsForNode', { nodeId });
    const measurements = await b.evaluate(`(() => {
      const element=document.querySelector(${JSON.stringify(selector)}), style=getComputedStyle(element);
      const canvas=document.createElement('canvas'), context=canvas.getContext('2d');
      context.font=style.fontWeight+' '+style.fontSize+' '+style.fontFamily;
      return {font:style.font,fontFamily:style.fontFamily,fontWeight:style.fontWeight,
        textNodes:[...element.childNodes].filter(node=>node.nodeType===Node.TEXT_NODE).map(node=>{
          const range=document.createRange();range.selectNodeContents(node);return{text:node.textContent,rangeWidth:range.getBoundingClientRect().width};
        }), widths:['Go to','Page','Previous','/','50','12345678'].map(text=>({text,width:context.measureText(text).width}))};
    })()`);
    samples.push({ kind, selector, platformFonts: fonts.fonts, measurements });
  }
  await fs.writeFile(path.join(output, 'font-probe.json'), JSON.stringify({ renderer: b.renderer, samples }, null, 2) + '\n');
  console.log(JSON.stringify(samples, null, 2));
} finally { await b.close(); }
