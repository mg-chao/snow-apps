// Copyright (C) 2026 mg-chao
// SPDX-License-Identifier: Apache-2.0
// Developer-only visual reference. No packages are installed or downloaded.
// Executes the original pinned Checkbox style generators and cssinjs serializer.
// node scripts/generate_checkbox_reference.mjs --upstream E:/ant-design --pnpm-store E:/.pnpm-store/v10

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
const output = path.resolve(options.output ?? path.join(crate, '../build/ant-design-gpui/checkbox-reference'));
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
// All Checkbox geometry, state, hover, focus and disabled CSS is upstream code.
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
const checkboxStyles = evaluateSource(path.join(upstream, 'components/checkbox/style/index.ts')).default;
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
  const [css, effects] = parseStyle(styles, { path: 'CheckboxReference' });
  return normalizeStyle(css) + Object.values(effects).map(effect => normalizeStyle(effect)).join('');
}

function checkbox(checked, indeterminate, disabled, mode, direction, index) {
  const wrapper = ['ant-checkbox-wrapper'];
  const icon = ['ant-checkbox'];
  if (checked) { wrapper.push('ant-checkbox-wrapper-checked'); icon.push('ant-checkbox-checked'); }
  if (disabled) { wrapper.push('ant-checkbox-wrapper-disabled'); icon.push('ant-checkbox-disabled'); }
  if (indeterminate) icon.push('ant-checkbox-indeterminate');
  if (direction === 'rtl') wrapper.push('ant-checkbox-rtl');
  const label = mode === 'empty' ? '' : mode === 'wrapped' ? 'A label that wraps onto several lines' : 'Checkbox';
  return `<div class="demo-case"><label data-case="${index}" data-checked="${checked}" data-indeterminate="${indeterminate}" data-disabled="${disabled}" data-mode="${mode}" data-direction="${direction}" class="${wrapper.join(' ')}" style="${mode === 'wrapped' ? 'width:130px' : ''}"><span class="${icon.join(' ')}"><input class="ant-checkbox-input" type="checkbox" ${checked ? 'checked' : ''} ${disabled ? 'disabled' : ''} aria-label="${label || 'Checkbox'}"></span>${label ? `<span class="ant-checkbox-label">${label}</span>` : ''}</label></div>`;
}

function gallery() {
  let index = 0;
  return ['ltr', 'rtl'].map(direction => `<section dir="${direction}"><h2>${direction.toUpperCase()}</h2><div class="ant-checkbox-group">${[false, true].flatMap(checked => [false, true].flatMap(indeterminate => [false, true].flatMap(disabled => ['empty', 'label', 'wrapped'].map(mode => checkbox(checked, indeterminate, disabled, mode, direction, index++))))).join('')}</div></section>`).join('');
}

