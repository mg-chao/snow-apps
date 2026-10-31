// Copyright (C) 2026 mg-chao
// SPDX-License-Identifier: Apache-2.0
import fs from 'node:fs';
let scenarios = [
  {component:'select',kind:'basic',open:true},
  {component:'select',kind:'multiple',open:true},
  {component:'select',kind:'empty',open:true},
  {component:'select',kind:'disabled'},
  {component:'slider',kind:'basic',open:true},
  {component:'slider',kind:'marks'},
  {component:'slider',kind:'vertical'},
  {component:'slider',kind:'range'},
  {component:'slider',kind:'disabled-handle'},
];
if (process.argv.includes('--detail') || process.argv.includes('--all')) {
  const detail = [
    ...['basic','multiple'].map(kind => ({component:'select',kind,open:true,tokens:true})),
    ...['bottomLeft','bottomRight','topLeft','topRight'].map(placement => ({component:'select',kind:'basic',open:true,placement,anchor_x:100,anchor_y:220,popup_width:placement.endsWith('Right')?360:200})),
    ...['outlined','filled','borderless','underlined'].flatMap(variant => ['error','warning'].map(status => ({component:'select',kind:'basic',variant,status,focus:true}))),
    {component:'select',kind:'custom',open:true},
    ...['basic','marks','vertical','range'].map(kind => ({component:'slider',kind,tokens:true})),
    {component:'slider',kind:'basic',focus:true},
  ];
  if (process.argv.includes('--all')) scenarios.push(...detail);
  else scenarios = detail;
}
if (process.argv.includes('--extended') || process.argv.includes('--all')) {
  for (const kind of ['basic','multiple','disabled']) {
    for (const variant of ['outlined','filled','borderless','underlined']) {
      for (const size of ['small','middle','large']) scenarios.push({component:'select',kind,variant,size});
      for (const status of ['error','warning']) scenarios.push({component:'select',kind,variant,status});
    }
  }
  for (const kind of ['placeholder','loading','search','tags','responsive']) scenarios.push({component:'select',kind});
  for (const kind of ['disabled','editable','draggable','vertical']) scenarios.push({component:'slider',kind,reverse:true});
}
const cases=[];
for (const theme of ['default','dark','compact','dark-compact']) {
  for (const direction of ['ltr','rtl']) {
    for (const scenario of scenarios) {
      const suffix=[scenario.component,scenario.kind,scenario.variant,scenario.size,scenario.status,scenario.reverse?'reverse':null,scenario.tokens?'tokens':null,scenario.placement,scenario.focus?'focus':null].filter(Boolean).join('-');
      cases.push({name:`${theme}-${direction}-${suffix}`,theme,direction,...scenario});
    }
  }
}
const output=process.argv[2];
if(!output||output.startsWith('--'))throw Error('Usage: node select_slider_visual_cases.mjs <output.json> [--extended | --detail | --all]');
fs.writeFileSync(output,JSON.stringify(cases,null,2)+'\n');
console.log(`Wrote ${cases.length} native/browser visual cases`);
