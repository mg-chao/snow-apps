// Copyright (C) 2026 mg-chao
// SPDX-License-Identifier: Apache-2.0
// Developer-only visual reference. No packages are installed or downloaded.
// Executes the original pinned Input style generators and cssinjs serializer.
// node scripts/generate_input_reference.mjs --upstream E:/ant-design --pnpm-store E:/.pnpm-store/v10

import fs from 'node:fs';
import path from 'node:path';
import crypto from 'node:crypto';
import { createRequire, isBuiltin } from 'node:module';
import { execFileSync } from 'node:child_process';
import { fileURLToPath } from 'node:url';
import { motionPage } from './input_motion_reference.mjs';
import { compositePage } from './input_composite_reference.mjs';

const options = Object.fromEntries(process.argv.slice(2).reduce((pairs, value, index, args) => {
  if (index % 2 === 0) pairs.push([value.replace(/^--/, ''), args[index + 1]]);
  return pairs;
}, []));
if (!options.upstream || !options['pnpm-store']) {
  throw new Error('Supply --upstream and --pnpm-store. This script uses existing pinned packages only.');
}
const crate = path.resolve(path.dirname(fileURLToPath(import.meta.url)), '..');
const profiles = {
  default: {},
  typography: { inputFontSize: 18, inputFontSizeLG: 21 },
  geometry: { inputFontSize: 15.5, inputFontSizeSM: 12, inputFontSizeLG: 19, paddingBlock: 7, paddingInline: 17, paddingBlockSM: 2.5, paddingInlineSM: 9, paddingBlockLG: 9, paddingInlineLG: 21 },
  colors: { activeBg: '#e6fffb', hoverBg: '#f0f5ff', activeBorderColor: '#531dab', hoverBorderColor: '#08979c', activeShadow: '0 0 0 3px rgba(83,29,171,0.2)' },
};
const profile = options.profile ?? 'default';
if (!(profile in profiles)) throw new Error(`Unknown input reference profile ${profile}`);
const componentPatch = profiles[profile];
const upstream = path.resolve(options.upstream);
const store = path.resolve(options['pnpm-store']);
const output = path.resolve(options.output ?? path.join(crate, '../build/ant-design-gpui/input-reference', profile === 'default' ? '' : profile));
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
// All Input geometry, state, hover, focus and disabled CSS is upstream code.
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
    if (request === '@rc-component/color-picker') {
      return loadPackage('@rc-component/color-picker/lib/color');
    }
    if (!request.startsWith('.')) return loadPackage(request);
    const base = path.resolve(path.dirname(file), request);
    const normalized = path.relative(upstream, base).replaceAll('\\', '/');
    if (normalized === 'components/theme/internal') return themeInternal();
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
    return evaluateSource(resolved);
  };
  new Function('require', 'module', 'exports', javascript)(require, module, module.exports);
  return module.exports;
}
function themeInternal() {
  return {
    genStyleHooks: (_name, generate, defaults, options = {}) => ({ generate, defaults, options }),
    genSubStyleComponent: (_name, generate, defaults, options = {}) => ({ generate, defaults, options: { resetStyle: false, ...options } }),
    mergeToken: (...tokens) => Object.assign({}, ...tokens),
    getLineHeight: evaluateSource(path.join(upstream, 'components/theme/themes/shared/genFontSizes.ts')).getLineHeight,
  };
}
const { parseStyle, normalizeStyle } = loadPackage('@ant-design/cssinjs/lib/hooks/useStyleRegister');
const calculator = loadPackage('@ant-design/cssinjs-utils/lib/util/calc').default('css', new Set());
// Retain provenance for the registration contract mirrored by the adapter.
read(packageFile('@ant-design/cssinjs-utils', 'lib/util/genStyleUtils.js'),
  '@ant-design/cssinjs-utils@2.1.2/lib/util/genStyleUtils.js');
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
  const [css, effects] = parseStyle(styles, { path: 'InputReference' });
  return normalizeStyle(css) + Object.values(effects).map(effect => normalizeStyle(effect)).join('');
}

