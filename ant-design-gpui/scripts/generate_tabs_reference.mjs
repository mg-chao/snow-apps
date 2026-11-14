// Copyright (C) 2026 mg-chao
// SPDX-License-Identifier: Apache-2.0
// Developer-only visual reference. No packages are installed or downloaded.
// Executes the original pinned Tabs style generators and cssinjs serializer.
// node scripts/generate_tabs_reference.mjs --upstream E:/ant-design --pnpm-store E:/.pnpm-store/v10

import fs from 'node:fs';
import path from 'node:path';
import crypto from 'node:crypto';
import { createRequire, isBuiltin } from 'node:module';
import { execFileSync, spawn } from 'node:child_process';
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
const output = path.resolve(options.output ?? path.join(crate, '../build/ant-design-gpui/tabs-reference'));
const fixturePath = path.join(crate, 'fixtures/theme/reference.json');
const fixtures = JSON.parse(fs.readFileSync(fixturePath, 'utf8'));
const revision = execFileSync('git', ['-C', upstream, 'rev-parse', 'HEAD'], { encoding: 'utf8' }).trim();
if (revision !== fixtures.provenance.revision) throw new Error(`Unexpected Ant Design revision ${revision}`);
process.env.NODE_ENV = 'production';

const versions = {
  '@ant-design/cssinjs': '2.1.2',
  '@ant-design/cssinjs-utils': '2.1.2',
  '@ant-design/fast-color': '3.0.1',
  '@babel/runtime': '7.28.6',
  '@emotion/hash': '0.8.0',
  '@emotion/unitless': '0.7.5',
  '@rc-component/color-picker': '3.1.1',
  '@rc-component/util': '1.9.0',
  clsx: '2.1.1',
  react: '19.2.4',
  'react-dom': '19.2.4',
  'react-is': '18.3.1',
  scheduler: '0.27.0',
  stylis: '4.3.6',
  typescript: '5.9.3',
};
const indexes = new Map();
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
  const record = packageIndex(name).files[relative];
  if (!record) throw new Error(`Missing ${name}/${relative}`);
  const hex = Buffer.from(record.integrity.slice('sha512-'.length), 'base64').toString('hex');
  return path.join(store, 'files', hex.slice(0, 2), hex.slice(2));
}
function packageRelative(name, candidate) {
  const files = packageIndex(name).files;
  const relative = [candidate, `${candidate}.js`, `${candidate}.json`, `${candidate}/index.js`]
    .find(file => files[file]);
  if (!relative) throw new Error(`Cannot resolve ${name}/${candidate}`);
  return relative;
}
const nativeRequire = createRequire(import.meta.url);
const moduleCache = new Map();
function loadPackage(request) {
  if (isBuiltin(request)) return nativeRequire(request);
  const parts = request.split('/');
  const name = request.startsWith('@') ? parts.splice(0, 2).join('/') : parts.shift();
  const relative = parts.length ? parts.join('/') : (() => {
    const metadata = JSON.parse(read(packageFile(name, 'package.json'), `${name}@${versions[name]}/package.json`));
    return (metadata.main ?? 'index.js').replace(/^\.\//, '');
  })();
  return loadPackageFile(name, packageRelative(name, relative));
}
function loadPackageFile(name, relative) {
  const key = `${name}@${versions[name]}/${relative}`;
  if (moduleCache.has(key)) return moduleCache.get(key).exports;
  const source = read(packageFile(name, relative), key);
  const module = { exports: {} };
  moduleCache.set(key, module);
  if (relative.endsWith('.json')) {
    module.exports = JSON.parse(source);
  } else {
    const require = request => request.startsWith('.')
      ? loadPackageFile(name, packageRelative(name, path.posix.normalize(path.posix.join(path.posix.dirname(relative), request))))
      : loadPackage(request);
    new Function('require', 'module', 'exports', source)(require, module, module.exports);
  }
  return module.exports;
}
const ts = nativeRequire(packageFile('typescript', 'lib/typescript.js'));
read(packageFile('typescript', 'lib/typescript.js'), 'typescript@5.9.3/lib/typescript.js');

// These adapters replace only React hook registration and token plumbing.
// All Tabs declarations and selectors are upstream code.
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
    compilerOptions: { module: ts.ModuleKind.CommonJS, target: ts.ScriptTarget.ES2022, jsx: ts.JsxEmit.React },
  }).outputText;
  const require = request => {
    if (!request.startsWith('.')) return loadPackage(request);
    const base = path.resolve(path.dirname(file), request);
    const normalized = path.relative(upstream, base).replaceAll('\\', '/');
    if (normalized === 'components/theme/internal') return themeInternal();
    const resolved = [`${base}.ts`, `${base}.tsx`, path.join(base, 'index.ts'), path.join(base, 'index.tsx')]
      .find(file => fs.existsSync(file));
    if (!resolved) throw new Error(`Cannot resolve source ${request} from ${file}`);
    return evaluateSource(resolved);
  };
  new Function('require', 'module', 'exports', javascript)(require, module, module.exports);
  return module.exports;
}
function themeInternal() {
  return {
    genStyleHooks: (_name, generate, defaults) => ({ generate, defaults }),
    mergeToken: (...tokens) => Object.assign({}, ...tokens),
  };
}
const { parseStyle, normalizeStyle } = loadPackage('@ant-design/cssinjs/lib/hooks/useStyleRegister');
const calculator = loadPackage('@ant-design/cssinjs-utils/lib/util/calc').default('num');
// Retain provenance for the real registration contract mirrored by the adapter:
// rootCls defaults to prefixCls and is passed to getCommonStyle as argument three.
read(packageFile('@ant-design/cssinjs-utils', 'lib/util/genStyleUtils.js'),
  '@ant-design/cssinjs-utils@2.1.2/lib/util/genStyleUtils.js');
