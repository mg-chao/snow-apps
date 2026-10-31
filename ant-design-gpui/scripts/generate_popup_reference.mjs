// Copyright (C) 2026 mg-chao
// SPDX-License-Identifier: Apache-2.0
// Developer-only visual reference. No packages are installed or downloaded.
// Executes the original pinned Popup style generators and cssinjs serializer.
// node scripts/generate_popup_reference.mjs --upstream E:/ant-design --pnpm-store E:/.pnpm-store/v10

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
const output = path.resolve(options.output ?? path.join(crate, '../build/ant-design-gpui/popup-reference'));
const fixturePath = path.join(crate, 'fixtures/theme/reference.json');
const fixtures = JSON.parse(fs.readFileSync(fixturePath, 'utf8'));
const revision = execFileSync('git', ['-C', upstream, 'rev-parse', 'HEAD'], { encoding: 'utf8' }).trim();
if (revision !== fixtures.provenance.revision) throw new Error(`Unexpected Ant Design revision ${revision}`);
process.env.NODE_ENV = 'production';

const versions = {
  '@ant-design/icons-svg': '4.4.2',
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
// All Popup geometry, state, hover, focus and disabled CSS is upstream code.
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
    genStyleHooks: (_name, generate, defaults) => Object.assign(() => ({ generate, defaults }), { generate, defaults }),
    mergeToken: (...tokens) => Object.assign({}, ...tokens),
    getLineHeight: evaluateSource(path.join(upstream, 'components/theme/themes/shared/genFontSizes.ts')).getLineHeight,
    genPresetColor: evaluateSource(path.join(upstream, 'components/theme/util/genPresetColor.ts')).default,
    PresetColors: evaluateSource(path.join(upstream, 'components/theme/interface/presetColors.ts')).PresetColors,
  };
}

const { parseStyle, normalizeStyle } = loadPackage('@ant-design/cssinjs/lib/hooks/useStyleRegister');
const calculator = loadPackage('@ant-design/cssinjs-utils/lib/util/calc').default('css', new Set());
const commonStyles = evaluateSource(path.join(upstream, 'components/style/index.tsx'));
const buttonStyles = evaluateSource(path.join(upstream, 'components/button/style/index.ts')).default;
const generators = {
  tooltip: evaluateSource(path.join(upstream, 'components/tooltip/style/index.ts')).default('ant-tooltip', 'ant-tooltip'),
  popover: evaluateSource(path.join(upstream, 'components/popover/style/index.ts')).default,
  popconfirm: evaluateSource(path.join(upstream, 'components/popconfirm/style/index.ts')).default,
};
// Record the structural source used by the static markup probes.
for (const component of ['tooltip', 'popover', 'popconfirm']) read(path.join(upstream, `components/${component}/PurePanel.tsx`), `components/${component}/PurePanel.tsx`);

function cssValue(key, value) {
  if (value && typeof value === 'object' && !Array.isArray(value) && 'r' in value) return `rgba(${value.r},${value.g},${value.b},${value.a})`;
  if (Array.isArray(value)) {
    if (key.startsWith('fontFamily')) return value.map(name => JSON.stringify(name)).join(',');
    if (key.startsWith('motionEase')) return `cubic-bezier(${value.join(',')})`;
    if (key.startsWith('dropShadow')) return value.map(shadow => `drop-shadow(${shadow.x}px ${shadow.y}px ${shadow.blur}px ${cssValue('', shadow.color)})`).join(' ');
    if (key.startsWith('boxShadow')) return value.map(shadow => `${shadow.inset ? 'inset ' : ''}${shadow.x}px ${shadow.y}px ${shadow.blur}px ${shadow.spread}px ${cssValue('', shadow.color)}`).join(',');
  }
  if (key.startsWith('motionDuration')) return `${value}s`;
  return value;
}
function serialize(styles) {
  const [css, effects] = parseStyle(styles, { path: 'PopupReference' });
  return normalizeStyle(css) + Object.values(effects).map(normalizeStyle).join('');
}
const placements = ['top', 'topLeft', 'topRight', 'bottom', 'bottomLeft', 'bottomRight', 'left', 'leftTop', 'leftBottom', 'right', 'rightTop', 'rightBottom'];
const warningIcon = loadPackage('@ant-design/icons-svg/lib/asn/ExclamationCircleFilled').default;
function iconMarkup(node) {
  const attrs = { ...node.attrs, ...(node.tag === 'svg' ? { width: '1em', height: '1em', fill: 'currentColor' } : {}) };
  return `<${node.tag} ${Object.entries(attrs).map(([name, value]) => `${name}="${String(value).replaceAll('"', '&quot;')}"`).join(' ')}>${(node.children ?? []).map(iconMarkup).join('')}</${node.tag}>`;
}
function markup(component, placement, direction, variant, index) {
  const prefix = component === 'tooltip' ? 'ant-tooltip' : 'ant-popover';
  let children;
  if (component === 'tooltip') children = '<span class="sample">Hint</span>';
  else if (component === 'popover') children = '<div class="ant-popover-title">Title</div><div class="ant-popover-content">Content</div>';
  else children = `<div class="ant-popover-content"><div class="ant-popconfirm-inner-content"><div class="ant-popconfirm-message"><span class="ant-popconfirm-message-icon"><span class="anticon">${iconMarkup(warningIcon.icon)}</span></span><div class="ant-popconfirm-message-text"><div class="ant-popconfirm-title">Delete the item?</div>${variant === 'title-only' ? '' : '<div class="ant-popconfirm-description">This operation cannot be undone.</div>'}</div></div><div class="ant-popconfirm-buttons"><button>Cancel</button><button>OK</button></div></div></div>`;
  const color = variant === 'preset' ? ` ${prefix}-blue` : '';
  return `<section dir="${direction}"><div data-case="${index}" data-component="${component}" data-placement="${placement}" data-direction="${direction}" data-variant="${variant}" class="${prefix} ${prefix}-placement-${placement}${direction === 'rtl' ? ` ${prefix}-rtl` : ''}${component === 'popconfirm' ? ' ant-popconfirm' : ''}${color}" style="position:relative;left:auto;top:auto;--arrow-x:50%;--arrow-y:50%"><div class="${prefix}-arrow"></div><div class="${prefix}-container">${children}</div></div></section>`;
}

