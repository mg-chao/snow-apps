// Copyright (C) 2026 mg-chao
// SPDX-License-Identifier: Apache-2.0
// Executes the original Ant placement generator and actual rc-trigger browser alignment.
import fs from 'node:fs';
import path from 'node:path';
import crypto from 'node:crypto';
import { createRequire } from 'node:module';
import { execFileSync } from 'node:child_process';
import { fileURLToPath } from 'node:url';

const args = Object.fromEntries(process.argv.slice(2).reduce((pairs, arg, index, all) => {
  if (index % 2 === 0) pairs.push([arg.replace(/^--/, ''), all[index + 1]]);
  return pairs;
}, []));
const crate = path.resolve(path.dirname(fileURLToPath(import.meta.url)), '..');
const upstream = path.resolve(args.upstream ?? 'E:/ant-design');
const tools = path.resolve(args.tools ?? path.join(crate, '../build/ant-design-gpui/popup-placement-tools'));
const output = path.resolve(args.output ?? path.join(crate, '../build/ant-design-gpui/popup-placement-reference'));
const require = createRequire(path.join(tools, 'package.json'));
const esbuild = require('esbuild');
const ts = require('typescript');
const hash = value => crypto.createHash('sha256').update(value).digest('hex');
const revision = execFileSync('git', ['-C', upstream, 'rev-parse', 'HEAD'], {encoding: 'utf8'}).trim();
if (revision !== 'f8f5595ab5e891fff6a2a06c5b3b98af643d008e') throw new Error(`Unexpected reference revision ${revision}`);
const lockPath = path.join(crate, 'fixtures/popup/placement-tools/package-lock.json');
if (hash(fs.readFileSync(lockPath)) !== hash(fs.readFileSync(path.join(tools, 'package-lock.json')))) {
  throw new Error('Installed fixture tools do not match the checked-in lock file. Run npm ci with that lock.');
}
fs.mkdirSync(output, {recursive: true});

const placements = ['top', 'topLeft', 'topRight', 'bottom', 'bottomLeft', 'bottomRight', 'left', 'leftTop', 'leftBottom', 'right', 'rightTop', 'rightBottom'];
const cases = [];
function add(anchors, sizes, arrows, directions = ['ltr', 'rtl'], overflow = true, radius = 8) {
  for (const anchor of anchors) for (const popupSize of sizes) for (const arrow of arrows)
    for (const direction of directions) for (const placement of placements) {
      cases.push({id: cases.length, anchor, popupSize, arrow, direction, placement, overflow, radius});
    }
}
add(['center', 'tl', 'tr', 'bl', 'br', 'left', 'right', 'top', 'bottom', 'outside-left', 'outside-right', 'narrow'], [[160, 80]], ['normal', 'hidden', 'center']);
add(['center', 'tl', 'tr', 'bl', 'br'], [[40, 32], [360, 180], [240, 290]], ['normal', 'hidden', 'center']);
add(['tl', 'tr', 'bl', 'br'], [[160, 80]], ['normal'], ['ltr', 'rtl'], false);
for (const radius of [0, 20]) add(['center', 'tl', 'tr', 'bl', 'br'], [[160, 80]], ['center'], ['ltr', 'rtl'], true, radius);
const narrowArrowCases = cases.map(c => ({...c, arrowSize:8}));
cases.length = 0;
for (const arrowSize of [8,16]) for (const c of narrowArrowCases) cases.push({...c, id:cases.length, arrowSize});

const sources = {};
const selected = new Map([
  ['components/style/placementArrow.ts', ['MAX_VERTICAL_CONTENT_RADIUS', 'getArrowOffsetToken']],
  ['components/_util/is.ts', ['isPlainObject']],
]);
const plugins = [{name: 'original-placement-sources', setup(build) {
  build.onLoad({filter: /\.ts$/}, ({path: file}) => {
    const relative = path.relative(upstream, file).replaceAll('\\', '/');
    if (!relative.startsWith('components/')) return;
    const source = fs.readFileSync(file, 'utf8');
    sources[relative] = hash(source);
    const names = selected.get(relative);
    if (!names) return {contents: source, loader: 'ts', resolveDir: path.dirname(file)};
    // Keep original function bodies verbatim; omit unrelated styling imports.
    const ast = ts.createSourceFile(file, source, ts.ScriptTarget.Latest, true);
    const contents = ast.statements.filter(node => ts.isVariableStatement(node)
      ? node.declarationList.declarations.some(declaration => names.includes(declaration.name.getText(ast)))
      : node.name && names.includes(node.name.getText(ast)))
      .map(node => node.getFullText(ast)).join('\n');
    return {contents, loader: 'ts', resolveDir: path.dirname(file)};
  });
}}];

