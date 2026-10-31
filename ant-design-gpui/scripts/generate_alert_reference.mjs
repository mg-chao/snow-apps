// Copyright (C) 2026 mg-chao
// SPDX-License-Identifier: Apache-2.0
// Developer-only reference. No packages are installed or downloaded by this tool.
// Executes the original pinned Alert component and style generators and cssinjs serializer.
// node scripts/generate_alert_reference.mjs --upstream E:/ant-design --pnpm-store E:/.pnpm-store/v10

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
const output = path.resolve(options.output ?? path.join(crate, '../build/ant-design-gpui/alert-reference'));
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
// All Tag geometry, state, hover, focus and disabled CSS is upstream code.
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
    compilerOptions: { module: ts.ModuleKind.CommonJS, target: ts.ScriptTarget.ES2022, jsx: ts.JsxEmit.React },
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
    if (normalized === 'components/alert/style' && file.endsWith('Alert.tsx')) {
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

// Execute Alert.tsx itself through React's renderer. Only ConfigProvider/token
// registration is adapted; Alert's element tree and default resolution are real.
let currentConfig = {};
const React = loadPackage('react');
const { renderToStaticMarkup } = loadPackage('react-dom/server');
const Alert = evaluateSource(path.join(upstream, 'components/alert/Alert.tsx')).default;
const alertStyles = evaluateSource(path.join(upstream, 'components/alert/style/index.ts')).default;
const commonStyles = evaluateSource(path.join(upstream, 'components/style/index.tsx'));
loadPackage('react-dom/client');
const e = React.createElement;

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
  const [css, effects] = parseStyle(styles, { path: 'AlertReference' });
  return normalizeStyle(css) + Object.values(effects).map(effect => normalizeStyle(effect)).join('');
}
const samples = [];
for (const type of ['success', 'info', 'warning', 'error']) {
  for (const variant of ['outlined', 'filled']) {
    for (const mode of ['title', 'description', 'description-only', 'wrap', 'long-word']) {
      for (const showIcon of [false, true]) {
        samples.push({ type, variant, mode, showIcon, closable: true });
      }
    }
  }
}
samples.push(
  { mode: 'title', label: 'Notice' },
  { mode: 'title', banner: true },
  { mode: 'title', banner: true, showIcon: false },
  { mode: 'description', banner: true, type: 'error', closable: true },
  { mode: 'description', showIcon: true, customIcon: true },
  { mode: 'title', customIcon: true },
  { mode: 'title', closable: true, closeDisabled: true },
  { mode: 'title', closable: true, closeText: true },
  { mode: 'title', showIcon: true, closable: true, action: true },
  { mode: 'description', showIcon: true, closable: true, action: true },
  { mode: 'wrap', showIcon: true, closable: true, firstLine: true },
  { mode: 'empty', closable: false },
  { mode: 'rich', showIcon: true, closable: true },
  { mode: 'title', showIcon: true, closable: true, tokenCase: true },
  { mode: 'description', showIcon: true, closable: true, tokenCase: true },
  { mode: 'description', showIcon: true, closable: true, tokenCase: true, banner: true },
);

const visualScenes = JSON.parse(read(path.join(crate, 'fixtures/alert/visual-cases.json'), 'native/browser shared visual cases'));

function renderAlert(sample, direction, index) {
  currentConfig = { direction };
  const title = sample.label ?? (sample.mode === 'wrap'
    ? 'Long alert title wraps to multiple lines when the alert container is narrow enough.'
    : sample.mode === 'long-word'
      ? 'LongUnbrokenAlertContentThatMustWrapWithinItsAvailableContentWidthWithoutPushingTheCloseButtonAway'
      : 'Alert title');
  const description = 'Additional description and information about this alert.';
  const props = { title, ...sample, 'data-case': index, 'data-options': JSON.stringify({ ...sample, direction }) };
  if (sample.tokenCase) props.prefixCls = 'ant-alert-custom';
  if (['description', 'description-only'].includes(sample.mode)) props.description = description;
  if (['description-only', 'empty'].includes(sample.mode)) delete props.title;
  if (sample.mode === 'rich') {
    props.title = e('span', null, 'Rich ', e('strong', null, 'alert title'));
    props.description = e('span', null, 'Description with ', e('strong', null, 'details'));
  }
  if (sample.customIcon) {
    const Smile = loadPackage('@ant-design/icons/SmileOutlined').default;
    props.icon = e(Smile);
  }
  if (sample.closable) {
    props.closable = { disabled: sample.closeDisabled ?? false, 'aria-label': 'Close alert' };
    if (sample.closeText) props.closable.closeIcon = 'Dismiss';
  }
  if (sample.action) props.action = e('button', { type: 'button', className: 'reference-action' }, 'Action');
  if (sample.firstLine) {
    props.styles = { root: { alignItems: 'flex-start' }, icon: { marginBlockStart: 4 }, close: { marginBlockStart: 5 } };
  }
  return renderToStaticMarkup(e(Alert, props));
}

