// Copyright (C) 2026 mg-chao
// SPDX-License-Identifier: Apache-2.0
// Original Ant CSS and placement rules with live rc-trigger. JSON supplies only inputs.
import fs from 'node:fs';
import path from 'node:path';
import crypto from 'node:crypto';
import {createRequire} from 'node:module';
import {execFileSync} from 'node:child_process';
import {fileURLToPath} from 'node:url';
const crate = path.resolve(path.dirname(fileURLToPath(import.meta.url)), '..');
const upstream = process.argv[2] ?? 'E:/ant-design';
const base = path.resolve(crate, '../build/ant-design-gpui/popup-reference');
const radius = path.resolve(crate, '../build/ant-design-gpui/popup-reference-radius');
const output = path.resolve(crate, '../build/ant-design-gpui/popup-extended-reference');
const tools = path.resolve(crate, '../build/ant-design-gpui/popup-placement-tools');
const require = createRequire(path.join(tools, 'package.json'));
const esbuild = require('esbuild'), ts = require('typescript');
const hash = x => crypto.createHash('sha256').update(x).digest('hex');
const lock = fs.readFileSync(path.join(crate, 'fixtures/popup/placement-tools/package-lock.json'));
if (hash(lock) !== hash(fs.readFileSync(path.join(tools, 'package-lock.json')))) throw Error('Fixture tools lock mismatch');
const revision = execFileSync('git', ['-C', upstream, 'rev-parse', 'HEAD'], {encoding:'utf8'}).trim();
if (revision !== 'f8f5595ab5e891fff6a2a06c5b3b98af643d008e') throw Error('Unexpected Ant revision');
fs.mkdirSync(output, {recursive:true});
const sources = {};
const selected = new Map([
 ['components/style/placementArrow.ts', ['MAX_VERTICAL_CONTENT_RADIUS','getArrowOffsetToken']],
 ['components/_util/is.ts', ['isPlainObject']],
]);
const entry = `
import React from 'react';
import {createRoot} from 'react-dom/client';
import Trigger from '@rc-component/trigger';
import placements from ${JSON.stringify(path.join(upstream,'components/_util/placements.ts'))};
const c=JSON.parse(document.querySelector('#case').textContent);
const template = new DOMParser().parseFromString(JSON.parse(document.querySelector('#template').textContent),'text/html');
const prefix=c.component==='tooltip'?'ant-tooltip':'ant-popover';
const container=template.querySelector('.'+prefix+'-container');
const longText='This longer explanation wraps naturally onto several lines without losing its padding or moving the arrow away from the trigger.';
if(c.long) container.querySelector(c.component==='tooltip'?'.sample':c.component==='popover'?'.ant-popover-content':'.ant-popconfirm-description').textContent=longText;
if(c.semantic) {
 container.style.padding='20px'; container.style.border='1px solid #1677ff';
 const title=container.querySelector(c.component==='popover'?'.ant-popover-title':'.ant-popconfirm-title');
 if(title) title.style.color='#1677ff';
 container.querySelector(c.component==='tooltip'?'.sample':c.component==='popover'?'.ant-popover-content':'.ant-popconfirm-description').style.fontStyle='italic';
}
if(c.color?.startsWith('#')) {
 container.style.backgroundColor=c.color;
 if(c.component==='tooltip') container.style.color=c.color==='#ffe58f'?'#000':'#fff';
 const style=document.createElement('style');style.textContent='.'+prefix+'-arrow::before{background:'+c.color+'!important}';document.head.append(style);
}
let align;
createRoot(document.querySelector('#root')).render(React.createElement(Trigger,{
 prefixCls:prefix,popupVisible:true,popupPlacement:c.placement,arrow:c.arrow!=='hidden',
 builtinPlacements:placements({arrowWidth:c.arrow==='hidden'?0:16,arrowPointAtCenter:c.arrow==='center',autoAdjustOverflow:true,offset:4,borderRadius:c.theme==='radius-16'?16:6}),
 popupClassName:(c.component==='popconfirm'?'ant-popconfirm ':'')+(c.rtl?prefix+'-rtl ':'')+(c.color==='magenta'?prefix+'-magenta':''),
 popupStyle:{width:c.width},popup:React.createElement('div',{className:prefix+'-container',style:Object.fromEntries([...container.style].map(p=>[p.replace(/-([a-z])/g,(_,x)=>x.toUpperCase()),container.style.getPropertyValue(p)])),dangerouslySetInnerHTML:{__html:container.innerHTML}}),
 onPopupAlign:(_,info)=>{align=info;},
},React.createElement('button',{className:'ant-btn ant-btn-default ant-btn-color-default ant-btn-variant-outlined',style:{position:'absolute',left:c.anchor[0],top:c.anchor[1],width:80,height:32}},React.createElement('span',null,'Open'))));
const measure=async()=>{
 await document.fonts.ready;
 let previous='',stable=0;
 for(let i=0;i<100;i++) {
  await new Promise(r=>setTimeout(r,25));
  const popup=document.querySelector('.'+prefix);
  if(!align||!popup)continue;
  const b=popup.getBoundingClientRect();
  const body={x:b.x,y:b.y,width:b.width,height:b.height};
  const current=JSON.stringify(body);
  stable=current===previous?stable+1:0;previous=current;
  if(stable>=3){window.popupMetrics={name:c.name,body,effectivePlacement:popup.className.match(/placement-([a-zA-Z]+)/)?.[1],viewport:[document.documentElement.clientWidth,document.documentElement.clientHeight]};return;}
 }
 throw Error('Popup did not settle');
};measure();
`;
await esbuild.build({stdin:{contents:entry,loader:'js',resolveDir:tools},bundle:true,platform:'browser',format:'iife',outfile:path.join(output,'bundle.js'),nodePaths:[path.join(tools,'node_modules')],plugins:[{name:'original-sources',setup(build){
 build.onLoad({filter:/\.ts$/},({path:file})=>{
  const relative=path.relative(upstream,file).replaceAll('\\','/');
  if(!relative.startsWith('components/'))return;
  const source=fs.readFileSync(file,'utf8');sources[relative]=hash(source);
  const names=selected.get(relative);
  if(!names)return{contents:source,loader:'ts',resolveDir:path.dirname(file)};
  const ast=ts.createSourceFile(file,source,ts.ScriptTarget.Latest,true);
  const contents=ast.statements.filter(n=>ts.isVariableStatement(n)?n.declarationList.declarations.some(d=>names.includes(d.name.getText(ast))):n.name&&names.includes(n.name.getText(ast))).map(n=>n.getFullText(ast)).join('\n');
  return{contents,loader:'ts',resolveDir:path.dirname(file)};
 });
}}]});
const inputs=JSON.parse(fs.readFileSync(path.join(crate,'fixtures/popup/visual-cases.json'),'utf8'));
for(const c of inputs){
 const source=path.join(c.theme==='radius-16'?radius:base,`${c.theme}-${c.rtl?'rtl':'ltr'}-${c.component}-top.html`);
 const template=fs.readFileSync(source,'utf8');sources[path.relative(crate,source)]=hash(template);
 const css=template.match(/<style>([\s\S]*?)<\/style>/)[1];
 const html=`<!doctype html><html lang="en" dir="${c.rtl?'rtl':'ltr'}"><meta charset="utf-8"><style>${css}</style><body><div id="root"></div><script id="case" type="application/json">${JSON.stringify(c)}</script><script id="template" type="application/json">${JSON.stringify(template).replaceAll('<','\\u003c')}</script><script src="bundle.js"></script></body></html>`;
 fs.writeFileSync(path.join(output,c.name+'.html'),html);
}
fs.writeFileSync(path.join(output,'provenance.json'),JSON.stringify({revision,trigger:'3.10.1',inputSha256:hash(fs.readFileSync(path.join(crate,'fixtures/popup/visual-cases.json'))),lockSha256:hash(lock),sources,bundleSha256:hash(fs.readFileSync(path.join(output,'bundle.js')))},null,2)+'\n');
console.log(`Generated ${inputs.length} independent extended popup pages`);

