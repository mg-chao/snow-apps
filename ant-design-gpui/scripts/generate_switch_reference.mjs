// Copyright (C) 2026 mg-chao
// SPDX-License-Identifier: Apache-2.0
// Developer-only visual reference. No packages are installed or downloaded.
// Executes the original pinned Switch style generators and cssinjs serializer.
// node scripts/generate_switch_reference.mjs --upstream E:/ant-design --pnpm-store E:/.pnpm-store/v10

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
const output = path.resolve(options.output ?? path.join(crate, '../build/ant-design-gpui/switch-reference'));
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
// All Switch geometry, state, hover, focus and disabled CSS is upstream code.
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
    genStyleHooks: (_name, generate, defaults) => ({ generate, defaults }),
    mergeToken: (...tokens) => Object.assign({}, ...tokens),
    getLineHeight: evaluateSource(path.join(upstream, 'components/theme/themes/shared/genFontSizes.ts')).getLineHeight,
  };
}
const { parseStyle, normalizeStyle } = loadPackage('@ant-design/cssinjs/lib/hooks/useStyleRegister');
const calculator = loadPackage('@ant-design/cssinjs-utils/lib/util/calc').default('css', new Set());
// Retain provenance for the real registration contract mirrored by the adapter:
// rootCls defaults to prefixCls and is passed to getCommonStyle as argument three.
read(packageFile('@ant-design/cssinjs-utils', 'lib/util/genStyleUtils.js'),
  '@ant-design/cssinjs-utils@2.1.2/lib/util/genStyleUtils.js');
const switchStyles = evaluateSource(path.join(upstream, 'components/switch/style/index.ts')).default;
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
  const [css, effects] = parseStyle(styles, { path: 'SwitchReference' });
  return normalizeStyle(css) + Object.values(effects).map(effect => normalizeStyle(effect)).join('');
}

const customTokens = {
  trackHeight: 14, trackHeightSM: 12, trackMinWidth: 32, trackMinWidthSM: 24,
  trackPadding: -3, handleBg: 'rgb(25, 118, 210)', handleSize: 20, handleSizeSM: 16,
  handleShadow: '0 2px 1px -1px rgba(0,0,0,.2),0 1px 1px 0 rgba(0,0,0,.14)',
  innerMinMargin: 10, innerMaxMargin: 28, innerMinMarginSM: 8, innerMaxMarginSM: 24,
};

function switchControl(checked, availability, content, direction, small, custom, index) {
  const disabled = availability !== 'enabled';
  const loading = availability === 'loading';
  const classes = ['ant-switch'];
  if (checked) classes.push('ant-switch-checked');
  if (disabled) classes.push('ant-switch-disabled');
  if (loading) classes.push('ant-switch-loading');
  if (direction === 'rtl') classes.push('ant-switch-rtl');
  if (small) classes.push('ant-switch-small');
  const labels = content === 'empty' ? ['', ''] : content === 'text' ? ['On', 'Off']
    : content === 'asymmetric' ? ['Connected', 'Off']
    : ['<i class="content-icon"></i>', '<i class="content-icon"></i>'];
  const loader = loading ? '<span class="anticon anticon-loading ant-switch-loading-icon"><svg width="1em" height="1em" viewBox="0 0 1024 1024"><path fill="currentColor" d="M988 548c-19.9 0-36-16.1-36-36 0-59.4-11.6-117-34.6-171.3a440.45 440.45 0 0 0-94.3-139.9 437.71 437.71 0 0 0-139.9-94.3C629 83.6 571.4 72 512 72c-19.9 0-36-16.1-36-36s16.1-36 36-36c69.1 0 136.2 13.5 199.3 40.3C772.3 66 827 103 874 150c47 47 83.9 101.8 109.7 162.7 26.7 63.1 40.2 130.2 40.2 199.3.1 19.9-16 36-35.9 36z"/></svg></span>' : '';
  return `<div class="demo-case${custom ? ' custom' : ''}"><button type="button" role="switch" aria-checked="${checked}" aria-label="Connection" data-case="${index}" data-checked="${checked}" data-disabled="${disabled}" data-loading="${loading}" data-small="${small}" data-direction="${direction}" data-content="${content}" data-custom="${custom}" class="${classes.join(' ')}" ${disabled ? 'disabled' : ''}><div class="ant-switch-handle">${loader}</div><span class="ant-switch-inner"><span class="ant-switch-inner-checked">${labels[0]}</span><span class="ant-switch-inner-unchecked">${labels[1]}</span></span></button></div>`;
}

