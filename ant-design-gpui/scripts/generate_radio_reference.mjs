// Copyright (C) 2026 mg-chao
// SPDX-License-Identifier: Apache-2.0
// Developer-only visual reference. No packages are installed or downloaded.
// Executes the original pinned Radio style generators and cssinjs serializer.
// node scripts/generate_radio_reference.mjs --upstream E:/ant-design --pnpm-store E:/.pnpm-store/v10

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
const output = path.resolve(options.output ?? path.join(crate, '../build/ant-design-gpui/radio-reference'));
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
// All Radio geometry, state, hover, focus and disabled CSS is upstream code.
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
// Retain provenance for the registration contract mirrored by the adapter.
read(packageFile('@ant-design/cssinjs-utils', 'lib/util/genStyleUtils.js'),
  '@ant-design/cssinjs-utils@2.1.2/lib/util/genStyleUtils.js');
const radioStyles = evaluateSource(path.join(upstream, 'components/radio/style/index.ts')).default;
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
  const [css, effects] = parseStyle(styles, { path: 'RadioReference' });
  return normalizeStyle(css) + Object.values(effects).map(effect => normalizeStyle(effect)).join('');
}

const variants = {
  default: {},
  wireframe: { wireframe: true },
  custom: {
    radioSize: 20, dotSize: 10, dotColorDisabled: 'grey',
    buttonBg: '#f6ffed', buttonCheckedBg: '#d9f7be', buttonColor: '#faad14',
    buttonPaddingInline: 20, buttonCheckedBgDisabled: '#fffbe6',
    buttonCheckedColorDisabled: '#ffe58f', buttonSolidCheckedColor: '#ffa39e',
    wrapperMarginInlineEnd: 20,
  },
};
let index = 0;
function radio(options, standalone = false) {
  const { variant = 'default', checked = false, disabled = false, mode = 'label', button = false, direction = 'ltr' } = options;
  const prefix = `ant-radio-${variant}`;
  const base = `${prefix}${button ? '-button' : ''}`;
  const wrapper = [`${base}-wrapper`, `${prefix}-css-var`];
  const icon = [base];
  if (checked) { wrapper.push(`${base}-wrapper-checked`); icon.push(`${base}-checked`); }
  if (disabled) { wrapper.push(`${base}-wrapper-disabled`); icon.push(`${base}-disabled`); }
  if (direction === 'rtl') wrapper.push(`${base}-wrapper-rtl`);
  if (options.block) wrapper.push(`${base}-wrapper-block`);
  const label = mode === 'empty' ? '' : mode === 'wrapped' ? 'A label that wraps onto several lines' : button ? 'Radio' : 'Radio option';
  const data = standalone ? `data-control='${JSON.stringify({ index: index++, ...options })}'` : '';
  return `<label ${data} class="${wrapper.join(' ')}" style="${mode === 'wrapped' ? 'width:130px' : ''}"><span class="${icon.join(' ')}"><input class="${base}-input" type="radio" ${checked ? 'checked' : ''} ${disabled ? 'disabled' : ''} aria-label="${label || 'Radio'}"></span>${label ? `<span class="${base}-label">${label}</span>` : ''}</label>`;
}

function group(options, content, measured = false) {
  const { variant = 'default', direction = 'ltr', size = 'medium', buttonStyle = 'outline', vertical = false, block = false } = options;
  const prefix = `ant-radio-${variant}`;
  const classes = [`${prefix}-group`, `${prefix}-group-${buttonStyle}`, `${prefix}-css-var`];
  if (size !== 'medium') classes.push(`${prefix}-group-${size}`);
  if (direction === 'rtl') classes.push(`${prefix}-group-rtl`);
  if (vertical) classes.push(`${prefix}-group-vertical`);
  if (block) classes.push(`${prefix}-group-block`);
  const data = measured ? `data-group='${JSON.stringify(options)}'` : '';
  return `<div ${data} class="${classes.join(' ')}" style="${block ? 'width:360px' : ''}">${content}</div>`;
}

