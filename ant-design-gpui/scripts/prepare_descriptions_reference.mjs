// Copyright (C) 2026 mg-chao
// SPDX-License-Identifier: Apache-2.0
// Bundle the genuine checked-out component and its CSS-in-JS styles.
import fs from 'node:fs';
import path from 'node:path';
import {fileURLToPath} from 'node:url';
import {createRequire} from 'node:module';
import {execFileSync} from 'node:child_process';
import crypto from 'node:crypto';

const args = Object.fromEntries(process.argv.slice(2).reduce((all, value, index, values) => {
  if (index % 2 === 0) all.push([value.replace(/^--/, ''), values[index + 1]]);
  return all;
}, []));
const root = path.resolve(path.dirname(fileURLToPath(import.meta.url)), '..');
const upstream = path.resolve(args.upstream ?? 'E:/ant-design');
const candidates = ['select-slider-reference/browser', 'modal-reference/browser'];
const dependencies = path.resolve(args.dependencies ?? candidates
  .map(name => path.join(root, '../build/ant-design-gpui', name))
  .find(dir => fs.existsSync(path.join(dir, 'node_modules/esbuild')))
  ?? path.join(root, '../build/ant-design-gpui/select-slider-reference/browser'));
const output = path.resolve(args.output ?? path.join(root, '../build/ant-design-gpui/descriptions-reference'));
const revision = execFileSync('git', ['-C', upstream, 'rev-parse', 'HEAD'], {encoding: 'utf8'}).trim();
if (revision !== 'f8f5595ab5e891fff6a2a06c5b3b98af643d008e') throw Error(`Unexpected reference revision ${revision}`);
const digest = file => crypto.createHash('sha256').update(fs.readFileSync(file)).digest('hex');
const lock = path.join(dependencies, 'package-lock.json');
if (digest(lock) !== digest(path.join(root, 'fixtures/select/browser-package-lock.json'))) {
  throw Error('Reference dependency lock differs from the checked-in lock');
}
const require = createRequire(path.join(dependencies, 'package.json'));
const {build} = require('esbuild');
fs.mkdirSync(output, {recursive: true});
const version = JSON.parse(fs.readFileSync(path.join(upstream, 'package.json'), 'utf8')).version;
const versionModule = path.join(output, 'version.js');
fs.writeFileSync(versionModule, `export default ${JSON.stringify(version)};\n`);
const result = await build({
  entryPoints: [path.join(root, 'scripts/descriptions_reference.tsx')],
  outfile: path.join(output, 'reference.js'), bundle: true, metafile: true,
  platform: 'browser', format: 'iife', nodePaths: [path.join(dependencies, 'node_modules')],
  alias: {'antd-reference': path.join(upstream, 'components')},
  define: {'process.env.NODE_ENV': '"production"'},
  plugins: [{name: 'upstream-version', setup(builder) {
    builder.onResolve({filter: /^\.\/version$/}, info =>
      info.resolveDir.replaceAll('\\', '/').endsWith('/components/version')
        ? {path: versionModule} : undefined);
  }}],
});
fs.writeFileSync(path.join(output, 'index.html'), '<!doctype html><html><head><meta charset="UTF-8"><title>Descriptions reference</title></head><body style="margin:0"><div id="root"></div><script src="reference.js"></script></body></html>\n');
fs.writeFileSync(path.join(output, 'provenance.json'), JSON.stringify({
  revision, version, lock_sha256: digest(lock),
  generator_sha256: digest(fileURLToPath(import.meta.url)),
  cases_sha256: digest(path.join(root, 'fixtures/descriptions/visual-cases.json')),
  inputs: Object.fromEntries(Object.keys(result.metafile.inputs)
    .map(file => [file.replaceAll('\\', '/'), digest(file)])),
}, null, 2) + '\n');
console.log(`Built original Descriptions ${version} at ${output}`);
