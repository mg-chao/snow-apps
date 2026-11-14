// Copyright (C) 2026 mg-chao
// SPDX-License-Identifier: Apache-2.0
import fs from 'node:fs';
import path from 'node:path';
import { fileURLToPath } from 'node:url';
import { execFileSync } from 'node:child_process';
const root = path.resolve(path.dirname(fileURLToPath(import.meta.url)), '..');
const built = path.resolve(process.argv[2] ?? path.join(root, '../build/ant-design-gpui/pagination-reference'));
const captures = path.resolve(process.argv[3] ?? path.join(built, 'visual-150'));
const browser = JSON.parse(fs.readFileSync(path.join(captures, 'browser-metrics.json'), 'utf8'));
const provenance = JSON.parse(fs.readFileSync(path.join(built, 'browser/provenance.json'), 'utf8'));
const paintPath = path.join(captures, 'painted-controls.json');
execFileSync('python', [path.join(root, 'scripts/pack_pagination_paint.py'), '--reference', captures, '--output', paintPath], { stdio: 'inherit' });
const paintedControls = JSON.parse(fs.readFileSync(paintPath, 'utf8'));
const fixture = { provenance: { ...provenance, renderer: browser.renderer,
  inputs: Object.fromEntries(Object.entries(provenance.inputs).filter(([name]) => name.includes('/components/pagination/') ||
    name.includes('/@rc-component/pagination/') || name.endsWith('pagination_reference.tsx'))) },
  cases: browser.cases.map(({ scenario, metrics }) => ({ scenario, metrics: { ...metrics,
    ...(paintedControls[scenario.name] ? { browser_painted_appearance: paintedControls[scenario.name] } : {}),
    elements: metrics.elements.filter(element => !['.ant-select-input', '.ant-select-suffix'].includes(element.selector)) } })) };
// Keep generated evidence compact while retaining readable provenance and one case per line.
const { cases, ...header } = fixture;
fs.writeFileSync(path.join(root, 'fixtures/pagination/visual-reference.json'),
  JSON.stringify(header, null, 2).slice(0, -1).trimEnd() + ',\n  "cases": [\n' +
  cases.map(value => `    ${JSON.stringify(value)}`).join(',\n') + '\n  ]\n}\n');
console.log(`Packed ${fixture.cases.length} source-rendered Pagination visual cases`);