const measureScript = `
const state = new URLSearchParams(location.search).get('state');
if (state === 'hover' || state === 'focus') {
  const sheet = document.createElement('style');
  sheet.textContent = document.querySelector('style').textContent
    .replaceAll(':hover', '[data-hover]').replaceAll(':focus-visible', '[data-focus]');
  document.head.append(sheet);
  const nodes=[...document.querySelectorAll('.anticon-close,button[class*="-close-icon"]')];
  (document.body.dataset.visual === 'true' ? nodes.slice(0,2) : nodes).forEach(node => node.setAttribute('data-' + state, ''));
}
const bounds = node => { const b=node.getBoundingClientRect(); return {x:b.x,y:b.y,width:b.width,height:b.height}; };
const part = (root, selector) => {
  const node = root.querySelector(selector); if (!node) return null;
  const s=getComputedStyle(node);
  return {...bounds(node), color:s.color, fontSize:s.fontSize, lineHeight:s.lineHeight,
    fontWeight:s.fontWeight, gap:s.gap, marginStart:s.marginInlineStart, marginEnd:s.marginInlineEnd,
    marginTop:s.marginTop, marginBottom:s.marginBottom, alignItems:s.alignItems, minWidth:s.minWidth,
    padding:s.padding, outlineColor:s.outlineColor,outlineWidth:s.outlineWidth,outlineOffset:s.outlineOffset};
};
const metrics = document.createElement('script');metrics.type='application/json';metrics.id='reference-metrics';
metrics.textContent=JSON.stringify([...document.querySelectorAll('[data-case]')].map(root=>{
 const s=getComputedStyle(root),prefix='.'+root.classList[0];
 return {index:Number(root.dataset.case),options:JSON.parse(root.dataset.options),...bounds(root),
 background:s.backgroundColor,borderColor:s.borderColor,borderWidth:s.borderTopWidth,radius:s.borderRadius,
 color:s.color,fontSize:s.fontSize,lineHeight:s.lineHeight,fontWeight:s.fontWeight,
 padding:[s.paddingTop,s.paddingRight,s.paddingBottom,s.paddingLeft],direction:s.direction,alignItems:s.alignItems,
 title:part(root,prefix+'-title'),description:part(root,prefix+'-description'),
 icon:part(root,prefix+'-icon'),iconGlyph:part(root,prefix+'-icon .anticon'),
 section:part(root,prefix+'-section'),actions:part(root,prefix+'-actions'),
 close:part(root,prefix+'-close-icon'),closeGlyph:part(root,prefix+'-close-icon .anticon'),
 role:root.getAttribute('role'),closeDisabled:root.querySelector('button'+prefix+'-close-icon')?.disabled??false,
 closeLabel:root.querySelector('button'+prefix+'-close-icon')?.getAttribute('aria-label')??null};
}));document.body.append(metrics);`;

