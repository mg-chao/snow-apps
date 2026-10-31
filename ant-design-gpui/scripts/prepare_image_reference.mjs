// Copyright (C) 2026 mg-chao
// SPDX-License-Identifier: Apache-2.0
// Bundle the actual pinned upstream component with a locked developer toolchain.
import fs from 'node:fs';
import path from 'node:path';
import crypto from 'node:crypto';
import { createRequire } from 'node:module';
import { fileURLToPath } from 'node:url';
import { execFileSync } from 'node:child_process';

const args = Object.fromEntries(process.argv.slice(2).reduce((out, value, index, all) => {
  if (index % 2 === 0) out.push([value.replace(/^--/, ''), all[index + 1]]);
  return out;
}, []));
const root = path.resolve(path.dirname(fileURLToPath(import.meta.url)), '..');
const upstream = path.resolve(args.upstream ?? 'E:/ant-design');
const output = path.resolve(args.output ?? path.join(root, '../build/ant-design-gpui/image-reference/browser'));
const dependencies = path.resolve(args.dependencies ?? path.join(root, '../build/ant-design-gpui/image-reference/tools'));
const revision = execFileSync('git', ['-C', upstream, 'rev-parse', 'HEAD'], { encoding: 'utf8' }).trim();
if (revision !== 'f8f5595ab5e891fff6a2a06c5b3b98af643d008e') throw Error(`Unexpected upstream revision ${revision}`);
const sourcePackage = JSON.parse(fs.readFileSync(path.join(upstream, 'package.json'), 'utf8'));
const digest = file => crypto.createHash('sha256').update(fs.readFileSync(file)).digest('hex');
const lockPath = path.join(root, 'fixtures/image/browser-package-lock.json');
const lock = JSON.parse(fs.readFileSync(lockPath, 'utf8'));
fs.mkdirSync(output, { recursive: true });
fs.mkdirSync(dependencies, { recursive: true });
if (args.install === 'true') {
  fs.writeFileSync(path.join(dependencies, 'package.json'), JSON.stringify({ name: lock.name, private: true, type: 'module', dependencies: lock.packages[''].dependencies }, null, 2) + '\n');
  fs.copyFileSync(lockPath, path.join(dependencies, 'package-lock.json'));
  const command = ['ci', '--ignore-scripts', '--no-audit', '--no-fund', ...(args.offline === 'true' ? ['--offline'] : [])];
  execFileSync(process.platform === 'win32' ? 'cmd.exe' : 'npm', process.platform === 'win32'
    ? ['/d', '/s', '/c', `npm.cmd ${command.join(' ')}`] : command,
  { cwd: dependencies, stdio: 'inherit' });
}
if (digest(path.join(dependencies, 'package-lock.json')) !== digest(lockPath)) throw Error('Reference dependency lock mismatch; use --install true');
const require = createRequire(path.join(dependencies, 'package.json'));
for (const [name, metadata] of Object.entries(lock.packages)) {
  if (!name || metadata.optional) continue;
  const packageFile = path.join(dependencies, name, 'package.json');
  if (!fs.existsSync(packageFile)) throw Error(`Missing locked dependency ${name}`);
  if (JSON.parse(fs.readFileSync(packageFile, 'utf8')).version !== metadata.version) throw Error(`Installed dependency version differs: ${name}`);
}
const imageVersion = require('@rc-component/image/package.json').version;
if (imageVersion !== '1.10.0') throw Error(`Unexpected image behavior reference ${imageVersion}`);
const { build } = require('esbuild');
const versionModule = path.join(output, 'version.js');
fs.writeFileSync(versionModule, `export default ${JSON.stringify(sourcePackage.version)};\n`);
const result = await build({
  entryPoints: [path.join(root, 'scripts/image_reference.tsx')],
  outfile: path.join(output, 'reference.js'), bundle: true, metafile: true,
  platform: 'browser', format: 'iife', sourcemap: true,
  nodePaths: [path.join(dependencies, 'node_modules')],
  alias: { 'antd-reference': path.join(upstream, 'components') },
  define: { 'process.env.NODE_ENV': '"production"' },
  plugins: [{ name: 'generated-upstream-version', setup(build) {
    build.onResolve({ filter: /^\.\/version$/ }, args =>
      args.resolveDir.replaceAll('\\', '/').endsWith('/components/version') ? { path: versionModule } : undefined);
  } }],
});
fs.writeFileSync(path.join(output, 'index.html'), '<!doctype html><html><head><meta charset="UTF-8"><title>Image reference</title><link rel="stylesheet" href="reference.css"></head><body style="margin:0"><div id="root"></div><script src="reference.js"></script></body></html>\n');
const inputs = Object.fromEntries(Object.keys(result.metafile.inputs).map(file => [file.replaceAll('\\', '/'), digest(file)]));
const packageDirectories = new Set();
for (const file of Object.keys(inputs)) {
  const marker = file.lastIndexOf('/node_modules/');
  if (marker < 0) continue;
  const tail = file.slice(marker + '/node_modules/'.length).split('/');
  const count = tail[0].startsWith('@') ? 2 : 1;
  packageDirectories.add(file.slice(0, marker + '/node_modules/'.length) + tail.slice(0, count).join('/'));
}
const licenseDirectory = path.join(root, 'fixtures/image/licenses');
fs.mkdirSync(licenseDirectory, { recursive: true });
const packages = [];
for (const directory of [...packageDirectories].sort()) {
  const metadata = JSON.parse(fs.readFileSync(path.join(directory, 'package.json'), 'utf8'));
  const files = [];
  for (const file of fs.readdirSync(directory).filter(file => /^(LICENSE|COPYING|NOTICE)(\.|$)/i.test(file))) {
    const filename = `${metadata.name.replace(/^@/, '').replaceAll('/', '-')}-${file}`;
    fs.copyFileSync(path.join(directory, file), path.join(licenseDirectory, filename));
    files.push({ name: filename, sha256: digest(path.join(directory, file)) });
  }
  packages.push({ name: metadata.name, version: metadata.version, license: metadata.license, files });
}
fs.copyFileSync(path.join(upstream, 'LICENSE'), path.join(licenseDirectory, 'ant-design-LICENSE'));
const provenance = { revision, version: sourcePackage.version, imageVersion, packages,
  lock_sha256: digest(lockPath), generator_sha256: digest(fileURLToPath(import.meta.url)), inputs,
  assets: JSON.parse(fs.readFileSync(path.join(root, 'resources/image/manifest.json'), 'utf8')) };
fs.writeFileSync(path.join(output, 'provenance.json'), JSON.stringify(provenance, null, 2) + '\n');
fs.writeFileSync(path.join(root, 'fixtures/image/reference-packages.json'), JSON.stringify({ lock_sha256: provenance.lock_sha256, packages }, null, 2) + '\n');
console.log(`Built actual Ant Design ${sourcePackage.version} Image + @rc-component/image ${imageVersion}: ${output}`);