fs.mkdirSync(output, { recursive: true });
for (const name of ['default', 'dark', 'compact', 'dark-compact']) {
  const fixture = fixtures.cases.find(fixture => fixture.name === name);
  const global = Object.fromEntries(Object.entries(fixture.tokens).map(([key, value]) => [key, cssValue(key, value)]));
  // Checkbox interpolates these length aliases directly into CSS calc() text.
  // Preserve their units as the real CSS-variable mode does; its original CSS
  // calculator handles arithmetic without rewriting any source declarations.
  const tokens = {
    ...global, controlInteractiveSize: `${global.controlInteractiveSize}px`, lineWidth: `${global.lineWidth}px`,
    componentCls: '.ant-checkbox', antCls: '.ant', iconCls: '.anticon', calc: calculator,
  };
  const css = serialize([
    commonStyles.genCommonStyle(tokens, 'ant-checkbox', 'ant-checkbox'),
    checkboxStyles.generate(tokens, { prefixCls: 'ant-checkbox' }),
  ]);
  const html = `<!doctype html><html lang="en"><meta charset="utf-8"><meta name="viewport" content="width=device-width,initial-scale=1"><title>Ant Design Checkbox reference · ${name}</title><style>
    ${css}
    body{margin:0;padding:28px;font-family:${global.fontFamily};font-size:${global.fontSize}px;line-height:${global.lineHeight};color:${global.colorText};background:${global.colorBgContainer}}
    h1{font-size:28px;margin:0 0 8px}p{color:${global.colorTextSecondary};margin:0 0 24px}
    h2{font-size:18px;margin:0 0 12px}section{margin-bottom:32px}.ant-checkbox-group{max-width:1100px}.demo-case{padding:12px;border:1px solid ${global.colorBorderSecondary};width:162px;box-sizing:border-box;display:flex;align-items:center}
    :root{color-scheme:${name.includes('dark') ? 'dark' : 'light'}}*{animation:none!important;transition:none!important}
  </style><body><h1>Checkbox</h1><p>Original Ant Design 6.6.7 styles · all checked, mixed and disabled combinations</p>${gallery()}<script>
    const state = new URLSearchParams(location.search).get('state');
    if (state === 'hover' || state === 'focus') {
      const styles = document.createElement('style');
      styles.textContent = document.querySelector('style').textContent.replaceAll(':hover', '[data-hover]').replaceAll(':focus-visible', '[data-focus-visible]');
      document.head.append(styles);
      document.querySelectorAll('.ant-checkbox-wrapper,.ant-checkbox').forEach(element => {
        if (state === 'hover') element.setAttribute('data-hover', '');
      });
      if (state === 'focus') document.querySelectorAll('input:not(:disabled)').forEach(input => input.setAttribute('data-focus-visible', ''));
    }
    document.querySelectorAll('[data-indeterminate="true"] input').forEach(input => input.indeterminate = true);
    const metrics = document.createElement('script');
    metrics.type = 'application/json'; metrics.id = 'reference-metrics';
    metrics.textContent = JSON.stringify([...document.querySelectorAll('[data-case]')].map(wrapper => {
      const icon = wrapper.querySelector('.ant-checkbox');
      const label = wrapper.querySelector('.ant-checkbox-label');
      const rootStyle = getComputedStyle(wrapper);
      const styles = getComputedStyle(icon);
      const mark = getComputedStyle(icon, '::after');
      const labelStyle = label ? getComputedStyle(label) : null;
      const bounds = icon.getBoundingClientRect();
      const rootBounds = wrapper.getBoundingClientRect();
      const labelBounds = label?.getBoundingClientRect();
      return {
        index: Number(wrapper.dataset.case), checked: wrapper.dataset.checked === 'true',
        indeterminate: wrapper.dataset.indeterminate === 'true', disabled: wrapper.dataset.disabled === 'true',
        direction: wrapper.dataset.direction, mode: wrapper.dataset.mode, label: label?.textContent ?? '',
        width: bounds.width, height: bounds.height, background: styles.backgroundColor,
        borderColor: styles.borderColor, borderWidth: styles.borderWidth, borderStyle: styles.borderStyle,
        radius: styles.borderRadius, labelColor: labelStyle?.color ?? rootStyle.color,
        fontSize: rootStyle.fontSize, lineHeight: rootStyle.lineHeight, fontWeight: rootStyle.fontWeight,
        labelPaddingStart: labelStyle?.paddingInlineStart ?? null, labelPaddingEnd: labelStyle?.paddingInlineEnd ?? null,
        wrapperHeight: rootBounds.height, labelHeight: labelBounds?.height ?? null,
        iconOffsetTop: bounds.top - rootBounds.top, iconDirection: styles.direction,
        markWidth: mark.width, markHeight: mark.height, markTop: mark.top, markLeft: mark.left,
        markBorderWidth: mark.borderRightWidth,
        markBorderColor: mark.borderRightColor, markBackground: mark.backgroundColor,
        markOpacity: mark.opacity, markTransform: mark.transform, markTransformOrigin: mark.transformOrigin,
        outlineColor: styles.outlineColor, outlineWidth: styles.outlineWidth, outlineOffset: styles.outlineOffset,
        outlineStyle: styles.outlineStyle,
        groupGap: getComputedStyle(wrapper.closest('.ant-checkbox-group')).columnGap,
      };
    }));
    document.body.append(metrics);
  </script></body></html>`;
  fs.writeFileSync(path.join(output, `checkbox-reference-${name}.html`), html);
}
const provenance = {
  upstream: { version: fixtures.provenance.version, revision },
  packages: Object.fromEntries([...indexes.keys()].sort().map(name => [name, versions[name]])),
  sources: Object.fromEntries([...inputs].sort(([a], [b]) => a.localeCompare(b))),
  fixtureSha256: crypto.createHash('sha256').update(fs.readFileSync(fixturePath)).digest('hex'),
  notes: 'Original Checkbox style TypeScript, CSS-mode calculator and cssinjs serializer; React hook registration captured and concrete aliases supplied by independently executed theme fixtures. Directly interpolated controlInteractiveSize/lineWidth retain px units as in real CSS-variable mode. Static DOM matches source Checkbox.tsx classes and input/label structure. No Rust style values or behavior are used.',
};
fs.writeFileSync(path.join(output, 'provenance.json'), JSON.stringify(provenance, null, 2) + '\n');
console.log(`Generated four original Checkbox style references in ${output}`);
