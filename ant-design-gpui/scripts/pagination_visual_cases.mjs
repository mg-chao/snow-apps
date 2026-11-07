// Copyright (C) 2026 mg-chao
// SPDX-License-Identifier: Apache-2.0
// Shared native/browser scenario inputs; all expected metrics come from original code.
import fs from 'node:fs';
const scenarios = [
  { kind: 'basic', total: 50, current: 1 },
  { kind: 'basic', current: 1 }, { kind: 'basic', current: 3 },
  { kind: 'basic', current: 6 }, { kind: 'basic', current: 50 },
  { kind: 'basic', total: 0, current: 1 }, { kind: 'basic', total: 1, current: 1 },
  { kind: 'basic', show_less: true }, { kind: 'basic', disabled: true },
  { kind: 'basic', size: 'small' }, { kind: 'basic', size: 'large' },
  { kind: 'simple' }, { kind: 'simple-readonly' }, { kind: 'simple', disabled: true },
  { kind: 'quick' }, { kind: 'quick-go' }, { kind: 'changer' }, { kind: 'changer', open: true },
  { kind: 'all' }, { kind: 'total', total: 85, page_size: 20, current: 1 },
  { kind: 'custom' }, { kind: 'custom-changer' }, { kind: 'semantic' },
  { kind: 'basic', wireframe: true }, { kind: 'basic', wireframe: true, disabled: true },
  { kind: 'basic', tokens: true }, { kind: 'all', tokens: true },
  { kind: 'basic', hover: 'page' }, { kind: 'basic', hover: 'active' },
  { kind: 'basic', hover: 'prev' }, { kind: 'basic', hover: 'jump' },
  { kind: 'basic', hover: 'page', pressed: true }, { kind: 'basic', focus: 'page' },
  { kind: 'quick', focus: 'quick' }, { kind: 'simple', focus: 'simple' },
];
const cases = [];
for (const theme of ['default', 'dark', 'compact', 'dark-compact']) {
  for (const direction of ['ltr', 'rtl']) {
    scenarios.forEach((scenario, index) => cases.push({ name: `${theme}-${direction}-${String(index).padStart(2, '0')}-${scenario.kind}`,
      theme, direction, ...scenario }));
  }
}
// Chromium's plain Go button has its own native form appearance. Drive real
// pointer/keyboard state rather than inferring its paint from computed CSS.
for (const theme of ['default', 'dark', 'compact', 'dark-compact']) {
  for (const direction of ['ltr', 'rtl']) {
    for (const [state, interaction] of [
      ['hover', { hover: 'go' }], ['pressed', { hover: 'go', pressed: true }],
      ['focus', { focus: 'go' }], ['space', { focus: 'go', held_key: 'space' }],
      ['enter', { focus: 'go', held_key: 'enter' }],
      ['disabled', { disabled: true }], ['disabled-hover', { disabled: true, hover: 'go' }],
      ['disabled-pressed', { disabled: true, hover: 'go', pressed: true }],
    ]) cases.push({ name: `go-${theme}-${direction}-${state}`, theme, direction, kind: 'quick-go', ...interaction });
  }
}
for (const direction of ['ltr', 'rtl']) {
  for (const align of ['start', 'center', 'end']) cases.push({ name: `align-${direction}-${align}`, theme: 'default', direction, kind: 'basic', align });
  for (const viewport_width of [480, 575, 576, 577, 800, 991, 992, 993]) cases.push({ name: `responsive-${direction}-${viewport_width}`,
    theme: 'default', direction, kind: 'all', viewport_width, responsive: true });
  for (const variant of ['filled', 'borderless', 'underlined']) cases.push({ name: `variant-${direction}-${variant}`,
    theme: 'default', direction, kind: 'quick', variant });
  for (const locale of ['zh_CN', 'zh_TW']) cases.push({ name: `locale-${direction}-${locale}`,
    theme: 'default', direction, kind: 'all', locale });
}
for (const theme of ['default', 'dark', 'compact', 'dark-compact']) {
  for (const direction of ['ltr', 'rtl']) {
    cases.push({ name: `simple-auto-${theme}-${direction}`, theme, direction, kind: 'simple', show_size_changer: 'auto' });
    cases.push({ name: `all-disabled-${theme}-${direction}`, theme, direction, kind: 'all', disabled: true });
    for (const size of ['small', 'large']) cases.push({ name: `all-${size}-${theme}-${direction}`, theme, direction, kind: 'all', size });
    cases.push({ name: `long-pages-${theme}-${direction}`, theme, direction, kind: 'basic',
      total: 9007199254740991, current: 12345678, page_size: 10, show_size_changer: false });
  }
}
const output = process.argv[2];
if (!output) throw Error('Usage: node pagination_visual_cases.mjs <output.json> [--smoke]');
fs.writeFileSync(output, JSON.stringify(process.argv.includes('--smoke') ? cases.filter((_, index) => index < 35) : cases, null, 2) + '\n');
console.log(`Wrote ${process.argv.includes('--smoke') ? 35 : cases.length} Pagination visual scenarios`);