const tabsStyles = evaluateSource(path.join(upstream, 'components/tabs/style/index.ts')).default;
const commonStyles = evaluateSource(path.join(upstream, 'components/style/index.tsx'));


function cssValue(key, value) {
  if (value && typeof value === 'object' && !Array.isArray(value) && 'r' in value) {
    return `rgba(${value.r},${value.g},${value.b},${value.a})`;
  }
  if (Array.isArray(value)) {
    if (key.startsWith('fontFamily')) return value.map(name => JSON.stringify(name)).join(',');
    if (key.startsWith('motionEase')) return `cubic-bezier(${value.join(',')})`;
    if (key.startsWith('boxShadow') || key.startsWith('dropShadow')) {
      return value.map(shadow => `${shadow.inset ? 'inset ' : ''}${shadow.x}px ${shadow.y}px ${shadow.blur}px ${shadow.spread}px ${cssValue('', shadow.color)}`).join(',');
    }
  }
  if (key.startsWith('motionDuration')) return `${value}s`;
  return value;
}
function serialize(styles) {
  const [css, effects] = parseStyle(styles, { path: 'TabsReference' });
  return normalizeStyle(css) + Object.values(effects).map(effect => normalizeStyle(effect)).join('');
}
function escape(value) {
  return String(value).replaceAll('&', '&amp;').replaceAll('<', '&lt;').replaceAll('"', '&quot;');
}

// Measured browser DOM follows the checked-in Tabs snapshots. CSS, tokens and
// selectors are evaluated from the reference checkout, never from Rust values.
read(path.join(upstream, 'components/tabs/index.tsx'), 'components/tabs/index.tsx');
read(path.join(upstream, 'components/tabs/__tests__/__snapshots__/demo.test.ts.snap'), 'components/tabs/__tests__/__snapshots__/demo.test.ts.snap');
const samples = [];
for (const size of ['small', 'medium', 'large']) {
  for (const type of ['line', 'card', 'editable-card']) {
    for (const placement of ['top', 'bottom', 'start', 'end']) {
      samples.push({ id: `${size}-${type}-${placement}`, size, type, placement });
    }
  }
}
samples.push({ id: 'icons', icons: true }, { id: 'centered', centered: true },
  { id: 'custom', custom: true }, { id: 'gutter-zero', gutter: 0 });
const customGlobals = { fontSize: 16, fontHeight: 24, lineHeight: 1.5, lineWidth: 2 };
const customComponent = { cardHeight: 52, horizontalItemGutter: 18, inkBarColor: '#eb2f96', itemSelectedColor: '#722ed1' };

