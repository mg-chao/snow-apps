// Copyright (C) 2026 mg-chao
// SPDX-License-Identifier: Apache-2.0
// Developer-only visual reference. No packages are installed or downloaded.
// Executes the original pinned Menu style generators and cssinjs serializer.
// node scripts/generate_menu_reference.mjs --upstream E:/ant-design --pnpm-store E:/.pnpm-store/v10

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
const output = path.resolve(options.output ?? path.join(crate, '../build/ant-design-gpui/menu-reference'));
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
// All Menu declarations and selectors are upstream code.
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
    genStyleHooks: (_name, generate, defaults) => Object.assign(() => ({ generate, defaults }), { generate, defaults }),
    mergeToken: (...tokens) => Object.assign({}, ...tokens),
  };
}
const { parseStyle, normalizeStyle } = loadPackage('@ant-design/cssinjs/lib/hooks/useStyleRegister');
const calculator = loadPackage('@ant-design/cssinjs-utils/lib/util/calc').default('num');
// Retain provenance for the real registration contract mirrored by the adapter:
// rootCls defaults to prefixCls and is passed to getCommonStyle as argument three.
read(packageFile('@ant-design/cssinjs-utils', 'lib/util/genStyleUtils.js'),
  '@ant-design/cssinjs-utils@2.1.2/lib/util/genStyleUtils.js');
const menuStyles = evaluateSource(path.join(upstream, 'components/menu/style/index.ts')).default('ant-menu');
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
  const [css, effects] = parseStyle(styles, { path: 'MenuReference' });
  return normalizeStyle(css) + Object.values(effects).map(effect => normalizeStyle(effect)).join('');
}
function escape(value) {
  return String(value).replaceAll('&', '&amp;').replaceAll('<', '&lt;').replaceAll('"', '&quot;');
}

