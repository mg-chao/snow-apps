// Copyright (C) 2026 mg-chao
// SPDX-License-Identifier: Apache-2.0
// Bundle the original pinned component, ConfigProvider and theme, independently of GPUI.
import fs from 'node:fs';
import path from 'node:path';
import {fileURLToPath} from 'node:url';
import {createRequire} from 'node:module';
import {execFileSync} from 'node:child_process';
import crypto from 'node:crypto';

const args=Object.fromEntries(process.argv.slice(2).reduce((out,value,i,all)=>{if(i%2===0)out.push([value.replace(/^--/,''),all[i+1]]);return out;},[]));
const root=path.resolve(path.dirname(fileURLToPath(import.meta.url)),'..');
const upstream=path.resolve(args.upstream??'E:/ant-design');
const output=path.resolve(args.output??path.join(root,'../build/ant-design-gpui/carousel-reference-current'));
const dependencies=path.resolve(args.dependencies??path.join(root,'../build/ant-design-gpui/modal-reference/browser'));
const revision=execFileSync('git',['-C',upstream,'rev-parse','HEAD'],{encoding:'utf8'}).trim();
if(revision!=='f8f5595ab5e891fff6a2a06c5b3b98af643d008e')throw Error(`Unexpected reference revision: ${revision}`);
const require=createRequire(path.join(dependencies,'package.json'));
const slickVersion=require('@ant-design/react-slick/package.json').version;
if(slickVersion!=='2.0.0')throw Error(`Unexpected Slick version: ${slickVersion}`);
const {build}=require('esbuild');
const digest=file=>crypto.createHash('sha256').update(fs.readFileSync(file)).digest('hex');
fs.mkdirSync(output,{recursive:true});
const version=JSON.parse(fs.readFileSync(path.join(upstream,'package.json'),'utf8')).version;
const versionModule=path.join(output,'version.js');
fs.writeFileSync(versionModule,`export default ${JSON.stringify(version)};\n`);
const result=await build({entryPoints:[path.join(root,'scripts/carousel_reference.tsx')],outfile:path.join(output,'reference.js'),
  bundle:true,metafile:true,platform:'browser',format:'iife',sourcemap:true,nodePaths:[path.join(dependencies,'node_modules')],
  alias:{'antd-reference':path.join(upstream,'components')},define:{'process.env.NODE_ENV':'"production"'},
  plugins:[{name:'upstream-version',setup(build){build.onResolve({filter:/^\.\/version$/},args=>args.resolveDir.replaceAll('\\','/').endsWith('/components/version')?{path:versionModule}:undefined);}}]});
fs.writeFileSync(path.join(output,'index.html'),'<!doctype html><html lang="en"><head><meta charset="UTF-8"><title>Original Ant Design Carousel reference</title></head><body style="margin:0"><div id="root"></div><script src="reference.js"></script></body></html>\n');
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
const licenseDirectory=path.join(root,'fixtures/carousel/licenses');
fs.mkdirSync(licenseDirectory,{recursive:true});
const packages=[];
for(const directory of [...packageRoots].sort()){
  const info=JSON.parse(fs.readFileSync(path.join(directory,'package.json'),'utf8'));
  const licenses=fs.readdirSync(directory).filter(name=>/^licen[cs]e(?:\..*)?$/i.test(name)).map(name=>path.join(directory,name));
  if(!licenses.length&&info.name==='@ant-design/icons-svg')licenses.push(path.join(root,'fixtures/message/licenses/ant-design-icons-svg.txt'));
  if(!licenses.length&&info.name==='is-mobile')licenses.push(path.join(root,'fixtures/message/licenses/is-mobile.txt'));
  if(!licenses.length)throw Error(`Missing original license: ${info.name}`);
  const files=licenses.map(file=>{
    const name=info.name.replaceAll('/','-').replace('@','')+'-'+path.basename(file);
    fs.copyFileSync(file,path.join(licenseDirectory,name));return{name,sha256:digest(file)};
  });
  packages.push({name:info.name,version:info.version,license:info.license,files});
}
fs.copyFileSync(path.join(upstream,'LICENSE'),path.join(licenseDirectory,'ant-design-LICENSE'));
const provenance={schema_version:1,upstream:{version,revision},slick_version:slickVersion,packages,
  generator_sha256:digest(fileURLToPath(import.meta.url)),lock_sha256:digest(path.join(dependencies,'package-lock.json')),
  inputs:Object.fromEntries(Object.keys(result.metafile.inputs).map(file=>[file.replaceAll('\\','/'),digest(file)]))};
fs.writeFileSync(path.join(root,'fixtures/carousel/reference-packages.json'),JSON.stringify({upstream:provenance.upstream,packages},null,2)+'\n');
fs.writeFileSync(path.join(output,'provenance.json'),JSON.stringify(provenance,null,2)+'\n');
console.log(`Bundled original Carousel ${version}, Slick ${slickVersion}: ${output}`);