function writeVisualPages(global, css, label) {
  const buttonTokens = { ...global, ...buttonStyles.defaults(global), componentCls: '.ant-btn', antCls: '.ant', iconCls: '.anticon', calc: loadPackage('@ant-design/cssinjs-utils/lib/util/calc').default('num') };
  const buttonCss = serialize([commonStyles.genCommonStyle(buttonTokens, 'ant-btn', 'ant-btn'), commonStyles.genIconStyle('anticon'), { '.ant-btn': commonStyles.resetComponent(buttonTokens) }, buttonStyles.generate(buttonTokens)]);
  const button = (text, small = false, primary = false) => `<button class="ant-btn ant-btn-${primary ? 'primary' : 'default'} ant-btn-color-${primary ? 'primary' : 'default'} ant-btn-variant-${primary ? 'solid' : 'outlined'}${small ? ' ant-btn-sm' : ''}"><span>${text}</span></button>`;
  for (const direction of ['ltr', 'rtl']) for (const component of Object.keys(generators)) for (const placement of placements) {
    const name = `${label}-${direction}-${component}-${placement}`;
    const surface = markup(component, placement, direction, 'normal', 0)
      .replace('position:relative;left:auto;top:auto;', 'position:absolute;left:0;top:0;')
      .replace('<button>Cancel</button><button>OK</button>', button('Cancel', true) + button('OK', true, true));
    const html = `<!doctype html><html lang="en" dir="${direction}"><meta charset="utf-8"><style>${css}${buttonCss}
      html,body{margin:0;width:800px;height:500px;overflow:hidden;font-family:${global.fontFamily};font-size:${global.fontSize}px;line-height:${global.lineHeight};background:${global.colorBgContainer};color:${global.colorText}}
      *{animation:none!important;transition:none!important}#trigger{position:absolute;left:360px;top:234px;width:80px;height:32px}#trigger>.ant-btn{width:80px;height:32px}
    </style><body><div id="trigger">${button('Open')}</div>${surface}<script>
      const root = document.querySelector('[data-case]'), trigger = document.querySelector('#trigger').getBoundingClientRect();
      const box = root.getBoundingClientRect(); const placement = ${JSON.stringify(placement)}; const gap = ${global.marginXXS + global.sizePopupArrow / 2};
      const vertical = placement.startsWith('top') || placement.startsWith('bottom'); let x, y;
      if (vertical) {
        x = placement.endsWith('Left') ? trigger.left : placement.endsWith('Right') ? trigger.right - box.width : trigger.left + (trigger.width - box.width) / 2;
        y = placement.startsWith('top') ? trigger.top - box.height - gap : trigger.bottom + gap;
      } else {
        x = placement.startsWith('left') ? trigger.left - box.width - gap : trigger.right + gap;
        y = placement.endsWith('Top') ? trigger.top : placement.endsWith('Bottom') ? trigger.bottom - box.height : trigger.top + (trigger.height - box.height) / 2;
      }
      root.style.left = x + 'px'; root.style.top = y + 'px';
      const metrics = document.createElement('script'); metrics.id = 'visual-metrics'; metrics.type = 'application/json'; metrics.textContent = JSON.stringify({name:${JSON.stringify(name)},body:{x,y,width:box.width,height:box.height}}); document.body.append(metrics);
    </script></body></html>`;
    fs.writeFileSync(path.join(output, `${name}.html`), html);
  }
}
fs.mkdirSync(output, { recursive: true });
const cases = [];
for (const name of (options.extended === 'true' ? ['radius-16'] : ['default', 'dark', 'compact', 'dark-compact'])) {
  for (const wireframe of [false, true]) {
    const fixture = fixtures.cases.find(fixture => fixture.name === name);
    const global = Object.fromEntries(Object.entries(fixture.tokens).map(([key, value]) => [key, cssValue(key, value)]));
    global.wireframe = wireframe;
    let css = '';
    const defaults = {};
    for (const [component, generator] of Object.entries(generators)) {
      const tokens = { ...global, componentCls: `.ant-${component}`, antCls: '.ant', iconCls: '.anticon', calc: calculator, min: Math.min, max: Math.max };
      defaults[component] = generator.defaults(tokens);
      css += serialize([commonStyles.genCommonStyle(tokens, `ant-${component}`, `ant-${component}`), generator.generate({ ...tokens, ...defaults[component] }, { prefixCls: `ant-${component}` })]);
    }
    let index = 0;
    const examples = Object.keys(generators).flatMap(component => ['ltr', 'rtl'].flatMap(direction => placements.map(placement => markup(component, placement, direction, 'normal', index++))));
    examples.push(markup('popconfirm', 'top', 'ltr', 'title-only', index++), markup('tooltip', 'top', 'ltr', 'preset', index++), markup('popover', 'top', 'ltr', 'preset', index++));
    const label = `${name}${wireframe ? '-wireframe' : ''}`;
    const html = `<!doctype html><html lang="en"><meta charset="utf-8"><title>Ant Design popup reference · ${label}</title><style>${css}
      body{margin:0;padding:32px;font-family:${global.fontFamily};font-size:${global.fontSize}px;line-height:${global.lineHeight};color:${global.colorText};background:${global.colorBgContainer}}
      section{padding:32px;display:inline-block;vertical-align:top}*{animation:none!important;transition:none!important}
      .anticon{display:inline-flex;align-items:center;justify-content:center;line-height:1;vertical-align:-.125em}
      .ant-popconfirm-buttons button{font:inherit;height:24px;border:1px solid ${global.colorBorder};border-radius:4px;padding:0 7px;background:${global.colorBgContainer};color:${global.colorText}}
    </style><body>${examples.join('')}<script>
      const selected = (element, pseudo) => {
        if (!element) return null;
        const style = getComputedStyle(element, pseudo);
        const rect = element.getBoundingClientRect();
        const properties = ['minWidth','maxWidth','minHeight','fontSize','lineHeight','fontWeight','color','backgroundColor','borderRadius','paddingTop','paddingBottom','paddingLeft','paddingRight','marginTop','marginBottom','marginInlineStart','marginInlineEnd','borderBottomWidth','borderBottomColor','filter','clipPath','width','height','direction','textAlign','zIndex'];
        return { boundsWidth: rect.width, boundsHeight: rect.height, ...Object.fromEntries(properties.map(key => [key, style[key]])) };
      };
      const result = [...document.querySelectorAll('[data-case]')].map(root => {
        const component = root.dataset.component;
        const prefix = component === 'tooltip' ? 'ant-tooltip' : 'ant-popover';
        return { ...root.dataset, root: selected(root), container: selected(root.querySelector('.' + prefix + '-container')), arrow: selected(root.querySelector('.' + prefix + '-arrow')), arrowShape: selected(root.querySelector('.' + prefix + '-arrow'), '::before'), title: selected(root.querySelector(component === 'popconfirm' ? '.ant-popconfirm-title' : '.ant-popover-title')), content: selected(root.querySelector(component === 'popconfirm' ? '.ant-popconfirm-description' : '.ant-popover-content')), icon: selected(root.querySelector('.anticon')), message: selected(root.querySelector('.ant-popconfirm-message')), button: selected(root.querySelector('button')) };
      });
      const metrics = document.createElement('script'); metrics.type = 'application/json'; metrics.id = 'reference-metrics'; metrics.textContent = JSON.stringify(result); document.body.append(metrics);
    </script></body></html>`;
    fs.writeFileSync(path.join(output, `popup-reference-${label}.html`), html);
    writeVisualPages(global, css, label);
    cases.push({ label, theme: name, wireframe, count: index, defaults });
  }
}
fs.writeFileSync(path.join(output, 'provenance.json'), JSON.stringify({
  upstream: { version: fixtures.provenance.version, revision },
  packages: Object.fromEntries([...indexes.keys()].sort().map(name => [name, versions[name]])),
  sources: Object.fromEntries([...inputs].sort(([a], [b]) => a.localeCompare(b))),
  fixtureSha256: crypto.createHash('sha256').update(fs.readFileSync(fixturePath)).digest('hex'),
  notes: ['Original upstream style generators and cssinjs serializer; existing independently generated global tokens.', 'Static DOM mirrors PurePanel; the reference does not use native component values.', 'Button dimensions are fixture placeholders; Button visual parity is tested by its dedicated suite.', 'Motion is disabled. Placement geometry requires separate live rc-trigger fixtures.'], cases,
}, null, 2) + '\n');
console.log(`Generated ${cases.length} upstream popup reference pages at ${output}`);
