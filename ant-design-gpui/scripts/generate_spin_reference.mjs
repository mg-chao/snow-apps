// Copyright (C) 2026 mg-chao
// SPDX-License-Identifier: Apache-2.0
// Developer-only reference. No packages are installed or downloaded by this tool.
// Executes the original pinned Spin component and style generators and cssinjs serializer.
// node scripts/generate_spin_reference.mjs --upstream E:/ant-design --pnpm-store E:/.pnpm-store/v10

import fs from 'node:fs';
import path from 'node:path';
import crypto from 'node:crypto';
import { createRequire, isBuiltin } from 'node:module';
import { execFileSync } from 'node:child_process';
import { fileURLToPath } from 'node:url';

const options = Object.fromEntries(process.argv.slice(2).reduce((pairs, value, index, args) => {
  if (index % 2 === 0) pairs.push([value.replace(/^--/, ''), args[index + 1]]);
  return pairs;
}, []));
if (!options.upstream || !options['pnpm-store']) {
  throw new Error('Supply --upstream and --pnpm-store. This script uses existing pinned packages only.');
}
const crate = path.resolve(path.dirname(fileURLToPath(import.meta.url)), '..');
const upstream = path.resolve(options.upstream);
const store = path.resolve(options['pnpm-store']);
const output = path.resolve(options.output ?? path.join(crate, '../build/ant-design-gpui/spin-reference'));
const fixturePath = path.join(crate, 'fixtures/theme/reference.json');
const fixtures = JSON.parse(fs.readFileSync(fixturePath, 'utf8'));
const revision = execFileSync('git', ['-C', upstream, 'rev-parse', 'HEAD'], { encoding: 'utf8' }).trim();
if (revision !== fixtures.provenance.revision) throw new Error(`Unexpected Ant Design revision ${revision}`);
process.env.NODE_ENV = 'production';

const versions = {
  '@ant-design/colors': '8.0.1',
  '@ant-design/cssinjs': '2.1.2',
  '@ant-design/icons': '6.1.0',
  '@ant-design/icons-svg': '4.4.2',
  '@rc-component/motion': '1.3.1',
  classnames: '2.5.1',
  '@ant-design/cssinjs-utils': '2.1.2',
  '@ant-design/fast-color': '3.0.1',
  '@babel/runtime': '7.28.6',
  '@emotion/hash': '0.8.0',
  '@emotion/unitless': '0.7.5',
  '@rc-component/color-picker': '3.1.1',
  '@rc-component/util': '1.14.0',
  clsx: '2.1.1',
  'is-mobile': '5.0.0',
  react: '19.2.4',
  'react-dom': '19.2.4',
  'react-is': '18.3.1',
  scheduler: '0.27.0',
  stylis: '4.3.6',
  typescript: '5.9.3',
  'throttle-debounce': '5.0.2',
};
const indexes = new Map();
const usedPackages = new Set();
const inputs = new Map();
function read(file, label) {
  const content = fs.readFileSync(file, 'utf8');
  inputs.set(label, crypto.createHash('sha256').update(content).digest('hex'));
  return content;
}
const indexFiles = fs.readdirSync(path.join(store, 'index'), { withFileTypes: true })
  .filter(entry => entry.isDirectory())
  .flatMap(entry => fs.readdirSync(path.join(store, 'index', entry.name))
    .map(file => path.join(store, 'index', entry.name, file)));
