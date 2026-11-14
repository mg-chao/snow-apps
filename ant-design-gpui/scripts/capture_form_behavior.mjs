// Copyright (C) 2026 mg-chao
// SPDX-License-Identifier: Apache-2.0
// Captures actual original store/action traces, including user input through CDP.
import fs from 'node:fs/promises';
import path from 'node:path';
import {fileURLToPath,pathToFileURL} from 'node:url';
import crypto from 'node:crypto';
import {setTimeout as delay} from 'node:timers/promises';
import {browser} from './pagination_browser.mjs';
const args=Object.fromEntries(process.argv.slice(2).reduce((all,value,index,values)=>{
  if(index%2===0)all.push([value.replace(/^--/,''),values[index+1]]);return all;},[]));
const root=path.resolve(path.dirname(fileURLToPath(import.meta.url)),'..');
const reference=path.resolve(args.reference??path.join(root,'../build/ant-design-gpui/form-reference'));
const output=path.resolve(args.output??path.join(root,'../build/ant-design-gpui/form-behavior'));
const b=await browser({output,scale:1,edge:args.browser??'C:/Program Files/Google/Chrome/Application/chrome.exe'});
const traces=[];
try {
  await b.page('Emulation.setDeviceMetricsOverride',{width:900,height:900,deviceScaleFactor:1,mobile:false});
  await b.page('Page.navigate',{url:pathToFileURL(path.join(reference,'behavior.html')).href});
  for(let i=0;i<300;i++){if(await b.evaluate('!!window.formOracle'))break;if(i===299)throw Error('Oracle not ready');await delay(20);}
  const call=(method,...values)=>b.evaluate(`formOracle.${method}(${values.map(value=>JSON.stringify(value)??'undefined').join(',')})`,true);
  async function edit(id,value) {
    await b.evaluate(`(()=>{const e=document.querySelector('input[data-field='+${JSON.stringify(JSON.stringify(id))}+']');if(!e)throw Error('Missing editor');e.focus();e.select();})()`);
    await b.page('Input.insertText',{text:value});await b.settle();
  }
  async function run(name,scenario,actions) {
    await call('reset',scenario);await b.settle();
    const observations=[{action:'initial',observed:await call('snapshot')}];
    for(const [action,work] of actions){await work();await b.settle();observations.push({action,observed:await call('snapshot')});}
    traces.push({name,scenario,observations});console.log(`Captured Form behavior ${name}`);
  }
  await run('initial-values-programmatic-and-user-events',{initialValues:{a:'initial'},fields:[{id:'a',initialValue:'item'},{id:'b',initialValue:'fallback'}]},[
    ['equal-single-set',()=>call('set','a','initial')],['merge-same-and-new',()=>call('merge',{a:'initial',b:'changed',unregistered:7})],
    ['user-edit',()=>edit('a','typed')],['selected-reset',()=>call('resetFields',['a'])],['all-reset',()=>call('resetFields')],
  ]);
  await run('metadata-and-reset',{fields:[{id:'a'},{id:'b'}]},[
    ['set-errors',()=>call('fields',[{name:'a',errors:['Server error'],warnings:['Hint'],touched:false}])],
    ['single-set-clears-messages',()=>call('set','a','value')],['set-warnings',()=>call('fields',[{name:'b',warnings:['Warning'],touched:false}])],
    ['merge-clears-changed',()=>call('merge',{a:'next',b:'next'})],['reset',()=>call('resetFields')],
  ]);
  await run('watcher-selector-and-preserved-unregistered',{fields:[{id:'a'}]},[
    ['set-registered',()=>call('set','a','watched')],['set-unregistered',()=>call('set','unregistered',42)],
    ['remove-watched-field',()=>call('patch',{fields:[]})],['update-preserved-value',()=>call('set','a','changed')],
    ['remount',()=>call('patch',{fields:[{id:'a'}]})],
  ]);
  await run('preserve-removed-fields',{initialValues:{a:'start',b:'start'},fields:[{id:'a'},{id:'b',preserve:false}]},[
    ['edit-fields',async()=>{await edit('a','kept');await edit('b','removed');}],
    ['remove-fields',()=>call('patch',{fields:[]})],['remount-fields',()=>call('patch',{fields:[{id:'a'},{id:'b',preserve:false}]})],
  ]);
  await run('hidden-validates-and-warning-submit',{fields:[{id:'a',hidden:true,rules:[{required:true,message:'Required hidden'}]},
    {id:'b',rules:[{required:true,warningOnly:true,message:'Only warning'}]}]},[
    ['submit-invalid',()=>call('submit')],['fill-hidden',()=>call('set','a','value')],['submit-warning',()=>call('submit')],
  ]);
  await run('validate-only-dirty-recursive',{initialValues:{user:{a:'initial'}},fields:[
    {id:'a',name:['user','a'],rules:[{min:8,message:'Too short'}]},
    {id:'b',name:['user','b'],rules:[{required:true,message:'Required b'}]},
    {id:'c',rules:[{required:true,message:'Required c'}]}]},[
    ['dirty-only',()=>call('validate',undefined,{dirty:true})],['validate-only-recursive',()=>call('validate',[['user']],{recursive:true,validateOnly:true})],
    ['recursive',()=>call('validate',[['user']],{recursive:true})],['reset',()=>call('resetFields')],
  ]);
  await run('dependencies-user-versus-programmatic',{fields:[{id:'password'},
    {id:'confirm',dependencies:['password'],rules:[{equalTo:'password',message:'Mismatch'}]}]},[
    ['edit-password',()=>edit('password','one')],['edit-confirm',()=>edit('confirm','one')],
    ['user-password-invalidates',()=>edit('password','two')],['programmatic-password-does-not-revalidate',()=>call('set','password','one')],
    ['explicit-validation',()=>call('validate')],
  ]);
  await run('dynamic-list-stable-identities',{list:{initialValue:[{name:'A'},{name:'B'},{name:'C'}]}},[
    ['move-last-to-first',()=>call('list','move',2,0)],['insert-middle',()=>call('list','add',{name:'X'},1)],
    ['remove-multiple',()=>call('list','remove',[0,2])],['invalid-move-noop',()=>call('list','move',50,0)],
    ['append-invalid-index',()=>call('list','add',{name:'Y'},100)],['reset-list',()=>call('resetFields')],
  ]);
  await run('async-stale-and-reset',{initialValues:{a:'old'},fields:[{id:'a',rules:[{deferred:true}]}]},[
    ['start-old',()=>call('startValidate',['a'])],['replace-value',()=>call('set','a','new')],
    ['start-new',()=>call('startValidate',['a'])],['resolve-old-error',()=>call('resolve','old','Stale error')],
    ['resolve-new-success',()=>call('resolve','new')],['start-then-reset',async()=>{await call('startValidate',['a']);await b.settle();await call('resetFields');}],
    ['resolve-after-reset',()=>call('resolve','new','Reset error')],
  ]);
  for(const validateFirst of [false,true,'parallel']) await run(`first-mode-${validateFirst}`,{fields:[{id:'a',validateFirst,rules:[
    {required:true,warningOnly:true,message:'Warning first in source'},
    {required:true,message:'Error first'}, {required:true,message:'Error second'}]}]},[['validate',()=>call('validate')]]);
  await run('reset-blurs-focused-control',{initialValues:{a:'same'},fields:[{id:'a'}]},[
    ['focus',()=>b.evaluate('document.querySelector("input[data-field=a]").focus()')],['reset-same-value',()=>call('resetFields')],
  ]);
  const provenance=JSON.parse(await fs.readFile(path.join(reference,'provenance.json'),'utf8'));
  const result={schema_version:1,provenance:{revision:provenance.revision,version:provenance.version,rc_form:provenance.rc_form,
    lock_sha256:provenance.lock_sha256,source_sha256:crypto.createHash('sha256').update(await fs.readFile(path.join(root,'scripts/form_behavior_reference.tsx'))).digest('hex')},
    renderer:b.renderer,traces};
  await fs.writeFile(path.join(output,'behavior-reference.json'),JSON.stringify(result,null,2)+'\n');
  await fs.writeFile(path.join(root,'fixtures/form/behavior-reference.json'),JSON.stringify(result,null,2)+'\n');
  console.log(`Recorded ${traces.length} independent original Form behavior traces`);
}finally{await b.close();}
