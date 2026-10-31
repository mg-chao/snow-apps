// Copyright (C) 2026 mg-chao
// SPDX-License-Identifier: Apache-2.0
// Developer-only visual reference. No packages are installed or downloaded.
// Executes the original pinned Tag style generators and cssinjs serializer.
// node scripts/generate_tag_reference.mjs --upstream E:/ant-design --pnpm-store E:/.pnpm-store/v10

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
const output = path.resolve(options.output ?? path.join(crate, '../build/ant-design-gpui/tag-reference'));
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
    compilerOptions: { module: ts.ModuleKind.CommonJS, target: ts.ScriptTarget.ES2022, jsx: ts.JsxEmit.React },
  }).outputText;
  const require = request => {
    if (request === 'react') return { ...loadPackage('react'), useMemo: fn => fn() };
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
const tagStyles = evaluateSource(path.join(upstream, 'components/tag/style/index.ts')).default;
const presetStyles = evaluateSource(path.join(upstream, 'components/tag/style/presetCmp.ts')).default;
const statusStyles = evaluateSource(path.join(upstream, 'components/tag/style/statusCmp.ts')).default;
const useColor = evaluateSource(path.join(upstream, 'components/tag/hooks/useColor.ts')).default;
const commonStyles = evaluateSource(path.join(upstream, 'components/style/index.tsx'));
for (const file of ['index.tsx', 'CheckableTag.tsx', 'CheckableTagGroup.tsx']) {
  read(path.join(upstream, 'components/tag', file), `components/tag/${file}`);
}

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
  const [css, effects] = parseStyle(styles, { path: 'TagReference' });
  return normalizeStyle(css) + Object.values(effects).map(effect => normalizeStyle(effect)).join('');
}
function escape(value) {
  return String(value).replaceAll('&', '&amp;').replaceAll('<', '&lt;').replaceAll('"', '&quot;');
}
function inlineStyle(style) {
  return Object.entries(style).map(([key, value]) => `${key.replace(/[A-Z]/g, letter => `-${letter.toLowerCase()}`)}:${value}`).join(';');
}

const colors = ['default', 'blue', 'purple', 'cyan', 'green', 'magenta', 'pink', 'red', 'orange', 'yellow', 'volcano', 'geekblue', 'gold', 'lime', 'success', 'processing', 'error', 'warning', '#f50', '#2db7f5', '#87d068', '#108ee9', '#ff550080'];
const samples = [];
for (const variant of ['filled', 'solid', 'outlined']) {
  for (const color of colors) {
    for (const disabled of [false, true]) {
      samples.push({ variant, color, disabled, mode: 'icon-close' });
    }
  }
}
for (const checked of [false, true]) {
  for (const disabled of [false, true]) {
    samples.push({ checked, disabled, mode: 'icon', color: 'default', variant: 'filled' });
  }
}
for (const mode of ['text', 'icon', 'close', 'empty', 'icon-only']) {
  samples.push({ mode, color: 'default', variant: 'filled', disabled: false });
}
const icon = (close = false) => `<span class="anticon${close ? ' ant-tag-close-icon' : ''}"${close ? ' role="button" aria-label="Close"' : ''}><svg viewBox="0 0 1024 1024" width="1em" height="1em" fill="currentColor" aria-hidden="true"><path d="M256 256L768 768M768 256L256 768" stroke="currentColor" stroke-width="64"/></svg></span>`;
function tag(options, direction, index) {
  const [variant, color, preset, status, customStyle] = useColor(options);
  const checkable = Object.hasOwn(options, 'checked');
  const classes = ['ant-tag'];
  if (direction === 'rtl') classes.push('ant-tag-rtl');
  if (checkable) {
    classes.push('ant-tag-checkable');
    if (options.checked) classes.push('ant-tag-checkable-checked');
    if (options.disabled) classes.push('ant-tag-checkable-disabled');
  } else {
    classes.push(`ant-tag-${variant}`);
    if (preset || status) classes.push(`ant-tag-${color}`);
    if (options.disabled) classes.push('ant-tag-disabled');
  }
  const hasIcon = ['icon-close', 'icon', 'icon-only'].includes(options.mode);
  const hasClose = ['icon-close', 'close'].includes(options.mode);
  const label = options.mode === 'empty' || options.mode === 'icon-only' ? '' : (hasIcon || checkable ? '<span data-label>Tag</span>' : 'Tag');
  const style = options.disabled || checkable ? '' : inlineStyle(customStyle);
  return `<span data-case="${index}" data-options="${escape(JSON.stringify({ ...options, direction }))}" class="${classes.join(' ')}" style="${style}">${hasIcon ? icon() : ''}${label}${hasClose ? icon(true) : ''}</span>`;
}

