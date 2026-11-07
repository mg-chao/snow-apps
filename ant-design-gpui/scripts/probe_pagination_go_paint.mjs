// Copyright (C) 2026 mg-chao
// SPDX-License-Identifier: Apache-2.0
// Reproduce disabled native-form compositing and cursor evidence without changing the renderer.
// node scripts/probe_pagination_go_paint.mjs [--reference DIRECTORY] [--output DIRECTORY] [--scale 1|1.5|2]
import fs from 'node:fs/promises';
import path from 'node:path';
import crypto from 'node:crypto';
import { fileURLToPath, pathToFileURL } from 'node:url';
import { setTimeout as delay } from 'node:timers/promises';
import { browser } from './pagination_browser.mjs';

const script = fileURLToPath(import.meta.url);
const root = path.resolve(path.dirname(script), '..');
const args = {};
for (let index = 2; index < process.argv.length; index += 2) {
  const name = process.argv[index];
  if (!['--reference', '--output', '--scale', '--edge'].includes(name) || !process.argv[index + 1]) {
    throw Error(`Unknown or incomplete argument: ${name}`);
  }
  args[name.slice(2)] = process.argv[index + 1];
}
const reference = path.resolve(args.reference ?? path.join(root, '../build/ant-design-gpui/pagination-reference/browser'));
const output = path.resolve(args.output ?? path.join(root, '../build/ant-design-gpui/pagination-reference/go-paint-probe'));
const scale = Number(args.scale ?? 1);
if (![1, 1.5, 2].includes(scale)) throw Error('Scale must be 1, 1.5 or 2');
const digest = data => crypto.createHash('sha256').update(data).digest('hex');
const provenanceFile = path.join(reference, 'provenance.json');
const provenanceBytes = await fs.readFile(provenanceFile);
const provenance = JSON.parse(provenanceBytes);
if (provenance.version !== '6.6.7' || provenance.revision !== 'f8f5595ab5e891fff6a2a06c5b3b98af643d008e') {
  throw Error('Prepare the pinned original Pagination 6.6.7 reference before running this probe');
}
const rcPagination = Object.entries(provenance.packages).find(([name]) => name.endsWith('/@rc-component/pagination'))?.[1];
if (rcPagination?.version !== '1.4.0') throw Error('Expected the locked @rc-component/pagination 1.4.0');
const backgrounds = { white: '#ffffff', 'root-dark': '#141414', blue: '#336699', gray: '#808080', 'blue-alt': '#2468ac' };
const selector = '.ant-pagination-options-quick-jumper button';
const b = await browser({ output, scale, edge: args.edge });
const records = [], cursorRecords = [];
try {
  await b.page('Emulation.setDeviceMetricsOverride', { width: 1280, height: 420, deviceScaleFactor: scale, mobile: false });
  async function load(theme, disabled, audit) {
    const url = pathToFileURL(path.join(reference, 'index.html'));
    for (const [name, value] of Object.entries({ kind: 'quick-go', current: 6, total: 500,
      page_size: 10, show_size_changer: false, direction: 'ltr', theme, disabled, audit })) {
      url.searchParams.set(name, value);
    }
    await b.page('Input.dispatchMouseEvent', { type: 'mouseMoved', x: 1, y: 1 });
    await b.page('Page.navigate', { url: url.href });
    for (let attempt = 0; attempt < 200; attempt++) {
      if (await b.evaluate(`location.href === ${JSON.stringify(url.href)} && !!document.querySelector('#visual-metrics')`)) return url.href;
      if (attempt === 199) throw Error('Original fixture did not initialize');
      await delay(20);
    }
  }
  const metrics = () => b.evaluate(`(() => {
    const e = document.querySelector(${JSON.stringify(selector)}), r = e.getBoundingClientRect(), c = getComputedStyle(e);
    const cursor = selector => getComputedStyle(e.closest(selector)).cursor;
    return { bounds: {x:r.x,y:r.y,w:r.width,h:r.height}, scale:devicePixelRatio, disabled:e.disabled,
      hover:e.matches(':hover'), active:e.matches(':active'), focus:e.matches(':focus'),
      parent_background:getComputedStyle(document.querySelector('main')).backgroundColor,
      background:c.backgroundColor, border:c.border, border_top_color:c.borderTopColor,
      color:c.color, appearance:c.appearance,
      color_scheme:c.colorScheme, opacity:c.opacity, filter:c.filter, html:e.outerHTML,
      cursor:{button:c.cursor,quick:cursor('.ant-pagination-options-quick-jumper'),
        options:cursor('.ant-pagination-options'),pagination:cursor('.ant-pagination')} };
  })()`);
  for (const theme of ['default', 'dark']) {
    const sourceUrl = await load(theme, true, `disabled-paint-${theme}`);
    for (const [name, background] of Object.entries(backgrounds)) {
      // This controlled backdrop is the only authored-style perturbation; button/source CSS stays unchanged.
      await b.evaluate(`document.querySelector('main').style.backgroundColor = ${JSON.stringify(background)}`);
      await b.settle();
      const state = await metrics();
      if (!state.disabled) throw Error('Expected an original disabled button');
      if (state.scale !== scale) throw Error('Browser scale differs from the requested scale');
      const { data } = await b.page('Page.captureScreenshot', { format: 'png', fromSurface: true,
        clip: { x: 0, y: 0, width: 1280, height: 420, scale: 1 }, captureBeyondViewport: false });
      const png = path.join(output, `${theme}-${name}.png`);
      const bytes = Buffer.from(data, 'base64');
      await fs.writeFile(png, bytes);
      // Decode the actual screenshot into a detached canvas, never into the rendered source document.
      const pixels = await b.evaluate(`(async () => {
        const image = new Image(); image.src = 'data:image/png;base64,${data}'; await image.decode();
        const canvas = document.createElement('canvas'); canvas.width=image.width; canvas.height=image.height;
        const context=canvas.getContext('2d'); context.drawImage(image,0,0);
        const sample=(x,y)=>[...context.getImageData(x,y,1,1).data].slice(0,3);
        const r=${JSON.stringify(state.bounds)}, scale=${scale};
        const parent=sample(Math.round(32*scale),Math.round(32*scale));
        const fill=sample(Math.round((r.x+4)*scale),Math.round((r.y+r.h/2)*scale));
        const cx=Math.round((r.x+r.w/2)*scale), cy=Math.round((r.y+r.h/2)*scale);
        const starts={top:Math.floor(r.y*scale),bottom:Math.ceil((r.y+r.h)*scale)-1,
          left:Math.floor(r.x*scale),right:Math.ceil((r.x+r.w)*scale)-1};
        const profiles={}, bands={};
        for(const side of Object.keys(starts)) {
          const edge=[];
          for(let offset=-3;offset<=Math.ceil(5*scale);offset++) {
            const x=side==='left'?starts[side]+offset:side==='right'?starts[side]-offset:cx;
            const y=side==='top'?starts[side]+offset:side==='bottom'?starts[side]-offset:cy;
            edge.push({offset,x,y,rgb:sample(x,y)});
          }
          // The flat native edge is floor(scale) physical pixels. Its inner end is
          // anchored to actual fill pixels, avoiding fractional DOM-edge rounding
          // and adjacent Page-label glyphs just outside the left edge.
          const fillStart=edge.findIndex(pixel=>pixel.offset>=0&&pixel.rgb.every((value,index)=>value===fill[index]));
          const width=Math.floor(scale);
          if(fillStart<width)throw Error('Native border/fill transition was not found: '+side);
          profiles[side]=edge;
          bands[side]=edge.slice(fillStart-width,fillStart);
        }
        return {parent,fill,border:bands.top[0].rgb,edge_profiles:profiles,edge_bands:bands,
          physical_stroke_width:Math.floor(scale),source_layout_phase:{
            left:r.x*scale-Math.floor(r.x*scale),top:r.y*scale-Math.floor(r.y*scale),
            right:(r.x+r.w)*scale-Math.floor((r.x+r.w)*scale),
            bottom:(r.y+r.h)*scale-Math.floor((r.y+r.h)*scale)}};
      })()`, true);
      const expected = background.match(/[0-9a-f]{2}/gi).map(value => parseInt(value, 16));
      if (pixels.parent.some((value, index) => value !== expected[index])) throw Error('Backdrop pixel differs from its authored color');
      records.push({ theme, name, background, scale, source_url: sourceUrl, metrics: state, png,
        png_sha256: digest(bytes), verified_parent_pixel: pixels.parent, painted_fill_rgb: pixels.fill,
        painted_border_rgb: pixels.border, top_border_probe: pixels.edge_profiles.top,
        edge_profiles: pixels.edge_profiles, edge_bands: pixels.edge_bands,
        physical_stroke_width: pixels.physical_stroke_width, source_layout_phase: pixels.source_layout_phase });
    }
  }
  for (const disabled of [false, true]) {
    const sourceUrl = await load('default', disabled, `cursor-${disabled}`);
    for (const hover of [false, true]) {
      const state = await metrics(), r = state.bounds;
      await b.page('Input.dispatchMouseEvent', { type: 'mouseMoved',
        ...(hover ? { x: r.x + r.w / 2, y: r.y + r.h / 2 } : { x: 1, y: 1 }) });
      await b.settle();
      cursorRecords.push({ source_url: sourceUrl, ...await metrics() });
    }
  }
  const fit = channel => {
    const light = records.find(record => record.theme === 'default' && record.name === 'white')[channel][0];
    const dark = records.find(record => record.theme === 'default' && record.name === 'root-dark')[channel][0];
    const alpha = 1 - (light - dark) / (255 - 20);
    return { gray: (dark - 20 * (1 - alpha)) / alpha, alpha };
  };
  const fill = fit('painted_fill_rgb'), border = fit('painted_border_rgb');
  let maximumBandError = 0;
  for (const record of records) {
    const prediction = fit => record.verified_parent_pixel.map(value => Math.round(value * (1 - fit.alpha) + fit.gray * fit.alpha));
    record.two_point_affine_predictions = { fill: prediction(fill), border: prediction(border) };
    record.fill_prediction_error = record.painted_fill_rgb.map((value, index) => value - record.two_point_affine_predictions.fill[index]);
    record.border_prediction_error = record.painted_border_rgb.map((value, index) => value - record.two_point_affine_predictions.border[index]);
    const css = record.metrics.border_top_color.match(/^rgba\((\d+),\s*(\d+),\s*(\d+),\s*([\d.]+)\)$/);
    if (!css) throw Error('Expected the original disabled UA border rgba color');
    const rgba = css.slice(1).map(Number);
    if (rgba.some((value, index) => value !== [118, 118, 118, 0.3][index])) throw Error('Original UA border color changed');
    record.edge_band_checks = {};
    for (const [side, samples] of Object.entries(record.edge_bands)) {
      record.edge_band_checks[side] = samples.map((sample, index) => {
        const role = record.physical_stroke_width === 1 ? 'collapsed' : index === 0 ? 'outer' : 'inner';
        const backdrop = role === 'outer' ? record.verified_parent_pixel : record.painted_fill_rgb;
        const predicted = role === 'collapsed' ? record.two_point_affine_predictions.border
          : backdrop.map((value, channel) => Math.round(value * (1 - rgba[3]) + rgba[channel] * rgba[3]));
        const error = sample.rgb.map((value, channel) => value - predicted[channel]);
        maximumBandError = Math.max(maximumBandError, ...error.map(Math.abs));
        return { role, ...sample, predicted_rgb: predicted, error_rgb: error };
      });
    }
  }
  await fs.writeFile(path.join(output, 'go-paint-probe.json'), JSON.stringify({
    renderer: b.renderer, scale, reference_version: provenance.version, reference_revision: provenance.revision,
    rc_pagination_version: rcPagination.version,
    source_provenance: provenanceFile, source_provenance_sha256: digest(provenanceBytes),
    source_bundle_sha256: digest(await fs.readFile(path.join(reference, 'reference.js'))),
    probe_script_sha256: digest(await fs.readFile(script)),
    method: 'Actual original PNG pixels over five verified main backgrounds, under both themes; source button CSS unchanged.',
    input: 'Isolated headless CDP only; no visible window or global input.',
    two_point_fits_note: 'Effective first-sample fits from rounded pixels, not uniquely exact CSS alpha. At 200% the second inner band is retained separately.',
    two_point_equivalent_fill: fill, two_point_equivalent_border: border,
    edge_band_model: { css_rgba: [118, 118, 118, 0.3], outer_backdrop: 'verified parent pixel',
      inner_backdrop: 'actual painted fill pixel', collapsed: '100%/150% single physical pixel, verified with its effective fit',
      checked_sides: ['top', 'bottom', 'left', 'right'], maximum_error_rgb_bytes: maximumBandError,
      allowed_error_rgb_bytes: 1, passed: maximumBandError <= 1 },
    records, cursor_records: cursorRecords,
  }, null, 2) + '\n');
  if (maximumBandError > 1) throw Error(`Native edge-band model differs by ${maximumBandError} RGB bytes`);
  console.log(`Captured ${records.length} native paint probes and ${cursorRecords.length} cursor states in ${output}`);
} finally { await b.close(); }
