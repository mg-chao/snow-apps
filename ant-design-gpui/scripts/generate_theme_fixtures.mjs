// Copyright (C) 2026 mg-chao
// SPDX-License-Identifier: Apache-2.0
// Optional developer tool. Cargo never executes Node or this generator.
// Executes the original Ant Design TypeScript and original pinned npm packages.
// Example: node scripts/generate_theme_fixtures.mjs --upstream E:/ant-design --pnpm-store E:/.pnpm-store/v10
// Alternative: --node-modules <directory containing pinned packages and typescript>

import fs from 'node:fs';
import path from 'node:path';
import crypto from 'node:crypto';
import { createRequire } from 'node:module';
import { execFileSync } from 'node:child_process';
import { fileURLToPath } from 'node:url';

const options = Object.fromEntries(process.argv.slice(2).reduce((pairs, value, index, args) => {
  if (index % 2 === 0) pairs.push([value.replace(/^--/, ''), args[index + 1]]);
  return pairs;
}, []));
if (!options.upstream || (!options['pnpm-store'] && !options['node-modules'])) {
  throw new Error('Supply --upstream plus --pnpm-store or --node-modules. Packages must be colors 8.0.1, fast-color 3.0.1, TypeScript 5.9.3.');
}
const upstream = path.resolve(options.upstream);
const expectedRevision = 'f8f5595ab5e891fff6a2a06c5b3b98af643d008e';
const revision = execFileSync('git', ['-C', upstream, 'rev-parse', 'HEAD'], { encoding: 'utf8' }).trim();
if (revision !== expectedRevision) throw new Error(`Expected Ant Design ${expectedRevision}, got ${revision}`);
const packageVersions = { '@ant-design/colors': '8.0.1', '@ant-design/fast-color': '3.0.1', typescript: '5.9.3' };
const inputs = new Map();
function readInput(file, label) {
  const source = fs.readFileSync(file, 'utf8');
  inputs.set(label, crypto.createHash('sha256').update(source).digest('hex'));
  return source;
}
const packageIndexes = new Map();
function packageFile(name, relative) {
  if (options['node-modules']) return path.resolve(options['node-modules'], name, relative);
  const store = path.resolve(options['pnpm-store']);
  let index = packageIndexes.get(name);
  if (!index) {
    const suffix = `-${name.replaceAll('/', '+')}@${packageVersions[name]}.json`;
    for (const directory of fs.readdirSync(path.join(store, 'index'))) {
      const folder = path.join(store, 'index', directory);
      if (!fs.statSync(folder).isDirectory()) continue;
      const entry = fs.readdirSync(folder).find(file => file.endsWith(suffix));
      if (entry) { index = JSON.parse(fs.readFileSync(path.join(folder, entry), 'utf8')); break; }
    }
    if (!index) throw new Error(`Missing ${name}@${packageVersions[name]} in pnpm store`);
    packageIndexes.set(name, index);
  }
  const record = index.files[relative];
  if (!record) throw new Error(`Missing ${name}/${relative}`);
  const hex = Buffer.from(record.integrity.slice('sha512-'.length), 'base64').toString('hex');
  return path.join(store, 'files', hex.slice(0, 2), hex.slice(2));
}
for (const [name, version] of Object.entries(packageVersions)) {
  const metadata = JSON.parse(readInput(packageFile(name, 'package.json'), `${name}@${version}/package.json`));
  if (metadata.version !== version) throw new Error(`Wrong ${name} version: ${metadata.version}`);
}
const nativeRequire = createRequire(import.meta.url);
const tsFile = packageFile('typescript', 'lib/typescript.js');
readInput(tsFile, 'typescript@5.9.3/lib/typescript.js');
const ts = nativeRequire(tsFile);
const moduleCache = new Map();
function loadPackage(name, relative = 'lib/index.js') {
  const key = `${name}/${relative}`;
  if (moduleCache.has(key)) return moduleCache.get(key).exports;
  const module = { exports: {} }; moduleCache.set(key, module);
  const source = readInput(packageFile(name, relative), `${name}@${packageVersions[name]}/${relative}`);
  const require = request => request.startsWith('.')
    ? loadPackage(name, path.posix.normalize(path.posix.join(path.posix.dirname(relative), request.endsWith('.js') ? request : `${request}.js`)))
    : loadPackage(request);
  new Function('require', 'module', 'exports', source)(require, module, module.exports);
  return module.exports;
}
function loadSource(file) {
  file = path.resolve(file);
  if (moduleCache.has(file)) return moduleCache.get(file).exports;
  const module = { exports: {} }; moduleCache.set(file, module);
  const source = readInput(file, path.relative(upstream, file).replaceAll('\\', '/'));
  const javascript = ts.transpileModule(source, { compilerOptions: { module: ts.ModuleKind.CommonJS, target: ts.ScriptTarget.ES2022 } }).outputText;
  const require = request => {
    if (!request.startsWith('.')) return loadPackage(request);
    const base = path.resolve(path.dirname(file), request);
    const resolved = [`${base}.ts`, `${base}.tsx`, path.join(base, 'index.ts'), path.join(base, 'index.tsx')].find(candidate => fs.existsSync(candidate));
    if (!resolved) throw new Error(`Unable to resolve ${request} from ${file}`);
    return loadSource(resolved);
  };
  new Function('require', 'module', 'exports', javascript)(require, module, module.exports);
  return module.exports;
}
const theme = name => loadSource(path.join(upstream, 'components/theme', name)).default;
const defaults = theme('themes/seed.ts');
const algorithms = { default: theme('themes/default/index.ts'), dark: theme('themes/dark/index.ts'), compact: theme('themes/compact/index.ts') };
const formatToken = theme('util/alias.ts');
const { FastColor } = loadPackage('@ant-design/fast-color');
const seedNames = new Set(Object.keys(defaults));
const presets = ['blue', 'purple', 'cyan', 'green', 'magenta', 'pink', 'red', 'orange', 'yellow', 'volcano', 'geekblue', 'gold', 'lime'];
const presetPattern = new RegExp(`^(${presets.join('|')})(?:-?\\d+|Hover|Active)?$`);
function shadowList(source) {
  const entries = source.match(/(?:inset\s+)?(?:-?(?:\d*\.)?\d+(?:px)?\s+){2,4}(?:rgba?\([^)]*\)|#[\da-f]{3,8})/gi);
  if (!entries) throw new Error(`Invalid reference shadow ${source}`);
  return entries.map(entry => {
    const color = entry.match(/rgba?\([^)]*\)|#[\da-f]{3,8}/i)[0];
    const dimensions = entry.slice(0, entry.indexOf(color)).match(/-?(?:\d*\.)?\d+/g).map(Number);
    return { x: dimensions[0], y: dimensions[1], blur: dimensions[2] || 0, spread: dimensions[3] || 0, color: new FastColor(color).toRgb(), inset: entry.startsWith('inset') };
  });
}
function normalizeValue(key, value) {
  if (typeof value !== 'string') return value;
  if (key.startsWith('fontFamily')) return value.split(',').map(family => family.trim().replace(/^['"]|['"]$/g, ''));
  if (key.startsWith('motionEase')) return value.match(/-?(?:\d*\.)?\d+/g).map(Number);
  if (key.startsWith('motionDuration')) return parseFloat(value);
  if (key.startsWith('boxShadow') || key.startsWith('dropShadow')) return shadowList(value);
  if (key.startsWith('color') || key === 'controlOutline' || key === 'controlTmpOutline' || key.startsWith('controlItemBg') || presetPattern.test(key)) {
    return value === '' ? null : value === 'transparent' ? { r: 0, g: 0, b: 0, a: 0 } : new FastColor(value).toRgb();
  }
  return value;
}
// The native API exposes a typed palette, rather than two independently mutable
// copies of every tone under current and deprecated CSS token spellings.
const legacyPalettePattern = new RegExp(`^(${presets.join('|')})-\\d+$`);
const normalize = object => Object.fromEntries(Object.entries(object).filter(([key]) => !legacyPalettePattern.test(key)).map(([key, value]) => [key, normalizeValue(key, value)]));
function declarations(file) {
  const source = readInput(path.join(upstream, 'components/theme/interface', file), `components/theme/interface/${file}`);
  return [...source.matchAll(/^  ([A-Za-z][A-Za-z0-9]*): ([^;]+);/gm)].map(match => [match[1], match[2]]);
}
const mapDeclarations = ['maps/colors.ts', 'maps/font.ts', 'maps/size.ts', 'maps/style.ts', 'maps/index.ts'].flatMap(declarations).filter(([key]) => !seedNames.has(key));
const mapNames = new Set(mapDeclarations.map(([key]) => key));
const aliasDeclarations = declarations('alias.ts').filter(([key]) => !seedNames.has(key) && !mapNames.has(key));
declarations('seeds.ts'); declarations('presetColors.ts');
const allSeed = {
  ...Object.fromEntries(presets.map((name, index) => [name, `#${(0x245b89 + index * 0x0b0305).toString(16).padStart(6, '0')}`])),
  colorPrimary: '#b53e98', colorSuccess: '#2c9c83', colorWarning: '#d27c23', colorError: '#c74251', colorInfo: '#357db3', colorLink: '#bc12d3',
  colorTextBase: '#f4ead7', colorBgBase: '#27324a', fontFamily: "'Test UI', sans-serif", fontFamilyCode: "'Test Mono', monospace", fontSize: 15,
  lineWidth: 2.5, lineType: 'dashed', borderRadius: 9.25, sizeUnit: 5, sizeStep: 5, sizePopupArrow: 18, controlHeight: 36,
  zIndexBase: 10, zIndexPopupBase: 2000, opacityImage: 0.8, motionUnit: 0.125, motionBase: 0.05,
  ...Object.fromEntries(Object.keys(defaults).filter(key => key.startsWith('motionEase')).map((key, index) => [key, `cubic-bezier(0.2, ${index % 2 ? -0.3 : 0.4}, 0.8, 1.2)`])),
  wireframe: true, focusOutline: false, motion: false,
};
if (Object.keys(allSeed).length !== seedNames.size) throw new Error('all-seeds case must exercise every seed');
function overrideFor(key, type, index) {
  if (key.startsWith('motionDuration')) return `${0.7 + index % 3 / 10}s`;
  if (key.startsWith('boxShadow') || key.startsWith('dropShadow')) return `${key.includes('TabsOverflow') ? 'inset ' : ''}1px -2px 3px 4px rgba(10,20,30,0.45)`;
  if (key.startsWith('link')) return 'underline';
  if (type === 'string') return `#${(0x356fa2 + index * 0x010301).toString(16).padStart(6, '0')}`;
  return key.startsWith('lineHeight') ? 1.25 : 12.5 + index % 17;
}
const mapOverrides = Object.fromEntries(mapDeclarations.map(([key, type], index) => [key, overrideFor(key, type, index)]));
const aliasOverrides = Object.fromEntries(aliasDeclarations.map(([key, type], index) => [key, overrideFor(key, type, index)]));
const cases = [];
function add(name, chain, seed = {}, override = {}) {
  const effectiveSeed = { ...defaults, ...seed, ...Object.fromEntries(Object.entries(override).filter(([key]) => seedNames.has(key))) };
  const derived = chain.reduce((map, name) => algorithms[name](effectiveSeed, map), undefined);
  const map = { ...derived, ...Object.fromEntries(Object.entries(override).filter(([key]) => !seedNames.has(key) && Object.hasOwn(derived, key))) };
  const tokens = formatToken({ ...derived, override });
  cases.push({ name, input: { algorithms: chain, seed: normalize(seed), overrides: normalize(override) }, seed: normalize(effectiveSeed), map: normalize(map), tokens: normalize(tokens) });
}
add('default', ['default']); add('dark', ['dark']); add('compact', ['compact']); add('dark-compact', ['dark', 'compact']);
add('all-seeds-light', ['default'], allSeed); add('all-seeds-dark-compact', ['dark', 'compact'], allSeed);
add('all-map-overrides', ['default'], {}, mapOverrides); add('all-alias-overrides', ['dark'], {}, aliasOverrides);
add('mixed-overrides', ['default'], { fontSize: 15, colorPrimary: '#286a91' }, { colorPrimary: '#d63674', colorFillSecondary: '#335577', colorFillContent: '#ab231e', lineHeight: 1.8, lineWidthFocus: 9, colorShadow: 'rgba(20,30,40,0.5)', motion: false, motionDurationFast: '0.7s' });
add('palette-overrides', ['dark'], {}, { blue1: '#112233', blueHover: '#abc123', pinkActive: '#ff1234' });
add('decimal-duration-rounding', ['default'], { motionBase: 0.25, motionUnit: 0.1 });
add('transparent-seeds', ['default'], { colorPrimary: '#1286ad80', colorBgBase: '#e1f5ea90', colorTextBase: '#15354d80' });
for (const radius of [0, 4.5, 5, 6, 7, 8, 14, 16]) add(`radius-${radius}`, ['default'], { borderRadius: radius });
const fixture = { provenance: { upstream: 'ant-design', version: '6.6.7', revision, packages: packageVersions, sources: Object.fromEntries([...inputs].sort(([left], [right]) => left.localeCompare(right))) }, cases };
const destination = path.resolve(path.dirname(fileURLToPath(import.meta.url)), '../fixtures/theme/reference.json');
fs.mkdirSync(path.dirname(destination), { recursive: true });
fs.writeFileSync(destination, `${JSON.stringify(fixture)}\n`);
console.log(`Generated ${cases.length} independent theme fixtures (${fs.statSync(destination).size} bytes) from upstream ${revision}.`);