fs.mkdirSync(output, { recursive: true });
for (const name of ['default', 'dark', 'compact', 'dark-compact']) {
  const fixture = fixtures.cases.find(fixture => fixture.name === name);
  const global = Object.fromEntries(Object.entries(fixture.tokens).map(([key, value]) => [key, cssValue(key, value)]));
  const tokens = { ...global, ...alertStyles.defaults(global), componentCls: '.ant-alert', antCls: '.ant', iconCls: '.anticon', calc: calculator };
  const custom = { ...tokens, componentCls: '.ant-alert-custom', borderRadius: 5, defaultPadding: '10px 18px', withDescriptionPadding: '14px 18px', withDescriptionIconSize: 32 };
  const css = serialize([commonStyles.genCommonStyle(tokens, 'ant-alert', 'ant-alert'), commonStyles.genIconStyle('anticon'), alertStyles.generate(tokens), alertStyles.generate(custom)]);
  if (name === 'default') {
    fs.writeFileSync(path.join(output,'alert-lifecycle.html'),`<!doctype html><html lang="en"><meta charset="utf-8"><title>Actual upstream Alert close lifecycle</title><style>${css}body{margin:20px;width:700px}</style><body><div id="lifecycle"></div><script src="alert-client.js"></script></body></html>`);
  }
  for (const scene of ['reference', ...Object.keys(visualScenes)]) {
    const visual = scene !== 'reference';
    for (const direction of (visual ? ['ltr', 'rtl'] : ['both'])) {
      let index = 0;
      const dirs = visual ? [direction] : ['ltr', 'rtl'];
      const content = dirs.map(dir => `<section dir="${dir}">${(visual ? visualScenes[scene] : samples).map(sample =>
        `<div class="demo-case"${sample.mode === 'wrap' || sample.mode === 'long-word' ? ' style="width:360px"' : ''}>${renderAlert(sample, dir, index++)}</div>`).join('')}</section>`).join('');
      const html = `<!doctype html><html lang="en"><meta charset="utf-8"><title>Original Ant Design Alert · ${name}</title><style>
${css}
body{margin:0;padding:20px;font-family:${global.fontFamily};font-size:${global.fontSize}px;line-height:${global.lineHeight};color:${global.colorText};background:${global.colorBgContainer};width:700px;box-sizing:content-box}
section{display:flex;flex-direction:column;gap:12px}section+section{margin-top:24px}.demo-case{width:700px}
.reference-action{font:inherit;font-size:12px;height:24px;box-sizing:border-box;border:1px solid ${global.colorBorder};border-radius:4px;background:${global.colorBgContainer};color:${global.colorText};padding:0 7px}
*{animation:none!important;transition:none!important}
</style><body data-visual="${visual}">${content}<script>${measureScript}</script></body></html>`;
      const label = visual ? `visual-${scene}-${name}-${direction}` : `reference-${name}`;
      fs.writeFileSync(path.join(output, `alert-${label}.html`), html);
    }
  }
}
const clientBundle = `(()=>{
const definitions=${JSON.stringify(Object.fromEntries(browserDefinitions))};
const links=${JSON.stringify(Object.fromEntries(browserLinks))};
const cache={};const process={env:{NODE_ENV:'production'}};
function load(id){
 if(cache[id])return cache[id].exports;
 if(id==='adapter:config')return{useComponentConfig:()=>({getPrefixCls:(name,custom)=>custom??'ant-'+name,direction:'ltr'})};
 if(id==='adapter:warning')return{devUseWarning:()=>({deprecated(){}})};
 if(id==='adapter:style')return{__esModule:true,default:()=>['','']};
 if(!definitions[id])throw Error('Missing browser module '+id);
 const module={exports:{}};cache[id]=module;
 const require=request=>{const target=links[id]?.[request];if(!target)throw Error('Unresolved '+request+' from '+id);return load(target);};
 new Function('require','module','exports','process',definitions[id])(require,module,module.exports,process);
 return module.exports;
}
try{
 const React=load(${JSON.stringify(packageRequest('react').id)});
 const ReactDOM=load(${JSON.stringify(packageRequest('react-dom').id)});
 const client=load(${JSON.stringify(packageRequest('react-dom/client').id)});
 const Alert=load('source:components/alert/Alert.tsx').default;
 const mode=new URLSearchParams(location.search).get('mode')??'description';
 const events=[],samples=[];let started=0;
 const root=client.createRoot(document.querySelector('#lifecycle'));
 const record=kind=>events.push({kind,time:performance.now()-started});
 ReactDOM.flushSync(()=>root.render(React.createElement(Alert,{title:'Alert title',description:mode!=='title'?'Additional description and information about this alert.':undefined,styles:mode==='styled'?{root:{padding:'14px 18px',marginBottom:16}}:undefined,type:'warning',showIcon:true,closable:{'aria-label':'Close alert',onClose:()=>record('onClose'),afterClose:()=>record('afterClose')}})));
 const measure=node=>{if(!node)return null;const s=getComputedStyle(node),b=node.getBoundingClientRect();return{x:b.x,y:b.y,width:b.width,height:b.height,fontSize:s.fontSize,lineHeight:s.lineHeight,padding:s.padding,marginBottom:s.marginBottom,borderWidth:s.borderTopWidth,inlineStyle:node.getAttribute('style')};};
 const beforeNode=document.querySelector('.ant-alert');
 const before={root:measure(beforeNode),title:measure(beforeNode.querySelector('.ant-alert-title')),description:measure(beforeNode.querySelector('.ant-alert-description'))};
 const cssRules=[...document.styleSheets].flatMap(sheet=>[...sheet.cssRules]).filter(rule=>rule.selectorText&&(/ant-alert-motion-leave|ant-alert-with-description/.test(rule.selectorText))).map(rule=>rule.cssText);
 setTimeout(()=>{
  started=performance.now();
  ReactDOM.flushSync(()=>document.querySelector('.ant-alert-close-icon').click());
  const take=()=>{
   const node=document.querySelector('.ant-alert'),s=node?getComputedStyle(node):null;
   samples.push({time:performance.now()-started,height:node?.getBoundingClientRect().height??0,maxHeight:s?.maxHeight??null,opacity:s?.opacity??'0',paddingTop:s?.paddingTop??'0px',paddingBottom:s?.paddingBottom??'0px',marginBottom:s?.marginBottom??'0px',transitionProperty:s?.transitionProperty??null,transitionDuration:s?.transitionDuration??null,transitionTimingFunction:s?.transitionTimingFunction??null,inlineStyle:node?.getAttribute('style')??null,classes:node?.className??null});
   if(performance.now()-started<650)setTimeout(take,16);else{
    const result=document.createElement('script');result.type='application/json';result.id='lifecycle-metrics';result.textContent=JSON.stringify({mode,beforeHeight:before.root.height,before,cssRules,events,samples});document.body.append(result);
   }
  };take();
 },80);
}catch(error){const result=document.createElement('script');result.type='application/json';result.id='lifecycle-metrics';result.textContent=JSON.stringify({error:error.stack});document.body.append(result);}
})();`;
fs.writeFileSync(path.join(output,'alert-client.js'),clientBundle);
const licenseDirectory = path.join(crate, 'fixtures/alert/licenses');
fs.mkdirSync(licenseDirectory, { recursive: true });
for (const name of ['react', 'react-dom', '@rc-component/motion', '@rc-component/util', '@ant-design/icons', '@ant-design/cssinjs']) {
  const relative = name === '@rc-component/util' && options['util-package'] ? 'LICENSE'
    : ['LICENSE', 'LICENSE.md', 'LICENSE.txt'].find(file => packageIndex(name).files[file]);
  if (!relative) throw new Error(`Missing original license for ${name}`);
  const content = read(packageFile(name, relative), `${name}@${versions[name]}/${relative}`);
  fs.writeFileSync(path.join(licenseDirectory, name.replaceAll('@', '').replaceAll('/', '-') + '.txt'), content.replaceAll('\r\n', '\n'));
}
const provenance = {
  schemaVersion: 1, upstream: { version: fixtures.provenance.version, revision },
  packages: Object.fromEntries([...usedPackages].sort().map(name => [name, versions[name]])),
  sources: Object.fromEntries([...inputs].sort(([a], [b]) => a.localeCompare(b))),
  themeFixtureSha256: crypto.createHash('sha256').update(fs.readFileSync(fixturePath)).digest('hex'),
  generator: { path: 'scripts/generate_alert_reference.mjs', sha256: crypto.createHash('sha256').update(fs.readFileSync(fileURLToPath(import.meta.url))).digest('hex') },
  notes: 'Actual upstream Alert.tsx rendered through ReactDOMServer, original CSSMotion and Ant Design icon components, original style generators and cssinjs serializer. Adapters supply scoped configuration and capture CSS registration. No native Rust or manually reconstructed Alert DOM is read.',
};
fs.writeFileSync(path.join(output, 'provenance.json'), JSON.stringify(provenance, null, 2) + '\n');
console.log(`Generated real upstream Alert references: ${samples.length * 2} cases per theme; two shared visual scenes.`);