function packageIndex(name) {
  if (!versions[name]) throw new Error(`Add an explicit pin for required package ${name}`);
  if (!indexes.has(name)) {
    const suffix = `-${name.replaceAll('/', '+')}@${versions[name]}.json`;
    const file = indexFiles.find(file => file.endsWith(suffix));
    if (!file) throw new Error(`Missing ${name}@${versions[name]} in ${store}`);
    indexes.set(name, JSON.parse(fs.readFileSync(file, 'utf8')));
  }
  return indexes.get(name);
}
function packageFile(name, relative) {
  usedPackages.add(name);
  if (name === '@rc-component/util' && options['util-package']) return path.resolve(options['util-package'], relative);
  const record = packageIndex(name).files[relative];
  if (!record) throw new Error(`Missing ${name}/${relative}`);
  const hex = Buffer.from(record.integrity.slice('sha512-'.length), 'base64').toString('hex');
  return path.join(store, 'files', hex.slice(0, 2), hex.slice(2));
}
function packageRelative(name, candidate) {
  if (name === '@rc-component/util' && options['util-package']) {
    const relative = [candidate, `${candidate}.js`, `${candidate}.json`, `${candidate}/index.js`]
      .find(file => fs.existsSync(path.resolve(options['util-package'], file)));
    if (!relative) throw new Error(`Cannot resolve ${name}/${candidate}`);
    return relative;
  }
  const files = packageIndex(name).files;
  const relative = [candidate, `${candidate}.js`, `${candidate}.json`, `${candidate}/index.js`]
    .find(file => files[file]);
  if (!relative) throw new Error(`Cannot resolve ${name}/${candidate}`);
  return relative;
}
const nativeRequire = createRequire(import.meta.url);
const moduleCache = new Map();
const browserDefinitions = new Map();
const browserLinks = new Map();
function packageRequest(request) {
  if (isBuiltin(request)) return { id: `node:${request}` };
  const parts = request.split('/');
  const name = request.startsWith('@') ? parts.splice(0, 2).join('/') : parts.shift();
  const relative = parts.length ? parts.join('/') : (() => {
    const metadata = JSON.parse(read(packageFile(name, 'package.json'), `${name}@${versions[name]}/package.json`));
    if (metadata.version !== versions[name]) throw new Error(`Wrong ${name} version: ${metadata.version}`);
    return (metadata.main ?? 'index.js').replace(/^\.\//, '');
  })();
  const file = packageRelative(name, relative);
  return { name, file, id: `${name}@${versions[name]}/${file}` };
}
function loadPackage(request) {
  if (isBuiltin(request)) return nativeRequire(request);
  const resolved = packageRequest(request);
  return loadPackageFile(resolved.name, resolved.file);
}
function loadPackageFile(name, relative) {
  const key = `${name}@${versions[name]}/${relative}`;
  if (moduleCache.has(key)) return moduleCache.get(key).exports;
  const source = read(packageFile(name, relative), key);
  browserDefinitions.set(key, relative.endsWith('.json') ? `module.exports=${source};` : source);
  const links = {}; browserLinks.set(key, links);
  const module = { exports: {} };
  moduleCache.set(key, module);
  if (relative.endsWith('.json')) {
    module.exports = JSON.parse(source);
  } else {
    const require = request => {
      if (request.startsWith('.')) {
        const target=packageRelative(name,path.posix.normalize(path.posix.join(path.posix.dirname(relative),request)));
        links[request]=`${name}@${versions[name]}/${target}`;
        return loadPackageFile(name,target);
      }
      links[request]=packageRequest(request).id;
      return loadPackage(request);
    };
    new Function('require', 'module', 'exports', source)(require, module, module.exports);
  }
  return module.exports;
}
const ts = nativeRequire(packageFile('typescript', 'lib/typescript.js'));
read(packageFile('typescript', 'lib/typescript.js'), 'typescript@5.9.3/lib/typescript.js');

// These adapters replace only React hook registration and token plumbing.
// All Spin structure, indicator geometry, token defaults and CSS are upstream code.
const sourceCache = new Map();
function evaluateSource(file, select) {
  file = path.resolve(file);
  const key = select ? `${file}#${select.join(',')}` : file;
  if (sourceCache.has(key)) return sourceCache.get(key).exports;
  const module = { exports: {} };
  sourceCache.set(key, module);
  let source = read(file, path.relative(upstream, file).replaceAll('\\', '/'));
  if (select) {
    const ast = ts.createSourceFile(file, source, ts.ScriptTarget.Latest, true);
    source = ast.statements.filter(statement => {
      if (ts.isImportDeclaration(statement)) return true;
      if (ts.isVariableStatement(statement)) {
        return statement.declarationList.declarations.some(declaration => select.includes(declaration.name.getText(ast)));
      }
      return statement.name && select.includes(statement.name.getText(ast));
    }).map(statement => statement.getFullText(ast)).join('\n');
  }
  const javascript = ts.transpileModule(source, {
    fileName: file,
    compilerOptions: { module: ts.ModuleKind.CommonJS, target: ts.ScriptTarget.ES2022, jsx: ts.JsxEmit.React, esModuleInterop: true },
  }).outputText;
  const browserId = `source:${path.relative(upstream, file).replaceAll('\\', '/')}${select ? '#' + select.join(',') : ''}`;
  browserDefinitions.set(browserId,javascript);
  const links = {}; browserLinks.set(browserId,links);
  const require = request => {
    if (request === '@rc-component/color-picker') {
      return loadPackage('@rc-component/color-picker/lib/color');
    }
    if (!request.startsWith('.')) {
      links[request]=packageRequest(request).id;
      return loadPackage(request);
    }
    const base = path.resolve(path.dirname(file), request);
    const normalized = path.relative(upstream, base).replaceAll('\\', '/');
    if (normalized === 'components/theme/internal') return themeInternal();
    if (normalized === 'components/config-provider/context') {
      links[request]='adapter:config';
      return { useComponentConfig: () => ({ getPrefixCls: (name, custom) => custom ?? 'ant-' + name, ...currentConfig }) };
    }
    if (normalized === 'components/_util/warning') {
      links[request]='adapter:warning'; return { devUseWarning: () => ({ deprecated() {} }) };
    }
    if (normalized === 'components/spin/style/index' && file.endsWith('index.tsx')) {
      links[request]='adapter:style'; return { __esModule: true, default: () => ['', ''] };
    }
    if (normalized === 'components/theme/interface') {
      return evaluateSource(path.join(upstream, 'components/theme/interface/presetColors.ts'));
    }
    if (normalized === 'components/theme/util/genStyleUtils') {
      return evaluateSource(`${base}.ts`, ['genCssVar']);
    }
    if (normalized === 'components/color-picker/components/ColorPresets') {
      return evaluateSource(`${base}.tsx`, ['isBright']);
    }
    if (normalized === 'components/style/motion') {
      return evaluateSource(path.join(base, 'util.ts'));
    }
    const resolved = [`${base}.ts`, `${base}.tsx`, path.join(base, 'index.ts'), path.join(base, 'index.tsx')]
      .find(file => fs.existsSync(file));
    if (!resolved) throw new Error(`Cannot resolve source ${request} from ${file}`);
    links[request]=`source:${path.relative(upstream,resolved).replaceAll('\\','/')}`;
    return evaluateSource(resolved);
  };
  try {
    new Function('require', 'module', 'exports', javascript)(require, module, module.exports);
  } catch (error) {
    error.message += ` (while evaluating ${file})`;
    throw error;
  }
  return module.exports;
}
function themeInternal() {
  return {
    genStyleHooks: (_name, generate, defaults) => ({ generate, defaults }),
    genSubStyleComponent: (_name, generate, defaults) => ({ generate, defaults }),
    genPresetColor: evaluateSource(path.join(upstream, 'components/theme/util/genPresetColor.ts')).default,
    mergeToken: (...tokens) => Object.assign({}, ...tokens),
    getLineHeight: evaluateSource(path.join(upstream, 'components/theme/themes/shared/genFontSizes.ts')).getLineHeight,
  };
}
const { parseStyle, normalizeStyle } = loadPackage('@ant-design/cssinjs/lib/hooks/useStyleRegister');
const calculator = loadPackage('@ant-design/cssinjs-utils/lib/util/calc').default('num');
// Retain provenance for the real registration contract mirrored by the adapter:
// rootCls defaults to prefixCls and is passed to getCommonStyle as argument three.
read(packageFile('@ant-design/cssinjs-utils', 'lib/util/genStyleUtils.js'),
  '@ant-design/cssinjs-utils@2.1.2/lib/util/genStyleUtils.js');

let currentConfig = {};
const React = loadPackage('react');
const ReactDOMServer = loadPackage('react-dom/server');
loadPackage('react-dom');
loadPackage('react-dom/client');
const Spin = evaluateSource(path.join(upstream, 'components/spin/index.tsx')).default;
const spinStyles = evaluateSource(path.join(upstream, 'components/spin/style/index.ts')).default;
const commonStyles = evaluateSource(path.join(upstream, 'components/style/index.tsx'));
// Populate the module graph by rendering the genuine component, not a reconstructed tree.
ReactDOMServer.renderToStaticMarkup(React.createElement(Spin));
const visualScenes = JSON.parse(read(path.join(crate, 'fixtures/spin/visual-cases.json'), 'native/browser shared visual cases'));
const samples = [];
for (const size of ['small', 'medium', 'large']) {
  for (const percent of [undefined, -20, 0, 1, 25, 50, 65, 100, 140]) {
    for (const description of [undefined, 'Loading...']) samples.push({ size, percent, description, width: 360, height: 88 });
  }
  samples.push({ size, nested: true, description: 'Loading...', width: 360, height: 112 },
    { size, nested: true, spinning: false, width: 360, height: 112 },
    { size, indicator: 'square', percent: 42, width: 360, height: 88 },
    { size, tokenCase: true, width: 360, height: 88 });
}
// Backdrop cases explicitly measure the accepted native compositing limitation;
// keep them visual-only so the independent 728-row numeric matrix stays stable.
samples.push(...Object.values(visualScenes).flat().filter(sample => !sample.fullscreen && !sample.backdropCase));
function cssValue(key, value) {
  if (value && typeof value === 'object' && !Array.isArray(value) && 'r' in value) return `rgba(${value.r},${value.g},${value.b},${value.a})`;
  if (Array.isArray(value)) {
    if (key.startsWith('fontFamily')) return value.map(name => JSON.stringify(name)).join(',');
    if (key.startsWith('motionEase')) return `cubic-bezier(${value.join(',')})`;
    if (key.startsWith('boxShadow') || key.startsWith('dropShadow')) return value.map(shadow => `${shadow.inset ? 'inset ' : ''}${shadow.x}px ${shadow.y}px ${shadow.blur}px ${shadow.spread}px ${cssValue('', shadow.color)}`).join(',');
  }
  if (key.startsWith('motionDuration')) return `${value}s`;
  return value;
}
function serialize(styles) {
  const [css, effects] = parseStyle(styles, { path: 'SpinReference' });
  return normalizeStyle(css) + Object.values(effects).map(effect => normalizeStyle(effect)).join('');
}

// This executes inside the isolated browser and measures actual computed CSS/geometry.
function browserMeasure() {
  const bounds = node => { const b = node.getBoundingClientRect(); return { x: b.x, y: b.y, width: b.width, height: b.height }; };
  const part = node => {
    if (!node) return null;
    const s = getComputedStyle(node);
    return { ...bounds(node), color: s.color, background: s.backgroundColor, opacity: Number(s.opacity),
      fontSize: parseFloat(s.fontSize), lineHeight: s.lineHeight, gap: s.gap, transform: s.transform,
      layoutWidth: parseFloat(s.width), layoutHeight: parseFloat(s.height),
      pointerEvents: s.pointerEvents, borderWidth: s.borderTopWidth, borderRadius: s.borderRadius,
      animationDuration: s.animationDuration, animationDelay: s.animationDelay, animationDirection: s.animationDirection };
  };
  return [...document.querySelectorAll('.demo-case')].map((row, index) => {
    const root = row.firstElementChild;
    if (!root) throw Error(`Missing Spin root in case ${index}`);
    const prefix = '.' + root.classList[0];
    const holder = root.querySelector(prefix + '-dot-holder:not(' + prefix + '-dot-progress)');
    const container = root.querySelector(prefix + '-container');
    const mask = container ? getComputedStyle(container, '::after') : null;
    const circle = root.querySelector(prefix + '-dot-circle:not(' + prefix + '-dot-circle-bg)');
    return { index, options: JSON.parse(row.dataset.options), ...bounds(row), spin: part(root),
      section: part(root.matches(prefix + '-section') ? root : root.querySelector(prefix + '-section')),
      holder: part(holder), progressHolder: part(root.querySelector(prefix + '-dot-progress')), description: part(root.querySelector(prefix + '-description')),
      content: part(container), mask: mask ? { opacity: Number(mask.opacity), background: mask.backgroundColor, pointerEvents: mask.pointerEvents } : null,
      dots: [...root.querySelectorAll(prefix + '-dot-item')].map(part),
      rotation: part(root.querySelector(prefix + '-dot-spin')),
      progress: circle ? { ...part(circle), radius: circle.getAttribute('r'), strokeWidth: circle.getAttribute('stroke-width'), strokeDasharray: getComputedStyle(circle).strokeDasharray, strokeDashoffset: getComputedStyle(circle).strokeDashoffset, value: Number(circle.parentElement.getAttribute('aria-valuenow')), track: getComputedStyle(root.querySelector(prefix + '-dot-circle-bg')).stroke } : null,
      busy: root.getAttribute('aria-busy'), live: root.getAttribute('aria-live'),
      holderSize: parseFloat(getComputedStyle(root).getPropertyValue('--ant-spin-dot-holder-size')) };
  });
}
fs.mkdirSync(output, { recursive: true });
const themes = {};
for (const name of ['default', 'dark', 'compact', 'dark-compact']) {
  const fixture = fixtures.cases.find(f => f.name === name);
  const global = Object.fromEntries(Object.entries(fixture.tokens).map(([key, value]) => [key, cssValue(key, value)]));
  // Ant v6 registration normally turns numeric size aliases into CSS variables.
  // Its raw calc template interpolates marginXXS, so retain that real unit-bearing
  // contract instead of substituting a dimensionless number into CSS subtraction.
  const tokens = { ...global, ...spinStyles.defaults(global), marginXXS: 'var(--ant-margin-xxs)', componentCls: '.ant-spin', antCls: '.ant', iconCls: '.anticon', calc: calculator };
  const custom = { ...tokens, componentCls: '.ant-spin-custom', dotSize: 28, dotSizeSM: 18, dotSizeLG: 44, contentHeight: 120 };
  themes[name] = { global, defaults: spinStyles.defaults(global) };
  const css = serialize([commonStyles.genCommonStyle(tokens, 'ant-spin', 'ant-spin'), commonStyles.genCommonStyle(custom, 'ant-spin-custom', 'ant-spin-custom'), spinStyles.generate(tokens), spinStyles.generate(custom)]);
  for (const scene of ['reference', ...Object.keys(visualScenes)]) {
    const visual = scene !== 'reference';
    for (const direction of visual ? ['ltr', 'rtl'] : ['both']) {
      const dirs = visual ? [direction] : ['ltr', 'rtl'];
      const content = dirs.map(dir => `<section dir="${dir}">${(visual ? visualScenes[scene] : samples).map(sample => `<div class="demo-case" data-options='${JSON.stringify({ ...sample, direction: dir }).replaceAll("'", '&#39;')}' style="width:${sample.width}px;height:${sample.height}px"></div>`).join('')}</section>`).join('');
      const html = `<!doctype html><html lang="en"><meta charset="utf-8"><title>Actual Ant Design Spin · ${name} · ${scene}</title><style>${css}
:root{--ant-margin-xxs:${global.marginXXS}px}body{margin:0;padding:20px;font-family:${global.fontFamily};font-size:${global.fontSize}px;line-height:${global.lineHeight};color:${global.colorText};background:${global.colorBgContainer};width:700px;box-sizing:content-box}section{display:flex;flex-direction:column;align-items:flex-start;gap:12px}section+section{margin-top:24px}.demo-case{display:flex;align-items:center;justify-content:flex-start;flex-shrink:0}.spin-content{width:340px;height:100px;border:1px solid ${global.colorBorder};border-radius:6px;padding:16px;box-sizing:border-box;background:${global.colorBgContainer};font-size:14px;line-height:22px}.spin-square{display:block;box-sizing:border-box;border:2px solid currentColor;border-radius:2px}body:not([data-transitions="true"]) *{transition:none!important}
</style><body data-scene="${scene}">${content}<script>window.__spinTheme=${JSON.stringify({ colorError: global.colorError, colorSuccess: global.colorSuccess })};</script><script src="spin-client.js"></script></body></html>`;
      fs.writeFileSync(path.join(output, `spin-${visual ? `visual-${scene}-${name}-${direction}` : `reference-${name}`}.html`), html);
    }
  }
}
const clientBundle = `(()=>{
const definitions=${JSON.stringify(Object.fromEntries(browserDefinitions))};
const links=${JSON.stringify(Object.fromEntries(browserLinks))};
const cache={};const process={env:{NODE_ENV:'production'}};let currentConfig={};
function load(id){
 if(cache[id])return cache[id].exports;
 if(id==='adapter:config')return{useComponentConfig:()=>({getPrefixCls:(name,custom)=>custom??'ant-'+name,...currentConfig})};
 if(id==='adapter:warning')return{devUseWarning:()=>({deprecated(){}})};
 if(id==='adapter:style')return{__esModule:true,default:()=>['','']};
 if(!definitions[id])throw Error('Missing browser module '+id);
 const module={exports:{}};cache[id]=module;
 const require=request=>{const target=links[id]?.[request];if(!target)throw Error('Unresolved '+request+' from '+id);return load(target);};
 new Function('require','module','exports','process',definitions[id])(require,module,module.exports,process);return module.exports;
}
try{
 const React=load(${JSON.stringify(packageRequest('react').id)}),ReactDOM=load(${JSON.stringify(packageRequest('react-dom').id)}),client=load(${JSON.stringify(packageRequest('react-dom/client').id)});
 const Spin=load('source:components/spin/index.tsx').default,e=React.createElement;
 const roots=[];
 for(const row of document.querySelectorAll('.demo-case')){
  const sample=JSON.parse(row.dataset.options);currentConfig={direction:sample.direction};
  if(sample.backdropCase==='uniform')row.style.background='#426e99';
  if(sample.backdropCase==='stripes')row.style.background='repeating-linear-gradient(90deg,#e58d8d 0px,#e58d8d 40px,#7aa8cc 40px,#7aa8cc 80px,#d6c077 80px,#d6c077 120px)';
  const props={size:sample.size,spinning:sample.spinning,description:sample.description,percent:sample.percent,fullscreen:sample.fullscreen};
  if(sample.tokenCase)props.prefixCls='ant-spin-custom';
  if(sample.nested)props.children=e('div',{className:'spin-content'},'Loading content');
  if(sample.indicator==='square'){
   const prefix=sample.tokenCase?'.ant-spin-custom':'.ant-spin';
   const probe=document.createElement('div');probe.className=prefix.slice(1)+(sample.size==='small'?' '+prefix.slice(1)+'-sm':sample.size==='large'?' '+prefix.slice(1)+'-lg':'');row.append(probe);
   const size=parseFloat(getComputedStyle(probe).getPropertyValue('--ant-spin-dot-holder-size'));probe.remove();
   props.indicator=e('span',{className:'spin-square',style:{width:size,height:size}});
  }
  if(sample.semantic)props.styles={section:{gap:6},indicator:{color:window.__spinTheme.colorError},description:{color:window.__spinTheme.colorSuccess,fontSize:12,lineHeight:'12px'}};
  const root=client.createRoot(row);roots.push(root);ReactDOM.flushSync(()=>root.render(e(Spin,props)));
 }
 const measure=${browserMeasure.toString()};
 const setPhase=time=>{for(const animation of document.getAnimations()){animation.pause();animation.currentTime=time;}return measure();};
 const behavior=async()=>{
  const saved={setTimeout:window.setTimeout,setInterval:window.setInterval,clearTimeout:window.clearTimeout,clearInterval:window.clearInterval,dateNow:Date.now};
  const tasks=new Map();let now=1000000,next=1000000;const origin=now;
  const add=(callback,duration,repeat,args)=>{const id=++next;tasks.set(id,{callback,due:now+Math.max(0,Number(duration)||0),repeat,args});return id;};
  window.setTimeout=(callback,duration,...args)=>add(callback,duration,0,args);
  window.setInterval=(callback,duration,...args)=>add(callback,duration,Math.max(1,Number(duration)||0),args);
  window.clearTimeout=window.clearInterval=id=>tasks.delete(id);Date.now=()=>now;
  const host=document.createElement('div');document.body.append(host);let root=client.createRoot(host);
  const render=async props=>{ReactDOM.flushSync(()=>root.render(e(Spin,props)));await new Promise(resolve=>requestAnimationFrame(()=>requestAnimationFrame(resolve)));ReactDOM.flushSync(()=>{});};
  const take=label=>{const node=host.firstElementChild;return{label,time:now-origin,busy:node?.getAttribute('aria-busy'),progress:Number(node?.querySelector('[role="progressbar"]')?.getAttribute('aria-valuenow')??0),pendingTimers:tasks.size};};
  const advance=duration=>{const end=now+duration;for(let count=0;count<10000;count++){const entry=[...tasks].filter(([,task])=>task.due<=end).sort((a,b)=>a[1].due-b[1].due||a[0]-b[0])[0];if(!entry)break;const[id,task]=entry;now=task.due;if(task.repeat)task.due+=task.repeat;else tasks.delete(id);ReactDOM.flushSync(()=>task.callback(...task.args));}now=end;};
  try{
   currentConfig={direction:'ltr'};const delay=[];
   await render({spinning:true,delay:500});delay.push(take('pending-on-mount'));
   advance(499);delay.push(take('before-deadline'));
   await render({spinning:false,delay:500});delay.push(take('cancel-before-deadline'));
   advance(501);delay.push(take('cancel-stays-hidden'));
   await render({spinning:true,delay:200});advance(200);delay.push(take('show-at-deadline'));
   await render({spinning:false,delay:200});delay.push(take('stop-immediately'));
   await render({spinning:true,delay:500});advance(100);await render({spinning:true,delay:200});advance(199);delay.push(take('changed-delay-pending'));advance(1);delay.push(take('changed-delay-shown'));
   await render({spinning:false});await render({spinning:true,delay:500});ReactDOM.flushSync(()=>root.unmount());delay.push({label:'unmount-cancels',time:now-origin,pendingTimers:tasks.size});
   root=client.createRoot(host);await render({percent:'auto'});const auto=[take('auto-start')];
   for(let tick=1;tick<=240;tick++){advance(200);auto.push(take('auto-tick'));}
   await render({spinning:false,percent:'auto'});const stopped=take('auto-stopped');await render({spinning:true,percent:'auto'});advance(0);const restarted=take('auto-restarted');advance(200);const restartedFirstTick=take('auto-restarted-first-tick');
   ReactDOM.flushSync(()=>root.unmount());return{delay,auto,stopped,restarted,restartedFirstTick,remainingTimersAfterUnmount:tasks.size};
  }finally{window.setTimeout=saved.setTimeout;window.setInterval=saved.setInterval;window.clearTimeout=saved.clearTimeout;window.clearInterval=saved.clearInterval;Date.now=saved.dateNow;host.remove();}
 };
 const transitions=async()=>{
  document.body.dataset.transitions='true';const host=document.createElement('div');host.className='demo-case';host.dataset.options='{}';host.style.cssText='width:360px;height:88px;position:fixed;left:20px;top:20px';document.body.append(host);
  const root=client.createRoot(host),frame=()=>new Promise(resolve=>requestAnimationFrame(resolve));
  const render=percent=>ReactDOM.flushSync(()=>root.render(e(Spin,{percent})));
  const ownAnimations=()=>document.getAnimations().filter(animation=>host.contains(animation.effect?.target));
  const take=time=>{for(const animation of ownAnimations()){animation.pause();animation.currentTime=time;}return{time,...measure().at(-1)};};
  const rows=[];
  try{
   render(0);await frame();await frame();take(0);
   for(const[from,to]of[[0,60],[60,90],[90,0]]){
    render(to);await frame();await frame();
    const animations=ownAnimations();const properties=animations.filter(a=>a.transitionProperty).map(a=>({property:a.transitionProperty,duration:a.effect.getTiming().duration,easing:a.effect.getTiming().easing}));
    rows.push({from,to,properties,samples:[0,75,150,225,300].map(take)});
   }
   return rows;
  }finally{ReactDOM.flushSync(()=>root.unmount());host.remove();delete document.body.dataset.transitions;}
 };
 window.__spinReference={roots,measure,setPhase,behavior,transitions};
 requestAnimationFrame(()=>requestAnimationFrame(()=>{
  const metrics=document.createElement('script');metrics.type='application/json';metrics.id='reference-metrics';metrics.textContent=JSON.stringify(setPhase(0));document.body.append(metrics);window.__spinReference.ready=true;
 }));
}catch(error){document.body.dataset.error=error.stack;console.error(error);}
})();`;
fs.writeFileSync(path.join(output, 'spin-client.js'), clientBundle);
const licenseDirectory = path.join(crate, 'fixtures/spin/licenses');
fs.mkdirSync(licenseDirectory, { recursive: true });
for (const name of [...usedPackages].sort()) {
  const relative = name === '@rc-component/util' && options['util-package'] ? 'LICENSE' : ['LICENSE', 'LICENSE.md', 'LICENSE.txt'].find(file => packageIndex(name).files[file]);
  if (!relative) continue;
  const content = read(packageFile(name, relative), `${name}@${versions[name]}/${relative}`);
  fs.writeFileSync(path.join(licenseDirectory, name.replaceAll('@', '').replaceAll('/', '-') + '.txt'), content.replaceAll('\r\n', '\n'));
}
const provenance = { schemaVersion: 1, upstream: { version: fixtures.provenance.version, revision }, packages: Object.fromEntries([...usedPackages].sort().map(n => [n, versions[n]])), sources: Object.fromEntries([...inputs].sort(([a], [b]) => a.localeCompare(b))), themeFixtureSha256: crypto.createHash('sha256').update(fs.readFileSync(fixturePath)).digest('hex'), generator: { path: 'scripts/generate_spin_reference.mjs', sha256: crypto.createHash('sha256').update(fs.readFileSync(fileURLToPath(import.meta.url))).digest('hex') }, capture: { path: 'scripts/capture_spin_reference.mjs', sha256: crypto.createHash('sha256').update(fs.readFileSync(path.join(crate, 'scripts/capture_spin_reference.mjs'))).digest('hex') }, notes: 'Actual upstream Spin index.tsx, Indicator, Looper, Progress and usePercent mounted through ReactDOMClient. Original style generators and cssinjs serializer. Adapters supply scoped configuration, capture style registration, and preserve unit-bearing marginXXS CSS-variable plumbing. WAAPI pause/currentTime supplies exact animation/transition phases. Delay/auto traces virtualize host timers while original hooks run; React commits drain over two real browser frames. No native Rust or reconstructed Spin markup is read.' };
fs.writeFileSync(path.join(output, 'provenance.json'), JSON.stringify(provenance, null, 2) + '\n');
fs.writeFileSync(path.join(output, 'theme-defaults.json'), JSON.stringify(themes, null, 2) + '\n');
console.log(`Generated actual upstream Spin: ${samples.length * 2} cases per theme; ${Object.keys(visualScenes).length} shared visual scenes.`);
if (options.browser) {
  const destination = path.resolve(options.fixture ?? path.join(crate, 'fixtures/spin/reference.json'));
  const captureScript = path.join(crate, 'scripts/capture_spin_reference.mjs');
  execFileSync(process.execPath, [captureScript, '--reference', output, '--output', path.join(output, 'measurements'), '--browser', options.browser, '--fixture', destination, '--mode', 'metrics'], { stdio: 'inherit', windowsHide: true });
}

