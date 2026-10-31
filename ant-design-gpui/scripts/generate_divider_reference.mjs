// Copyright (C) 2026 mg-chao
// SPDX-License-Identifier: Apache-2.0
// Developer-only visual reference. No packages are installed or downloaded.
// Executes the original pinned Divider style generators and cssinjs serializer.
// node scripts/generate_divider_reference.mjs --upstream E:/ant-design --pnpm-store E:/.pnpm-store/v10

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
const output = path.resolve(options.output ?? path.join(crate, '../build/ant-design-gpui/divider-reference'));
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
// All Divider declarations and selectors are upstream code.
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
const dividerStyles = evaluateSource(path.join(upstream, 'components/divider/style/index.ts')).default;
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
  const [css, effects] = parseStyle(styles, { path: 'DividerReference' });
  return normalizeStyle(css) + Object.values(effects).map(effect => normalizeStyle(effect)).join('');
}
function escape(value) {
  return String(value).replaceAll('&', '&amp;').replaceAll('<', '&lt;').replaceAll('"', '&quot;');
}

// Static DOM follows components/divider/index.tsx; the declaration values and
// selectors come entirely from its original style generator, never native code.
read(path.join(upstream, 'components/divider/index.tsx'), 'components/divider/index.tsx');
const samples = [];
const sizes = [null, 'small', 'medium', 'large'];
const variants = ['solid', 'dashed', 'dotted'];
for (const size of sizes) {
  for (const variant of variants) {
    samples.push({ id: `horizontal-${size ?? 'default'}-${variant}`, size, variant });
    samples.push({ id: `vertical-${size ?? 'default'}-${variant}`, orientation: 'vertical', size, variant });
  }
}
for (const placement of ['center', 'start', 'end', 'left', 'right']) {
  for (const plain of [false, true]) {
    for (const variant of variants) {
      samples.push({ id: `title-${placement}-${plain ? 'plain' : 'heading'}-${variant}`, label: 'Text', placement, plain, variant });
    }
  }
}
for (const size of ['small', 'medium', 'large']) {
  for (const plain of [false, true]) {
    samples.push({ id: `title-${size}-${plain ? 'plain' : 'heading'}`, label: 'Text', size, plain });
  }
}
for (const placement of ['start', 'end']) {
  for (const titleMargin of [0, 20, '10%']) {
    samples.push({ id: `margin-${placement}-${titleMargin}`, label: 'Text', placement, titleMargin });
  }
}
samples.push(
  { id: 'empty-title', label: '' },
  { id: 'numeric-zero-title', label: '0' },
  { id: 'narrow-long-title', label: 'A long divider title stays on one line', width: 180 },
  { id: 'vertical-ignores-title', label: 'Hidden', orientation: 'vertical' },
  { id: 'custom-start', label: 'Text', placement: 'start', custom: true },
  { id: 'custom-end', label: 'Text', placement: 'end', custom: true },
  { id: 'custom-plain', label: 'Text', plain: true, custom: true },
  { id: 'custom-vertical', orientation: 'vertical', custom: true },
);
const customGlobals = { margin: 24, marginLG: 48, lineWidth: 5, colorSplit: '#1677ff' };
const customComponent = { verticalMarginInline: 16, textPaddingInline: 16, orientationMargin: 0.2 };

function divider(sample, direction) {
  const prefix = sample.custom ? 'ant-divider-custom' : 'ant-divider';
  const vertical = sample.orientation === 'vertical';
  const hasTitle = sample.label !== undefined && sample.label !== '';
  const placement = sample.placement === 'left' ? (direction === 'rtl' ? 'end' : 'start')
    : sample.placement === 'right' ? (direction === 'rtl' ? 'start' : 'end')
      : sample.placement ?? 'center';
  const classes = [prefix, `${prefix}-${vertical ? 'vertical' : 'horizontal'}`];
  if (direction === 'rtl') classes.push(`${prefix}-rtl`);
  if (sample.variant && sample.variant !== 'solid') classes.push(`${prefix}-${sample.variant}`);
  if (sample.size === 'small') classes.push(`${prefix}-sm`);
  if (sample.size === 'medium') classes.push(`${prefix}-md`);
  if (sample.plain) classes.push(`${prefix}-plain`);
  if (hasTitle) classes.push(`${prefix}-with-text`, `${prefix}-with-text-${placement}`);
  else classes.push(`${prefix}-rail`);
  const hasMargin = sample.titleMargin !== undefined && placement !== 'center';
  if (hasMargin) classes.push(`${prefix}-no-default-orientation-margin-${placement}`);
  const margin = hasMargin ? ` style="margin-inline-${placement}:${typeof sample.titleMargin === 'number' ? `${sample.titleMargin}px` : sample.titleMargin}"` : '';
  const content = hasTitle && !vertical
    ? `<div class="${prefix}-rail ${prefix}-rail-start"></div><span class="${prefix}-inner-text"${margin}>${escape(sample.label)}</span><div class="${prefix}-rail ${prefix}-rail-end"></div>`
    : '';
  return `<section class="sample" style="width:${sample.width ?? 480}px;direction:${direction}" data-options="${escape(JSON.stringify({ ...sample, direction, resolvedPlacement: placement }))}"><div class="${classes.join(' ')}" role="separator">${content}</div></section>`;
}