fs.mkdirSync(output, { recursive: true });
for (const name of ['default', 'dark', 'compact', 'dark-compact']) {
  const fixture = fixtures.cases.find(fixture => fixture.name === name);
  const global = Object.fromEntries(Object.entries(fixture.tokens).map(([key, value]) => [key, cssValue(key, value)]));
  const tokens = { ...global, ...tagStyles.defaults(global), componentCls: '.ant-tag', antCls: '.ant', iconCls: '.anticon', calc: calculator };
  const css = serialize([
    commonStyles.genCommonStyle(tokens, 'ant-tag', 'ant-tag'),
    commonStyles.genIconStyle('anticon'),
    tagStyles.generate(tokens), presetStyles.generate(tokens), statusStyles.generate(tokens),
  ]);
  let index = 0;
  const gallery = ['ltr', 'rtl'].map(direction => `<section dir="${direction}"><h2>${direction.toUpperCase()}</h2><div class="ant-tag-checkable-group">${samples.map(sample => `<div class="demo-case">${tag(sample, direction, index++)}</div>`).join('')}</div></section>`).join('');
  const html = `<!doctype html><html lang="en"><meta charset="utf-8"><title>Ant Design Tag reference · ${name}</title><style>
    ${css}
    body{margin:0;padding:28px;font-family:${global.fontFamily};font-size:${global.fontSize}px;line-height:${global.lineHeight};color:${global.colorText};background:${global.colorBgContainer}}
    h1{font-size:28px;margin:0 0 8px}h2{font-size:18px;margin:0 0 12px}section{margin-bottom:32px}.demo-case{padding:8px;border:1px solid ${global.colorBorderSecondary};width:130px;box-sizing:border-box;display:flex;align-items:center}.ant-tag-checkable-group{max-width:1100px}
    *{animation:none!important;transition:none!important}
  </style><body><h1>Tag</h1>${gallery}<script>
    const state = new URLSearchParams(location.search).get('state');
    if (state === 'hover' || state === 'active') {
      const styles = document.createElement('style');
      styles.textContent = document.querySelector('style').textContent.replaceAll(':hover', '[data-hover]').replaceAll(':active', '[data-active]');
      document.head.append(styles);
      document.querySelectorAll('.ant-tag,.ant-tag-close-icon').forEach(element => element.setAttribute('data-' + state, ''));
    }
    const metrics = document.createElement('script'); metrics.type = 'application/json'; metrics.id = 'reference-metrics';
    metrics.textContent = JSON.stringify([...document.querySelectorAll('[data-case]')].map(root => {
      const styles = getComputedStyle(root), bounds = root.getBoundingClientRect();
      const icon = root.querySelector('.anticon:not(.ant-tag-close-icon)'), close = root.querySelector('.ant-tag-close-icon');
      const label = root.querySelector('[data-label]');
      return { index: Number(root.dataset.case), options: JSON.parse(root.dataset.options), width: bounds.width, height: bounds.height,
        background: styles.backgroundColor, borderColor: styles.borderColor, color: styles.color,
        fontSize: styles.fontSize, lineHeight: styles.lineHeight, fontWeight: styles.fontWeight,
        paddingStart: styles.paddingInlineStart, paddingEnd: styles.paddingInlineEnd,
        borderWidth: styles.borderInlineStartWidth, radius: styles.borderRadius, direction: styles.direction,
        iconSize: icon ? getComputedStyle(icon).fontSize : null,
        iconGap: label && icon ? getComputedStyle(label).marginInlineStart : null,
        closeSize: close ? getComputedStyle(close).fontSize : null,
        closeGap: close ? getComputedStyle(close).marginInlineStart : null,
        closeColor: close ? getComputedStyle(close).color : null,
        groupGap: getComputedStyle(root.closest('.ant-tag-checkable-group')).gap };
    }));
    document.body.append(metrics);
  </script></body></html>`;
  fs.writeFileSync(path.join(output, `tag-reference-${name}.html`), html);
}
const provenance = {
  schemaVersion: 1,
  upstream: { version: fixtures.provenance.version, revision },
  packages: Object.fromEntries([...indexes.keys()].sort().map(name => [name, versions[name]])),
  sources: Object.fromEntries([...inputs].sort(([a], [b]) => a.localeCompare(b))),
  themeFixtureSha256: crypto.createHash('sha256').update(fs.readFileSync(fixturePath)).digest('hex'),
  generator: { path: 'scripts/generate_tag_reference.mjs', sha256: crypto.createHash('sha256').update(fs.readFileSync(fileURLToPath(import.meta.url))).digest('hex') },
  notes: 'Original Tag style generators, color hook, color conversion and cssinjs serializer. React registration and memo hooks are captured; global aliases come from independent upstream theme fixtures. Static DOM follows Tag/CheckableTag wrappers and icon/content/close siblings. No native Rust values are read.',
};
fs.writeFileSync(path.join(output, 'provenance.json'), JSON.stringify(provenance, null, 2) + '\n');
console.log(`Generated four original Tag references (${samples.length * 2} controls each) in ${output}`);