function gallery() {
  let index = 0;
  const defaults = ['ltr', 'rtl'].map(direction => `<section dir="${direction}"><h2>${direction.toUpperCase()}</h2><div class="examples">${[false, true].flatMap(small => [false, true].flatMap(checked => ['enabled', 'disabled', 'loading'].flatMap(availability => ['empty', 'text', 'asymmetric', 'icon'].map(content => switchControl(checked, availability, content, direction, small, false, index++))))).join('')}</div></section>`).join('');
  const custom = `<section><h2>Component tokens</h2><div class="examples">${['ltr', 'rtl'].flatMap(direction => [false, true].flatMap(small => [false, true].map(checked => switchControl(checked, 'enabled', 'empty', direction, small, true, index++)))).join('')}</div></section>`;
  return defaults + custom;
}

fs.mkdirSync(output, { recursive: true });
for (const name of ['default', 'dark', 'compact', 'dark-compact']) {
  const fixture = fixtures.cases.find(fixture => fixture.name === name);
  const global = Object.fromEntries(Object.entries(fixture.tokens).map(([key, value]) => [key, cssValue(key, value)]));
  const tokens = { ...global, componentCls: '.ant-switch', antCls: '.ant', iconCls: '.anticon', calc: calculator };
  const defaults = switchStyles.defaults(tokens);
  const css = serialize([
    commonStyles.genCommonStyle(tokens, 'ant-switch', 'ant-switch'),
    switchStyles.generate({ ...tokens, ...defaults }, { prefixCls: 'ant-switch' }),
  ]);
  const customCss = serialize(switchStyles.generate({ ...tokens, ...defaults, ...customTokens }, { prefixCls: 'ant-switch' }));
  // Scope token probes through a containing class without changing selector specificity
  // relative to other Switch selectors in that same group.
  const scopedCustomCss = `@scope (.custom){${customCss}}`;
  const html = `<!doctype html><html lang="en"><meta charset="utf-8"><meta name="viewport" content="width=device-width,initial-scale=1"><title>Ant Design Switch reference · ${name}</title><style>
    ${css}\n${scopedCustomCss}
    body{margin:0;padding:28px;font-family:${global.fontFamily};font-size:${global.fontSize}px;line-height:${global.lineHeight};color:${global.colorText};background:${global.colorBgContainer}}
    h1{font-size:28px;margin:0 0 8px}p{color:${global.colorTextSecondary};margin:0 0 24px}
    h2{font-size:18px;margin:0 0 12px}section{margin-bottom:32px}.examples{display:flex;flex-wrap:wrap;gap:12px;max-width:1100px}.demo-case{padding:12px;border:1px solid ${global.colorBorderSecondary};width:162px;box-sizing:border-box;display:flex;align-items:center}
    .content-icon{display:inline-block;flex:none;width:12px;height:12px;background:currentColor}.anticon{display:inline-flex;align-items:center;justify-content:center;line-height:1;vertical-align:-.125em}.anticon svg{display:block}
    :root{color-scheme:${name.includes('dark') ? 'dark' : 'light'}}*{animation:none!important;transition:none!important}
  </style><body><h1>Switch</h1><p>Original Ant Design 6.6.7 styles · geometry, states, RTL, sizes and component tokens</p>${gallery()}<script>
    const state = new URLSearchParams(location.search).get('state');
    if (['hover', 'focus', 'active'].includes(state)) {
      const styles = document.createElement('style');
      styles.textContent = document.querySelector('style').textContent.replaceAll(':hover', '[data-hover]').replaceAll(':focus-visible', '[data-focus-visible]').replaceAll(':active', '[data-active]');
      document.head.append(styles);
      document.querySelectorAll('.ant-switch').forEach(element => {
        if (state === 'hover' || state === 'active') element.setAttribute('data-hover', '');
        if (!element.disabled && state === 'focus') element.setAttribute('data-focus-visible', '');
        if (!element.disabled && state === 'active') element.setAttribute('data-active', '');
      });
    }
    const metrics = document.createElement('script');
    metrics.type = 'application/json'; metrics.id = 'reference-metrics';
    metrics.textContent = JSON.stringify([...document.querySelectorAll('[data-case]')].map(root => {
      const handle = root.querySelector('.ant-switch-handle');
      const inner = root.querySelector('.ant-switch-inner');
      const checked = root.querySelector('.ant-switch-inner-checked');
      const unchecked = root.querySelector('.ant-switch-inner-unchecked');
      const loader = root.querySelector('.ant-switch-loading-icon');
      const styles = getComputedStyle(root);
      const handleStyle = getComputedStyle(handle);
      const handleBefore = getComputedStyle(handle, '::before');
      const innerStyle = getComputedStyle(inner);
      const checkedStyle = getComputedStyle(checked);
      const uncheckedStyle = getComputedStyle(unchecked);
      const loaderStyle = loader ? getComputedStyle(loader) : null;
      const bounds = root.getBoundingClientRect();
      const handleBounds = handle.getBoundingClientRect();
      const checkedBounds = checked.getBoundingClientRect();
      const uncheckedBounds = unchecked.getBoundingClientRect();
      const naturalWidth = element => {
        const range = document.createRange(); range.selectNodeContents(element);
        return range.getBoundingClientRect().width;
      };
      return {
        index: Number(root.dataset.case), checked: root.dataset.checked === 'true',
        disabled: root.dataset.disabled === 'true', loading: root.dataset.loading === 'true',
        small: root.dataset.small === 'true', direction: root.dataset.direction,
        content: root.dataset.content, custom: root.dataset.custom === 'true',
        width: bounds.width, height: bounds.height, minWidth: styles.minWidth,
        background: styles.backgroundColor, opacity: styles.opacity, borderWidth: styles.borderWidth,
        radius: styles.borderRadius, cursor: styles.cursor, fontSize: styles.fontSize,
        handleSize: handleStyle.width, handleHeight: handleStyle.height,
        handleTop: handleBounds.top - bounds.top, handleLeft: handleBounds.left - bounds.left,
        handleBackground: handleBefore.backgroundColor, handleRadius: handleBefore.borderRadius,
        handleShadow: handleBefore.boxShadow, handleInsetStart: handleBefore.insetInlineStart,
        handleInsetEnd: handleBefore.insetInlineEnd, handlePaintWidth: handleBefore.width,
        contentColor: checkedStyle.color, contentFontSize: checkedStyle.fontSize,
        contentPaddingStart: innerStyle.paddingInlineStart, contentPaddingEnd: innerStyle.paddingInlineEnd,
        checkedContentLeft: checkedBounds.left - bounds.left, checkedContentWidth: checkedBounds.width,
        uncheckedContentLeft: uncheckedBounds.left - bounds.left, uncheckedContentWidth: uncheckedBounds.width,
        checkedMarginStart: checkedStyle.marginInlineStart, checkedMarginEnd: checkedStyle.marginInlineEnd,
        uncheckedMarginStart: uncheckedStyle.marginInlineStart, uncheckedMarginEnd: uncheckedStyle.marginInlineEnd,
        checkedNaturalWidth: naturalWidth(checked), uncheckedNaturalWidth: naturalWidth(unchecked),
        loadingIconSize: loaderStyle?.fontSize ?? null, loadingIconColor: loaderStyle?.color ?? null,
        loadingIconTop: loaderStyle?.top ?? null,
        outlineColor: styles.outlineColor, outlineWidth: styles.outlineWidth, outlineOffset: styles.outlineOffset,
        outlineStyle: styles.outlineStyle,
      };
    }));
    document.body.append(metrics);
  </script></body></html>`;
  fs.writeFileSync(path.join(output, `switch-reference-${name}.html`), html);
}
const provenance = {
  upstream: { version: fixtures.provenance.version, revision },
  packages: Object.fromEntries([...indexes.keys()].sort().map(name => [name, versions[name]])),
  sources: Object.fromEntries([...inputs].sort(([a], [b]) => a.localeCompare(b))),
  fixtureSha256: crypto.createHash('sha256').update(fs.readFileSync(fixturePath)).digest('hex'),
  notes: 'Original Switch style TypeScript, CSS-mode calculator and cssinjs serializer; React hook registration captured and concrete aliases supplied by independently executed theme fixtures. Static button, handle and both content spans match rc-switch DOM from upstream snapshots; only an independent container scopes custom-token probes. Pseudo-class substitutions retain original selector guards and declarations. No Rust styles or behavior are used.',
};
fs.writeFileSync(path.join(output, 'provenance.json'), JSON.stringify(provenance, null, 2) + '\n');
console.log(`Generated four original Switch style references in ${output}`);