fs.mkdirSync(output, { recursive: true });
for (const name of ['default', 'dark', 'compact', 'dark-compact']) {
  const fixture = fixtures.cases.find(fixture => fixture.name === name);
  const global = Object.fromEntries(Object.entries(fixture.tokens).map(([key, value]) => [key, cssValue(key, value)]));
  const makeStyles = (prefix, aliases, component = {}) => {
    const tokens = { ...aliases, ...dividerStyles.defaults(aliases), ...component,
      componentCls: `.${prefix}`, antCls: '.ant', iconCls: '.anticon', calc: calculator };
    return [commonStyles.genCommonStyle(tokens, prefix, prefix), dividerStyles.generate(tokens)];
  };
  const css = serialize([
    ...makeStyles('ant-divider', global),
    ...makeStyles('ant-divider-custom', { ...global, ...customGlobals }, customComponent),
  ]);
  const html = `<!doctype html><html lang="en"><meta charset="utf-8"><meta name="viewport" content="width=device-width,initial-scale=1"><title>Ant Design Divider reference · ${name}</title><style>
    ${css}
    body{margin:0;padding:24px;font-family:${global.fontFamily};font-size:${global.fontSize}px;line-height:${global.lineHeight};color:${global.colorText};background:${global.colorBgContainer}}
    h1{font-size:28px;font-weight:600;margin:0 0 24px}.sample{display:flow-root;margin-bottom:12px;outline:1px solid ${global.colorBorderSecondary};background:${global.colorBgContainer}}
    :root{color-scheme:${name.includes('dark') ? 'dark' : 'light'}}*{animation:none!important;transition:none!important}
  </style><body><h1>Divider · ${name}</h1>${['ltr', 'rtl'].map(direction => samples.map(sample => divider(sample, direction)).join('')).join('')}<script>
    const number = value => Number.parseFloat(value);
    const rect = (element, origin) => {
      const bounds = element.getBoundingClientRect();
      return { x: bounds.x - origin.x, y: bounds.y - origin.y, width: bounds.width, height: bounds.height };
    };
    const style = element => {
      const css = getComputedStyle(element);
      return { color: css.color, fontSize: number(css.fontSize), fontWeight: number(css.fontWeight), lineHeight: number(css.lineHeight),
        marginTop: number(css.marginTop), marginRight: number(css.marginRight), marginBottom: number(css.marginBottom), marginLeft: number(css.marginLeft),
        paddingLeft: number(css.paddingLeft), paddingRight: number(css.paddingRight),
        borderTopWidth: number(css.borderTopWidth), borderTopStyle: css.borderTopStyle, borderTopColor: css.borderTopColor,
        borderLeftWidth: number(css.borderLeftWidth), borderLeftStyle: css.borderLeftStyle, borderLeftColor: css.borderLeftColor,
        borderRightWidth: number(css.borderRightWidth), borderRightStyle: css.borderRightStyle, borderRightColor: css.borderRightColor,
        top: number(css.top) || 0, whiteSpace: css.whiteSpace };
    };
    const metrics = document.createElement('script');
    metrics.type = 'application/json'; metrics.id = 'reference-metrics';
    metrics.textContent = JSON.stringify([...document.querySelectorAll('.sample')].map(sample => {
      const root = sample.firstElementChild;
      const origin = root.getBoundingClientRect();
      const content = root.querySelector('span');
      return { options: JSON.parse(sample.dataset.options), bounds: rect(root, sample.getBoundingClientRect()), style: style(root),
        content: content ? { bounds: rect(content, origin), style: style(content) } : null,
        rails: [...root.querySelectorAll('div')].map(rail => ({ bounds: rect(rail, origin), style: style(rail) })) };
    }));
    document.body.append(metrics);
  </script></body></html>`;
  fs.writeFileSync(path.join(output, `divider-reference-${name}.html`), html);
}
const provenance = {
  upstream: { version: fixtures.provenance.version, revision },
  packages: Object.fromEntries([...indexes.keys()].sort().map(name => [name, versions[name]])),
  sources: Object.fromEntries([...inputs].sort(([a], [b]) => a.localeCompare(b))),
  themeFixtureSha256: crypto.createHash('sha256').update(fs.readFileSync(fixturePath)).digest('hex'),
  generator: { path: 'scripts/generate_divider_reference.mjs', sha256: crypto.createHash('sha256').update(fs.readFileSync(fileURLToPath(import.meta.url))).digest('hex') },
  customGlobals, customComponent,
  notes: 'Original Divider style generator and cssinjs serializer; style-hook registration is replaced by capture. Static DOM mirrors Divider source classes and rail/content structure. No native implementation values are read. Geometry is relative to each root. Explicit titleMargin samples exercise the deprecated source orientationMargin prop.',
};
fs.writeFileSync(path.join(output, 'provenance.json'), JSON.stringify(provenance, null, 2) + '\n');
console.log(`Generated four upstream Divider reference pages (${samples.length} controls per direction) in ${output}`);

