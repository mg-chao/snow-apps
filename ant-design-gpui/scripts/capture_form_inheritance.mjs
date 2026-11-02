// Copyright (C) 2026 mg-chao
// SPDX-License-Identifier: Apache-2.0
// Executes pinned controls to distinguish Form context from global configuration.
import fs from 'node:fs/promises';
import fsSync from 'node:fs';
import path from 'node:path';
import crypto from 'node:crypto';
import {createRequire} from 'node:module';
import {fileURLToPath,pathToFileURL} from 'node:url';
import {execFileSync} from 'node:child_process';
import {setTimeout as delay} from 'node:timers/promises';
import {browser} from './pagination_browser.mjs';
const root=path.resolve(path.dirname(fileURLToPath(import.meta.url)),'..');
const upstream='E:/ant-design';
const dependencies=path.join(root,'../build/ant-design-gpui/modal-reference/browser');
const output=path.join(root,'../build/ant-design-gpui/form-inheritance');
const digest=file=>crypto.createHash('sha256').update(fsSync.readFileSync(file)).digest('hex');
const revision=execFileSync('git',['-C',upstream,'rev-parse','HEAD'],{encoding:'utf8'}).trim();
if(revision!=='f8f5595ab5e891fff6a2a06c5b3b98af643d008e')throw Error('Unexpected upstream revision');
const require=createRequire(path.join(dependencies,'package.json'));
const {build}=require('esbuild');
await fs.mkdir(output,{recursive:true});await fs.writeFile(path.join(output,'version.js'),'export default "6.6.7";\n');
const source=`import React from 'react';import {createRoot} from 'react-dom/client';import {flushSync} from 'react-dom';
import Form from 'antd-reference/form';import ConfigProvider from 'antd-reference/config-provider';
import Input from 'antd-reference/input';import InputNumber from 'antd-reference/input-number';import Select from 'antd-reference/select';
import DatePicker from 'antd-reference/date-picker';import TimePicker from 'antd-reference/time-picker';
import Segmented from 'antd-reference/segmented';import Checkbox from 'antd-reference/checkbox';import Radio from 'antd-reference/radio';
import Switch from 'antd-reference/switch';import Slider from 'antd-reference/slider';import ColorPicker from 'antd-reference/color-picker';
const root=createRoot(document.getElementById('root'));
let labelForm;
function LabelFixture(){const [form]=Form.useForm();labelForm=form;return <Form form={form} name='labels' initialValues={{checkbox:false,switch:false}} labelCol={{span:6}} wrapperCol={{span:18}} style={{width:600}}>
<Form.Item label='Checkbox' name='checkbox' valuePropName='checked'><Checkbox/></Form.Item>
<Form.Item label='Switch' name='switch' valuePropName='checked'><Switch/></Form.Item>
<Form.Item label='Input' name='input'><Input/></Form.Item></Form>;}
window.setupLabelProbe=()=>{flushSync(()=>root.render(<ConfigProvider theme={{token:{motion:false}}}><LabelFixture/></ConfigProvider>));return [...document.querySelectorAll('.ant-form-item-label>label')].map(label=>{const r=label.getBoundingClientRect();return {text:label.textContent,for:label.htmlFor,x:r.x+r.width/2,y:r.y+r.height/2};});};
window.labelProbeSnapshot=()=>({values:labelForm.getFieldsValue(true),focused:document.activeElement.id});
const controls=(c)=>{const own={disabled:c.ownDisabled,size:c.ownSize,variant:c.ownVariant};return {
input:<Input {...own}/>,password:<Input.Password {...own}/>,textarea:<Input.TextArea {...own}/>,otp:<Input.OTP {...own}/>,
number:<InputNumber {...own}/>,select:<Select {...own} options={[{value:'a',label:'A'}]} style={{width:180}}/>,
date:<DatePicker {...own}/>,range:<DatePicker.RangePicker {...own}/>,time:<TimePicker {...own}/>,
segmented:<Segmented disabled={c.ownDisabled} size={c.ownSize} options={['A','B']}/>,
checkbox:<Checkbox disabled={c.ownDisabled}>Checkbox</Checkbox>,
radio:<Radio.Group disabled={c.ownDisabled} size={c.ownSize} optionType='button' options={['A','B']}/>,
switch:<Switch disabled={c.ownDisabled} size={c.ownSize}/>,slider:<Slider disabled={c.ownDisabled}/>,
color:<ColorPicker disabled={c.ownDisabled} size={c.ownSize}/>};};
window.probeInheritance=async(c)=>{flushSync(()=>root.render(<ConfigProvider componentDisabled={c.globalDisabled} theme={{token:{motion:false}}}>
<Form disabled={c.formDisabled} size={c.formSize} variant={c.formVariant} style={{width:800}}>
{Object.entries(controls(c)).map(([id,control])=><div key={id} data-control={id}><Form.Item noStyle>{control}</Form.Item></div>)}
</Form></ConfigProvider>));await new Promise(resolve=>requestAnimationFrame(()=>requestAnimationFrame(resolve)));
return [...document.querySelectorAll('[data-control]')].map(wrapper=>{const element=wrapper.firstElementChild,s=getComputedStyle(element),r=element.getBoundingClientRect();
const inputs=[...wrapper.querySelectorAll('input,textarea,button')];return {id:wrapper.getAttribute('data-control'),class:element.className,
width:r.width,height:r.height,fontSize:s.fontSize,background:s.backgroundColor,borderColor:s.borderColor,
disabledClass:/disabled/.test(element.className),ariaDisabled:element.getAttribute('aria-disabled'),
effectiveDisabled:/disabled/.test(element.className)||inputs.some(input=>input.disabled),
inputs:inputs.map(input=>({disabled:input.disabled,ariaDisabled:input.getAttribute('aria-disabled'),class:input.className})),
variants:[...new Set([...wrapper.querySelectorAll('[class]')].flatMap(node=>[...node.classList].filter(name=>/outlined|filled|borderless|underlined/.test(name))))]};});};`;
const built=await build({stdin:{contents:source,loader:'tsx',resolveDir:root,sourcefile:'original-form-inheritance-probe.tsx'},
  outfile:path.join(output,'reference.js'),bundle:true,metafile:true,platform:'browser',format:'iife',
  nodePaths:[path.join(dependencies,'node_modules')],alias:{'antd-reference':path.join(upstream,'components')},
  define:{'process.env.NODE_ENV':'"production"'},plugins:[{name:'version',setup(builder){builder.onResolve({filter:/^\.\/version$/},info=>
    info.resolveDir.replaceAll('\\','/').endsWith('/components/version')?{path:path.join(output,'version.js')}:undefined);}}]});