function gallery() {
  index = 0;
  const standalone = [];
  const grouped = [];
  for (const direction of ['ltr', 'rtl']) {
    for (const variant of Object.keys(variants)) {
      for (const checked of [false, true]) for (const disabled of [false, true]) {
        for (const mode of variant === 'default' ? ['empty', 'label', 'wrapped'] : ['label']) {
          const options = { direction, variant, checked, disabled, mode, button: false };
          standalone.push(`<div class="sample" dir="${direction}">${radio(options, true)}</div>`);
        }
      }
    }
    for (const variant of ['default', 'custom']) {
      for (const buttonStyle of ['outline', 'solid']) {
        for (const size of variant === 'default' ? ['small', 'medium', 'large'] : ['medium']) {
          for (const checked of [false, true]) for (const disabled of [false, true]) {
            const options = { direction, variant, size, buttonStyle, checked, disabled, mode: 'label', button: true };
            standalone.push(`<div class="sample" dir="${direction}">${group(options, radio(options, true))}</div>`);
          }
        }
      }
    }
    for (const vertical of [false, true]) {
      for (const size of ['small', 'medium', 'large']) for (const buttonStyle of ['outline', 'solid']) {
        const options = { direction, variant: 'default', size, buttonStyle, vertical, button: true };
        const content = [0, 1, 2].map(position => radio({ ...options, checked: position === 1, disabled: position === 2 })).join('');
        grouped.push(`<div class="sample" dir="${direction}">${group(options, content, true)}</div>`);
      }
      for (const block of [false, true]) {
        const options = { direction, variant: 'default', size: 'medium', buttonStyle: 'outline', vertical, block, button: false };
        const content = [0, 1, 2].map(position => radio({ ...options, checked: position === 1, disabled: position === 2 })).join('');
        grouped.push(`<div class="sample" dir="${direction}">${group(options, content, true)}</div>`);
      }
    }
  }
  if (index !== 104 || grouped.length !== 32) throw new Error(`Unexpected matrix: ${index} controls, ${grouped.length} groups`);
  return `<h2>Standalone state matrix</h2>${standalone.join('')}<h2>Connected groups</h2>${grouped.join('')}`;
}

// These files define the original DOM class structure reproduced below. They
// are recorded for review; no React runtime or Rust style implementation is used.
for (const file of ['radio.tsx', 'radioButton.tsx', 'group.tsx']) {
  read(path.join(upstream, 'components/radio', file), `components/radio/${file}`);
}
read(path.join(upstream, 'components/config-provider/hooks/useCSSVarCls.ts'),
  'components/config-provider/hooks/useCSSVarCls.ts');