if (options.browser) {
  const cases = [];
  for (const theme of ['default', 'dark', 'compact', 'dark-compact']) {
    for (const state of ['rest', 'hover', 'focus']) {
      const alerts = await capture(theme, state, options.browser);
      if (alerts.length !== samples.length * 2) throw new Error(`Incomplete ${theme}/${state} capture`);
      cases.push({ theme, state, alerts });
      console.log(`Captured ${theme}/${state}: ${alerts.length} alerts`);
    }
  }
  const versionScript = `$ProgressPreference = 'SilentlyContinue'; (Get-Item -LiteralPath '${options.browser.replaceAll("'", "''")}').VersionInfo.ProductVersion`;
  const version = process.platform === 'win32'
    ? execFileSync('powershell', ['-NoProfile', '-EncodedCommand', Buffer.from(versionScript, 'utf16le').toString('base64')], { encoding: 'utf8', windowsHide: true }).trim()
    : execFileSync(options.browser, ['--version'], { encoding: 'utf8' }).trim();
  provenance.renderer = { name: path.basename(options.browser), version, platform: process.platform, scaleFactor: 1, viewport: [740, 1200], motion: 'disabled' };
  const destination = path.resolve(options.fixture ?? path.join(crate, 'fixtures/alert/reference.json'));
  fs.mkdirSync(path.dirname(destination), { recursive: true });
  fs.writeFileSync(destination, JSON.stringify({ provenance, cases }) + '\n');
  console.log(`Saved ${cases.reduce((sum, entry) => sum + entry.alerts.length, 0)} independent Alert browser measurements to ${destination}`);
}

