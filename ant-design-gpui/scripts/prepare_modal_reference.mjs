// Copyright (C) 2026 mg-chao
// SPDX-License-Identifier: Apache-2.0
// Install developer-only reference dependencies under the ignored build tree.
// The renderer imports the actual checked-out Ant Design components, not a CSS facsimile.
import fs from 'node:fs';
import path from 'node:path';
import { fileURLToPath } from 'node:url';
import { execFileSync } from 'node:child_process';
import { createRequire } from 'node:module';
import crypto from 'node:crypto';

const args = Object.fromEntries(process.argv.slice(2).reduce((out, value, index, all) => {
  if (index % 2 === 0) out.push([value.replace(/^--/, ''), all[index + 1]]);
  return out;
}, []));
const root = path.resolve(path.dirname(fileURLToPath(import.meta.url)), '..');
const upstream = path.resolve(args.upstream ?? 'E:/ant-design');
const output = path.resolve(args.output ?? path.join(root, '../build/ant-design-gpui/modal-reference/browser'));
const revision = execFileSync('git', ['-C', upstream, 'rev-parse', 'HEAD'], { encoding: 'utf8' }).trim();
if (revision !== 'f8f5595ab5e891fff6a2a06c5b3b98af643d008e') throw Error(`Unexpected reference revision ${revision}`);
fs.mkdirSync(output, { recursive: true });
const sourcePackage = JSON.parse(fs.readFileSync(path.join(upstream, 'package.json'), 'utf8'));
// Reuse the repository's pinned developer-only reference dependency lock.
const referenceLock = JSON.parse(fs.readFileSync(path.join(root, 'fixtures/select/browser-package-lock.json'), 'utf8'));
const dependencies = referenceLock.packages[''].dependencies;
fs.writeFileSync(path.join(output, 'package.json'), JSON.stringify({ name: referenceLock.name, private: true, type: 'module', dependencies }, null, 2) + '\n');
fs.copyFileSync(path.join(root, 'fixtures/select/browser-package-lock.json'), path.join(output, 'package-lock.json'));
if (args.install === 'true') {
  // Preserve the lock after its first resolution. npm is a cmd shim on Windows.
  const command = fs.existsSync(path.join(output, 'package-lock.json')) ? 'ci' : 'install';
  execFileSync(process.platform === 'win32' ? 'cmd.exe' : 'npm', process.platform === 'win32'
    ? ['/d', '/s', '/c', `npm.cmd ${command} --ignore-scripts --no-audit --no-fund`]
    : [command, '--ignore-scripts', '--no-audit', '--no-fund'], { cwd: output, stdio: 'inherit' });
}
const require = createRequire(path.join(output, 'package.json'));
const { build } = require('esbuild');
const versionModule = path.join(output, 'version.js');
fs.writeFileSync(versionModule, `export default ${JSON.stringify(sourcePackage.version)};\n`);
const result = await build({
  entryPoints: [path.join(root, 'scripts/modal_reference.tsx')],
  outfile: path.join(output, 'reference.js'), bundle: true, metafile: true,
  platform: 'browser', format: 'iife', sourcemap: true,
  nodePaths: [path.join(output, 'node_modules')],
  alias: { 'antd-reference': path.join(upstream, 'components') },
  define: { 'process.env.NODE_ENV': '"production"' },
  plugins: [{ name: 'generated-upstream-version', setup(build) {
    build.onResolve({ filter: /^\.\/version$/ }, args =>
      args.resolveDir.replaceAll('\\', '/').endsWith('/components/version')
        ? { path: versionModule } : undefined);
  } }],
});
fs.writeFileSync(path.join(output, 'index.html'), '<!doctype html><html><head><meta charset="UTF-8"><title>Modal reference</title><link rel="stylesheet" href="reference.css"></head><body style="margin:0"><div id="root"></div><script src="reference.js"></script></body></html>\n');
const digest = file => crypto.createHash('sha256').update(fs.readFileSync(file)).digest('hex');
const inputs = Object.fromEntries(Object.keys(result.metafile.inputs).map(file => [file.replaceAll('\\', '/'), digest(file)]));
const lock = JSON.parse(fs.readFileSync(path.join(output, 'package-lock.json'), 'utf8'));
fs.writeFileSync(path.join(output, 'provenance.json'), JSON.stringify({ revision, version: sourcePackage.version, lock_sha256: digest(path.join(output, 'package-lock.json')), packages: lock.packages, inputs }, null, 2) + '\n');
console.log(`Built executable Ant Design reference in ${output}`);
