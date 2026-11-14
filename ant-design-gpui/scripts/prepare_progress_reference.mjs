// Copyright (C) 2026 mg-chao
// SPDX-License-Identifier: Apache-2.0
// Executes the pinned upstream source, using the existing locked reference toolchain.
import fs from 'node:fs';
import path from 'node:path';
import { fileURLToPath } from 'node:url';
import { createRequire } from 'node:module';
import { execFileSync } from 'node:child_process';
import crypto from 'node:crypto';

const args = Object.fromEntries(process.argv.slice(2).reduce((a,v,i,all)=>{if(i%2===0)a.push([v.replace(/^--/,''),all[i+1]]);return a;},[]));
const root = path.resolve(path.dirname(fileURLToPath(import.meta.url)), '..');
const upstream = path.resolve(args.upstream ?? 'E:/ant-design');
const dependencies = path.resolve(args.dependencies ?? '../build/ant-design-gpui/select-slider-reference/browser');
const output = path.resolve(args.output ?? '../build/ant-design-gpui/progress-reference-current/browser');
const revision = execFileSync('git',['-C',upstream,'rev-parse','HEAD'],{encoding:'utf8'}).trim();
if (revision !== 'f8f5595ab5e891fff6a2a06c5b3b98af643d008e') throw Error(`Unexpected reference ${revision}`);
const hash = file => crypto.createHash('sha256').update(fs.readFileSync(file)).digest('hex');
if(hash(path.join(dependencies,'package-lock.json'))!==hash(path.join(root,'fixtures/select/browser-package-lock.json'))) throw Error('Reference dependency lock differs from the checked-in lock');
const require = createRequire(path.join(dependencies,'package.json'));
const rc = require('@rc-component/progress/package.json');
if(rc.version !== '1.0.3') throw Error(`Wrong rc-progress: ${rc.version}`);
const {build} = require('esbuild');
fs.mkdirSync(output,{recursive:true});
const version = JSON.parse(fs.readFileSync(path.join(upstream,'package.json'),'utf8')).version;
fs.writeFileSync(path.join(output,'version.js'),`export default ${JSON.stringify(version)};\n`);
const result = await build({entryPoints:[path.join(root,'scripts/progress_reference.tsx')],outfile:path.join(output,'reference.js'),bundle:true,metafile:true,
  platform:'browser',format:'iife',nodePaths:[path.join(dependencies,'node_modules')],alias:{'antd-reference':path.join(upstream,'components')},
  define:{'process.env.NODE_ENV':'"production"'},plugins:[{name:'version',setup(build){build.onResolve({filter:/^\.\/version$/},args=>args.resolveDir.replaceAll('\\','/').endsWith('/components/version')?{path:path.join(output,'version.js')}:undefined);}}]});
fs.writeFileSync(path.join(output,'index.html'),'<!doctype html><html><head><meta charset="UTF-8"><title>Progress reference</title></head><body style="margin:0"><div id="root"></div><script src="reference.js"></script></body></html>\n');
const lock=JSON.parse(fs.readFileSync(path.join(dependencies,'package-lock.json'),'utf8'));
const provenance={version,revision,rc_progress:rc.version,lock_sha256:hash(path.join(dependencies,'package-lock.json')),bundle_sha256:hash(path.join(output,'reference.js')),
  inputs:Object.fromEntries(Object.keys(result.metafile.inputs).map(file=>[file.replaceAll('\\','/'),hash(file)])),packages:lock.packages};
fs.writeFileSync(path.join(output,'provenance.json'),JSON.stringify(provenance,null,2)+'\n');
console.log(`Built original Progress ${version}, rc-progress ${rc.version}`);
