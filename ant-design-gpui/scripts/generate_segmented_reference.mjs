// Copyright (C) 2026 mg-chao
// SPDX-License-Identifier: Apache-2.0
// Developer-only visual reference. No packages are installed or downloaded.
// Executes the original pinned Segmented style generators and cssinjs serializer.
// node scripts/generate_segmented_reference.mjs --upstream E:/ant-design --pnpm-store E:/.pnpm-store/v10

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
const output = path.resolve(options.output ?? path.join(crate, '../build/ant-design-gpui/segmented-reference'));
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
// All Segmented declarations and selectors are upstream code.
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
const segmentedStyles = evaluateSource(path.join(upstream, 'components/segmented/style/index.ts')).default;
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
  const [css, effects] = parseStyle(styles, { path: 'SegmentedReference' });
  return normalizeStyle(css) + Object.values(effects).map(effect => normalizeStyle(effect)).join('');
}
function escape(value) {
  return String(value).replaceAll('&', '&amp;').replaceAll('<', '&lt;').replaceAll('"', '&quot;');
}


// Static labels reproduce the original component's semantic structure. All
// declarations, state selectors and dimensions come from its style generator.
read(path.join(upstream, 'components/segmented/index.tsx'), 'components/segmented/index.tsx');
const samples = [];
for (const size of ['small', 'medium', 'large']) {
  for (const shape of ['default', 'round']) {
    for (const orientation of ['horizontal', 'vertical']) {
      samples.push({ id: `${size}-${shape}-${orientation}`, size, shape, orientation });
    }
  }
}
samples.push(
  { id: 'disabled', disabled: true },
  { id: 'disabled-item', disabledItem: true },
  { id: 'block', block: true },
  { id: 'vertical-block', block: true, orientation: 'vertical' },
  { id: 'icons', icons: true },
  { id: 'custom', custom: true },
);
const customGlobals = { controlHeight: 38, controlPaddingHorizontal: 18, lineWidthFocus: 4, marginSM: 16 };
const customComponent = { trackPadding: 3, trackBg: '#f0f8ff', itemHoverColor: '#654321', itemSelectedBg: '#d6e4ff' };

function segmented(sample, direction) {
  const prefix = sample.custom ? 'ant-segmented-custom' : 'ant-segmented';
  const classes = [prefix];
  if (direction === 'rtl') classes.push(`${prefix}-rtl`);
  if (sample.size === 'small') classes.push(`${prefix}-sm`);
  if (sample.size === 'large') classes.push(`${prefix}-lg`);
  if (sample.shape === 'round') classes.push(`${prefix}-shape-round`);
  if (sample.orientation === 'vertical') classes.push(`${prefix}-vertical`);
  if (sample.block) classes.push(`${prefix}-block`);
  if (sample.disabled) classes.push(`${prefix}-disabled`);
  const items = ['Daily', 'Weekly', 'Monthly'].map((label, index) => {
    const disabled = !!sample.disabled || (!!sample.disabledItem && index === 1);
    const selected = index === 0;
    const itemClasses = [`${prefix}-item`];
    if (selected) itemClasses.push(`${prefix}-item-selected`);
    if (disabled) itemClasses.push(`${prefix}-item-disabled`);
    const icon = sample.icons ? `<span class="${prefix}-item-icon"><svg width="1em" height="1em" viewBox="0 0 16 16" aria-hidden="true"><rect width="16" height="16" fill="currentColor"/></svg></span>` : '';
    return `<label class="${itemClasses.join(' ')}" data-item data-selected="${selected}" data-disabled="${disabled}"><input class="${prefix}-item-input" type="radio" name="${sample.id}-${direction}" ${selected ? 'checked' : ''} ${disabled ? 'disabled' : ''}><div class="${prefix}-item-label">${icon}<span>${escape(label)}</span></div></label>`;
  }).join('');
  return `<section class="sample" style="width:480px;direction:${direction}" data-options="${escape(JSON.stringify({ ...sample, direction }))}"><div class="${classes.join(' ')}"><div class="${prefix}-group">${items}</div></div></section>`;
}