if (options.browser) {
  const cases = [];
  for (const theme of ['default', 'dark', 'compact', 'dark-compact']) {
    const metrics = await capture(theme, options.browser);
    for (const direction of ['ltr', 'rtl']) {
      const dividers = metrics.filter(item => item.options.direction === direction);
      if (dividers.length !== samples.length) throw new Error(`Incomplete ${theme}/${direction} capture`);
      cases.push({ theme, direction, dividers });
    }
    console.log(`Captured ${theme}: ${metrics.length} controls`);
  }
  const metadataScript = `$ProgressPreference = 'SilentlyContinue'; (Get-Item -LiteralPath '${options.browser.replaceAll("'", "''")}').VersionInfo.ProductVersion`;
  const browserVersion = process.platform === 'win32'
    ? execFileSync('powershell', ['-NoProfile', '-EncodedCommand', Buffer.from(metadataScript, 'utf16le').toString('base64')], { encoding: 'utf8', windowsHide: true }).trim()
    : execFileSync(options.browser, ['--version'], { encoding: 'utf8' }).trim();
  provenance.renderer = { name: path.basename(options.browser), version: browserVersion, platform: process.platform, scaleFactor: 1, viewport: [1180, 1040], motion: 'disabled' };
  const destination = path.resolve(options.fixture ?? path.join(crate, 'fixtures/divider/reference.json'));
  fs.mkdirSync(path.dirname(destination), { recursive: true });
  fs.writeFileSync(destination, JSON.stringify({ provenance, cases }, null, 2) + '\n');
  console.log(`Saved ${cases.reduce((sum, entry) => sum + entry.dividers.length, 0)} independently measured Divider references to ${destination}`);
}

function capture(theme, browser) {
  return new Promise((resolve, reject) => {
    const profile = path.join(output, `headless-${theme}-${crypto.randomUUID()}`);
    const html = path.join(output, `divider-reference-${theme}.html`);
    const url = new URL(`file:///${html.replaceAll('\\', '/')}`).href;
    const child = spawn(browser, ['--headless', '--disable-gpu', '--no-first-run', '--hide-scrollbars', '--disable-extensions', '--disable-background-networking', '--disable-component-update', '--force-device-scale-factor=1', '--window-size=1180,1040', '--virtual-time-budget=1000', '--dump-dom', `--user-data-dir=${profile}`, url], { windowsHide: true });
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
      fs.writeFileSync(path.join(output, `metrics-${theme}.dom.html`), stdout);
      fs.writeFileSync(path.join(output, `metrics-${theme}.stderr.txt`), stderr);
      stop();
      if (error) reject(error);
      else {
        try { resolve(JSON.parse(stdout.match(/<script type="application\/json" id="reference-metrics">([^<]+)<\/script>/)[1])); }
        catch (error) { reject(error); }
      }
    };
    const timeout = setTimeout(() => finish(new Error(`Headless browser timed out for ${theme}`)), 20000);
    child.stdout.on('data', chunk => {
      stdout += chunk;
      if (/<script type="application\/json" id="reference-metrics">([^<]+)<\/script>/.test(stdout)) finish();
    });
    child.stderr.on('data', chunk => { stderr += chunk; });
    child.on('error', finish);
    child.on('close', () => {
      if (!finished) finish(new Error(`Headless browser exited before emitting ${theme} metrics; see ${output}`));
    });
  });
}
