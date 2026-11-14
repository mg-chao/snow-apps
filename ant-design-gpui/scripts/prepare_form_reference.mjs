// Copyright (C) 2026 mg-chao
// SPDX-License-Identifier: Apache-2.0
// Bundles unmodified original Form and records every input source hash.
import fs from 'node:fs';
import path from 'node:path';
import {fileURLToPath} from 'node:url';
import {createRequire} from 'node:module';
import {execFileSync} from 'node:child_process';
import crypto from 'node:crypto';

const args = Object.fromEntries(process.argv.slice(2).reduce((all, value, index, values) => {
  if (index % 2 === 0) all.push([value.replace(/^--/, ''), values[index + 1]]);return all;
}, []));
const root = path.resolve(path.dirname(fileURLToPath(import.meta.url)), '..');
const upstream = path.resolve(args.upstream ?? 'E:/ant-design');
const dependencies = path.resolve(args.dependencies ?? path.join(root, '../build/ant-design-gpui/modal-reference/browser'));
const output = path.resolve(args.output ?? path.join(root, '../build/ant-design-gpui/form-reference'));
const revision = execFileSync('git', ['-C', upstream, 'rev-parse', 'HEAD'], {encoding:'utf8'}).trim();
if (revision !== 'f8f5595ab5e891fff6a2a06c5b3b98af643d008e') throw Error(`Unexpected reference revision ${revision}`);
const digest = file => crypto.createHash('sha256').update(fs.readFileSync(file)).digest('hex');
const lock = path.join(dependencies, 'package-lock.json');
if (digest(lock) !== digest(path.join(root, 'fixtures/select/browser-package-lock.json'))) throw Error('Reference dependency lock differs');
const require = createRequire(path.join(dependencies, 'package.json'));
const rcForm = JSON.parse(fs.readFileSync(path.join(dependencies,'node_modules/@rc-component/form/package.json'),'utf8'));
if (rcForm.version !== '1.8.6') throw Error(`Unexpected rc Form ${rcForm.version}`);
const {build} = require('esbuild');
fs.mkdirSync(output, {recursive:true});
const version = JSON.parse(fs.readFileSync(path.join(upstream,'package.json'),'utf8')).version;
fs.writeFileSync(path.join(output,'version.js'),`export default ${JSON.stringify(version)};\n`);
const inputs = {};
const casesPath=path.resolve(args.cases??path.join(root,'fixtures/form/visual-cases.json'));
for (const entry of ['form_reference', 'form_behavior_reference']) {
  const source = path.join(root, 'scripts', `${entry}.tsx`);
  if (!fs.existsSync(source)) continue;
  const result = await build({entryPoints:[source],outfile:path.join(output,`${entry}.js`),bundle:true,metafile:true,
    platform:'browser',format:'iife',nodePaths:[path.join(dependencies,'node_modules')],
    alias:{'antd-reference':path.join(upstream,'components')},define:{'process.env.NODE_ENV':'"production"'},
    plugins:[{name:'fixture-cases',setup(builder){builder.onResolve({filter:/visual-cases\.json$/},()=>({path:casesPath}));}},
      {name:'upstream-version',setup(builder){builder.onResolve({filter:/^\.\/version$/},info =>
      info.resolveDir.replaceAll('\\','/').endsWith('/components/version') ? {path:path.join(output,'version.js')} : undefined);}}]});
  Object.assign(inputs,Object.fromEntries(Object.keys(result.metafile.inputs).map(file => [file.replaceAll('\\','/'),digest(file)])));
  const document = entry === 'form_reference' ? 'index.html' : 'behavior.html';
  fs.writeFileSync(path.join(output,document),`<!doctype html><html><head><meta charset="UTF-8"><title>Form reference</title></head><body style="margin:0"><div id="root"></div><script src="${entry}.js"></script></body></html>\n`);
}
const packages = new Map();
for (const file of Object.keys(inputs)) {
  if (!file.includes('/node_modules/')) continue;
  let directory = path.dirname(path.resolve(file));
  while (directory !== path.dirname(directory)) {
    const manifest = path.join(directory,'package.json');
    if (fs.existsSync(manifest)) {
      const info = JSON.parse(fs.readFileSync(manifest,'utf8'));
      if (info.name) {packages.set(directory,info);break;}
    }
    directory = path.dirname(directory);
  }
}
const licenseDirectory = path.join(root,'fixtures/form/licenses');fs.mkdirSync(licenseDirectory,{recursive:true});
const packageRecords = [];
for (const [directory,info] of [...packages].sort(([a],[b])=>a.localeCompare(b))) {
  const originals = fs.readdirSync(directory).filter(name=>/^licen[cs]e(?:\..*)?$/i.test(name)).map(name=>path.join(directory,name));
  if (!originals.length && info.name === '@ant-design/icons-svg') originals.push(path.join(root,'fixtures/message/licenses/ant-design-icons-svg.txt'));
  if (!originals.length && info.name === 'is-mobile') originals.push(path.join(root,'fixtures/message/licenses/is-mobile.txt'));
  if (!originals.length) throw Error(`Missing original license: ${info.name}`);
  const files = originals.map(file=>{const name=info.name.replaceAll('/','-').replace('@','')+'-'+path.basename(file);
    fs.copyFileSync(file,path.join(licenseDirectory,name));return {name,sha256:digest(file)};});
  packageRecords.push({name:info.name,version:info.version,license:info.license,files});
}
fs.copyFileSync(path.join(upstream,'LICENSE'),path.join(licenseDirectory,'ant-design-LICENSE'));
const provenance = {revision,version,rc_form:rcForm.version,lock_sha256:digest(lock),
  generator_sha256:digest(fileURLToPath(import.meta.url)),cases_sha256:digest(casesPath),
  inputs,packages:packageRecords};
fs.writeFileSync(path.join(output,'provenance.json'),JSON.stringify(provenance,null,2)+'\n');
fs.writeFileSync(path.join(root,'fixtures/form/reference-packages.json'),JSON.stringify({revision,version,rc_form:rcForm.version,packages:packageRecords},null,2)+'\n');
console.log(`Built original Form ${version}, rc Form ${rcForm.version}, ${packageRecords.length} licensed packages at ${output}`);
