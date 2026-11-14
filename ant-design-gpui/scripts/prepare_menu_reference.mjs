// Copyright (C) 2026 mg-chao
// SPDX-License-Identifier: Apache-2.0
// Build the actual checked-out Menu using existing developer-only dependencies.
import fs from 'node:fs';
import path from 'node:path';
import { fileURLToPath } from 'node:url';
import { execFileSync } from 'node:child_process';
import { createRequire } from 'node:module';
import crypto from 'node:crypto';

const args=Object.fromEntries(process.argv.slice(2).reduce((out,value,index,all)=>{
  if(index%2===0)out.push([value.replace(/^--/,''),all[index+1]]);return out;
},[]));
const root=path.resolve(path.dirname(fileURLToPath(import.meta.url)),'..');
const upstream=path.resolve(args.upstream??'E:/ant-design');
const output=path.resolve(args.output??path.join(root,'../build/ant-design-gpui/menu-reference/browser'));
const dependencies=path.resolve(args.dependencies??path.join(root,'../build/ant-design-gpui/select-slider-reference/browser'));
const revision=execFileSync('git',['-C',upstream,'rev-parse','HEAD'],{encoding:'utf8'}).trim();
if(revision!=='f8f5595ab5e891fff6a2a06c5b3b98af643d008e')throw Error(`Unexpected reference revision ${revision}`);
fs.mkdirSync(output,{recursive:true});
const require=createRequire(path.join(dependencies,'package.json'));
const {build}=require('esbuild');
const sourcePackage=JSON.parse(fs.readFileSync(path.join(upstream,'package.json'),'utf8'));
const versionModule=path.join(output,'version.js');
fs.writeFileSync(versionModule,`export default ${JSON.stringify(sourcePackage.version)};\n`);
const result=await build({
  entryPoints:[path.join(root,'scripts/menu_reference.tsx')],outfile:path.join(output,'reference.js'),
  bundle:true,metafile:true,platform:'browser',format:'iife',sourcemap:true,
  nodePaths:[path.join(dependencies,'node_modules')],alias:{'antd-reference':path.join(upstream,'components')},
  define:{'process.env.NODE_ENV':'"production"'},plugins:[{name:'upstream-version',setup(build){
    build.onResolve({filter:/^\.\/version$/},args=>args.resolveDir.replaceAll('\\','/').endsWith('/components/version')?{path:versionModule}:undefined);
  }}],
});
fs.writeFileSync(path.join(output,'index.html'),'<!doctype html><html><head><meta charset="UTF-8"><title>Menu reference</title></head><body style="margin:0"><div id="root"></div><script src="reference.js"></script></body></html>\n');
const digest=file=>crypto.createHash('sha256').update(fs.readFileSync(file)).digest('hex');
const inputs=Object.fromEntries(Object.keys(result.metafile.inputs).map(file=>[file.replaceAll('\\','/'),digest(file)]));
const packageNames=['react','react-dom','@rc-component/menu','@rc-component/overflow','@rc-component/trigger','@ant-design/cssinjs'];
const packages=Object.fromEntries(packageNames.map(name=>{const file=path.join(dependencies,'node_modules',name,'package.json');return[name,{version:JSON.parse(fs.readFileSync(file,'utf8')).version,sha256:digest(file)}];}));
fs.writeFileSync(path.join(output,'provenance.json'),JSON.stringify({revision,version:sourcePackage.version,packages,inputs},null,2)+'\n');
console.log(`Built executable original Menu reference in ${output}`);