// Reproduce native-semantic item slots while executing every declaration and
// selector from upstream. No native component, dimensions, or colors are read.
for (const source of ['menu.tsx', 'MenuItem.tsx', 'SubMenu.tsx', 'MenuDivider.tsx']) {
  read(path.join(upstream, 'components/menu', source), `components/menu/${source}`);
}
const modernTokens = [
  'dropdownWidth', 'zIndexPopup', 'groupTitleColor', 'groupTitleLineHeight', 'groupTitleFontSize',
  'itemBorderRadius', 'subMenuItemBorderRadius', 'itemColor', 'itemHoverColor',
  'horizontalItemHoverColor', 'itemSelectedColor', 'subMenuItemSelectedColor',
  'horizontalItemSelectedColor', 'itemDisabledColor', 'dangerItemColor', 'dangerItemHoverColor',
  'dangerItemSelectedColor', 'dangerItemActiveBg', 'dangerItemSelectedBg', 'itemBg', 'itemHoverBg',
  'subMenuItemBg', 'itemActiveBg', 'itemSelectedBg', 'horizontalItemSelectedBg', 'activeBarWidth',
  'activeBarHeight', 'activeBarBorderWidth', 'itemMarginInline', 'horizontalItemHoverBg',
  'horizontalItemBorderRadius', 'itemHeight', 'collapsedWidth', 'popupBg', 'itemMarginBlock',
  'itemPaddingInline', 'horizontalLineHeight', 'iconMarginInlineEnd', 'iconSize', 'collapsedIconSize',
  'darkPopupBg', 'darkItemColor', 'darkDangerItemColor', 'darkItemBg', 'darkSubMenuItemBg',
  'darkItemSelectedColor', 'darkItemSelectedBg', 'darkItemHoverBg', 'darkGroupTitleColor',
  'darkItemHoverColor', 'darkItemDisabledColor', 'darkDangerItemSelectedBg',
  'darkDangerItemHoverColor', 'darkDangerItemSelectedColor', 'darkDangerItemActiveBg',
];
const customComponent = {
  itemHeight: 44, itemMarginInline: 6, itemMarginBlock: 3, itemBorderRadius: 10,
  subMenuItemBorderRadius: 6, itemPaddingInline: 20, iconSize: 18,
  collapsedIconSize: 22, iconMarginInlineEnd: 12, collapsedWidth: 96,
  horizontalLineHeight: '52px', horizontalItemBorderRadius: 4,
  itemHoverBg: '#e6fffb', itemSelectedBg: '#fff7e6', itemSelectedColor: '#d46b08',
  subMenuItemSelectedColor: '#531dab', darkItemSelectedBg: '#531dab',
  activeBarWidth: 3, activeBarHeight: 4, activeBarBorderWidth: 2,
};
const customGlobals = { fontSize: 16, borderRadiusLG: 10, paddingXS: 10, lineWidthFocus: 4 };
function menu(options) {
  const { mode, colorTheme, direction, state, custom } = options;
  const prefix = custom ? 'ant-menu-custom' : 'ant-menu';
  const rootClasses = [prefix, `${prefix}-${colorTheme}`, `${prefix}-${mode === 'collapsed' || mode === 'popup' ? 'vertical' : mode}`];
  if (mode !== 'popup') rootClasses.push(`${prefix}-root`);
  if (mode === 'collapsed') rootClasses.push(`${prefix}-inline-collapsed`);
  if (mode === 'horizontal') rootClasses.push(`${prefix}-overflow`);
  if (direction === 'rtl') rootClasses.push(`${prefix}-rtl`);
  const attributes = state === 'rest' ? '' : `data-${state}=""`;
  const icon = `<span class="${prefix}-item-icon"><svg width="1em" height="1em" viewBox="0 0 16 16" aria-hidden="true"><rect width="16" height="16" fill="currentColor"/></svg></span>`;
  const leaf = (id, flags = {}) => {
    const classes = [`${prefix}-item`];
    if (flags.selected) classes.push(`${prefix}-item-selected`);
    if (flags.disabled) classes.push(`${prefix}-item-disabled`);
    if (flags.danger) classes.push(`${prefix}-item-danger`);
    const label = `<span class="${prefix}-title-content ${id === 'normal' ? `${prefix}-title-content-with-extra` : ''}"><span class="${prefix}-item-label">${id === 'normal' ? 'Navigation' : id}</span>${id === 'normal' ? `<span class="${prefix}-item-extra">Ctrl+P</span>` : ''}</span>`;
    return `<li class="${classes.join(' ')}" data-row data-kind="item" data-flags="${escape(JSON.stringify(flags))}" data-id="${id}" ${attributes} style="${mode === 'inline' ? `padding-inline-start:${id === 'nested' ? 48 : 24}px` : ''}">${icon}${label}</li>`;
  };
  const submenu = `<li class="${prefix}-submenu ${prefix}-submenu-selected ${prefix}-submenu-open ${prefix}-submenu-${mode === 'inline' ? 'inline' : 'vertical'}" data-submenu ${attributes}><div class="${prefix}-submenu-title" data-row data-kind="submenu" data-id="submenu" data-flags="${escape(JSON.stringify({ selected: true, open: true }))}" ${attributes} style="${mode === 'inline' ? 'padding-inline-start:24px' : ''}">${icon}<span class="${prefix}-title-content">Submenu</span><i class="${prefix}-submenu-arrow"></i></div>${mode === 'inline' ? `<ul class="${prefix} ${prefix}-sub ${prefix}-inline" data-nested>${leaf('nested')}</ul>` : ''}</li>`;
  const children = leaf('normal') + leaf('selected', { selected: true }) + leaf('danger', { danger: true }) + leaf('danger-selected', { danger: true, selected: true }) + leaf('disabled-selected', { disabled: true, selected: true }) + submenu;
  const group = mode === 'horizontal' ? '' : `<li class="${prefix}-item-divider" data-divider></li><li class="${prefix}-item-group"><div class="${prefix}-item-group-title" data-group>Group</div></li>`;
  const list = `<ul class="${rootClasses.join(' ')} ${mode === 'popup' ? `${prefix}-sub` : ''}" data-root ${attributes} dir="${direction}">${children}${group}</ul>`;
  const body = mode === 'popup' ? `<div class="${prefix}-submenu-popup ${prefix}-${colorTheme}" style="position:relative">${list}</div>` : list;
  return `<section class="sample" style="width:${mode === 'horizontal' ? 1100 : 256}px;direction:${direction}" data-options="${escape(JSON.stringify(options))}">${body}</section>`;
}

