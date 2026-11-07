// Copyright (C) 2026 mg-chao
// SPDX-License-Identifier: Apache-2.0
// Shared browser/native cases. Values describe the application composition,
// never precomputed layout or recreated Ant Design styles.
import fs from 'node:fs';
import path from 'node:path';
import {fileURLToPath} from 'node:url';

const root = path.resolve(path.dirname(fileURLToPath(import.meta.url)), '..');
const item = (id, label, extra = {}) => ({id, label, control: 'input', ...extra});
const basic = [item('username', 'Username', {required: true}),
  item('password', 'Password', {control: 'password', required: true}),
  item('remember', null, {control: 'checkbox', value: true, text: 'Remember me'}),
  item('submit', null, {control: 'button', text: 'Submit'})];
const form = (id, extra = {}) => ({id, width: 600, layout: 'horizontal',
  labelCol: {span: 8}, wrapperCol: {span: 16}, items: basic, ...extra});
const scene = (forms, width = 900, height = 900) => ({viewport: {width, height}, forms});
const cases = {
  horizontal: scene([form('basic')]),
  vertical: scene([form('vertical', {layout: 'vertical', labelCol: null, wrapperCol: null,
    items: [item('email', 'Email', {required: true, placeholder: 'Email address'}),
      item('nickname', 'Nickname', {extra: 'This name will be visible to other people.'}),
      item('submit', null, {control: 'button', text: 'Submit'})]})]),
  inline: scene([form('inline', {width: 820, layout: 'inline', labelCol: null, wrapperCol: null,
    items: [item('username', 'Username', {controlWidth: 160, placeholder: 'Username'}),
      item('password', 'Password', {control: 'password', controlWidth: 160, placeholder: 'Password'}),
      item('submit', undefined, {control: 'button', text: 'Log in'})]})]),
  mixed: scene([form('mixed', {labelCol: {flex: '120px'}, wrapperCol: {flex: 1},
    items: [item('name', 'Name', {required: true}),
      item('description', 'Description', {layout: 'vertical', control: 'textarea', rows: 3}),
      item('email', 'Email', {required: true}), item('submit', null, {control: 'button', text: 'Submit'})]})]),
  sizes: scene(['small', 'middle', 'large'].map(size => form(size, {size,
    items: [item('username', 'Username', {required: true}), item('password', 'Password', {control: 'password'})]}))),
  marks: scene([true, false, 'optional'].map((requiredMark, index) => form(`marks-${index}`, {
    requiredMark, items: [item('required', 'Required field', {required: true, tooltip: 'This is a required field'}),
      item('optional', 'Optional field')]}))),
  statuses: scene([form('statuses', {labelCol: {span: 6}, wrapperCol: {span: 18},
    items: [item('error', 'Fail', {status: 'error', help: 'Should be combination of numbers & alphabets'}),
      item('warning', 'Warning', {status: 'warning', feedback: true, placeholder: 'Warning'}),
      item('validating', 'Validating', {status: 'validating', feedback: true, help: 'The information is being validated...'}),
      item('success', 'Success', {status: 'success', feedback: true, value: 'Valid value'}),
      item('feedback-error', 'Fail', {status: 'error', feedback: true, help: 'Please check this value.'}),
      item('extra', 'Email', {status: 'error', help: 'Please enter Email', extra: 'We use this address to send updates.'})]})]),
  wrapped: scene([form('wrapped', {labelCol: {flex: '110px'}, wrapperCol: {flex: 1},
    labelAlign: 'left', labelWrap: true, colon: false,
    items: [item('normal', 'Normal label', {required: true}),
      item('long', 'A super long label text', {required: true}),
      item('optional', 'A super long label text', {help: 'A longer explanation that wraps naturally over the available control width without clipping.'}),
      item('submit', ' ', {control: 'button', text: 'Submit'})]})]),
  'label-absence': scene([form('labels', {items: [item('ordinary', 'Ordinary'),
    item('null', null, {placeholder: 'Null label reserves the label column'}),
    item('absent', undefined, {placeholder: 'Absent label has no reserved label column'}),
    item('colon', 'Already has colon:', {required: true}),
    item('nocolon', 'No colon', {colon: false})]})]),
  'narrow-520': scene([form('responsive', {width: 480})], 520, 650),
  'breakpoint-575': scene([form('responsive', {width: 535})], 575, 650),
  'breakpoint-576': scene([form('responsive', {width: 536})], 576, 650),
  variants: scene(['outlined', 'filled', 'borderless', 'underlined'].map(variant => form(variant, {variant,
    items: [item('name', 'Name', {required: true}), item('email', 'Email', {status: 'error', feedback: true})]}))),
  disabled: scene([form('disabled', {disabled: true, items: [item('name', 'Name', {value: 'Disabled value'}),
    item('enabled', 'Enabled child', {disabled: false, placeholder: 'Explicitly enabled'}),
    item('remember', 'Remember', {control: 'checkbox', value: true, text: 'Remember me'}),
    item('switch', 'Switch', {control: 'switch', value: true}),
    item('submit', null, {control: 'button', text: 'Submit'})]})]),
  tokens: scene([form('tokens', {tokens: {labelRequiredMarkColor: '#eb2f96', labelColor: '#389e0d',
    labelFontSize: 16, labelHeight: 34, labelColonMarginInlineStart: 4,
    labelColonMarginInlineEnd: 12, itemMarginBottom: 18, inlineItemMarginBottom: 18},
    items: [item('username', 'Username', {required: true}), item('password', 'Password', {control: 'password', required: true})]})]),
  'no-style': scene([form('compound', {labelCol:{span:6},wrapperCol:{span:18},items:[
    item('contact','Contact',{control:'group',gap:8,children:[
      item('given',undefined,{noStyle:true,controlWidth:180,placeholder:'First name',required:true,errors:['Missing first name']}),
      item('family',undefined,{noStyle:true,controlWidth:180,placeholder:'Last name',required:true}),
    ]}),
    item('address','Address',{control:'group',gap:8,extra:'Two fields share the same label and validation area.',children:[
      item('city',undefined,{noStyle:true,controlWidth:180,value:'Hangzhou'}),
      item('postcode',undefined,{noStyle:true,controlWidth:180,placeholder:'Postcode'}),
    ]}),item('submit',null,{control:'button',text:'Submit'})]})]),
  'nested-items': scene([form('nested',{labelCol:{span:6},wrapperCol:{span:18},items:[
    item('name','Full name',{control:'group',gap:8,children:[
      item('given','Given name',{layout:'vertical',controlWidth:200,required:true}),
      item('family','Family name',{layout:'vertical',controlWidth:200,help:'As shown on your ID.'}),
    ]}),item('email','Email',{required:true}),item('submit',null,{control:'button',text:'Submit'})]})]),
  'dynamic-list': scene([form('list',{labelCol:{span:6},wrapperCol:{span:18},items:[],list:{name:'users',gap:8,
    initialValue:[{first:'Ada',last:'Lovelace'},{first:'Grace',last:'Hopper'}],
    move:[1,0],children:[item('first',undefined,{noStyle:true,controlWidth:180,placeholder:'First name'}),
      item('last',undefined,{noStyle:true,controlWidth:180,placeholder:'Last name'})]}})]),
  semantic: scene([form('semantic',{styles:{label:{color:'#531dab',backgroundColor:'#f9f0ff',paddingInlineStart:4,paddingInlineEnd:4},
    content:{paddingInlineStart:12,backgroundColor:'#f6ffed'},help:{backgroundColor:'#fff1f0'},extra:{backgroundColor:'#f0f5ff'}},
    items:[item('username','Username',{required:true}),item('email','Email',{required:true,status:'error',feedback:true,
      help:'Please enter Email',extra:'This description stays aligned with the control column.'})]})]),
};
const destination = path.resolve(process.argv[2] ?? path.join(root, 'fixtures/form/visual-cases.json'));
fs.mkdirSync(path.dirname(destination), {recursive: true});
fs.writeFileSync(destination, JSON.stringify(cases, null, 2) + '\n');
console.log(`Wrote ${Object.keys(cases).length} shared Form scenes to ${destination}`);