fs.mkdirSync(output, { recursive: true });
for (const name of ['default', 'dark', 'compact', 'dark-compact']) {
  const fixture = fixtures.cases.find(fixture => fixture.name === name);
  const global = Object.fromEntries(Object.entries(fixture.tokens).map(([key, value]) => [key, cssValue(key, value)]));
  const css = Object.entries(variants).map(([variant, overrides]) => {
    const prefix = `ant-radio-${variant}`;
    const aliases = { ...global, ...(variant === 'wireframe' ? overrides : {}) };
    const tokens = {
      ...aliases, ...radioStyles.defaults(aliases), ...(variant === 'custom' ? overrides : {}),
      componentCls: `.${prefix}`, antCls: '.ant', iconCls: '.anticon', calc: calculator,
    };
    return serialize([
      commonStyles.genCommonStyle(tokens, prefix, `${prefix}-css-var`),
      radioStyles.generate(tokens),
    ]);
  }).join('\n');
  const html = `<!doctype html><html lang="en"><meta charset="utf-8"><meta name="viewport" content="width=device-width,initial-scale=1"><title>Ant Design Radio reference · ${name}</title><style>
    ${css}
    body{margin:0;padding:28px;font-family:${global.fontFamily};font-size:${global.fontSize}px;line-height:${global.lineHeight};color:${global.colorText};background:${global.colorBgContainer}}
    h1{font-size:28px;margin:0 0 8px}p{color:${global.colorTextSecondary};margin:0 0 24px}
    h2{font-size:18px;margin:24px 0 12px}.sample{padding:12px;margin:8px 0;border:1px solid ${global.colorBorderSecondary};width:500px;box-sizing:border-box}
    :root{color-scheme:${name.includes('dark') ? 'dark' : 'light'}}*{animation:none!important;transition:none!important}
  </style><body><h1>Radio</h1><p>Original Ant Design 6.6.7 Radio styles · circles, connected buttons, full state matrix</p>${gallery()}<script>
    const state = new URLSearchParams(location.search).get('state');
    if (['hover', 'active', 'focus'].includes(state)) {
      const styles = document.createElement('style');
      styles.textContent = document.querySelector('style').textContent.replaceAll(':hover', '[data-hover]').replaceAll(':active', '[data-active]').replaceAll(':focus-visible', '[data-focus-visible]');
      document.head.append(styles);
      document.querySelectorAll('label').forEach(element => {
        if (state === 'hover' || state === 'active') element.setAttribute('data-hover', '');
        if (state === 'active') element.setAttribute('data-active', '');
      });
      if (state === 'focus') document.querySelectorAll('input:not(:disabled)').forEach(input => input.setAttribute('data-focus-visible', ''));
    }
    const number = value => Number.parseFloat(value);
    const rect = (element, origin) => {
      const bounds = element.getBoundingClientRect();
      return { x: bounds.x - origin.x, y: bounds.y - origin.y, width: bounds.width, height: bounds.height };
    };
    const measure = wrapper => {
      const icon = wrapper.firstElementChild;
      const label = wrapper.querySelector('label > span:last-child:not(:first-child)');
      const root = getComputedStyle(wrapper);
      const circle = getComputedStyle(icon);
      const dot = getComputedStyle(icon, '::after');
      const text = label && getComputedStyle(label);
      const bounds = wrapper.getBoundingClientRect();
      const isButton = wrapper.className.includes('-button-wrapper');
      const painted = isButton ? root : circle;
      return {
        bounds: rect(wrapper, wrapper.parentElement.getBoundingClientRect()),
        iconBounds: rect(icon, bounds), labelBounds: label ? rect(label, bounds) : null,
        background: painted.backgroundColor, borderColor: painted.borderColor,
        borderWidth: number(painted.borderTopWidth), borderStyle: painted.borderTopStyle,
        corners: [painted.borderTopLeftRadius, painted.borderTopRightRadius, painted.borderBottomRightRadius, painted.borderBottomLeftRadius].map(number),
        color: text?.color ?? root.color, fontSize: number(root.fontSize), lineHeight: number(root.lineHeight), fontWeight: root.fontWeight,
        paddingStart: number(root.paddingInlineStart), paddingEnd: number(root.paddingInlineEnd),
        marginEnd: number(root.marginInlineEnd), marginBottom: number(root.marginBottom),
        labelPaddingStart: text ? number(text.paddingInlineStart) : null, labelPaddingEnd: text ? number(text.paddingInlineEnd) : null,
        dotWidth: number(dot.width) || 0, dotHeight: number(dot.height) || 0,
        dotColor: dot.backgroundColor, dotOpacity: number(dot.opacity), dotTransform: dot.transform,
        outlineColor: painted.outlineColor, outlineWidth: number(painted.outlineWidth),
        outlineOffset: number(painted.outlineOffset), outlineStyle: painted.outlineStyle,
        zIndex: root.zIndex, cursor: root.cursor, direction: root.direction,
      };
    };
    const metrics = document.createElement('script');
    metrics.type = 'application/json'; metrics.id = 'reference-metrics';
    metrics.textContent = JSON.stringify({
      controls: [...document.querySelectorAll('[data-control]')].map(wrapper => ({ options: JSON.parse(wrapper.dataset.control), ...measure(wrapper) })),
      groups: [...document.querySelectorAll('[data-group]')].map(group => ({
        options: JSON.parse(group.dataset.group), bounds: rect(group, group.parentElement.getBoundingClientRect()),
        rowGap: number(getComputedStyle(group).rowGap) || 0,
        children: [...group.children].map(measure),
      })),
    });
    document.body.append(metrics);
  </script></body></html>`;
  fs.writeFileSync(path.join(output, `radio-reference-${name}.html`), html);
}
const provenance = {
  upstream: { version: fixtures.provenance.version, revision },
  packages: Object.fromEntries([...indexes.keys()].sort().map(name => [name, versions[name]])),
  sources: Object.fromEntries([...inputs].sort(([a], [b]) => a.localeCompare(b))),
  fixtureSha256: crypto.createHash('sha256').update(fs.readFileSync(fixturePath)).digest('hex'),
  notes: 'Original Radio style TypeScript including component-token defaults, original CSS-mode calculator and cssinjs serializer; React hook registration captured and concrete aliases supplied by independently executed theme fixtures. Static DOM follows original Radio, Radio.Button and Radio.Group classes and input/label structure. Hover/active/focus-visible selectors are deterministically activated by equivalent data selectors after original CSS serialization. No Rust style values or behavior are used. Default, wireframe and source component-token customization matrices are measured.',
};
fs.writeFileSync(path.join(output, 'provenance.json'), JSON.stringify(provenance, null, 2) + '\n');
console.log(`Generated four original Radio style references in ${output}`);