const input = evaluateSource(path.join(upstream, 'components/input/style/index.ts'));
const textarea = evaluateSource(path.join(upstream, 'components/input/style/textarea.ts')).default;
const otp = evaluateSource(path.join(upstream, 'components/input/style/otp.ts')).default;
const numeric = evaluateSource(path.join(upstream, 'components/input-number/style/index.ts')).default;
const search = evaluateSource(path.join(upstream, 'components/input/style/search.ts')).default;
const button = evaluateSource(path.join(upstream, 'components/button/style/index.ts')).default;
const buttonCompact = evaluateSource(path.join(upstream, 'components/button/style/compact.ts')).default;
const compact = evaluateSource(path.join(upstream, 'components/space/style/compact.ts')).default;
const maxmin = loadPackage('@ant-design/cssinjs-utils/lib/util/maxmin').default('css');
const common = evaluateSource(path.join(upstream, 'components/style/index.tsx'));
for (const file of ['input/Input.tsx', 'input/TextArea.tsx', 'input/OTP/index.tsx', 'input/OTP/OTPInput.tsx', 'input-number/index.tsx', 'space/Compact.tsx', 'space/style/compact.ts']) {
  read(path.join(upstream, 'components', file), `components/${file}`);
}
const sizes = { small: 'sm', medium: '', large: 'lg' };
let index = 0;
function attributes(options, prefix = 'ant-input') {
  const classes = [prefix, `${prefix}-${options.variant}`, `${prefix}-css-var`];
  if (prefix === 'ant-input-number') classes.push(`${prefix}-mode-input`);
  if (sizes[options.size]) classes.push(`${prefix}-${sizes[options.size]}`);
  if (options.status !== 'none') classes.push(`${prefix}-status-${options.status}`);
  if (options.availability === 'disabled') classes.push(`${prefix}-disabled`);
  if (prefix === 'ant-input-number' && options.availability === 'readonly') classes.push(`${prefix}-readonly`);
  if (options.direction === 'rtl') classes.push(`${prefix}-rtl`);
  return `class="${classes.join(' ')}" ${options.availability === 'disabled' ? 'disabled' : ''} ${options.availability === 'readonly' ? 'readonly' : ''}`;
}
function control(options) {
  const data = `data-control='${JSON.stringify({ index: index++, ...options })}'`;
  const attrs = attributes(options);
  if (options.kind === 'text') return `<input ${data} ${attrs} value="Ant Design" placeholder="Placeholder">`;
  if (options.kind === 'textarea') return `<textarea ${data} ${attrs} rows="2">First line\n第二行</textarea>`;
  if (options.kind === 'numeric') return `<div ${data} ${attributes(options, 'ant-input-number')}><div class="ant-input-number-input-wrap"><input class="ant-input-number-input" value="1234.5" aria-label="Amount" ${options.availability === 'disabled' ? 'disabled' : ''} ${options.availability === 'readonly' ? 'readonly' : ''}></div><div class="ant-input-number-actions"><span class="ant-input-number-action ant-input-number-action-up" role="button"><span class="ant-input-number-action-up-inner">⌃</span></span><span class="ant-input-number-action ant-input-number-action-down" role="button"><span class="ant-input-number-action-down-inner">⌄</span></span></div></div>`;
  const cls = `ant-otp ant-otp-css-var ${sizes[options.size] ? `ant-otp-${sizes[options.size]}` : ''} ${options.direction === 'rtl' ? 'ant-otp-rtl' : ''}`;
  return `<div ${data} class="${cls}">${['1','2','3','4','5','6'].map((value, n) => `<div class="ant-otp-input-wrapper"><input ${attrs.replace('ant-input ', 'ant-input ant-otp-input ')} size="1" value="${value}" aria-label="Character ${n + 1}"></div>`).join('')}</div>`;
}
function gallery() {
  index = 0;
  const rows = [];
  for (const kind of ['otp', 'text', 'numeric', 'textarea']) {
    for (const direction of ['ltr', 'rtl']) for (const size of Object.keys(sizes)) {
      for (const variant of ['outlined', 'filled', 'borderless', 'underlined']) {
        for (const status of kind === 'otp' ? ['none'] : ['none', 'error', 'warning']) {
          for (const availability of kind === 'otp' ? ['enabled'] : ['enabled', 'disabled', 'readonly']) {
            const options = { kind, direction, size, variant, status, availability };
            rows.push(`<section class="sample" dir="${direction}"><label>${kind} · ${size} · ${variant} · ${status} · ${availability} · ${direction}</label>${control(options)}</section>`);
          }
        }
      }
    }
  }
  return rows.join('');
}
fs.mkdirSync(output, { recursive: true });
for (const name of ['default', 'dark', 'compact', 'dark-compact']) {
  const fixture = fixtures.cases.find(fixture => fixture.name === name);
  const globals = Object.fromEntries(Object.entries(fixture.tokens).map(([key, value]) => [key, cssValue(key, value)]));
  const generate = (hook, prefix, patch = hook === otp ? {} : componentPatch) => {
    // cssinjs-utils passes component overrides into the default-token factory,
    // then overlays explicit values. Derived padding must observe custom fonts.
    const tokens = { ...globals, ...hook.defaults({ ...globals, ...patch }), ...patch, componentCls: `.${prefix}`, antCls: '.ant', iconCls: '.anticon', calc: calculator, ...maxmin };
    return serialize([hook.options.resetStyle === false ? null : common.genCommonStyle(tokens, prefix, `${prefix}-css-var`, hook.options.resetFont), hook.generate(tokens)]);
  };
  const css = [[input.useSharedStyle, 'ant-input'], [input.default, 'ant-input'], [textarea, 'ant-input'], [otp, 'ant-otp'], [numeric, 'ant-input-number']].map(([hook, prefix]) => generate(hook, prefix)).join('\n');
  const html = `<!doctype html><html lang="en"><meta charset="utf-8"><title>Ant Design Input reference · ${name}</title><style>${css}
body{margin:0;padding:24px;font-family:${globals.fontFamily};font-size:${globals.fontSize}px;line-height:${globals.lineHeight};color:${globals.colorText};background:${globals.colorBgContainer};pointer-events:none}
.sample{display:flex;flex-direction:column;align-items:flex-start;gap:8px;padding:14px;width:480px;box-sizing:border-box;border:1px solid ${globals.colorBorderSecondary};margin:8px 0}.sample>label{font-size:12px;color:${globals.colorTextSecondary}}.sample>input,.sample>textarea{width:300px}
*{animation:none!important;transition:none!important}:root{color-scheme:${name.includes('dark') ? 'dark' : 'light'}}
</style><h1>Input reference · ${name}</h1>${gallery()}<script>
const state = new URLSearchParams(location.search).get('state') || 'rest';
if (['hover','focus'].includes(state)) {
  const styles = document.createElement('style');
  styles.textContent = document.querySelector('style').textContent.replaceAll(':hover', '[data-hover]').replaceAll(':focus-within', '[data-focus-within]').replaceAll(':focus-visible', '[data-focus-visible]').replaceAll(':focus', '[data-focus]');
  document.head.append(styles);
  document.querySelectorAll('[data-control],input,textarea').forEach(element => {
    if (state === 'hover') element.setAttribute('data-hover','');
    if (state === 'focus' && !element.hasAttribute('disabled')) {
      element.setAttribute('data-focus',''); element.setAttribute('data-focus-visible',''); element.setAttribute('data-focus-within','');
      if (element.classList.contains('ant-input-number')) element.classList.add('ant-input-number-focused');
    }
  });
}
function measure(element) {
  const css = getComputedStyle(element), box = element.getBoundingClientRect();
  const result = { width: box.width, height: box.height };
  for (const key of ['paddingTop','paddingRight','paddingBottom','paddingLeft','fontSize','lineHeight','borderTopWidth','borderRightWidth','borderBottomWidth','borderLeftWidth','borderTopLeftRadius','borderTopRightRadius','borderBottomLeftRadius','borderBottomRightRadius']) result[key] = parseFloat(css[key]);
  for (const key of ['backgroundColor','borderTopColor','borderBottomColor','color','boxShadow','outlineWidth','outlineColor','cursor','columnGap','direction']) result[key] = css[key];
  return result;
}
const metrics = document.createElement('script'); metrics.type = 'application/json'; metrics.id = 'reference-metrics';
metrics.textContent = JSON.stringify({theme:'${name}',profile:'${profile}',state,scale:devicePixelRatio,controls:[...document.querySelectorAll('[data-control]')].map(root => ({options:JSON.parse(root.dataset.control), root:measure(root), input: root.matches('input,textarea') ? null : measure(root.querySelector('input')), actions:root.querySelector('.ant-input-number-actions') ? measure(root.querySelector('.ant-input-number-actions')) : null}))});
document.body.append(metrics);
</script></html>`;
  fs.writeFileSync(path.join(output, `input-reference-${name}.html`), html);
  fs.writeFileSync(path.join(output, `input-motion-${name}.html`), motionPage(css, globals, name));
  // Register base Button styles before the Search specialization, as when
  // Button is already mounted. Compact substyles do not reset typography.
  const compositeCss = generate(button, 'ant-btn', {}) + css + generate(search, 'ant-input-search') + generate(buttonCompact, 'ant-btn', {}) + generate(compact, 'ant-space-compact', {});
  fs.writeFileSync(path.join(output, `input-composite-${name}.html`), compositePage(compositeCss, globals, name, profile));
}
fs.writeFileSync(path.join(output, 'provenance.json'), JSON.stringify({
  upstream: {version:fixtures.provenance.version,revision},
  profile, componentPatch,
  packages: Object.fromEntries([...indexes.keys()].sort().map(name => [name,versions[name]])),
  sources: Object.fromEntries([...inputs].sort(([a],[b]) => a.localeCompare(b))),
  fixtureSha256: crypto.createHash('sha256').update(fs.readFileSync(fixturePath)).digest('hex'),
  harnessSha256: Object.fromEntries(['generate_input_reference.mjs', 'input_motion_reference.mjs', 'input_composite_reference.mjs'].map(name =>
    [name, crypto.createHash('sha256').update(fs.readFileSync(path.join(crate, 'scripts', name))).digest('hex')])),
  notes: 'Original Input shared/component/textarea/OTP and InputNumber style generators, component defaults and cssinjs serialization. Hook registration and theme token plumbing only are adapted. Static DOM follows recorded component sources; no Rust visual values are used. Rest/hover/focus selectors are measured without transitions. Native browser intrinsic size=1 OTP width is preserved.'
},null,2)+'\n');
console.log(`Generated four Input reference pages (${index} controls each) in ${output}`);
