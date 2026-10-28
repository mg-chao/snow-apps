// Copyright (C) 2026 mg-chao
// SPDX-License-Identifier: Apache-2.0
// Developer-only reference. No packages are installed or downloaded by this tool.
// Executes the original pinned Result component and style generators and cssinjs serializer.
// node scripts/generate_result_reference.mjs --upstream E:/ant-design --pnpm-store E:/.pnpm-store/v10

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
const output = path.resolve(options.output ?? path.join(crate, '../build/ant-design-gpui/result-reference'));
const fixturePath = path.join(crate, 'fixtures/theme/reference.json');
const fixtures = JSON.parse(fs.readFileSync(fixturePath, 'utf8'));
const revision = execFileSync('git', ['-C', upstream, 'rev-parse', 'HEAD'], { encoding: 'utf8' }).trim();
if (revision !== fixtures.provenance.revision) throw new Error(`Unexpected Ant Design revision ${revision}`);
process.env.NODE_ENV = 'production';

const versions = {
  '@ant-design/colors': '8.0.1',
  '@ant-design/cssinjs': '2.1.2',
  '@ant-design/icons': '6.1.0',
  '@ant-design/icons-svg': '4.4.2',
  '@rc-component/motion': '1.3.1',
  classnames: '2.5.1',
  '@ant-design/cssinjs-utils': '2.1.2',
  '@ant-design/fast-color': '3.0.1',
  '@babel/runtime': '7.28.6',
  '@emotion/hash': '0.8.0',
  '@emotion/unitless': '0.7.5',
  '@rc-component/color-picker': '3.1.1',
  '@rc-component/util': '1.14.0',
  clsx: '2.1.1',
  'is-mobile': '5.0.0',
  react: '19.2.4',
  'react-dom': '19.2.4',
  'react-is': '18.3.1',
  scheduler: '0.27.0',
  stylis: '4.3.6',
  typescript: '5.9.3',
};
const indexes = new Map();
const usedPackages = new Set();
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
  usedPackages.add(name);
  if (name === '@rc-component/util' && options['util-package']) return path.resolve(options['util-package'], relative);
  const record = packageIndex(name).files[relative];
  if (!record) throw new Error(`Missing ${name}/${relative}`);
  const hex = Buffer.from(record.integrity.slice('sha512-'.length), 'base64').toString('hex');
  return path.join(store, 'files', hex.slice(0, 2), hex.slice(2));
}
function packageRelative(name, candidate) {
  if (name === '@rc-component/util' && options['util-package']) {
    const relative = [candidate, `${candidate}.js`, `${candidate}.json`, `${candidate}/index.js`]
      .find(file => fs.existsSync(path.resolve(options['util-package'], file)));
    if (!relative) throw new Error(`Cannot resolve ${name}/${candidate}`);
    return relative;
  }
  const files = packageIndex(name).files;
  const relative = [candidate, `${candidate}.js`, `${candidate}.json`, `${candidate}/index.js`]
    .find(file => files[file]);
  if (!relative) throw new Error(`Cannot resolve ${name}/${candidate}`);
  return relative;
}
const nativeRequire = createRequire(import.meta.url);
const moduleCache = new Map();
const browserDefinitions = new Map();
const browserLinks = new Map();
function packageRequest(request) {
  if (isBuiltin(request)) return { id: `node:${request}` };
  const parts = request.split('/');
  const name = request.startsWith('@') ? parts.splice(0, 2).join('/') : parts.shift();
  const relative = parts.length ? parts.join('/') : (() => {
    const metadata = JSON.parse(read(packageFile(name, 'package.json'), `${name}@${versions[name]}/package.json`));
    if (metadata.version !== versions[name]) throw new Error(`Wrong ${name} version: ${metadata.version}`);
    return (metadata.main ?? 'index.js').replace(/^\.\//, '');
  })();
  const file = packageRelative(name, relative);
  return { name, file, id: `${name}@${versions[name]}/${file}` };
}
function loadPackage(request) {
  if (isBuiltin(request)) return nativeRequire(request);
  const resolved = packageRequest(request);
  return loadPackageFile(resolved.name, resolved.file);
}
function loadPackageFile(name, relative) {
  const key = `${name}@${versions[name]}/${relative}`;
  if (moduleCache.has(key)) return moduleCache.get(key).exports;
  const source = read(packageFile(name, relative), key);
  browserDefinitions.set(key, relative.endsWith('.json') ? `module.exports=${source};` : source);
  const links = {}; browserLinks.set(key, links);
  const module = { exports: {} };
  moduleCache.set(key, module);
  if (relative.endsWith('.json')) {
    module.exports = JSON.parse(source);
  } else {
    const require = request => {
      if (request.startsWith('.')) {
        const target=packageRelative(name,path.posix.normalize(path.posix.join(path.posix.dirname(relative),request)));
        links[request]=`${name}@${versions[name]}/${target}`;
        return loadPackageFile(name,target);
      }
      links[request]=packageRequest(request).id;
      return loadPackage(request);
    };
    new Function('require', 'module', 'exports', source)(require, module, module.exports);
  }
  return module.exports;
}
const ts = nativeRequire(packageFile('typescript', 'lib/typescript.js'));
read(packageFile('typescript', 'lib/typescript.js'), 'typescript@5.9.3/lib/typescript.js');

// These adapters replace only React hook registration and token plumbing.
// All Result structure, artwork, token defaults and CSS are upstream code.
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
    fileName: file,
    compilerOptions: { module: ts.ModuleKind.CommonJS, target: ts.ScriptTarget.ES2022, jsx: ts.JsxEmit.React },
  }).outputText;
  const browserId = `source:${path.relative(upstream, file).replaceAll('\\', '/')}${select ? '#' + select.join(',') : ''}`;
  browserDefinitions.set(browserId,javascript);
  const links = {}; browserLinks.set(browserId,links);
  const require = request => {
    if (request === '@rc-component/color-picker') {
      return loadPackage('@rc-component/color-picker/lib/color');
    }
    if (!request.startsWith('.')) {
      links[request]=packageRequest(request).id;
      return loadPackage(request);
    }
    const base = path.resolve(path.dirname(file), request);
    const normalized = path.relative(upstream, base).replaceAll('\\', '/');
    if (normalized === 'components/theme/internal') return themeInternal();
    if (normalized === 'components/config-provider/context') {
      links[request]='adapter:config';
      return { useComponentConfig: () => ({ getPrefixCls: (name, custom) => custom ?? 'ant-' + name, ...currentConfig }) };
    }
    if (normalized === 'components/_util/warning') {
      links[request]='adapter:warning'; return { devUseWarning: () => ({ deprecated() {} }) };
    }
    if (normalized === 'components/result/style' && file.endsWith('index.tsx')) {
      links[request]='adapter:style'; return { __esModule: true, default: () => ['', ''] };
    }
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
    links[request]=`source:${path.relative(upstream,resolved).replaceAll('\\','/')}`;
    return evaluateSource(resolved);
  };
  try {
    new Function('require', 'module', 'exports', javascript)(require, module, module.exports);
  } catch (error) {
    error.message += ` (while evaluating ${file})`;
    throw error;
  }
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