if (options.browser) {
  const cases = [];
  for (const theme of ['default', 'dark', 'compact', 'dark-compact']) {
    for (const state of ['rest', 'hover', 'active']) {
      const tags = await capture(theme, state, options.browser);
      if (tags.length !== samples.length * 2) throw new Error(`Incomplete ${theme}/${state} capture`);
      cases.push({ theme, state, tags });
      console.log(`Captured ${theme}/${state}: ${tags.length} controls`);
    }
  }
  const metadataScript = `$ProgressPreference = 'SilentlyContinue'; (Get-Item -LiteralPath '${options.browser.replaceAll("'", "''")}').VersionInfo.ProductVersion`;
  const version = process.platform === 'win32'
    ? execFileSync('powershell', ['-NoProfile', '-EncodedCommand', Buffer.from(metadataScript, 'utf16le').toString('base64')], { encoding: 'utf8', windowsHide: true }).trim()
    : execFileSync(options.browser, ['--version'], { encoding: 'utf8' }).trim();
  provenance.renderer = { name: path.basename(options.browser), version, platform: process.platform, scaleFactor: 1, viewport: [1180, 1040], motion: 'disabled' };
  const destination = path.resolve(options.fixture ?? path.join(crate, 'fixtures/tag/reference.json'));
  fs.mkdirSync(path.dirname(destination), { recursive: true });
  fs.writeFileSync(destination, JSON.stringify({ provenance, cases }) + '\n');
  console.log(`Saved ${cases.reduce((sum, entry) => sum + entry.tags.length, 0)} independent Tag browser measurements to ${destination}`);
}

function capture(theme, state, browser) {
  return new Promise((resolve, reject) => {
    const profile = path.join(output, `headless-${theme}-${state}-${crypto.randomUUID()}`);
    const html = path.join(output, `tag-reference-${theme}.html`);
    const url = new URL(`file:///${html.replaceAll('\\', '/')}?state=${state}`).href;
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
