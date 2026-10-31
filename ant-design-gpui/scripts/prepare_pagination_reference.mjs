// Copyright (C) 2026 mg-chao
// SPDX-License-Identifier: Apache-2.0
// Uses existing local dependencies only; no package installation or downloads.
import fs from 'node:fs';
import path from 'node:path';
import crypto from 'node:crypto';
import { createRequire } from 'node:module';
import { execFileSync } from 'node:child_process';
import { fileURLToPath } from 'node:url';
const args = Object.fromEntries(process.argv.slice(2).reduce((out, value, index, all) => {
  if (index % 2 === 0) out.push([value.replace(/^--/, ''), all[index + 1]]);
  return out;
}, []));
const root = path.resolve(path.dirname(fileURLToPath(import.meta.url)), '..');
const upstream = path.resolve(args.upstream ?? 'E:/ant-design');
const dependencies = path.resolve(args.dependencies ?? path.join(root, '../build/ant-design-gpui/select-slider-reference/browser'));
const output = path.resolve(args.output ?? path.join(root, '../build/ant-design-gpui/pagination-reference/browser'));
const revision = execFileSync('git', ['-C', upstream, 'rev-parse', 'HEAD'], { encoding: 'utf8' }).trim();
if (revision !== 'f8f5595ab5e891fff6a2a06c5b3b98af643d008e') throw Error(`Unexpected reference revision ${revision}`);
const require = createRequire(path.join(dependencies, 'package.json'));
if (JSON.parse(fs.readFileSync(path.join(dependencies, 'node_modules/@rc-component/pagination/package.json'), 'utf8')).version !== '1.4.0') throw Error('Expected @rc-component/pagination 1.4.0');
const { build } = require('esbuild');
fs.mkdirSync(output, { recursive: true });
const sourcePackage = JSON.parse(fs.readFileSync(path.join(upstream, 'package.json'), 'utf8'));
const versionModule = path.join(output, 'version.js');
fs.writeFileSync(versionModule, `export default ${JSON.stringify(sourcePackage.version)};\n`);
const result = await build({
  entryPoints: [path.join(root, 'scripts/pagination_reference.tsx')],
  outfile: path.join(output, 'reference.js'), bundle: true, metafile: true,
  platform: 'browser', format: 'iife', nodePaths: [path.join(dependencies, 'node_modules')],
  alias: { 'antd-reference': path.join(upstream, 'components') },
  define: { 'process.env.NODE_ENV': '"production"' },
  plugins: [{ name: 'generated-upstream-version', setup(build) {
    build.onResolve({ filter: /^\.\/version$/ }, args =>
      args.resolveDir.replaceAll('\\', '/').endsWith('/components/version') ? { path: versionModule } : undefined);
  } }],
});
fs.writeFileSync(path.join(output, 'index.html'), '<!doctype html><html><head><meta charset="UTF-8"><title>Pagination reference</title></head><body style="margin:0"><div id="root"></div><script src="reference.js"></script></body></html>\n');
const digest = file => crypto.createHash('sha256').update(fs.readFileSync(file)).digest('hex');
const lockPath = path.join(dependencies, 'package-lock.json');
const lock = JSON.parse(fs.readFileSync(lockPath, 'utf8'));
const inputs = Object.fromEntries(Object.keys(result.metafile.inputs).map(file => [file.replaceAll('\\', '/'), digest(file)]));
fs.writeFileSync(path.join(output, 'provenance.json'), JSON.stringify({ revision, version: sourcePackage.version,
  lock_sha256: digest(lockPath), packages: Object.fromEntries(Object.entries(lock.packages).filter(([name]) => name)
    .map(([name, value]) => [name, { version: value.version, integrity: value.integrity }])), inputs }, null, 2) + '\n');
console.log(`Built original Pagination ${sourcePackage.version}, rc-pagination 1.4.0 in ${output}`);