await fs.writeFile(path.join(output,'index.html'),'<!doctype html><html><head><meta charset="UTF-8"></head><body><div id="root"></div><script src="reference.js"></script></body></html>\n');
const scenarios=[{name:'baseline'},
  {name:'form-disabled',formDisabled:true},{name:'global-disabled',globalDisabled:true},
  {name:'form-disabled-global-enabled',formDisabled:true,globalDisabled:false},
  {name:'form-enabled-global-disabled',formDisabled:false,globalDisabled:true},
  {name:'own-enabled',formDisabled:true,ownDisabled:false},{name:'own-disabled',formDisabled:false,ownDisabled:true},
  {name:'form-small',formSize:'small'},{name:'form-large',formSize:'large'},
  {name:'own-large-in-small',formSize:'small',ownSize:'large'},
  ...['outlined','filled','borderless','underlined'].map(formVariant=>({name:'variant-'+formVariant,formVariant})),
  {name:'own-outlined-in-filled',formVariant:'filled',ownVariant:'outlined'}];
const b=await browser({output,scale:1,edge:'C:/Program Files/Google/Chrome/Application/chrome.exe'});
try {
  await b.page('Emulation.setDeviceMetricsOverride',{width:1000,height:1400,deviceScaleFactor:1,mobile:false});
  await b.page('Page.navigate',{url:pathToFileURL(path.join(output,'index.html')).href});
  for(let i=0;i<300;i++){if(await b.evaluate('!!window.probeInheritance'))break;if(i===299)throw Error('Inheritance probe not ready');await delay(20);}
  const observed=[];for(const config of scenarios)observed.push({config,controls:await b.evaluate(`probeInheritance(${JSON.stringify(config)})`,true)});
  const labels=await b.evaluate('setupLabelProbe()');await b.settle();
  const labelClicks=[{action:'initial',observed:await b.evaluate('labelProbeSnapshot()')}];
  for(const label of labels){await b.page('Input.dispatchMouseEvent',{type:'mouseMoved',x:label.x,y:label.y});
    await b.page('Input.dispatchMouseEvent',{type:'mousePressed',button:'left',clickCount:1,x:label.x,y:label.y});
    await b.page('Input.dispatchMouseEvent',{type:'mouseReleased',button:'left',clickCount:1,x:label.x,y:label.y});await b.settle();
    labelClicks.push({action:'click-label-'+label.text.toLowerCase(),observed:await b.evaluate('labelProbeSnapshot()')});}
  const inputs=Object.fromEntries(Object.keys(built.metafile.inputs).filter(file=>fsSync.existsSync(file)).map(file=>[file.replaceAll('\\','/'),digest(file)]));
  const packageRoots=new Map();for(const file of Object.keys(inputs)){if(!file.includes('/node_modules/'))continue;let directory=path.dirname(path.resolve(file));
    while(directory!==path.dirname(directory)){const manifest=path.join(directory,'package.json');if(fsSync.existsSync(manifest)){const info=JSON.parse(fsSync.readFileSync(manifest,'utf8'));if(info.name){packageRoots.set(directory,info);break;}}directory=path.dirname(directory);}}
  const packages=[];const licenses=path.join(root,'fixtures/form/licenses');
  for(const [directory,info] of packageRoots){const originals=fsSync.readdirSync(directory).filter(name=>/^licen[cs]e(?:\..*)?$/i.test(name)).map(name=>path.join(directory,name));
    if(!originals.length&&info.name==='@ant-design/icons-svg')originals.push(path.join(root,'fixtures/message/licenses/ant-design-icons-svg.txt'));
    if(!originals.length&&info.name==='is-mobile')originals.push(path.join(root,'fixtures/message/licenses/is-mobile.txt'));
    if(!originals.length)throw Error('Missing original license '+info.name);
    const files=[];for(const file of originals){const name=info.name.replaceAll('/','-').replace('@','')+'-'+path.basename(file);await fs.copyFile(file,path.join(licenses,name));files.push({name,sha256:digest(file)});}
    packages.push({name:info.name,version:info.version,license:info.license,files});}
  const provenance={revision,version:'6.6.7',rc_form:'1.8.6',rc_segmented:'1.4.0',generator_sha256:digest(fileURLToPath(import.meta.url)),
    lock_sha256:digest(path.join(dependencies,'package-lock.json')),inputs};
  await fs.writeFile(path.join(root,'fixtures/form/inheritance-reference.json'),JSON.stringify({schema_version:1,provenance,renderer:b.renderer,scenarios:observed,label_clicks:labelClicks},null,2)+'\n');
  await fs.writeFile(path.join(root,'fixtures/form/inheritance-packages.json'),JSON.stringify({revision,version:'6.6.7',packages},null,2)+'\n');
  console.log(JSON.stringify({configurations:observed.length,controls:observed[0].controls.length,packages:packages.length,
    segmented_disabled:observed.filter(row=>['form-disabled','global-disabled','own-disabled'].includes(row.config.name))
      .map(row=>({case:row.config.name,disabled:row.controls.find(control=>control.id==='segmented').effectiveDisabled}))}));
  console.log(JSON.stringify({label_clicks:labelClicks}));
}finally{await b.close();}
