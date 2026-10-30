// Copyright (C) 2026 mg-chao
// SPDX-License-Identifier: Apache-2.0
// Bundles the original reference; JavaScript remains a developer-only dependency.
import fs from 'node:fs';
import path from 'node:path';
import {fileURLToPath} from 'node:url';
import {createRequire} from 'node:module';
import {execFileSync} from 'node:child_process';
import crypto from 'node:crypto';

const args=Object.fromEntries(process.argv.slice(2).reduce((a,v,i,all)=>{if(i%2===0)a.push([v.replace(/^--/,''),all[i+1]]);return a;},[]));
const root=path.resolve(path.dirname(fileURLToPath(import.meta.url)),'..');
const upstream=path.resolve(args.upstream??'E:/ant-design');
const dependencies=path.resolve(args.dependencies??'../build/ant-design-gpui/modal-reference/browser');
const output=path.resolve(args.output??'../build/ant-design-gpui/date-time-picker-reference/browser');
const revision=execFileSync('git',['-C',upstream,'rev-parse','HEAD'],{encoding:'utf8'}).trim();
if(revision!=='f8f5595ab5e891fff6a2a06c5b3b98af643d008e')throw Error(`Unexpected reference revision ${revision}`);
const digest=file=>crypto.createHash('sha256').update(fs.readFileSync(file)).digest('hex');
const lockPath=path.join(root,'fixtures/select/browser-package-lock.json');
if(digest(path.join(dependencies,'package-lock.json'))!==digest(lockPath))throw Error('Reference dependency lock differs from checked-in lock');
const require=createRequire(path.join(dependencies,'package.json'));
const picker=JSON.parse(fs.readFileSync(path.join(dependencies,'node_modules/@rc-component/picker/package.json'),'utf8'));
if(picker.version!=='1.12.2')throw Error(`Wrong rc-picker ${picker.version}`);
const {build}=require('esbuild');
fs.mkdirSync(output,{recursive:true});
const version=JSON.parse(fs.readFileSync(path.join(upstream,'package.json'),'utf8')).version;
fs.writeFileSync(path.join(output,'version.js'),`export default ${JSON.stringify(version)};\n`);
const result=await build({entryPoints:[path.join(root,'scripts/date_time_picker_reference.tsx')],outfile:path.join(output,'reference.js'),bundle:true,metafile:true,
  platform:'browser',format:'iife',nodePaths:[path.join(dependencies,'node_modules')],alias:{'antd-reference':path.join(upstream,'components')},
  define:{'process.env.NODE_ENV':'"production"'},plugins:[{name:'upstream-version',setup(build){build.onResolve({filter:/^\.\/version$/},a=>a.resolveDir.replaceAll('\\','/').endsWith('/components/version')?{path:path.join(output,'version.js')}:undefined);}}]});
fs.writeFileSync(path.join(output,'index.html'),'<!doctype html><html><head><meta charset="UTF-8"><title>Date/time picker reference</title></head><body style="margin:0"><div id="root"></div><script src="reference.js"></script></body></html>\n');
const packageRoots=new Set();
for(const input of Object.keys(result.metafile.inputs)){
  if(!input.replaceAll('\\','/').includes('/node_modules/'))continue;
  let directory=path.dirname(path.resolve(input));
  while(directory!==path.dirname(directory)){
    const file=path.join(directory,'package.json');
    if(fs.existsSync(file)&&JSON.parse(fs.readFileSync(file,'utf8')).name){packageRoots.add(directory);break;}
    directory=path.dirname(directory);
  }
}
const licenseDirectory=path.join(root,'fixtures/date_time_picker/licenses');
fs.mkdirSync(licenseDirectory,{recursive:true});
const loadedPackages=[];
for(const directory of [...packageRoots].sort()){
  const info=JSON.parse(fs.readFileSync(path.join(directory,'package.json'),'utf8'));
  const licenses=fs.readdirSync(directory).filter(name=>/^licen[cs]e(?:\..*)?$/i.test(name)).map(name=>path.join(directory,name));
  if(!licenses.length&&info.name==='@ant-design/icons-svg')licenses.push(path.join(root,'fixtures/message/licenses/ant-design-icons-svg.txt'));
  if(!licenses.length&&info.name==='is-mobile')licenses.push(path.join(root,'fixtures/message/licenses/is-mobile.txt'));
  if(!licenses.length)throw Error(`Missing original license: ${info.name}`);
  const files=licenses.map(file=>{const name=info.name.replaceAll('/','-').replace('@','')+'-'+path.basename(file);
    fs.copyFileSync(file,path.join(licenseDirectory,name));return{name,sha256:digest(file)};});
  loadedPackages.push({name:info.name,version:info.version,license:info.license,files});
}
fs.copyFileSync(path.join(upstream,'LICENSE'),path.join(licenseDirectory,'ant-design-LICENSE'));
const provenance={version,revision,rc_picker:picker.version,generator_sha256:digest(fileURLToPath(import.meta.url)),
  scene_sha256:digest(path.join(root,'fixtures/date_time_picker/visual-cases.json')),lock_sha256:digest(lockPath),
  bundle_sha256:digest(path.join(output,'reference.js')),inputs:Object.fromEntries(Object.keys(result.metafile.inputs)
    .map(f=>[f.replaceAll('\\','/'),digest(f)])),loaded_packages:loadedPackages};
fs.writeFileSync(path.join(output,'provenance.json'),JSON.stringify(provenance,null,2)+'\n');
fs.writeFileSync(path.join(root,'fixtures/date_time_picker/reference-packages.json'),JSON.stringify({version,revision,rc_picker:picker.version,packages:loadedPackages},null,2)+'\n');
console.log(`Built actual Ant Design ${version} DatePicker/TimePicker with rc-picker ${picker.version}`);