// Execute Result index.tsx itself through React's renderer. Only ConfigProvider/token
// registration is adapted; Result's element tree and default resolution are real.
let currentConfig = {};
const React = loadPackage('react');
const { renderToStaticMarkup } = loadPackage('react-dom/server');
const Result = evaluateSource(path.join(upstream, 'components/result/index.tsx')).default;
const resultStyles = evaluateSource(path.join(upstream, 'components/result/style/index.ts')).default;
const commonStyles = evaluateSource(path.join(upstream, 'components/style/index.tsx'));

const e = React.createElement;

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
  const [css, effects] = parseStyle(styles, { path: 'ResultReference' });
  return normalizeStyle(css) + Object.values(effects).map(effect => normalizeStyle(effect)).join('');
}
const samples = [];
for (const status of ['success','info','warning','error','403','404','500']) {
  for (let bits=0;bits<16;bits++) samples.push({status,title:bits&1?'Result title':undefined,subTitle:bits&2?'Additional information about this result.':undefined,actions:bits&4?2:0,body:!!(bits&8)});
  samples.push({status,title:'Custom icon',customIcon:true},{status,title:'Hidden icon',showIcon:false});
}
samples.push(
  {title:'Default info result',subTitle:'Subtitle'},
  {status:'info',title:'A long result title wraps to multiple lines when the container is narrow enough.',subTitle:'Additional information about this result.',width:360},
  {status:'error',title:'LongUnbrokenResultTitleThatCannotBreakAtAnOrdinaryWordBoundary',width:360},
  {status:'info',rich:true,actions:2,body:true},
  {status:'success',title:'Token override',subTitle:'Custom tokens',actions:2,tokenCase:true},
  {status:'info',title:'Semantic override',subTitle:'Custom styles',actions:2,body:true,semantic:true},
  {status:'success',title:0,subTitle:0,actions:1},
);
const visualScenes=JSON.parse(read(path.join(crate,'fixtures/result/visual-cases.json'),'native/browser shared visual cases'));
samples.push(...Object.values(visualScenes).flat());
const semanticStyles={root:{borderWidth:2,borderStyle:'dashed',borderColor:'#1677ff',padding:16},title:{fontStyle:'italic',color:'#1890ff'},subTitle:{fontWeight:700},icon:{opacity:0.8},extra:{backgroundColor:'#f0f0f0',padding:8},body:{backgroundColor:'#fafafa',padding:12}};
function renderResult(sample,direction,index) {
  currentConfig={direction};
  const props={status:sample.status,title:sample.title,subTitle:sample.subTitle,'data-case':index,'data-options':JSON.stringify({...sample,direction})};
  if(sample.tokenCase)props.prefixCls='ant-result-custom';
  if(sample.customIcon)props.icon=e(loadPackage('@ant-design/icons/SmileOutlined').default);
  if(sample.showIcon===false)props.icon=false;
  if(sample.rich){props.title=e('span',null,'Rich ',e('strong',null,'result title'));props.subTitle=e('span',null,'Description with ',e('strong',null,'details'));}
  if(sample.actions)props.extra=Array.from({length:sample.actions},(_,i)=>e('button',{key:i,className:'reference-action',style:{width:i===0?72:68}},i===0?'Action':'Again'));
  if(sample.extraText)props.extra=sample.extraText;
  if(sample.body)props.children=e('div',null,'The content you submitted needs attention.');
  if(sample.bodyText!==undefined)props.children=sample.bodyText;
  if(sample.semantic)props.styles=semanticStyles;
  return renderToStaticMarkup(e(Result,props));
}
const measureScript=`
const bounds=n=>{const b=n.getBoundingClientRect();return{x:b.x,y:b.y,width:b.width,height:b.height}};
const part=(root,selector)=>{const n=root.querySelector(selector);if(!n)return null;const s=getComputedStyle(n);return{...bounds(n),color:s.color,background:s.backgroundColor,fontSize:s.fontSize,lineHeight:s.lineHeight,fontWeight:s.fontWeight,fontStyle:s.fontStyle,opacity:s.opacity,padding:[s.paddingTop,s.paddingRight,s.paddingBottom,s.paddingLeft],margin:[s.marginTop,s.marginRight,s.marginBottom,s.marginLeft],textAlign:s.textAlign}};
const out=document.createElement('script');out.type='application/json';out.id='reference-metrics';out.textContent=JSON.stringify([...document.querySelectorAll('[data-case]')].map(root=>{const s=getComputedStyle(root),p='.'+root.classList[0];return{index:Number(root.dataset.case),options:JSON.parse(root.dataset.options),...bounds(root),direction:s.direction,padding:[s.paddingTop,s.paddingRight,s.paddingBottom,s.paddingLeft],background:s.backgroundColor,borderWidth:s.borderTopWidth,title:part(root,p+'-title'),subTitle:part(root,p+'-subtitle'),icon:part(root,p+'-icon'),iconGlyph:part(root,p+'-icon .anticon'),image:part(root,p+'-image svg'),extra:part(root,p+'-extra'),body:part(root,p+'-body'),actions:[...root.querySelectorAll('.reference-action')].map(n=>bounds(n))}}));document.body.append(out);`;
fs.mkdirSync(output,{recursive:true});
for(const name of ['default','dark','compact','dark-compact']){
  const fixture=fixtures.cases.find(f=>f.name===name);
  const global=Object.fromEntries(Object.entries(fixture.tokens).map(([key,value])=>[key,cssValue(key,value)]));
  const defaults=resultStyles.defaults(global);
  const tokens={...global,...defaults,componentCls:'.ant-result',antCls:'.ant',iconCls:'.anticon',calc:calculator,imageWidth:250,imageHeight:295,resultInfoIconColor:global.colorInfo,resultSuccessIconColor:global.colorSuccess,resultWarningIconColor:global.colorWarning,resultErrorIconColor:global.colorError};
  const custom={...tokens,componentCls:'.ant-result-custom',titleFontSize:18,subtitleFontSize:14,iconFontSize:48,extraMargin:'12px 0 0 0'};
  const css=serialize([commonStyles.genCommonStyle(tokens,'ant-result','ant-result'),commonStyles.genCommonStyle(custom,'ant-result-custom','ant-result-custom'),commonStyles.genIconStyle('anticon'),resultStyles.generate(tokens),resultStyles.generate(custom)]);
  for(const scene of ['reference',...Object.keys(visualScenes)]){
    const visual=scene!=='reference';
    for(const direction of visual?['ltr','rtl']:['both']){
      let index=0;
      const content=(visual?[direction]:['ltr','rtl']).map(dir=>`<section dir="${dir}">${(visual?visualScenes[scene]:samples).map(sample=>`<div class="demo-case" style="width:${sample.width??700}px">${renderResult(sample,dir,index++)}</div>`).join('')}</section>`).join('');
      const html=`<!doctype html><html lang="en"><meta charset="utf-8"><title>Actual Ant Design Result · ${name}</title><style>${css}
body{margin:0;padding:20px;font-family:${global.fontFamily};font-size:${global.fontSize}px;line-height:${global.lineHeight};color:${global.colorText};background:${global.colorBgContainer};width:700px;box-sizing:content-box}section{display:flex;flex-direction:column;gap:12px}section+section{margin-top:24px}.reference-action{font:inherit;display:inline-flex;align-items:center;justify-content:center;vertical-align:middle;font-size:14px;line-height:22px;height:32px;box-sizing:border-box;border:1px solid ${global.colorBorder};border-radius:6px;background:${global.colorBgContainer};color:${global.colorText};padding:0 15px}*{animation:none!important;transition:none!important}</style><body>${content}<script>${measureScript}</script></body></html>`;
      fs.writeFileSync(path.join(output,`result-${visual?`visual-${scene}-${name}-${direction}`:`reference-${name}`}.html`),html);
    }
  }
}
const licenseDirectory=path.join(crate,'fixtures/result/licenses');fs.mkdirSync(licenseDirectory,{recursive:true});
for(const name of [...usedPackages].sort()){
  const relative=name==='@rc-component/util'&&options['util-package']?'LICENSE':['LICENSE','LICENSE.md','LICENSE.txt'].find(file=>packageIndex(name).files[file]);
  if(relative){const content=read(packageFile(name,relative),`${name}@${versions[name]}/${relative}`);fs.writeFileSync(path.join(licenseDirectory,name.replaceAll('@','').replaceAll('/','-')+'.txt'),content.replaceAll('\r\n','\n'));}
}
const provenance={schemaVersion:1,upstream:{version:fixtures.provenance.version,revision},packages:Object.fromEntries([...usedPackages].sort().map(n=>[n,versions[n]])),sources:Object.fromEntries([...inputs].sort(([a],[b])=>a.localeCompare(b))),themeFixtureSha256:crypto.createHash('sha256').update(fs.readFileSync(fixturePath)).digest('hex'),generator:{path:'scripts/generate_result_reference.mjs',sha256:crypto.createHash('sha256').update(fs.readFileSync(fileURLToPath(import.meta.url))).digest('hex')},notes:'Actual upstream Result index.tsx and all three illustration components rendered by ReactDOMServer, original Result styles and cssinjs serializer. Adapters supply scoped configuration and capture style registration. No Rust implementation or reconstructed Result DOM is read.'};
fs.writeFileSync(path.join(output,'provenance.json'),JSON.stringify(provenance,null,2)+'\n');
console.log(`Generated actual upstream Result: ${samples.length*2} cases per theme; ${Object.keys(visualScenes).length} paired visual scenes.`);
if(options.browser){
  const cases=[];for(const theme of ['default','dark','compact','dark-compact']){const results=await capture(theme,options.browser);if(results.length!==samples.length*2)throw Error(`Incomplete ${theme}`);cases.push({theme,results});console.log(`Measured ${theme}: ${results.length} Results`);}
  const versionScript=`$ProgressPreference='SilentlyContinue';(Get-Item -LiteralPath '${options.browser.replaceAll("'","''")}').VersionInfo.ProductVersion`;
  provenance.renderer={name:path.basename(options.browser),version:execFileSync('powershell',['-NoProfile','-EncodedCommand',Buffer.from(versionScript,'utf16le').toString('base64')],{encoding:'utf8',windowsHide:true}).trim(),scaleFactor:1,viewport:[740,760]};
  fs.writeFileSync(path.resolve(options.fixture??path.join(crate,'fixtures/result/reference.json')),JSON.stringify({provenance,cases})+'\n');
}
function capture(theme,browser){return new Promise((resolve,reject)=>{
  const profile=path.join(output,`headless-${theme}-${crypto.randomUUID()}`);
  const html=path.join(output,`result-reference-${theme}.html`);const url=new URL(`file:///${html.replaceAll('\\','/')}`).href;
  const child=spawn(browser,['--headless','--disable-gpu','--no-first-run','--hide-scrollbars','--disable-extensions','--disable-background-networking','--disable-component-update','--force-device-scale-factor=1','--window-size=740,760','--virtual-time-budget=1000','--dump-dom',`--user-data-dir=${profile}`,url],{windowsHide:true});
  let stdout='',stderr='',finished=false;
  const finish=error=>{if(finished)return;finished=true;clearTimeout(timeout);fs.writeFileSync(path.join(output,`metrics-${theme}.dom.html`),stdout);fs.writeFileSync(path.join(output,`metrics-${theme}.stderr.txt`),stderr);if(child.pid&&child.exitCode===null){try{execFileSync('taskkill',['/PID',String(child.pid),'/T','/F'],{windowsHide:true,stdio:'ignore'});}catch{}}if(error)reject(error);else{try{resolve(JSON.parse(stdout.match(/<script type="application\/json" id="reference-metrics">([^<]+)<\/script>/)[1]));}catch(error){reject(error);}}};
  const timeout=setTimeout(()=>finish(Error(`Browser Result measurement timed out: ${theme}`)),25000);
  child.stdout.on('data',chunk=>{stdout+=chunk;if(/<script type="application\/json" id="reference-metrics">([^<]+)<\/script>/.test(stdout))finish();});child.stderr.on('data',chunk=>stderr+=chunk);child.on('error',finish);child.on('close',()=>{if(!finished)finish(Error(`Browser exited before measuring ${theme}: ${stderr}`));});
});}
