// Copyright (C) 2026 mg-chao
// SPDX-License-Identifier: Apache-2.0
// Developer-only visual reference. No packages are installed or downloaded.
// Executes the original pinned Button style generators and cssinjs serializer.
// node scripts/generate_button_reference.mjs --upstream E:/ant-design --pnpm-store E:/.pnpm-store/v10

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
const output = path.resolve(options.output ?? path.join(crate, '../build/ant-design-gpui/button-reference'));
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
// All color, sizing, variants, focus, disabled, ghost, icon, and motion CSS is upstream code.
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
const calculator = loadPackage('@ant-design/cssinjs-utils/lib/util/calc').default('num');
// Retain provenance for the real registration contract mirrored by the adapter:
// rootCls defaults to prefixCls and is passed to getCommonStyle as argument three.
read(packageFile('@ant-design/cssinjs-utils', 'lib/util/genStyleUtils.js'),
  '@ant-design/cssinjs-utils@2.1.2/lib/util/genStyleUtils.js');
const buttonStyles = evaluateSource(path.join(upstream, 'components/button/style/index.ts')).default;
const commonStyles = evaluateSource(path.join(upstream, 'components/style/index.tsx'));
const presets = evaluateSource(path.join(upstream, 'components/theme/interface/presetColors.ts')).PresetColors;

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
  const [css, effects] = parseStyle(styles, { path: 'ButtonReference' });
  return normalizeStyle(css) + Object.values(effects).map(effect => normalizeStyle(effect)).join('');
}
function escape(value) {
  return String(value).replaceAll('&', '&amp;').replaceAll('<', '&lt;').replaceAll('"', '&quot;');
}
function icon(name) {
  let svg = read(path.join(crate, `resources/icons/generated/outlined/${name}.svg`), `icons/outlined/${name}.svg`);
  svg = svg.replace('<svg ', '<svg width="1em" height="1em" aria-hidden="true" ');
  return `<span class="ant-btn-icon"><span class="anticon anticon-${name}">${svg}</span></span>`;
}
function button(label, { type = 'default', variant, color, danger, shape, size, disabled, loading, glyph, end, ghost, block, id } = {}) {
  const pairs = { primary: ['primary', 'solid'], default: ['default', 'outlined'], dashed: ['default', 'dashed'], text: ['default', 'text'], link: ['link', 'link'] };
  const [typeColor, typeVariant] = pairs[type];
  color ??= danger ? 'dangerous' : typeColor;
  variant ??= typeVariant;
  if (ghost && variant === 'solid') variant = 'outlined';
  const classes = ['ant-btn', `ant-btn-${type}`, `ant-btn-color-${color}`, `ant-btn-variant-${variant}`];
  if (shape) classes.push(`ant-btn-${shape}`);
  if (size) classes.push(`ant-btn-${size}`);
  if (loading) classes.push('ant-btn-loading');
  if (ghost && !['text', 'link'].includes(variant)) classes.push('ant-btn-background-ghost');
  if (block) classes.push('ant-btn-block');
  if (end) classes.push('ant-btn-icon-end');
  if (!label && (glyph || loading)) classes.push('ant-btn-icon-only');
  return `<button ${id ? `id="${escape(id)}"` : ''} class="${classes.join(' ')}" ${disabled ? 'disabled' : ''}>${glyph || loading ? icon(loading ? 'loading' : glyph) : ''}${label ? `<span>${escape(label)}</span>` : ''}</button>`;
}
const row = children => `<div class="demo-row">${children.join('')}</div>`;
const panel = (title, children) => `<section><h2>${escape(title)}</h2>${children}</section>`;
// Keep the public comparison gallery compact, then exercise the state combinations
// that cannot be represented by its representative examples. Appearance classes
// are direct inputs to the original style generators, independent of Rust rules.
function parityMatrix() {
  const variants = ['outlined', 'dashed', 'solid', 'filled', 'text', 'link'];
  const neutral = row(variants.map(variant => button(variant, { color: 'default', variant })));
  const colors = presets.map(color => row(variants.filter(variant => variant !== 'solid')
    .map(variant => button(variant, { color, variant })))).join('');
  const disabled = row(variants.map(variant => button(variant, {
    color: 'purple', variant, disabled: true,
  })));
  const ghosts = ['default', 'primary', 'dangerous'].flatMap(color => [false, true]
    .map(disabled => row(['outlined', 'dashed', 'filled'].map(variant => button(variant, {
      color, variant, ghost: true, disabled,
    }))))).join('');
  const geometry = [undefined, 'lg', 'sm'].flatMap(size => [undefined, 'square', 'circle', 'round']
    .map(shape => row([
      button('Shape', { size, shape }),
      button('', { size, shape, glyph: 'search' }),
    ]))).join('');
  return panel('Visual parity matrix', neutral + colors + disabled + ghosts + geometry);
}
function gallery(name) {
  const types = ['primary', 'default', 'dashed', 'text', 'link'];
  const labels = ['Primary', 'Default', 'Dashed', 'Text', 'Link'];
  return `<header><h1>Button</h1><p>Ant Design · original 6.6.7 style generators</p></header>
    <div class="toolbar demo-row">${[
      button(name.includes('dark') ? 'Light theme' : 'Dark theme'),
      button(name.includes('compact') ? 'Standard density' : 'Compact density'),
      button('Right to left', { id: 'direction-toggle' }),
      button('Disable buttons'), button('Enable motion'),
    ].join('')}</div>
    <p class="status">0 activations · Ready · Tab to navigate, Enter or Space to activate</p>
    <div class="panels">${panel('Button types', row(types.map((type, index) => button(labels[index], { type, id: type }))) + row(types.map((type, index) => button(labels[index], { type, danger: true }))))}
    ${panel('Color and variant', ['primary', 'dangerous'].map(color => row(['solid', 'outlined', 'dashed', 'filled', 'text', 'link'].map(variant => button(variant[0].toUpperCase() + variant.slice(1), { color, variant })))).join(''))}</div>
    <div class="panels">${panel('Sizes', row([['Large', 'lg'], ['Default', undefined], ['Small', 'sm']].map(([label, size]) => button(label, { type: 'primary', size, glyph: 'download' }))))}
    ${panel('Shapes', row([button('Default'), button('Round', { shape: 'round' }), button('', { shape: 'square', glyph: 'search' }), button('', { type: 'primary', shape: 'circle', glyph: 'search' })]))}</div>
    <div class="panels">${panel('Icons and placement', row([button('Search', { type: 'primary', glyph: 'search' }), button('Download', { glyph: 'download' }), button('Next', { glyph: 'arrow-right', end: true }), button('', { glyph: 'ellipsis' })]))}
    ${panel('Loading', row([button('Loading', { type: 'primary', loading: true }), button('Click to load', { type: 'primary' }), button('Delayed loading')]))}</div>
    <div class="panels">${panel('Disabled', row(types.map((type, index) => button(labels[index], { type, disabled: true }))))}
    ${panel('Ghost', `<div class="ghost">${row([button('Primary', { type: 'primary', ghost: true }), button('Default', { ghost: true }), button('Dashed', { type: 'dashed', ghost: true }), button('Danger', { danger: true, ghost: true })])}</div>`)}</div>
    <div class="panels">${panel('Preset colors', row([...presets.filter(color => color !== 'gold' && color !== 'lime'), 'gold', 'lime'].map(color => button(color, { color, variant: 'solid' }))))}
    ${panel('Block', button('Primary', { type: 'primary', block: true }) + button('Default', { block: true }))}</div>
    <div class="panels">${parityMatrix()}</div>`;
}
fs.mkdirSync(output, { recursive: true });
for (const name of ['default', 'dark', 'compact', 'dark-compact']) {
  const fixture = fixtures.cases.find(fixture => fixture.name === name);
  const global = Object.fromEntries(Object.entries(fixture.tokens).map(([key, value]) => [key, cssValue(key, value)]));
  const tokens = {
    ...global,
    ...buttonStyles.defaults(global),
    componentCls: '.ant-btn', antCls: '.ant', iconCls: '.anticon', calc: calculator,
  };
  const css = serialize([
    commonStyles.genCommonStyle(tokens, 'ant-btn', 'ant-btn'),
    commonStyles.genIconStyle('anticon'),
    { '.ant-btn': commonStyles.resetComponent(tokens) },
    buttonStyles.generate(tokens),
  ]);
  const html = `<!doctype html><html lang="en"><meta charset="utf-8"><meta name="viewport" content="width=device-width,initial-scale=1"><title>Ant Design Button reference · ${name}</title><style>
    ${css}
    body{margin:0;padding:28px;font-family:${global.fontFamily};font-size:${global.fontSize}px;line-height:${global.lineHeight};color:${global.colorText};background:${global.colorBgLayout}}
    header{margin-bottom:20px}h1{font-size:30px;font-weight:600;line-height:${global.lineHeight};margin:0 0 6px}p{color:${global.colorTextSecondary};margin:0}.toolbar,.status{margin-bottom:20px}
    .panels{display:flex;gap:20px;align-items:stretch;flex-wrap:wrap;margin-bottom:20px}.panels>section{flex:1;min-width:430px;box-sizing:border-box;border:1px solid ${global.colorBorderSecondary};background:${global.colorBgContainer};border-radius:8px;padding:20px}
    h2{font-size:16px;font-weight:600;line-height:${global.lineHeight};margin:0 0 16px}.demo-row{display:flex;flex-wrap:wrap;gap:8px;align-items:center}.demo-row+.demo-row{margin-top:16px}.ghost{background:#8c8c8c;padding:16px;border-radius:6px}.ant-btn-block+.ant-btn-block{margin-top:8px}
    :root{color-scheme:${name.includes('dark') ? 'dark' : 'light'}}body.rtl{direction:rtl}*{animation:none!important;transition:none!important}
  </style><body>${gallery(name)}<script>
    const parameters = new URLSearchParams(location.search);
    if (parameters.has('rtl')) {
      document.body.classList.add('rtl');
      document.querySelector('#direction-toggle span').textContent = 'Left to right';
    }
    const state = parameters.get('state');
    if (state === 'hover' || state === 'active') {
      const styles = document.createElement('style');
      styles.textContent = document.querySelector('style').textContent.replaceAll(':hover', '[data-hover]').replaceAll(':active', '[data-active]');
      document.head.append(styles);
      document.querySelectorAll('button').forEach(button => button.setAttribute('data-' + state, ''));
    }
    const metrics = document.createElement('script');
    metrics.type = 'application/json';
    metrics.id = 'reference-metrics';
    metrics.textContent = JSON.stringify([...document.querySelectorAll('button')].map((button, index) => {
      const bounds = button.getBoundingClientRect();
      const styles = getComputedStyle(button);
      const icon = button.querySelector('.anticon');
      const iconBounds = icon?.getBoundingClientRect();
      return {
        index, id: button.id, label: button.textContent.trim(), classes: button.className,
        disabled: button.disabled,
        width: bounds.width, height: bounds.height, background: styles.backgroundColor,
        color: styles.color, borderColor: styles.borderColor, borderWidth: styles.borderWidth,
        borderStyle: styles.borderStyle, radius: styles.borderRadius, gap: styles.gap,
        padding: styles.padding, font: styles.fontFamily, fontSize: styles.fontSize,
        fontWeight: styles.fontWeight, lineHeight: styles.lineHeight, opacity: styles.opacity,
        shadow: styles.boxShadow,
        iconFontSize: icon ? getComputedStyle(icon).fontSize : null,
        iconWidth: iconBounds?.width ?? null, iconHeight: iconBounds?.height ?? null,
      };
    }));
    document.body.append(metrics);
  </script></body></html>`;
  fs.writeFileSync(path.join(output, `button-reference-${name}.html`), html);
}
const provenance = {
  upstream: { version: fixtures.provenance.version, revision },
  packages: Object.fromEntries([...indexes.keys()].sort().map(name => [name, versions[name]])),
  sources: Object.fromEntries([...inputs].sort(([a], [b]) => a.localeCompare(b))),
  fixtureSha256: crypto.createHash('sha256').update(fs.readFileSync(fixturePath)).digest('hex'),
  notes: 'Original source style generators with React hook registration replaced by capture; original cssinjs parseStyle/normalizeStyle; static DOM uses upstream Button classes. No runtime component logic is emulated.',
};
fs.writeFileSync(path.join(output, 'provenance.json'), JSON.stringify(provenance, null, 2) + '\n');
console.log(`Generated four upstream Button visual references in ${output}`);
