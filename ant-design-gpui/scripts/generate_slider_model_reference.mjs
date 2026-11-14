// Copyright (C) 2026 mg-chao
// SPDX-License-Identifier: Apache-2.0
// Execute the original @rc-component/slider numeric hooks without a browser.
// React's memo hooks are identity adapters; numeric behavior is unmodified.
// Usage: node scripts/generate_slider_model_reference.mjs --package <extracted-package>
import fs from 'node:fs';
import path from 'node:path';
import crypto from 'node:crypto';
import { fileURLToPath } from 'node:url';

const args = Object.fromEntries(process.argv.slice(2).reduce((pairs, value, index, all) => {
  if (index % 2 === 0) pairs.push([value.replace(/^--/, ''), all[index + 1]]);
  return pairs;
}, []));
if (!args.package) throw new Error('Supply --package pointing to the extracted @rc-component/slider 1.1.1 reference.');
const directory = path.resolve(args.package);
const metadata = JSON.parse(fs.readFileSync(path.join(directory, 'package.json'), 'utf8'));
if (metadata.name !== '@rc-component/slider' || metadata.version !== '1.1.1') {
  throw new Error('The numeric reference must use @rc-component/slider 1.1.1.');
}
const source = fs.readFileSync(path.join(directory, 'es/hooks/useOffset.js'), 'utf8');
const adapted = source.replace(/import \* as React from 'react';/, '')
  .replaceAll('export const ', 'const ').replace('export default function ', 'function ');
const { useOffset, getClosestEnabledHandleIndex } = new Function('React', `${adapted}\nreturn { useOffset, getClosestEnabledHandleIndex };`)(
  { useCallback: callback => callback },
);
const scales = [
  { name: 'default', min: 0, max: 100, step: 1, marks: [], inputs: [-5, 0, 0.49, 0.5, 20.5, 99.8, 100, 110] },
  { name: 'decimal', min: -1, max: 1, step: 0.1, marks: [0.35], inputs: [-1.1, -0.85, -0.3, 0.25, 0.3, 0.349, 0.35, 0.375, 1.1] },
  { name: 'marks-only', min: 0, max: 100, step: null, marks: [26, 37, 100], inputs: [0, 13, 20, 26, 30, 31.5, 50, 68.5] },
  { name: 'between-marks', min: 0, max: 100, step: null, marks: [25, 75], inputs: [0, 12.5, 25, 50, 75, 87.5, 100] },
  { name: 'unaligned-endpoint', min: 5, max: 10, step: 2, marks: [8], inputs: [4, 5, 6, 7.5, 8.5, 9.5, 10, 11] },
];
const cases = [];
for (const scale of scales) {
  const [snap, offset] = useOffset(scale.min, scale.max, scale.step, scale.marks.map(value => ({ value })), true, false, () => false);
  for (const input of scale.inputs) {
    cases.push({ scale: scale.name, kind: 'snap', input, expected: snap(input) });
    const value = snap(input);
    for (const units of [-2, -1, 1, 2]) {
      cases.push({ scale: scale.name, kind: 'offset', input: value, units, expected: offset([value], units, 0).value });
    }
  }
  for (const origin of [scale.min, (scale.min + scale.max) / 2, scale.max].map(snap)) {
    for (const target of scale.inputs.filter(value => value >= scale.min && value <= scale.max)) {
      const result = offset([origin], target - origin, 0, 'dist');
      cases.push({ scale: scale.name, kind: 'drag', input: origin, target, expected: result.value });
    }
  }
}
const anchors = [];
for (const disabled of [[false, false, false], [true, false, true], [false, true, false]]) {
  const values = [20, 50, 80];
  for (const target of [0, 20, 35, 50, 65, 80, 100]) {
    anchors.push({ values, disabled, target, expected: getClosestEnabledHandleIndex(values, target, 0, 100, false, index => disabled[index]) });
  }
}
const crate = path.resolve(path.dirname(fileURLToPath(import.meta.url)), '..');
const output = path.resolve(args.output ?? path.join(crate, 'fixtures/slider/model.json'));
fs.mkdirSync(path.dirname(output), { recursive: true });
fs.writeFileSync(output, `${JSON.stringify({
  provenance: {
    antDesignRevision: 'f8f5595ab5e891fff6a2a06c5b3b98af643d008e',
    package: metadata.name, version: metadata.version,
    sourceSha256: crypto.createHash('sha256').update(source).digest('hex'),
    generatorSha256: crypto.createHash('sha256').update(fs.readFileSync(fileURLToPath(import.meta.url))).digest('hex'),
    method: 'Original useOffset.js with React.useCallback identity adapter; no Rust inputs or outputs',
  }, scales, cases, anchors,
}, null, 2)}\n`);
console.log(`Recorded ${cases.length} scale cases and ${anchors.length} disabled-anchor cases in ${output}`);
