// Copyright (C) 2026 mg-chao
// SPDX-License-Identifier: Apache-2.0
import fs from 'node:fs';
import path from 'node:path';
import crypto from 'node:crypto';
import { fileURLToPath } from 'node:url';
const root = path.resolve(path.dirname(fileURLToPath(import.meta.url)), '..');
const scales = [100, 150, 200].map(scale => {
  const filename = path.join(root, `../build/ant-design-gpui/pagination-reference/text-${scale}/text-metrics.json`);
  const contents = fs.readFileSync(filename);
  const evidence = JSON.parse(contents);
  if (evidence.cases.length !== 68 || evidence.scale !== scale / 100 || evidence.cases.some(c => !c.text.length)) {
    throw Error(`Incomplete original text evidence at ${scale}%`);
  }
  return { ...evidence, evidence_sha256: crypto.createHash('sha256').update(contents).digest('hex') };
});
const metadata = { description: 'Original browser DOMRange line counts and logical-character positions; native pixel line bands and paragraph shaping are checked separately.', scales };
// Keep each case on one line so changes remain reviewable without huge diffs.
let contents = JSON.stringify(metadata);
contents = contents.replace(/\{"scenario":/g, '\n{"scenario":');
fs.writeFileSync(path.join(root, 'fixtures/pagination/text-reference.json'), contents + '\n');
console.log(`Packed ${scales.reduce((count, scale) => count + scale.cases.length, 0)} original line/bidi records`);
