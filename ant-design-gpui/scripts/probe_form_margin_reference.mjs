// Copyright (C) 2026 mg-chao
// SPDX-License-Identifier: Apache-2.0
// Isolates genuine rendered Items to measure CSS margin-collapse behavior.
import fs from 'node:fs/promises';
import path from 'node:path';
import {fileURLToPath,pathToFileURL} from 'node:url';
import {setTimeout as delay} from 'node:timers/promises';
import crypto from 'node:crypto';
import {browser} from './pagination_browser.mjs';
const root=path.resolve(path.dirname(fileURLToPath(import.meta.url)),'..');
const reference=path.join(root,'../build/ant-design-gpui/form-reference');
const output=path.join(root,'../build/ant-design-gpui/form-margin-probes');
const b=await browser({output,scale:1,edge:'C:/Program Files/Google/Chrome/Application/chrome.exe'});
try {
  const probes=[];
  for(const [name,itemId,twoLines] of [['help-one-line','error',false],['help-two-lines','error',true],['help-and-extra','extra',false]]) {
    const url=pathToFileURL(path.join(reference,'index.html'));url.searchParams.set('scene','statuses');
    await b.page('Emulation.setDeviceMetricsOverride',{width:900,height:900,deviceScaleFactor:1,mobile:false});
    await b.page('Page.navigate',{url:url.href});
    for(let i=0;i<300;i++){if(await b.evaluate('!!document.querySelector("#visual-ready")'))break;await delay(20);}
    const measured=await b.evaluate(`(()=>{
      const form=document.querySelector('.ant-form');
      for(const item of [...form.children])if(item.querySelector(':scope>.ant-row')?.getAttribute('data-item')!==${JSON.stringify(itemId)})item.remove();
      if(${twoLines}){const line=form.querySelector('.ant-form-item-explain>div');line.replaceChildren(document.createTextNode('First line'),document.createElement('br'),document.createTextNode('Second line'));}
      const box=element=>{if(!element)return null;const r=element.getBoundingClientRect(),s=getComputedStyle(element);return {x:r.x,y:r.y,width:r.width,height:r.height,marginBottom:s.marginBottom,minHeight:s.minHeight};};
      return {form:box(form),wrapper:box(form.parentElement),item:box(form.firstElementChild),
        control:box(form.querySelector('.ant-form-item-control')),help:box(form.querySelector('.ant-form-item-explain')),
        extra:box(form.querySelector('.ant-form-item-extra')),additional:box(form.querySelector('.ant-form-item-additional')),
        marginOffset:box(form.querySelector('.ant-form-item-margin-offset'))};
    })()`);
    probes.push({name,...measured});
  }
  const layoutScenes=[];
  for(const scene of ['mixed','wrapped','inline','statuses','tokens']) {
    const url=pathToFileURL(path.join(reference,'index.html'));url.searchParams.set('scene',scene);
    await b.page('Page.navigate',{url:url.href});
    for(let i=0;i<300;i++){if(await b.evaluate(`location.href===${JSON.stringify(url.href)}&&!!document.querySelector('#visual-ready')`))break;await delay(20);}
    layoutScenes.push({scene,forms:await b.evaluate(`(()=>{
      const box=element=>{if(!element)return null;const r=element.getBoundingClientRect(),s=getComputedStyle(element);return {
        x:r.x,y:r.y,width:r.width,height:r.height,...Object.fromEntries(['display','flex','flexDirection','flexWrap','alignItems','alignSelf','minWidth','maxWidth','minHeight','maxHeight','boxSizing','lineHeight','fontSize','fontFamily','padding','margin'].map(key=>[key,s[key]]))};};
      return [...document.querySelectorAll('.fixture-form')].map(form=>({id:form.getAttribute('data-form'),items:[...form.querySelectorAll('.fixture-form-item')].map(item=>({
        id:item.querySelector(':scope>.ant-row').getAttribute('data-item'),root:box(item),row:box(item.querySelector(':scope>.ant-row')),
        labelColumn:box(item.querySelector(':scope>.ant-row>.ant-form-item-label')),
        label:box(item.querySelector(':scope>.ant-row>.ant-form-item-label>label')),
        control:box(item.querySelector(':scope>.ant-row>.ant-form-item-control')),
        content:box(item.querySelector('.ant-form-item-control-input-content')),
      }))}));
    })()`)});
  }
  const provenance=JSON.parse(await fs.readFile(path.join(reference,'provenance.json'),'utf8'));
  const result={schema_version:1,description:'Layout isolation removes sibling DOM nodes from genuine rendered original Form; two-line probe replaces only message DOM text with two lines. CSS and component element structure remain original.',
    provenance:{revision:provenance.revision,version:provenance.version,rc_form:provenance.rc_form,
      probe_sha256:crypto.createHash('sha256').update(await fs.readFile(fileURLToPath(import.meta.url))).digest('hex')},renderer:b.renderer,probes,layout_scenes:layoutScenes};
  await fs.writeFile(path.join(root,'fixtures/form/layout-probes.json'),JSON.stringify(result,null,2)+'\n');
  console.log(JSON.stringify(probes));
}finally{await b.close();}
