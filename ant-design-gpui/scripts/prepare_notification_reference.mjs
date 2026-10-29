// Copyright (C) 2026 mg-chao
// SPDX-License-Identifier: Apache-2.0
// Build the actual pinned upstream component with existing locked developer packages.
import fs from 'node:fs';
import path from 'node:path';
import {fileURLToPath} from 'node:url';
import {execFileSync} from 'node:child_process';
import {createRequire} from 'node:module';
import crypto from 'node:crypto';

const args = Object.fromEntries(process.argv.slice(2).reduce((out,v,i,all)=>{if(i%2===0)out.push([v.replace(/^--/,''),all[i+1]]);return out;},[]));
const root=path.resolve(path.dirname(fileURLToPath(import.meta.url)),'..');
const upstream=path.resolve(args.upstream??'E:/ant-design');
const output=path.resolve(args.output??path.join(root,'../build/ant-design-gpui/notification-reference'));
const dependencies=path.resolve(args.dependencies??path.join(root,'../build/ant-design-gpui/modal-reference/browser'));
const revision=execFileSync('git',['-C',upstream,'rev-parse','HEAD'],{encoding:'utf8'}).trim();
if(revision!=='f8f5595ab5e891fff6a2a06c5b3b98af643d008e')throw Error(`Unexpected reference revision ${revision}`);
const require=createRequire(path.join(dependencies,'package.json'));
const notificationVersion=require('@rc-component/notification/package.json').version;
if(notificationVersion!=='2.1.1')throw Error(`Unexpected notification dependency ${notificationVersion}`);
const {build}=require('esbuild');
fs.mkdirSync(output,{recursive:true});
const version=JSON.parse(fs.readFileSync(path.join(upstream,'package.json'),'utf8')).version;
const versionModule=path.join(output,'version.js');
fs.writeFileSync(versionModule,`export default ${JSON.stringify(version)};\n`);
const result=await build({entryPoints:[path.join(root,'scripts/notification_reference.tsx')],outfile:path.join(output,'reference.js'),
  bundle:true,metafile:true,platform:'browser',format:'iife',sourcemap:true,nodePaths:[path.join(dependencies,'node_modules')],
  alias:{'antd-reference':path.join(upstream,'components')},define:{'process.env.NODE_ENV':'"production"'},
  plugins:[{name:'upstream-version',setup(build){build.onResolve({filter:/^\.\/version$/},args=>args.resolveDir.replaceAll('\\','/').endsWith('/components/version')?{path:versionModule}:undefined);}}]});
fs.writeFileSync(path.join(output,'index.html'),'<!doctype html><html><head><meta charset="UTF-8"><title>Notification reference</title><link rel="stylesheet" href="reference.css"></head><body style="margin:0"><div id="root"></div><script src="reference.js"></script></body></html>\n');
const digest=file=>crypto.createHash('sha256').update(fs.readFileSync(file)).digest('hex');
const licenseDirectory=path.join(root,'fixtures/notification/licenses');
fs.mkdirSync(licenseDirectory,{recursive:true});
const packageRoots=new Set();
for(const input of Object.keys(result.metafile.inputs)){
  if(!input.replaceAll('\\','/').includes('/node_modules/'))continue;
  let directory=path.dirname(path.resolve(input));
  while(directory!==path.dirname(directory)){
    if(fs.existsSync(path.join(directory,'package.json'))&&JSON.parse(fs.readFileSync(path.join(directory,'package.json'),'utf8')).name){packageRoots.add(directory);break;}
    directory=path.dirname(directory);
  }
}
const packages=[];
for(const directory of [...packageRoots].sort()){
  const info=JSON.parse(fs.readFileSync(path.join(directory,'package.json'),'utf8'));
  const licenses=fs.readdirSync(directory).filter(name=>/^licen[cs]e(?:\..*)?$/i.test(name)).map(name=>path.join(directory,name));
  // icons-svg's npm files allowlist omits the monorepo MIT license; use the
  // original license already retained by the repository's Message fixture.
  if(!licenses.length&&info.name==='@ant-design/icons-svg')licenses.push(path.join(root,'fixtures/message/licenses/ant-design-icons-svg.txt'));
  if(!licenses.length&&info.name==='is-mobile')licenses.push(path.join(root,'fixtures/message/licenses/is-mobile.txt'));
  if(!licenses.length)throw Error(`No original license for ${info.name}`);
  const files=licenses.map(file=>{
    const destination=info.name.replaceAll('/','-').replace('@','')+'-'+path.basename(file);
    fs.copyFileSync(file,path.join(licenseDirectory,destination));
    return{name:destination,sha256:digest(file)};
  });
  packages.push({name:info.name,version:info.version,license:info.license,files});
}
fs.copyFileSync(path.join(upstream,'LICENSE'),path.join(licenseDirectory,'ant-design-LICENSE'));
fs.writeFileSync(path.join(root,'fixtures/notification/reference-packages.json'),JSON.stringify({revision,version,packages},null,2)+'\n');
fs.writeFileSync(path.join(output,'provenance.json'),JSON.stringify({revision,version,notificationVersion,
  packages,lock_sha256:digest(path.join(dependencies,'package-lock.json')),generator_sha256:digest(fileURLToPath(import.meta.url)),
  inputs:Object.fromEntries(Object.keys(result.metafile.inputs).map(file=>[file.replaceAll('\\','/'),digest(file)]))},null,2)+'\n');
console.log(`Built original Notification ${version} at ${output}`);
