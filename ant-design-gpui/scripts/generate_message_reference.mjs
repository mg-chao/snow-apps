// Copyright (C) 2026 mg-chao
// SPDX-License-Identifier: Apache-2.0
// Developer-only reference. No packages are installed or downloaded by this tool.
// Executes the original pinned Message component and style generators and cssinjs serializer.
// node scripts/generate_message_reference.mjs --upstream E:/ant-design --pnpm-store E:/.pnpm-store/v10

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
const output = path.resolve(options.output ?? path.join(crate, '../build/ant-design-gpui/message-reference'));
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
  '@rc-component/notification': '2.1.1',
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
function externalPackage(name) {
  return name === '@rc-component/util' ? options['util-package'] : name === '@rc-component/notification' ? options['notification-package'] : undefined;
}
function packageFile(name, relative) {
  usedPackages.add(name);
  if (externalPackage(name)) return path.resolve(externalPackage(name), relative);
  const record = packageIndex(name).files[relative];
  if (!record) throw new Error(`Missing ${name}/${relative}`);
  const hex = Buffer.from(record.integrity.slice('sha512-'.length), 'base64').toString('hex');
  return path.join(store, 'files', hex.slice(0, 2), hex.slice(2));
}
function packageRelative(name, candidate) {
  if (externalPackage(name)) {
    const relative = [candidate, `${candidate}.js`, `${candidate}.json`, `${candidate}/index.js`]
      .find(file => fs.existsSync(path.resolve(externalPackage(name), file)) && fs.statSync(path.resolve(externalPackage(name), file)).isFile());
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
// All Message structure, indicator geometry, token defaults and CSS are upstream code.
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
    if (normalized === 'components/_util/hooks') return { CONTAINER_MAX_OFFSET: 1000 };
    if (normalized === 'components/config-provider/context' || normalized === 'components/config-provider') {
      links[request]='adapter:config';
      return { ConfigContext: configContext, useComponentConfig: () => ({ getPrefixCls: (name, custom) => custom ?? 'ant-' + name, ...currentConfig }) };
    }
    if (normalized === 'components/_util/warning') {
      links[request]='adapter:warning'; return { devUseWarning: () => Object.assign(() => {}, { deprecated() {} }) };
    }
    if ((normalized === 'components/message/style' || normalized === 'components/message/style/index') && !file.endsWith('style/index.ts')) {
      links[request]='adapter:style'; return { __esModule: true, default: () => ['', ''], PurePanelStyle: () => null };
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
let currentConfig = {};
const React = loadPackage('react');
const configContext = React.createContext({});
const ReactDOMServer = loadPackage('react-dom/server');
loadPackage('react-dom');
loadPackage('react-dom/client');
const messageStyles = evaluateSource(path.join(upstream, 'components/message/style/index.ts')).default;
const pureStyles = evaluateSource(path.join(upstream, 'components/message/style/index.ts')).PurePanelStyle;
const useMessage = evaluateSource(path.join(upstream, 'components/message/useMessage.tsx')).default;
const PurePanel = evaluateSource(path.join(upstream, 'components/message/PurePanel.tsx')).default;
const commonStyles = evaluateSource(path.join(upstream, 'components/style/index.tsx'));
const SmileOutlined = loadPackage('@ant-design/icons/SmileOutlined').default;
ReactDOMServer.renderToStaticMarkup(React.createElement(PurePanel, { type: 'success', content: 'Saved' }));
const visualScenes = JSON.parse(read(path.join(crate, 'fixtures/message/visual-cases.json'), 'native/browser shared visual cases'));
function cssValue(key, value) {
  // Real token registration emits unitless line-height custom properties. Raw
  // cssinjs serialization otherwise appends px to a numeric custom property.
  if (key.startsWith('lineHeight') && typeof value === 'number') return String(value);
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
  const [css, effects] = parseStyle(styles, { path: 'MessageReference' });
  return normalizeStyle(css) + Object.values(effects).map(effect => normalizeStyle(effect)).join('');
}
function browserMain(settings) {
  const { definitions, links, ids, scene, direction, theme } = settings;
  const cache = {}, process = { env: { NODE_ENV: 'production' } };
  let context;
  const config = { direction };
  function load(id) {
    if (cache[id]) return cache[id].exports;
    if (id === 'adapter:config') {
      context ??= load(ids.react).createContext({});
      return { ConfigContext: context, useComponentConfig: () => ({ getPrefixCls: (name, custom) => custom ?? 'ant-' + name, ...config }) };
    }
    if (id === 'adapter:warning') return { devUseWarning: () => Object.assign(() => {}, { deprecated() {} }) };
    if (id === 'adapter:style') return { __esModule: true, default: () => ['', ''], PurePanelStyle: () => null };
    if (!definitions[id]) throw Error('Missing browser module ' + id);
    const module = { exports: {} }; cache[id] = module;
    const require = request => {
      const target = links[id]?.[request];
      if (!target) throw Error('Unresolved ' + request + ' from ' + id);
      return load(target);
    };
    new Function('require', 'module', 'exports', 'process', definitions[id])(require, module, module.exports, process);
    return module.exports;
  }
  const React = load(ids.react), ReactDOM = load(ids.dom), client = load(ids.client), e = React.createElement;
  const useMessage = load('source:components/message/useMessage.tsx').default;
  const Smile = load(ids.smile).default;
  const frame = () => new Promise(resolve => requestAnimationFrame(resolve));
  const drain = async () => { await frame(); await frame(); ReactDOM.flushSync(() => {}); };
  const prefix = scene.tokenCase ? 'ant-message-custom' : 'ant-message';
  const selector = '.' + prefix;
  let api, root, host;
  function Demo({ options }) {
    const [instance, holder] = useMessage(options);
    api = instance;
    return holder;
  }
  const options = { duration: 0, top: scene.top ?? 8, stack: scene.stack ? { threshold: scene.threshold ?? 3 } : false,
    maxCount: scene.maxCount, prefixCls: prefix, styles: scene.holderStyles ? { list: { padding: 31 }, listContent: { gap: 37 } } : undefined };
  function itemConfig(item) {
    const result = { key: item.key, type: item.type, duration: 0, content: item.content, className: 'case-' + item.key };
    if (item.icon === 'smile') result.icon = e(Smile);
    if (item.rich) result.content = e('span', null, item.content + ' ', e('a', { href: '#details', style: { color: theme.colorLink, textDecoration: 'none' } }, 'View details'));
    if (item.style) result.style = { background: item.style.background, padding: `${item.style.paddingVertical}px ${item.style.paddingHorizontal}px`, borderRadius: item.style.radius };
    if (item.semantic) result.styles = { root: { background: '#fffbe6' }, wrapper: { gap: 12 }, icon: { color: '#722ed1' }, title: { color: '#0958d9', fontSize: 12, lineHeight: '20px' } };
    // Native show_icon(false) maps to the upstream public semantic icon style.
    if (item.icon === 'none') result.styles = { ...result.styles, icon: { ...result.styles?.icon, display: 'none' } };
    return result;
  }
  function measure() {
    const bounds = node => { const b = node.getBoundingClientRect(); return { x: b.x, y: b.y, width: b.width, height: b.height }; };
    const part = node => {
      if (!node) return null;
      const s = getComputedStyle(node);
      return { ...bounds(node), color: s.color, background: s.backgroundColor, opacity: Number(s.opacity), fontSize: parseFloat(s.fontSize),
        lineHeight: parseFloat(s.lineHeight), paddingTop: parseFloat(s.paddingTop), paddingRight: parseFloat(s.paddingRight),
        paddingBottom: parseFloat(s.paddingBottom), paddingLeft: parseFloat(s.paddingLeft), gap: parseFloat(s.gap) || 0,
        radius: parseFloat(s.borderRadius), shadow: s.boxShadow, transform: s.transform, pointerEvents: s.pointerEvents,
        transitionDuration: s.transitionDuration, transitionTimingFunction: s.transitionTimingFunction };
    };
    const nodes = [...document.querySelectorAll(selector + '-notice')];
    return nodes.map(node => {
      const key = [...node.classList].find(value => value.startsWith('case-'))?.slice(5) ?? node.textContent;
      const style = getComputedStyle(node);
      return { key, options: scene.items.find(item => item.key === key), root: part(node),
        wrapper: part(node.querySelector(selector + '-notice-wrapper')), icon: part(node.querySelector(selector + '-notice-icon')),
        title: part(node.querySelector(selector + '-notice-title')), role: node.getAttribute('role'),
        visible: Number(style.opacity) > 0 && style.pointerEvents !== 'none', text: node.textContent };
    });
  }
  function pause(time = 0) { for (const animation of document.getAnimations()) { animation.pause(); animation.currentTime = time; } }
  async function behavior() {
    const saved = { raf: window.requestAnimationFrame.bind(window), caf: window.cancelAnimationFrame.bind(window), date: Date.now };
    let now = 1000000, next = 1000000;
    const tasks = new Map(), callbacks = [], rows = [];
    window.requestAnimationFrame = callback => { const id = ++next; tasks.set(id, callback); return id; };
    window.cancelAnimationFrame = id => tasks.delete(id);
    Date.now = () => now;
    const realDrain = async () => { await new Promise(resolve => saved.raf(() => saved.raf(resolve))); ReactDOM.flushSync(() => {}); };
    const advance = async duration => {
      now += duration;
      const pending = [...tasks]; tasks.clear();
      ReactDOM.flushSync(() => { for (const [, callback] of pending) callback(now); });
      await realDrain();
    };
    const clear = async () => { ReactDOM.flushSync(() => api.destroy()); await realDrain(); await advance(1000); };
    const take = label => rows.push({ label, time: now - 1000000, callbacks: [...callbacks], active: [...document.querySelectorAll(selector + '-notice')].filter(n => !n.className.includes('-leave')).map(n => n.textContent), pendingFrames: tasks.size });
    const open = async config => { ReactDOM.flushSync(() => api.open({ duration: 3, ...config, onClose: () => callbacks.push(config.key) })); await realDrain(); };
    try {
      await clear();
      await open({ key: 'hover', type: 'info', content: 'Hover timer' });
      await advance(1000); take('one-second');
      let node = document.querySelector(selector + '-notice:not([class*="-leave"])');
      ReactDOM.flushSync(() => node.dispatchEvent(new MouseEvent('mouseover', { bubbles: true, relatedTarget: document.body })));
      await realDrain(); await advance(4000); take('hovered-four-seconds');
      ReactDOM.flushSync(() => node.dispatchEvent(new MouseEvent('mouseout', { bubbles: true, relatedTarget: document.body })));
      await realDrain(); await advance(1999); take('remaining-before-deadline'); await advance(1); take('remaining-at-deadline');
      await clear();
      await open({ key: 'update', type: 'loading', content: 'Old' }); await advance(2000);
      await open({ key: 'update', type: 'success', content: 'Updated' }); take('same-duration-update');
      await advance(1000); take('same-duration-original-deadline');
      await clear();
      let resolved = false;
      ReactDOM.flushSync(() => { api.open({ key: 'manual', content: 'Manual close', duration: 0, onClose: () => callbacks.push('manual') }).then(() => { resolved = true; }); });
      await realDrain(); ReactDOM.flushSync(() => api.destroy('manual')); await realDrain(); await advance(1000); take('manual-close');
      rows.at(-1).completionResolved = resolved;
      await clear();
      await open({ key: 'slots', content: 'Semantic slots', duration: 0, styles: { list: { padding: 33 }, listContent: { gap: 41 }, root: { background: '#fffbe6' } } });
      const slots = label => {
        const list = document.querySelector(selector + '-list'), content = document.querySelector(selector + '-list-content');
        const notice = [...document.querySelectorAll(selector + '-notice')].find(node => node.textContent === 'Semantic slots');
        return { label, listPadding: parseFloat(getComputedStyle(list).paddingTop), listContentGap: parseFloat(getComputedStyle(content).gap), rootBackground: getComputedStyle(notice).backgroundColor };
      };
      const semanticSlots = [slots('per-open-list-slots-ignored')];
      ReactDOM.flushSync(() => root.render(e(Demo, { options: { ...options, styles: { list: { padding: 31 }, listContent: { gap: 37 } } } })));
      await realDrain(); semanticSlots.push(slots('holder-list-slots-applied'));
      return { rows, semanticSlots, manualCloseResolvesCompletion: resolved, notes: 'Original upstream callbacks/timers: hover preserves remaining time, same-duration keyed updates retain their deadline, and manual close leaves the upstream completion unresolved.' };
    } finally { ReactDOM.flushSync(() => api.destroy()); await realDrain(); window.requestAnimationFrame = saved.raf; window.cancelAnimationFrame = saved.caf; Date.now = saved.date; }
  }
  async function motion() {
    ReactDOM.flushSync(() => api.destroy()); await drain(); await new Promise(resolve => setTimeout(resolve, 450));
    ReactDOM.flushSync(() => api.open({ key: 'motion', type: 'info', content: 'Motion sample', duration: 0, className: 'case-motion' }));
    await drain(); await new Promise(resolve => setTimeout(resolve, 450));
    const node = document.querySelector(selector + '-notice.case-motion');
    const phaseClass = value => prefix + '-fade-' + value;
    const setPhase = phase => {
      for (const value of [...node.classList]) if (value.startsWith(prefix + '-fade-')) node.classList.remove(value);
      node.classList.add(prefix + '-fade', phaseClass(phase));
      node.getBoundingClientRect();
    };
    const take = time => {
      for (const animation of node.getAnimations()) { animation.pause(); animation.currentTime = time; }
      const style = getComputedStyle(node), bounds = node.getBoundingClientRect();
      return { time, opacity: Number(style.opacity), transform: style.transform, x: bounds.x, y: bounds.y, width: bounds.width, height: bounds.height };
    };
    const properties = () => node.getAnimations().filter(a => a.transitionProperty).map(a => ({ property: a.transitionProperty, duration: a.effect.getTiming().duration, easing: a.effect.getTiming().easing }));
    document.body.dataset.transitions = 'true';
    setPhase('enter-prepare'); await frame(); setPhase('enter-start'); await frame(); setPhase('enter-active');
    const enter = { properties: properties(), samples: [0, 50, 100, 150, 200].map(take) };
    for (const animation of node.getAnimations()) animation.finish();
    setPhase('leave-start'); await frame(); setPhase('leave-active');
    const leave = { properties: properties(), samples: [0, 50, 100, 150, 200].map(take) };
    delete document.body.dataset.transitions;
    return { enter, leave, method: 'Actual upstream card and original stylesheet; framework phase classes applied deterministically, CSS transitions paused and sampled with WAAPI.' };
  }
  async function runtimeMotion() {
    const settled = async () => { await drain(); await new Promise(resolve => setTimeout(resolve, 450)); };
    const reset = async stack => {
      ReactDOM.flushSync(() => root.unmount());
      root = client.createRoot(host);
      ReactDOM.flushSync(() => root.render(e(Demo, { options: { ...options, stack, maxCount: undefined, styles: undefined } })));
      await drain();
      document.body.dataset.transitions = 'true';
    };
    const open = async (key, content) => {
      ReactDOM.flushSync(() => api.open({ key, type: 'info', content, duration: 0, className: 'case-' + key }));
      await drain();
    };
    const card = key => document.querySelector(selector + '-notice.case-' + key);
    const content = () => document.querySelector(selector + '-list-content');
    const ownAnimations = node => document.getAnimations().filter(a => a.effect?.target === node || node.contains(a.effect?.target));
    const waitFor = async (node, property, pseudo) => {
      for (let i = 0; i < 30; i++) {
        const found = ownAnimations(node).filter(a => a.transitionProperty === property && (pseudo === undefined || a.effect?.pseudoElement === pseudo));
        if (found.length) return;
        node.getBoundingClientRect(); await frame();
      }
      throw Error('Original upstream transition missing: ' + property + '/' + pseudo);
    };
    const part = node => {
      const b = node.getBoundingClientRect(), s = getComputedStyle(node);
      return { x: b.x, y: b.y, width: b.width, height: b.height, opacity: Number(s.opacity), transform: s.transform };
    };
    const pseudo = name => {
      const s = getComputedStyle(content(), name);
      return { width: parseFloat(s.width), height: parseFloat(s.height), top: parseFloat(s.top), opacity: Number(s.opacity), transform: s.transform };
    };
    const sample = (node, times, describe) => {
      const animations = ownAnimations(node);
      const properties = animations.filter(a => a.transitionProperty).map(a => ({ property: a.transitionProperty,
        pseudo: a.effect.pseudoElement ?? null, duration: a.effect.getTiming().duration, delay: a.effect.getTiming().delay, easing: a.effect.getTiming().easing }));
      const samples = times.map(time => {
        for (const animation of animations) { animation.pause(); animation.currentTime = time; }
        return { time, ...describe() };
      });
      for (const animation of animations) animation.finish();
      return { properties, samples };
    };
    try {
      await reset(false);
      await open('reflow-a', 'Older message'); await settled();
      await open('reflow-b', 'Newest message'); await settled();
      const reflowStart = { card: part(card('reflow-a')), content: part(content()) };
      ReactDOM.flushSync(() => api.destroy('reflow-b')); await drain();
      await waitFor(content(), 'top');
      const reflow = { start: reflowStart, ...sample(content(), [0, 50, 100, 150, 200, 350, 500, 650, 800], () => ({ card: part(card('reflow-a')), content: part(content()) })) };
      await reset({ threshold: 3 });
      for (const key of ['stack-a', 'stack-b', 'stack-c']) await open(key, 'Stack sample');
      await settled();
      await open('stack-d', 'Stack sample');
      await waitFor(content(), 'opacity', '::before');
      const collapse = sample(content(), [0, 25, 50, 75, 100, 150, 200, 300, 350, 500, 650, 800], () => ({ content: part(content()), before: pseudo('::before'), after: pseudo('::after'), latest: part(card('stack-d')), oldest: part(card('stack-a')) }));
      await drain();
      ReactDOM.flushSync(() => card('stack-d').dispatchEvent(new MouseEvent('mouseover', { bubbles: true, relatedTarget: document.body })));
      await drain(); await waitFor(content(), 'opacity', '::before');
      const expand = sample(content(), [0, 25, 50, 75, 100, 150, 200, 300], () => ({ content: part(content()), before: pseudo('::before'), after: pseudo('::after'), latest: part(card('stack-d')), oldest: part(card('stack-a')) }));
      await drain();
      ReactDOM.flushSync(() => card('stack-d').dispatchEvent(new MouseEvent('mouseout', { bubbles: true, relatedTarget: document.body })));
      await settled();
      const beforeKeyedUpdate = { before: pseudo('::before'), after: pseudo('::after'), latest: part(card('stack-d')) };
      await open('stack-d', 'The latest message becomes substantially wider');
      const sameKeyWidthChange = { before: beforeKeyedUpdate, after: { before: pseudo('::before'), after: pseudo('::after'), latest: part(card('stack-d')) },
        widthTransitions: ownAnimations(content()).filter(a => a.transitionProperty === 'width').length };
      await settled();
      await open('stack-e', 'Another newly opened latest message has a different width');
      await waitFor(content(), 'width', '::before');
      const widthChange = sample(content(), [0, 75, 150, 225, 300], () => ({ before: pseudo('::before'), after: pseudo('::after'), latest: part(card('stack-e')) }));
      await reset(false);
      let rapidPeak = 0;
      const rapidObserver = new MutationObserver(() => {
        rapidPeak = Math.max(rapidPeak, document.querySelectorAll(selector + '-notice.case-rapid').length);
      });
      rapidObserver.observe(document.body, { childList: true, subtree: true });
      ReactDOM.flushSync(() => {
        api.open({ key: 'rapid', type: 'info', content: 'Closed before a visual frame', duration: 0, className: 'case-rapid' });
        api.destroy('rapid');
      });
      await drain(); await drain(); rapidObserver.disconnect();
      const rapidClose = { peakCommittedNotices: rapidPeak, remainingNotices: document.querySelectorAll(selector + '-notice.case-rapid').length };
      await reset(false);
      const pauseAtPhase = (key, phase) => new Promise((resolve, reject) => {
        let observer;
        const timeout = setTimeout(() => { observer.disconnect(); reject(Error('Original interrupted phase missing: ' + phase)); }, 2000);
        const check = () => {
          const node = card(key);
          if (!node || !(node.className.includes('-' + phase + '-active') || (phase === 'enter' && node.className.includes('-appear-active')))) return;
          node.getBoundingClientRect();
          const animations = ownAnimations(node);
          if (phase === 'enter' && !animations.some(a => a.transitionProperty === 'opacity')) return;
          for (const animation of animations) animation.pause();
          observer.disconnect(); clearTimeout(timeout); resolve({ node, animations });
        };
        observer = new MutationObserver(check);
        observer.observe(document.body, { attributes: true, childList: true, subtree: true });
        check();
      });
      const entryReady = pauseAtPhase('interrupted', 'enter');
      ReactDOM.flushSync(() => api.open({ key: 'interrupted', type: 'info', content: 'Interrupted entry', duration: 0, className: 'case-interrupted' }));
      const entry = await entryReady;
      for (const animation of entry.animations) animation.currentTime = 50;
      const beforeClose = part(entry.node);
      const exitReady = pauseAtPhase('interrupted', 'leave');
      ReactDOM.flushSync(() => api.destroy('interrupted'));
      const exit = await exitReady;
      const opacityAnimation = exit.animations.find(a => a.transitionProperty === 'opacity');
      const exitDuration = opacityAnimation?.effect.getTiming().duration ?? 0;
      const middleClose = { entryTime: 50, beforeClose, exitDuration,
        ...sample(exit.node, [0, exitDuration / 4, exitDuration / 2, exitDuration * 3 / 4, exitDuration], () => ({ card: part(exit.node) })) };
      const interrupted = { rapidClose, middleClose };
      return { reflow, collapse, expand, sameKeyWidthChange, widthChange, interrupted, method: 'Original Message/Notification runtime driven by open, destroy, hover and keyed update; original created CSS transitions sampled with WAAPI. No reconstructed styles or native measurements.' };
    } finally { delete document.body.dataset.transitions; }
  }
  async function main() {
    host = document.createElement('div'); document.body.append(host); root = client.createRoot(host);
    ReactDOM.flushSync(() => root.render(e(Demo, { options }))); await drain();
    ReactDOM.flushSync(() => { for (const item of scene.items) api.open(itemConfig(item)); });
    await drain(); await drain();
    if (scene.hover) {
      const node = document.querySelector(selector + '-notice');
      ReactDOM.flushSync(() => node.dispatchEvent(new MouseEvent('mouseover', { bubbles: true, relatedTarget: document.body })));
      await drain();
    }
    pause(0);
    const metrics = document.createElement('script'); metrics.type = 'application/json'; metrics.id = 'reference-metrics'; metrics.textContent = JSON.stringify(measure()); document.body.append(metrics);
    window.__messageReference = { measure, behavior, motion, runtimeMotion, api, root, pause };
  }
  main().catch(error => { document.body.dataset.error = error.stack; console.error(error); });
}
fs.mkdirSync(output, { recursive: true });
const themes = {};
for (const name of ['default', 'dark', 'compact', 'dark-compact']) {
  const fixture = fixtures.cases.find(f => f.name === name);
  const global = Object.fromEntries(Object.entries(fixture.tokens).map(([key, value]) => [key, cssValue(key, value)]));
  const tokens = { ...global, ...messageStyles.defaults(global), componentCls: '.ant-message', antCls: '.ant', iconCls: '.anticon', calc: calculator };
  const custom = { ...tokens, componentCls: '.ant-message-custom', contentBg: '#f6ffed', contentPadding: '14px 20px', borderRadiusLG: 12, fontSize: 16, lineHeight: '1.5', fontSizeLG: 20 };
  themes[name] = { global, defaults: messageStyles.defaults(global) };
  const css = serialize([commonStyles.genCommonStyle(tokens, 'ant-message', 'ant-message'), commonStyles.genCommonStyle(custom, 'ant-message-custom', 'ant-message-custom'),
    commonStyles.genIconStyle('anticon'), commonStyles.genLinkStyle(tokens), messageStyles.generate(tokens), messageStyles.generate(custom), pureStyles.generate(tokens)]);
  for (const [sceneName, scene] of Object.entries(visualScenes)) for (const direction of ['ltr', 'rtl']) {
    const settings = { definitions: Object.fromEntries(browserDefinitions), links: Object.fromEntries(browserLinks),
      ids: { react: packageRequest('react').id, dom: packageRequest('react-dom').id, client: packageRequest('react-dom/client').id, smile: packageRequest('@ant-design/icons/SmileOutlined').id }, scene, direction, theme: global };
    const html = `<!doctype html><html lang="en" dir="${direction}"><meta charset="utf-8"><title>Actual Ant Design Message · ${name} · ${sceneName}</title><style>${css}
body{margin:0;font-family:${global.fontFamily};font-size:${global.fontSize}px;line-height:${global.lineHeight};color:${global.colorText};background:${global.colorBgContainer}}body:not([data-transitions="true"]) *{transition:none!important}
</style><body><script>(${browserMain.toString()})(${JSON.stringify(settings).replaceAll('<', '\\u003c')});</script></body></html>`;
    fs.writeFileSync(path.join(output, `message-visual-${sceneName}-${name}-${direction}.html`), html);
  }
}
const licenseDirectory = path.join(crate, 'fixtures/message/licenses');
fs.mkdirSync(licenseDirectory, { recursive: true });
const licenseCandidates = ['LICENSE', 'LICENSE.md', 'LICENSE.txt', 'license', 'license.md'];
const retainedNotices = {
  '@ant-design/icons-svg': 'licenses/ant-design-icons-MIT.txt',
  'is-mobile': 'fixtures/popup/placement-tools/licenses/is-mobile--MIT.txt',
};
for (const name of [...usedPackages].sort()) {
  const relative = externalPackage(name)
    ? licenseCandidates.find(file => fs.existsSync(path.resolve(externalPackage(name), file)))
    : licenseCandidates.find(file => packageIndex(name).files[file]);
  let notice;
  if (relative) notice = read(packageFile(name, relative), `${name}@${versions[name]}/${relative}`);
  else if (retainedNotices[name]) notice = read(path.join(crate, retainedNotices[name]), `retained-notice:${retainedNotices[name]}`);
  else throw Error(`Missing developer-package notice for ${name}@${versions[name]}`);
  fs.writeFileSync(path.join(licenseDirectory, name.replaceAll('@', '').replaceAll('/', '-') + '.txt'), notice.replaceAll('\r\n', '\n'));
}
const provenance = { schemaVersion: 1, upstream: { version: fixtures.provenance.version, revision }, packages: Object.fromEntries([...usedPackages].sort().map(n => [n, versions[n]])),
  sources: Object.fromEntries([...inputs].sort(([a], [b]) => a.localeCompare(b))), themeFixtureSha256: crypto.createHash('sha256').update(fs.readFileSync(fixturePath)).digest('hex'),
  generator: { path: 'scripts/generate_message_reference.mjs', sha256: crypto.createHash('sha256').update(fs.readFileSync(fileURLToPath(import.meta.url))).digest('hex') },
  capture: { path: 'scripts/capture_message_reference.mjs', sha256: crypto.createHash('sha256').update(fs.readFileSync(path.join(crate, 'scripts/capture_message_reference.mjs'))).digest('hex') },
  notes: 'Actual upstream Message useMessage.tsx/PurePanel.tsx and @rc-component/notification 2.1.1 mounted through ReactDOMClient. Original Message/Notification style generators and cssinjs serializer. Adapters supply scoped configuration and capture style registration only. No native source or reconstructed Message DOM is used.' };
fs.writeFileSync(path.join(output, 'provenance.json'), JSON.stringify(provenance, null, 2) + '\n');
fs.writeFileSync(path.join(output, 'theme-defaults.json'), JSON.stringify(themes, null, 2) + '\n');
console.log(`Generated actual upstream Message: ${Object.keys(visualScenes).length} scenes, four themes, both directions.`);
if (options.browser) execFileSync(process.execPath, [path.join(crate, 'scripts/capture_message_reference.mjs'), '--reference', output, '--output', path.join(output, 'measurements'), '--browser', options.browser, '--fixture', path.resolve(options.fixture ?? path.join(crate, 'fixtures/message/reference.json')), '--mode', 'metrics'], { stdio: 'inherit', windowsHide: true });
