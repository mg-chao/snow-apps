// Copyright (C) 2026 mg-chao
// SPDX-License-Identifier: Apache-2.0
// Execute the pinned reference color implementation, independently of Rust.
import fs from 'node:fs';
import path from 'node:path';
import crypto from 'node:crypto';
import {createRequire} from 'node:module';
import {fileURLToPath} from 'node:url';

const args=Object.fromEntries(process.argv.slice(2).reduce((a,v,i,all)=>{if(i%2===0)a.push([v.replace(/^--/,''),all[i+1]]);return a;},[]));
const root=path.resolve(path.dirname(fileURLToPath(import.meta.url)),'..');
const dependencies=path.resolve(args.dependencies??'../build/ant-design-gpui/modal-reference/browser');
const require=createRequire(path.join(dependencies,'package.json'));
const picker=require('@rc-component/color-picker/package.json');
const fast=require('@ant-design/fast-color/package.json');
if(picker.version!=='3.1.1'||fast.version!=='3.0.1')throw Error('Color reference dependency version mismatch');
const {Color}=require('@rc-component/color-picker/lib/color');
const sources=['@rc-component/color-picker/lib/color.js','@ant-design/fast-color/lib/FastColor.js','@ant-design/fast-color/lib/presetColors.js'];
const digest=file=>crypto.createHash('sha256').update(fs.readFileSync(file)).digest('hex');
const inputs=['#1677ff','#2ddcb4','#fff','#1234','#722ed180','11223344','red','rebeccapurple','lightgoldenrodyellow',
  'rgb(22, 119, 255)','rgba(22,119,255,.5)','rgb(100% 50% 0% / 50%)','rgb(0 255 255 / 0%)',
  'hsl(120,100%,50%)','hsla(270,60%,40%,.25)','hsl(30deg 80% 60% / 25%)',
  'hsb(215,91%,100%)','hsba(217.125,73.125%,81.25%,.375)','hsv(240 100% 100% / 50%)',
  'hsb(219.125,0%,50%)','hsb(219.125,71.234%,0%)','rgba(0,0,0,0)'];
const record={schema_version:1,upstream:{rc_color_picker:picker.version,fast_color:fast.version},
  generator_sha256:digest(fileURLToPath(import.meta.url)),
  sources:Object.fromEntries(sources.map(name=>[name,digest(require.resolve(name))])),
  excluded_quirks:['Malformed5/7-digit hexadecimal strings','Unknown strings silently becoming black','transparent keyword incorrectly becoming opaque black','Negative hue signs ignored by reference parser'],
  cases:inputs.map(input=>{const c=new Color(input);return{input,rgb:c.toRgb(),hsb:c.toHsb(),hex:c.toHexString(),rgb_string:c.toRgbString(),hsb_string:c.toHsbString()};})};
const output=path.resolve(args.output??path.join(root,'fixtures/color_picker/color-reference.json'));
fs.mkdirSync(path.dirname(output),{recursive:true});
fs.writeFileSync(output,JSON.stringify(record,null,2)+'\n');
console.log(`Generated${record.cases.length} independent color conversion cases`);