fs.mkdirSync(output, { recursive: true });
const cases = [];
for (const name of ['default', 'dark', 'compact', 'dark-compact']) {
  const fixture = fixtures.cases.find(fixture => fixture.name === name);
  const global = Object.fromEntries(Object.entries(fixture.tokens).map(([key, value]) => [key, cssValue(key, value)]));
  const makeStyles = (prefix, aliases, component = {}) => {
    // Ant's default-token preparation reads these component values to derive
    // itemWidth; preserve that registration order for active-bar overrides.
    const defaults = menuStyles.defaults({ ...aliases, ...component });
    const tokens = { ...aliases, ...defaults, ...component,
      componentCls: `.${prefix}`, antCls: '.ant', iconCls: '.anticon', calc: calculator };
    return [commonStyles.genCommonStyle(tokens, prefix, prefix), menuStyles.generate(tokens)];
  };
  const css = serialize([
    ...makeStyles('ant-menu', global),
    ...makeStyles('ant-menu-custom', { ...global, ...customGlobals }, customComponent),
  ]);
  const deterministicCss = css.replaceAll(':focus-visible', '[data-focus]').replaceAll(':hover', '[data-hover]').replaceAll(':active', '[data-active]');
  const samples = [];
  for (const colorTheme of ['light', 'dark']) for (const direction of ['ltr', 'rtl']) {
    for (const mode of ['vertical', 'inline', 'collapsed', 'horizontal', 'popup']) {
      for (const state of ['rest', 'hover', 'active', 'focus']) {
        for (const custom of [false, true]) samples.push({ mode, colorTheme, direction, state, custom });
      }
    }
  }
  const html = `<!doctype html><html lang="en"><meta charset="utf-8"><title>Ant Design Menu reference · ${name}</title><style>
    ${deterministicCss}
    body{margin:0;padding:24px;font-family:${global.fontFamily};font-size:${global.fontSize}px;line-height:${global.lineHeight};background:${global.colorBgContainer}}
    .sample{display:flow-root;margin-bottom:16px}*{animation:none!important;transition:none!important}
  </style><body>${samples.map(menu).join('')}<script>
    const number = value => Number.parseFloat(value);
    const bounds = element => { const r = element.getBoundingClientRect(); return { x:r.x, y:r.y, width:r.width, height:r.height }; };
    const style = (element, pseudo) => {
      const css = getComputedStyle(element, pseudo);
      return { color:css.color, background:css.backgroundColor, fontSize:number(css.fontSize), lineHeight:number(css.lineHeight),
        paddingLeft:number(css.paddingLeft), paddingRight:number(css.paddingRight), paddingTop:number(css.paddingTop), paddingBottom:number(css.paddingBottom),
        marginLeft:number(css.marginLeft), marginRight:number(css.marginRight), marginTop:number(css.marginTop), marginBottom:number(css.marginBottom),
        radius:number(css.borderTopLeftRadius), outlineColor:css.outlineColor, outlineWidth:number(css.outlineWidth), outlineOffset:number(css.outlineOffset),
        boxShadow:css.boxShadow, opacity:number(css.opacity), borderBottomWidth:number(css.borderBottomWidth), borderBottomColor:css.borderBottomColor,
        borderRightWidth:number(css.borderRightWidth), borderLeftWidth:number(css.borderLeftWidth), width:css.width, height:css.height };
    };
    const menus = [...document.querySelectorAll('.sample')].map(sample => {
      const root = sample.querySelector('[data-root]');
      return { options:JSON.parse(sample.dataset.options), bounds:bounds(root), style:style(root),
        rows:[...root.querySelectorAll('[data-row]')].map(row => ({id:row.dataset.id,kind:row.dataset.kind,flags:JSON.parse(row.dataset.flags),bounds:bounds(row),style:style(row),indicator:style(row.dataset.kind==='submenu'&&root.classList.contains('ant-menu-horizontal')?row.parentElement:row,'::after'),icon:style(row.querySelector('svg')),label:style(row.querySelector('[class*=title-content]'))})),
        group:root.querySelector('[data-group]')?style(root.querySelector('[data-group]')):null,
        divider:root.querySelector('[data-divider]')?style(root.querySelector('[data-divider]')):null,
        nested:root.querySelector('[data-nested]')?style(root.querySelector('[data-nested]')):null };
    });
    const result = document.createElement('script'); result.type='application/json'; result.id='reference-metrics'; result.textContent=JSON.stringify(menus); document.body.append(result);
  </script></body></html>`;
  fs.writeFileSync(path.join(output, `menu-reference-${name}.html`), html);
  const tokens = menuStyles.defaults(global);
  cases.push({ theme:name, tokens:Object.fromEntries(modernTokens.map(key => [key,tokens[key]])), menus:[] });
}
const provenance = {
  upstream:{version:fixtures.provenance.version,revision},
  packages:Object.fromEntries([...indexes.keys()].sort().map(name=>[name,versions[name]])),
  sources:Object.fromEntries([...inputs].sort(([a],[b])=>a.localeCompare(b))),
  themeFixtureSha256:crypto.createHash('sha256').update(fs.readFileSync(fixturePath)).digest('hex'),
  generator:{path:'scripts/generate_menu_reference.mjs',sha256:crypto.createHash('sha256').update(fs.readFileSync(fileURLToPath(import.meta.url))).digest('hex')},
  customGlobals,customComponent,
  notes:'Original pinned Menu style generator and cssinjs serializer; static DOM follows original item/icon/title/extra/submenu slots. Equally specific attributes substitute pseudo states. No native implementation values are read.',
};
fs.writeFileSync(path.join(output,'provenance.json'),JSON.stringify(provenance,null,2)+'\n');
if (options.browser) {
  for (const entry of cases) {
    entry.menus = await capture(entry.theme,options.browser);
    if(entry.menus.length!==160) throw new Error(`Incomplete Menu capture ${entry.theme}: ${entry.menus.length}`);
    const pick = (value, keys) => value && Object.fromEntries(keys.map(key=>[key,value[key]]));
    // Keep the checked-in fixture small; raw browser captures remain in build/.
    for (const menu of entry.menus) {
      menu.style=pick(menu.style,['color','background','fontSize','lineHeight','radius','boxShadow','borderBottomWidth','borderLeftWidth','borderRightWidth']);
      for(const row of menu.rows){
        row.style=pick(row.style,['color','background','fontSize','lineHeight','paddingLeft','paddingRight','marginLeft','marginTop','marginBottom','radius','outlineColor','outlineWidth','outlineOffset']);
        row.indicator=pick(row.indicator,['borderBottomWidth','borderBottomColor','borderRightWidth','borderLeftWidth','opacity']);
        row.icon=pick(row.icon,['fontSize']);row.label=pick(row.label,['opacity']);
      }
      menu.group=pick(menu.group,['color','fontSize','lineHeight','paddingLeft','paddingRight','paddingTop','paddingBottom']);
      menu.divider=pick(menu.divider,['marginTop','marginBottom']);
      menu.nested=pick(menu.nested,['background']);
    }
    console.log(`Captured ${entry.theme}: ${entry.menus.length} Menu states`);
  }
  const metadataScript = `$ProgressPreference = 'SilentlyContinue'; (Get-Item -LiteralPath '${options.browser.replaceAll("'", "''")}').VersionInfo.ProductVersion`;
  const browserVersion = process.platform==='win32'
    ? execFileSync('powershell',['-NoProfile','-EncodedCommand',Buffer.from(metadataScript,'utf16le').toString('base64')],{encoding:'utf8',windowsHide:true}).trim()
    : execFileSync(options.browser,['--version'],{encoding:'utf8'}).trim();
  provenance.renderer={name:path.basename(options.browser),version:browserVersion,platform:process.platform,scaleFactor:1,viewport:[1180,1040],motion:'disabled'};
}
// HTML-only exploration must not replace the complete checked-in browser fixture.
const destination=path.resolve(options.fixture??(options.browser
  ? path.join(crate,'fixtures/menu/reference.json') : path.join(output,'token-reference.json')));