const entry = `
import React from 'react';
import {createRoot} from 'react-dom/client';
import Trigger from '@rc-component/trigger';
import getPlacements from ${JSON.stringify(path.join(upstream, 'components/_util/placements.ts'))};
const cases = JSON.parse(document.getElementById('cases').textContent);
const alignments = new Map();
const rect = element => { const r = element.getBoundingClientRect(); return {x:r.x, y:r.y, width:r.width, height:r.height}; };
function anchorBounds(name) {
  const vw = document.documentElement.clientWidth, vh = document.documentElement.clientHeight;
  const width = name === 'narrow' ? 4 : 80, height = 32;
  let x=(vw-width)/2, y=(vh-height)/2;
  if (['tl','bl','left'].includes(name)) x=0;
  if (['tr','br','right'].includes(name)) x=vw-width;
  if (['tl','tr','top'].includes(name)) y=0;
  if (['bl','br','bottom'].includes(name)) y=vh-height;
  if (name === 'outside-left') x=-20;
  if (name === 'outside-right') x=vw-30;
  return {x,y,width,height};
}
createRoot(document.getElementById('root')).render(cases.map(c => {
  const anchor = anchorBounds(c.anchor);
  const builtinPlacements = getPlacements({arrowWidth:c.arrow==='hidden'?0:c.arrowSize, arrowPointAtCenter:c.arrow==='center', autoAdjustOverflow:c.overflow, offset:4, borderRadius:c.radius});
  return React.createElement(Trigger, {
    key:c.id, prefixCls:'reference-popup', popupClassName:'case-'+c.id,
    popupVisible:true, action:[], popupPlacement:c.placement, builtinPlacements,
    arrow:c.arrow!=='hidden', getPopupContainer:()=>document.body,
    popupStyle:{position:'absolute', direction:c.direction},
    onPopupAlign:(_, align)=>alignments.set(c.id, align),
    popup:React.createElement('div',{style:{width:c.popupSize[0],height:c.popupSize[1],background:'#ddd'}}),
  }, React.createElement('div', {id:'anchor-'+c.id, style:{position:'absolute',left:anchor.x,top:anchor.y,width:anchor.width,height:anchor.height,direction:c.direction}}));
}));
let previous = '', stable = 0, attempts = 0;
const timer = setInterval(() => {
  if (++attempts > 80) { document.body.dataset.error='Alignment did not settle'; clearInterval(timer); return; }
  if (alignments.size !== cases.length) return;
  const measured = cases.map(c => {
    const popup = document.querySelector('.case-'+c.id);
    const bounds = rect(popup), anchor = rect(document.getElementById('anchor-'+c.id));
    const arrow = popup.querySelector('.reference-popup-arrow');
    const arrowStyle = arrow ? getComputedStyle(arrow) : null;
    const alignment = alignments.get(c.id);
    return {...c, anchorBounds:anchor, bounds,
      effectivePlacement:Array.from(popup.classList).find(name=>name.startsWith('reference-popup-placement-'))?.slice('reference-popup-placement-'.length),
      points:alignment.points, arrowOffset:arrowStyle ? {left:arrowStyle.left,top:arrowStyle.top} : null,
      viewport:{width:document.documentElement.clientWidth,height:document.documentElement.clientHeight}};
  });
  const serialized = JSON.stringify(measured);
  stable = serialized === previous ? stable+1 : 0; previous=serialized;
  if (stable >= 2) {
    clearInterval(timer);
    const script=document.createElement('script'); script.id='reference-placement'; script.type='application/json'; script.textContent=serialized; document.body.appendChild(script);
  }
}, 50);
`;
const bundle = await esbuild.build({stdin:{contents:entry,resolveDir:tools,loader:'js'}, bundle:true,write:false,format:'iife',platform:'browser',define:{'process.env.NODE_ENV':'"production"'},plugins,metafile:true});
const code = bundle.outputFiles[0].text;
const batches = [];
for (let first=0; first<cases.length; first+=60) {
  const batch = cases.slice(first, first+60), name = `placement-${batches.length}`;
  fs.writeFileSync(path.join(output, `${name}.html`), `<!doctype html><html><head><meta charset="utf-8"><style>html,body{margin:0;width:100%;height:100%;overflow:hidden}.reference-popup{position:absolute;box-sizing:border-box}.reference-popup-arrow{position:absolute;width:8px;height:8px;pointer-events:none}</style></head><body><div id="root"></div><script id="cases" type="application/json">${JSON.stringify(batch)}</script><script>${code.replaceAll('</script', '<\\/script')}</script></body></html>`);
  batches.push({name,count:batch.length});
}
const lock = JSON.parse(fs.readFileSync(lockPath,'utf8'));
fs.writeFileSync(path.join(output,'provenance.json'), JSON.stringify({
  upstream:{version:'6.6.7',revision}, packageLockSha256:hash(fs.readFileSync(lockPath)),
  packages:Object.fromEntries(Object.entries(lock.packages).filter(([key])=>key).map(([key,value])=>[key,{version:value.version,integrity:value.integrity,license:value.license}])),
  sources, bundleSha256:hash(code), batches,
  notes:['Actual rc-trigger 3.10.1 runs in the browser with original Ant Design getPlacements and getArrowOffsetToken.', 'Only unused exports/imports are removed from placementArrow.ts and is.ts; geometry function bodies are unchanged.', 'Fixed-size bodies isolate placement from typography. Trigger and popup bounds, effective placements and arrow offsets are measured from the live DOM.'],
},null,2)+'\n');
console.log(`Generated ${cases.length} live placement cases in ${batches.length} browser batches.`);
