// Copyright (C) 2026 mg-chao
// SPDX-License-Identifier: Apache-2.0
import fs from 'node:fs';
const scenarios=[
  {mode:'vertical'}, {mode:'vertical',open:true}, {mode:'vertical',open:true,kind:'deep'},
  {mode:'inline',open:true}, {mode:'inline',kind:'groups'},
  {mode:'collapsed'}, {mode:'collapsed',kind:'no-icon'},
  {mode:'horizontal'}, {mode:'horizontal',open:true}, {mode:'horizontal',width:280},
  {mode:'inline',open:true,tokens:true}, {mode:'vertical',kind:'extra'},
  {mode:'vertical',selected:'delete'},
  {mode:'inline',open:true,hover:true}, {mode:'vertical',focus:true},
  {mode:'vertical',kind:'custom-expand',open:true},
  {mode:'horizontal',kind:'custom-overflow',width:280},
];
const cases=[];
for(const color_theme of ['light','dark'])for(const direction of ['ltr','rtl']){
  for(const scenario of scenarios){
    const suffix=[scenario.mode,scenario.kind,scenario.open?'open':null,scenario.width?'overflow':null,scenario.tokens?'tokens':null,scenario.selected,scenario.hover?'hover':null,scenario.focus?'focus':null].filter(Boolean).join('-');
    cases.push({name:`default-${color_theme}-${direction}-${suffix}`,theme:'default',color_theme,direction,...scenario});
  }
}
for(const theme of ['dark','compact','dark-compact'])for(const direction of ['ltr','rtl']){
  cases.push({name:`${theme}-light-${direction}-inline-open`,theme,color_theme:'light',direction,mode:'inline',open:true});
}
for(const direction of ['ltr','rtl']) {
  cases.push({name:`default-dark-${direction}-submenu-theme`,theme:'default',color_theme:'dark',direction,mode:'vertical',open:true,kind:'submenu-theme'});
}
for(const color_theme of ['light','dark'])for(const theme of ['default','compact']) {
  for(const pressed of ['normal','danger','selected']) {
    cases.push({name:`${theme}-${color_theme}-ltr-vertical-pressed-${pressed}`,theme,color_theme,direction:'ltr',mode:'vertical',pressed,selected:pressed==='normal'?'projects':'home'});
  }
}
if(!process.argv[2])throw Error('Usage: node scripts/menu_visual_cases.mjs <output.json>');
fs.writeFileSync(process.argv[2],JSON.stringify(cases,null,2)+'\n');
console.log(`Wrote ${cases.length} Menu native/browser visual scenarios`);
