// Copyright (C) 2026 mg-chao
// SPDX-License-Identifier: Apache-2.0
// Independent opacity compositions, separate from the primary Form scene matrix.
import fs from 'node:fs';
import path from 'node:path';
import {fileURLToPath} from 'node:url';
const root=path.resolve(path.dirname(fileURLToPath(import.meta.url)),'..');
const cases={};
for(const [name,background] of [['container',undefined],['white','#ffffff'],['gray','#f0f0f0'],
  ['red','#e65a5a'],['dark','#141414'],['blue','#19395b']]){
  const items=[];
  for(const opacity of [undefined,.25,.5,1])for(const size of ['small','middle'])for(const checked of [false,true]){
    items.push({id:`alpha-${opacity??'disabled'}-${size}-${checked}`,control:'switch',value:checked,size,
      disabled:opacity===undefined,opacity});
  }
  for(const checked of [false,true])items.push({id:`custom-${checked}`,control:'switch',value:checked,
    opacity:.35,customContent:true});
  for(const size of ['small','middle'])items.push({id:`loading-${size}`,control:'switch',value:true,size,loading:true});
  cases[`switch-opacity-${name}`]={viewport:{width:600,height:1180},background,
    forms:[{id:'opacity',width:560,layout:'vertical',opacityBackdrop:background,items}]};
}
fs.writeFileSync(path.join(root,'fixtures/form/switch-opacity-cases.json'),JSON.stringify(cases,null,2)+'\n');
console.log(JSON.stringify({scenes:Object.keys(cases).length,controls_per_scene:cases[Object.keys(cases)[0]].forms[0].items.length}));