function markup(sample, direction) {
  const prefix = sample.custom ? 'ant-tabs-custom' : 'ant-tabs';
  const placement = sample.placement === 'start' ? (direction === 'rtl' ? 'right' : 'left')
    : sample.placement === 'end' ? (direction === 'rtl' ? 'left' : 'right') : (sample.placement || 'top');
  const card = sample.type && sample.type !== 'line';
  const classes = [prefix, `${prefix}-${placement}`,
    ...(direction === 'rtl' ? [`${prefix}-rtl`] : []),
    ...(sample.size !== 'medium' && sample.size ? [`${prefix}-${sample.size}`] : []),
    ...(card ? [`${prefix}-card`] : []),
    ...(sample.type === 'editable-card' ? [`${prefix}-editable`, `${prefix}-editable-card`] : []),
    ...(sample.centered ? [`${prefix}-centered`] : [])];
  const svg = name => `<span class="anticon">${read(path.join(crate, 'resources/icons/generated/outlined', `${name}.svg`), `icons/${name}.svg`).replace('<svg ', '<svg width="1em" height="1em" ')}</span>`;
  const icon = svg('apple');
  const tabs = ['Tab 1', 'Tab 2', 'Tab 3'].map((label, i) => `<div data-item data-selected="${i === 0}" data-disabled="${i === 1}" class="${prefix}-tab${i === 0 ? ` ${prefix}-tab-active` : ''}${i === 1 ? ` ${prefix}-tab-disabled` : ''}"${sample.gutter === 0 && i > 0 ? ' style="margin:0"' : ''}>
    <div data-label class="${prefix}-tab-btn" role="tab" tabindex="${i === 0 ? 0 : -1}">${sample.icons ? `<span class="${prefix}-tab-icon">${icon}</span>` : ''}${label}</div>
    ${sample.type === 'editable-card' && i !== 1 ? `<button data-remove class="${prefix}-tab-remove">${svg('close')}</button>` : ''}</div>`).join('');
  return `<section class="sample" style="direction:${direction}" data-options="${escape(JSON.stringify({...sample, direction, physicalPlacement: placement}))}"><div class="${classes.join(' ')}" style="${['left', 'right'].includes(placement) ? 'height:180px;' : ''}">
    <div data-header class="${prefix}-nav" role="tablist"><div class="${prefix}-nav-wrap"><div data-list class="${prefix}-nav-list">${tabs}<div data-indicator class="${prefix}-ink-bar"></div>${sample.type === 'editable-card' ? `<button data-add class="${prefix}-nav-add">${svg('plus')}</button>` : ''}</div></div></div>
    <div data-body class="${prefix}-body-holder"><div class="${prefix}-body"><div data-content class="${prefix}-content ${prefix}-content-active" role="tabpanel">Content of Tab Pane 1</div></div></div></div></section>`;
}
fs.mkdirSync(output, { recursive: true });
for (const name of ['default', 'dark', 'compact', 'dark-compact']) {
  const aliases = Object.fromEntries(Object.entries(fixtures.cases.find(c => c.name === name).tokens).map(([key, value]) => [key, cssValue(key, value)]));
  const makeStyles = (prefix, globals, patch = {}) => {
    const merged = {...globals, ...patch};
    const token = {...globals, ...tabsStyles.defaults(merged), ...patch, componentCls: `.${prefix}`, antCls: '.ant', iconCls: '.anticon', calc: calculator};
    return [commonStyles.genCommonStyle(token, prefix, prefix), tabsStyles.generate(token)];
  };
  const css = serialize([...makeStyles('ant-tabs', aliases), ...makeStyles('ant-tabs-custom', {...aliases, ...customGlobals}, customComponent)]);
  const stateCss = css.replaceAll(':hover', '[data-hover]').replaceAll(':active', '[data-active]').replaceAll(':focus-visible', '[data-focus-visible]').replaceAll(':focus', '[data-focus]');
  const html = `<!doctype html><html lang="en"><meta charset="utf-8"><title>Tabs reference · ${name}</title><style>${css}${stateCss}
    body{margin:0;padding:24px;font-family:${aliases.fontFamily};font-size:${aliases.fontSize}px;line-height:${aliases.lineHeight};color:${aliases.colorText};background:${aliases.colorBgContainer}}
    .sample{width:480px;margin-bottom:24px;display:flow-root}.anticon{display:inline-flex;align-items:center;vertical-align:-.125em;line-height:0}.anticon svg{display:inline-block}
    *{animation:none!important;transition:none!important}
  </style><body>${['ltr','rtl'].map(direction => samples.map(sample => markup(sample,direction)).join('')).join('')}<script>
    const params = new URLSearchParams(location.search);
    const state = params.get('state') || 'rest';
    if (params.has('sample')) {
      for (const sample of document.querySelectorAll('.sample')) {
        const options = JSON.parse(sample.dataset.options);
        if (options.id !== params.get('sample') || options.direction !== params.get('direction')) sample.remove();
      }
    }
    const measure = element => {
      if (!element) return null;
      const r = element.getBoundingClientRect(), c = getComputedStyle(element);
      const numbers = ['fontSize','lineHeight','paddingTop','paddingRight','paddingBottom','paddingLeft','marginTop','marginRight','marginBottom','marginLeft','borderTopWidth','borderRightWidth','borderBottomWidth','borderLeftWidth','borderTopLeftRadius','borderTopRightRadius','borderBottomRightRadius','borderBottomLeftRadius','outlineWidth','outlineOffset'];
      return {x:r.x,y:r.y,width:r.width,height:r.height, ...Object.fromEntries(numbers.map(k => [k, parseFloat(c[k])])), color:c.color,background:c.backgroundColor,borderColor:c.borderColor,outlineColor:c.outlineColor,boxShadow:c.boxShadow};
    };
    const results = [...document.querySelectorAll('.sample')].map(sample => {
      const options = JSON.parse(sample.dataset.options), root = sample.firstElementChild;
      const prefix = options.custom ? 'ant-tabs-custom' : 'ant-tabs';
      const items = [...sample.querySelectorAll('[data-item]')];
      for (const [index,item] of items.entries()) {
        if (params.has('sample') && state !== 'rest' && index !== (state==='focus'?0:2)) continue;
        if (state === 'hover') item.setAttribute('data-hover','');
        if (state === 'active') {item.querySelector('[data-label]').setAttribute('data-active','');item.querySelector('[data-label]').setAttribute('data-focus','');}
        if (state === 'focus') {item.classList.add(prefix + '-tab-focus');item.querySelector('[data-label]').setAttribute('data-focus-visible','');}
      }
      const vertical = ['left','right'].includes(options.physicalPlacement);
      // The rc-tabs navigation reserves operations only when its intrinsic list
      // exceeds the available axis. Reconstruct that measured layout for the
      // screenshot harness; style-only fixture samples keep their original DOM.
      if (params.has('sample')) {
        const wrap=sample.querySelector('.'+prefix+'-nav-wrap');
        const list=sample.querySelector('[data-list]');
        const axis=vertical?'height':'width';
        if (list.getBoundingClientRect()[axis] > wrap.getBoundingClientRect()[axis] + .5) {
          const operations=document.createElement('div'); operations.className=prefix+'-nav-operations';
          const more=document.createElement('button');more.className=prefix+'-nav-more';
          more.innerHTML=${JSON.stringify(fs.readFileSync(path.join(crate, 'resources/icons/generated/outlined/ellipsis.svg'),'utf8').replace('<svg ', '<svg width="1em" height="1em" ').trim())};
          operations.append(more);
          const add=sample.querySelector('[data-add]');if(add)operations.append(add);
          wrap.after(operations);
        }
      }
      sample.querySelector('[data-indicator]').style[vertical ? 'height' : 'width'] = (vertical ? items[0].getBoundingClientRect().height : items[0].getBoundingClientRect().width) + 'px';
      return {options,state,root:measure(root),header:measure(sample.querySelector('[data-header]')),list:measure(sample.querySelector('[data-list]')),body:measure(sample.querySelector('[data-body]')),content:measure(sample.querySelector('[data-content]')),indicator:measure(sample.querySelector('[data-indicator]')),add:measure(sample.querySelector('[data-add]')),items:items.map(item => ({selected:item.dataset.selected==='true',disabled:item.dataset.disabled==='true',box:measure(item),label:measure(item.querySelector('[data-label]')),remove:measure(item.querySelector('[data-remove]'))}))};
    });
    const payload=document.createElement('script');payload.type='application/json';payload.id='reference-metrics';payload.textContent=JSON.stringify(results);document.body.append(payload);
  </script></body></html>`;
  fs.writeFileSync(path.join(output, `tabs-reference-${name}.html`), html);
}
const provenance = {
  upstream: {version: fixtures.provenance.version, revision},
  packages: Object.fromEntries([...indexes.keys()].sort().map(name => [name, versions[name]])),
  sources: Object.fromEntries([...inputs].sort(([a],[b]) => a.localeCompare(b))),
  themeFixtureSha256: crypto.createHash('sha256').update(fs.readFileSync(fixturePath)).digest('hex'),
  generator: {path:'scripts/generate_tabs_reference.mjs',sha256:crypto.createHash('sha256').update(fs.readFileSync(fileURLToPath(import.meta.url))).digest('hex')},
  customGlobals, customComponent,
  notes: 'Original Ant Design Tabs style generators and snapshot-derived DOM. Hook registration is adapted. CSS state attributes preserve selector specificity. This measures appearance, not React event behavior.',
};
fs.writeFileSync(path.join(output,'provenance.json'),JSON.stringify(provenance,null,2)+'\n');
if (options.browser) {
  const cases = [];
  for (const theme of ['default','dark','compact','dark-compact']) {
    for (const state of ['rest','hover','active','focus']) {
      const results = await capture(theme, state);
      if (results.length !== samples.length * 2) throw Error('Incomplete measurement');
      cases.push({theme,state,tabs:results});
    }
  }
  provenance.environment = {platform:process.platform,browser:options.browser,scaleFactor:1,viewport:[1180,1040]};
  const destination = path.resolve(options.fixture ?? path.join(crate,'fixtures/tabs/reference.json'));
  fs.mkdirSync(path.dirname(destination),{recursive:true});
  // Intern repeated computed styles while retaining every measured scenario.
  // Full coordinates and HTML remain in the ignored capture directory.
  const measurements=[], known=new Map();
  const intern=value=>{
    if(value===null)return null;
    const {x,y,...measurement}=value;
    const signature=JSON.stringify(measurement);
    if(!known.has(signature)){known.set(signature,measurements.length);measurements.push(measurement);}
    return known.get(signature);
  };
  for(const entry of cases) for(const sample of entry.tabs) {
    sample.positions=sample.items.map(item=>[item.box.x-sample.root.x,item.box.y-sample.root.y]);
    for(const name of ['root','header','list','body','content','indicator','add'])sample[name]=intern(sample[name]);
    for(const item of sample.items)for(const name of ['box','label','remove'])item[name]=intern(item[name]);
  }
  fs.writeFileSync(destination,JSON.stringify({provenance,measurements,cases},null,2)+'\n');
  console.log(`Saved ${cases.reduce((sum,c) => sum+c.tabs.length,0)} measured Tabs states to ${destination}`);
} else console.log(`Generated Tabs references in ${output}`);