fs.mkdirSync(output, { recursive: true });
for (const name of ['default', 'dark', 'compact', 'dark-compact']) {
  const fixture = fixtures.cases.find(fixture => fixture.name === name);
  const global = Object.fromEntries(Object.entries(fixture.tokens).map(([key, value]) => [key, cssValue(key, value)]));
  const makeStyles = (prefix, aliases, component = {}) => {
    const tokens = { ...aliases, ...segmentedStyles.defaults(aliases), ...component,
      componentCls: `.${prefix}`, antCls: '.ant', iconCls: '.anticon', calc: calculator };
    return [commonStyles.genCommonStyle(tokens, prefix, prefix), segmentedStyles.generate(tokens)];
  };
  const css = serialize([
    ...makeStyles('ant-segmented', global),
    ...makeStyles('ant-segmented-custom', { ...global, ...customGlobals }, customComponent),
  ]);
  // Attribute substitutions preserve the original declarations and selector
  // specificity while measuring every state without OS pointer simulation.
  const stateCss = css.replaceAll(':hover', '[data-hover]').replaceAll(':active', '[data-active]');
  const html = `<!doctype html><html lang="en"><meta charset="utf-8"><meta name="viewport" content="width=device-width,initial-scale=1"><title>Ant Design Segmented reference · ${name}</title><style>
    ${css}${stateCss}
    body{margin:0;padding:24px;font-family:${global.fontFamily};font-size:${global.fontSize}px;line-height:${global.lineHeight};color:${global.colorText};background:${global.colorBgContainer}}
    h1{font-size:28px;font-weight:600;margin:0 0 24px}.sample{display:flow-root;margin-bottom:12px;background:${global.colorBgContainer}}
    :root{color-scheme:${name.includes('dark') ? 'dark' : 'light'}}*{animation:none!important;transition:none!important}
  </style><body><h1>Segmented · ${name}</h1>${['ltr', 'rtl'].map(direction => samples.map(sample => segmented(sample, direction)).join('')).join('')}<script>
    const number = value => Number.parseFloat(value);
    const bounds = element => {
      const box = element.getBoundingClientRect();
      return { width: box.width, height: box.height };
    };
    const style = (element, pseudo) => {
      const css = getComputedStyle(element, pseudo);
      return { color: css.color, background: css.backgroundColor, fontSize: number(css.fontSize), lineHeight: number(css.lineHeight),
        paddingLeft: number(css.paddingLeft), paddingRight: number(css.paddingRight), paddingTop: number(css.paddingTop),
        radius: number(css.borderTopLeftRadius), outlineColor: css.outlineColor, outlineWidth: number(css.outlineWidth),
        outlineOffset: number(css.outlineOffset), boxShadow: css.boxShadow, opacity: number(css.opacity) };
    };
    function collect() {
      const metrics = [];
      for (const state of [new URLSearchParams(location.search).get('state') || 'rest']) {
        for (const item of document.querySelectorAll('[data-item]')) {
          const prefix = item.classList[0].replace(/-item$/, '');
          item.removeAttribute('data-hover'); item.removeAttribute('data-active');
          item.classList.remove(prefix + '-item-focused');
          if (state === 'hover') item.setAttribute('data-hover', '');
          if (state === 'active') item.setAttribute('data-active', '');
          if (state === 'focus' && item.dataset.disabled !== 'true') item.classList.add(prefix + '-item-focused');
        }
        for (const sample of document.querySelectorAll('.sample')) {
          const root = sample.firstElementChild;
          const options = JSON.parse(sample.dataset.options);
          const items = [...root.querySelectorAll('[data-item]')];
          metrics.push({ options, state, bounds: bounds(root), style: style(root), items: items.map(item => {
          const label = item.lastElementChild;
          const iconLabel = label.querySelector('svg')?.parentElement.nextElementSibling;
          return { selected: item.dataset.selected === 'true', disabled: item.dataset.disabled === 'true', bounds: bounds(item),
            style: style(item), overlay: style(item, '::after'), label: style(label),
            iconGap: iconLabel ? number(getComputedStyle(iconLabel).marginInlineStart) : null };
          }) });
        }
      }
      const script = document.createElement('script');
      script.type = 'application/json'; script.id = 'reference-metrics';
      script.textContent = JSON.stringify(metrics); document.body.append(script);
    }
    collect();
  </script></body></html>`;
  fs.writeFileSync(path.join(output, `segmented-reference-${name}.html`), html);
}
const provenance = {
  upstream: { version: fixtures.provenance.version, revision },
  packages: Object.fromEntries([...indexes.keys()].sort().map(name => [name, versions[name]])),
  sources: Object.fromEntries([...inputs].sort(([a], [b]) => a.localeCompare(b))),
  themeFixtureSha256: crypto.createHash('sha256').update(fs.readFileSync(fixturePath)).digest('hex'),
  generator: { path: 'scripts/generate_segmented_reference.mjs', sha256: crypto.createHash('sha256').update(fs.readFileSync(fileURLToPath(import.meta.url))).digest('hex') },
  customGlobals, customComponent,
  notes: 'Original Segmented style generator and cssinjs serializer. Static DOM follows source label/input/icon slots. Hook registration is adapted for token capture. Hover/active pseudo selectors are replaced with equally specific attributes for deterministic measurements. No native implementation values are read.',
};
fs.writeFileSync(path.join(output, 'provenance.json'), JSON.stringify(provenance, null, 2) + '\n');
console.log(`Generated four upstream Segmented reference pages (${samples.length} controls per direction)`);
if (options.browser) {
  const cases = [];
  for (const theme of ['default', 'dark', 'compact', 'dark-compact']) {
    const metrics = [];
    for (const state of ['rest', 'hover', 'active', 'focus']) {
      metrics.push(...await capture(theme, options.browser, state));
    }
    for (const direction of ['ltr', 'rtl']) {
      const segmented = metrics.filter(item => item.options.direction === direction);
      if (segmented.length !== samples.length * 4) throw new Error(`Incomplete ${theme}/${direction} capture`);
      cases.push({ theme, direction, segmented });
    }
    console.log(`Captured ${theme}: ${metrics.length} control states`);
  }
  const metadataScript = `$ProgressPreference = 'SilentlyContinue'; (Get-Item -LiteralPath '${options.browser.replaceAll("'", "''")}').VersionInfo.ProductVersion`;
  const browserVersion = process.platform === 'win32'
    ? execFileSync('powershell', ['-NoProfile', '-EncodedCommand', Buffer.from(metadataScript, 'utf16le').toString('base64')], { encoding: 'utf8', windowsHide: true }).trim()
    : execFileSync(options.browser, ['--version'], { encoding: 'utf8' }).trim();
  provenance.renderer = { name: path.basename(options.browser), version: browserVersion, platform: process.platform, scaleFactor: 1, viewport: [1180, 1040], motion: 'disabled' };
  const destination = path.resolve(options.fixture ?? path.join(crate, 'fixtures/segmented/reference.json'));
  fs.mkdirSync(path.dirname(destination), { recursive: true });
  fs.writeFileSync(destination, JSON.stringify({ provenance, cases }, null, 2) + '\n');
  console.log(`Saved ${cases.reduce((sum, entry) => sum + entry.segmented.length, 0)} independently measured Segmented states to ${destination}`);
}

function capture(theme, browser, state) {
  return new Promise((resolve, reject) => {
    const profile = path.join(output, `headless-${theme}-${state}-${crypto.randomUUID()}`);
    const html = path.join(output, `segmented-reference-${theme}.html`);
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