// Browser interpolation of the original shared-container transition rule.
const motionTemplate=fs.readFileSync(path.join(base,'default-ltr-tooltip-top.html'),'utf8');
const motionCss=motionTemplate.match(/<style>([\s\S]*?)<\/style>/)[1].replace('*{animation:none!important;transition:none!important}','');
fs.writeFileSync(path.join(output,'motion.html'),`<!doctype html><html><meta charset="utf-8"><style>${motionCss}</style><body><script>
const from={x:100,y:200,width:80,height:32},to={x:420,y:120,width:240,height:76};
const element=document.createElement('div');element.className='ant-tooltip-unique-container ant-tooltip-unique-container-visible';
const apply=b=>Object.assign(element.style,{left:b.x+'px',top:b.y+'px',width:b.width+'px',height:b.height+'px'});
apply(from);document.body.append(element);void element.offsetWidth;
requestAnimationFrame(()=>{apply(to);void element.offsetWidth;requestAnimationFrame(()=>{
 const animations=element.getAnimations();if(animations.length!==4)throw Error('Expected four original CSS property transitions, got '+animations.length);
 animations.forEach(a=>a.pause());
 const samples=[0,25,50,75,100,150,200,250,299,300].map(time=>{animations.forEach(a=>a.currentTime=time);const b=element.getBoundingClientRect();return{time,bounds:{x:b.x,y:b.y,width:b.width,height:b.height}};});
 window.popupMotion={from,to,duration:getComputedStyle(element).transitionDuration,easing:getComputedStyle(element).transitionTimingFunction,samples};
});});
</script></body></html>`);