function capture(theme, state) {
  return new Promise((resolve,reject) => {
    const profile=path.join(output,`headless-${crypto.randomUUID()}`);
    const url=new URL(`file:///${path.join(output,`tabs-reference-${theme}.html`).replaceAll('\\','/')}?state=${state}`).href;
    const child=spawn(options.browser,['--headless','--disable-gpu','--no-first-run','--hide-scrollbars','--disable-extensions','--disable-background-networking','--disable-component-update','--force-device-scale-factor=1','--window-size=1180,1040','--virtual-time-budget=1000','--dump-dom',`--user-data-dir=${profile}`,url],{windowsHide:true});
    let stdout='',stderr='',finished=false;
    const done=error => {
      if(finished)return;finished=true;clearTimeout(timeout);
      fs.writeFileSync(path.join(output,`metrics-${theme}-${state}.dom.html`),stdout);
      fs.writeFileSync(path.join(output,`metrics-${theme}-${state}.stderr.txt`),stderr);
      if(child.exitCode===null && child.pid) {
        if(process.platform==='win32'){try{execFileSync('taskkill',['/PID',String(child.pid),'/T','/F'],{windowsHide:true,stdio:'ignore'});}catch{}}
        else child.kill();
      }
      if(error)return reject(error);
      try{resolve(JSON.parse(stdout.match(/<script type="application\/json" id="reference-metrics">([^<]+)<\/script>/)[1]));}catch(e){reject(e);}
    };
    const timeout=setTimeout(()=>done(Error(`Browser timed out: ${theme}/${state}`)),20000);
    child.stdout.on('data',chunk=>{stdout+=chunk;if(/<script type="application\/json" id="reference-metrics">([^<]+)<\/script>/.test(stdout))done();});
    child.stderr.on('data',chunk=>stderr+=chunk);child.on('error',done);child.on('close',()=>{if(!finished)done(Error(`Missing metrics: ${theme}/${state}`));});
  });
}