fs.mkdirSync(path.dirname(destination),{recursive:true});
fs.writeFileSync(destination,JSON.stringify({provenance,cases})+'\n');
console.log(`Saved independent Menu tokens and ${cases.reduce((sum,entry)=>sum+entry.menus.length,0)} browser states to ${destination}`);

function capture(theme,browser) {
  return new Promise((resolve,reject)=>{
    const profile=path.join(output,`headless-${theme}-${crypto.randomUUID()}`);
    const html=path.join(output,`menu-reference-${theme}.html`);
    const child=spawn(browser,['--headless','--disable-gpu','--no-first-run','--hide-scrollbars','--disable-extensions','--disable-background-networking','--disable-component-update','--force-device-scale-factor=1','--window-size=1180,1040','--virtual-time-budget=1000','--dump-dom',`--user-data-dir=${profile}`,new URL(`file:///${html.replaceAll('\\','/')}`).href],{windowsHide:true});
    let stdout='',stderr='',finished=false;
    const finish=error=>{
      if(finished)return;finished=true;clearTimeout(timeout);
      fs.writeFileSync(path.join(output,`metrics-${theme}.dom.html`),stdout);
      fs.writeFileSync(path.join(output,`metrics-${theme}.stderr.txt`),stderr);
      if(child.exitCode===null&&child.pid){
        if(process.platform==='win32'){try{execFileSync('taskkill',['/PID',String(child.pid),'/T','/F'],{windowsHide:true,stdio:'ignore'});}catch{}}
        else child.kill();
      }
      if(error)reject(error);else{try{resolve(JSON.parse(stdout.match(/<script type="application\/json" id="reference-metrics">([^<]+)<\/script>/)[1]));}catch(error){reject(error);}}
    };
    const timeout=setTimeout(()=>finish(new Error(`Headless browser timed out for ${theme}`)),25000);
    child.stdout.on('data',chunk=>{stdout+=chunk;if(/<script type="application\/json" id="reference-metrics">([^<]+)<\/script>/.test(stdout))finish();});
    child.stderr.on('data',chunk=>{stderr+=chunk;});child.on('error',finish);
    child.on('close',()=>{if(!finished)finish(new Error(`Headless browser exited before ${theme} metrics; see ${output}`));});
  });
}