function capture(theme, state, browser) {
  return new Promise((resolve, reject) => {
    const profile = path.join(output, `headless-${theme}-${state}-${crypto.randomUUID()}`);
    const html = path.join(output, `alert-reference-${theme}.html`);
    const url = new URL(`file:///${html.replaceAll('\\', '/')}?state=${state}`).href;
    const child = spawn(browser, ['--headless', '--disable-gpu', '--no-first-run', '--hide-scrollbars', '--disable-extensions', '--disable-background-networking', '--disable-component-update', '--force-device-scale-factor=1', '--window-size=740,1200', '--virtual-time-budget=1000', '--dump-dom', `--user-data-dir=${profile}`, url], { windowsHide: true });
    let stdout = '', stderr = '', finished = false;
    const stop = () => {
      if (child.exitCode !== null || !child.pid) return;
      if (process.platform === 'win32') {
        try { execFileSync('taskkill', ['/PID', String(child.pid), '/T', '/F'], { windowsHide: true, stdio: 'ignore' }); } catch {}
      } else child.kill();
    };
    const finish = error => {
      if (finished) return;
      finished = true; clearTimeout(timeout);
      fs.writeFileSync(path.join(output, `metrics-${theme}-${state}.dom.html`), stdout);
      fs.writeFileSync(path.join(output, `metrics-${theme}-${state}.stderr.txt`), stderr);
      stop();
      if (error) reject(error);
      else {
        try { resolve(JSON.parse(stdout.match(/<script type="application\/json" id="reference-metrics">([^<]+)<\/script>/)[1])); }
        catch (error) { reject(error); }
      }
    };
    const timeout = setTimeout(() => finish(new Error(`Headless browser timed out for ${theme}/${state}`)), 20000);
    child.stdout.on('data', chunk => {
      stdout += chunk;
      if (/<script type="application\/json" id="reference-metrics">([^<]+)<\/script>/.test(stdout)) finish();
    });
    child.stderr.on('data', chunk => { stderr += chunk; });
    child.on('error', finish);
    child.on('close', () => { if (!finished) finish(new Error(`Headless browser exited before emitting ${theme}/${state}; see ${output}`)); });
  });
}
