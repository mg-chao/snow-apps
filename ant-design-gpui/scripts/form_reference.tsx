// Copyright (C) 2026 mg-chao
// SPDX-License-Identifier: Apache-2.0
// Executes the genuine upstream Form, controls, theme algorithms, and styles.
import React from 'react';
import {createRoot} from 'react-dom/client';
import {flushSync} from 'react-dom';
import Form from 'antd-reference/form';
import Input from 'antd-reference/input';
import Checkbox from 'antd-reference/checkbox';
import Switch from 'antd-reference/switch';
import Button from 'antd-reference/button';
import ConfigProvider from 'antd-reference/config-provider';
import theme from 'antd-reference/theme';
import enUS from 'antd-reference/locale/en_US';
import cases from '../fixtures/form/visual-cases.json';

const query = new URLSearchParams(location.search);
const scene = query.get('scene') ?? 'horizontal';
const themeName = query.get('theme') ?? 'default';
const direction = query.get('direction') ?? 'ltr';
const sample = cases[scene];
if (!sample) throw Error(`Unknown Form scene ${scene}`);
const algorithm = themeName === 'dark' ? theme.darkAlgorithm
  : themeName === 'compact' ? theme.compactAlgorithm
  : themeName === 'dark-compact' ? [theme.darkAlgorithm, theme.compactAlgorithm]
  : theme.defaultAlgorithm;

function control(item: any) {
  const shared = {disabled: item.disabled, placeholder: item.placeholder,
    style: item.controlWidth ? {width: item.controlWidth} : undefined};
  if (item.control === 'password') return <Input.Password {...shared}/>;
  if (item.control === 'textarea') return <Input.TextArea {...shared} rows={item.rows ?? 3}/>;
  if (item.control === 'checkbox') return <Checkbox disabled={item.disabled}>{item.text}</Checkbox>;
  if (item.control === 'switch') return <Switch disabled={item.disabled} size={item.size}
    loading={item.loading} style={{opacity:item.opacity}}
    checkedChildren={item.customContent?<span style={{display:'block',width:12,height:22,background:'#e54242'}}/>:undefined}
    unCheckedChildren={item.customContent?<span style={{display:'block',width:12,height:22,background:'#e54242'}}/>:undefined}/>;
  if (item.control === 'button') return <Button disabled={item.disabled} type="primary" htmlType="submit">{item.text}</Button>;
  return <Input {...shared} data-child={item.childId}/>;
}
function Item({item,child=false,namePrefix=[]}:any) {
  if(item.control==='group')return <Form.Item className={child?'fixture-child-item':'fixture-form-item'}
    data-item={item.id} label={item.label} layout={item.layout} help={item.help} extra={item.extra}>
    <div data-form-group={item.id} style={{display:'flex',gap:item.gap??8,alignItems:'baseline'}}>
      {item.children.map((entry:any)=><Item key={entry.id} item={entry} child namePrefix={namePrefix}/>)}</div>
  </Form.Item>;
  return <Form.Item className={child?'fixture-child-item':'fixture-form-item'} data-item={item.id}
    name={item.control==='button'?undefined:[...namePrefix,item.id]} noStyle={item.noStyle}
    style={child&&!item.noStyle?{width:item.controlWidth,marginBottom:0}:undefined}
    valuePropName={['checkbox','switch'].includes(item.control)?'checked':'value'}
    label={item.label} layout={item.layout} colon={item.colon} tooltip={item.tooltip}
    required={item.required} rules={item.required?[{required:true}]:undefined}
    validateStatus={item.status} hasFeedback={item.feedback} help={item.help} extra={item.extra}>
    {control({...item,childId:child?item.id:undefined})}
  </Form.Item>;
}
function SampleForm({config}: any) {
  const {id, width, tokens, items, list, opacityBackdrop, ...props} = config;
  const [instance]=Form.useForm();const operations=React.useRef<any>(undefined);
  const values=items.flatMap((item:any)=>item.children??item);
  const initialValues = Object.fromEntries(values.filter((item: any) => item.value !== undefined)
    .map((item: any) => [item.id, item.value]));
  React.useEffect(()=>{
    const fields=values.filter((item:any)=>item.errors).map((item:any)=>({name:item.id,errors:item.errors}));
    if(fields.length)instance.setFields(fields);
    if(list?.move)operations.current.move(...list.move);
  },[]);
  const component = <Form {...props} form={instance} name={id} initialValues={initialValues} autoComplete="off">
    {items.map((item:any)=><Item key={item.id} item={item}/>)}
    {list&&<Form.List name={list.name} initialValue={list.initialValue}>
      {(fields,operation)=>{operations.current=operation;return fields.map(field=><Item key={field.key}
        item={{id:`row-${field.key}`,label:`User ${field.name+1}`,control:'group',gap:list.gap,children:list.children}}
        namePrefix={[field.name]}/>);}}
    </Form.List>}
  </Form>;
  return <div className="fixture-form" data-form={id} style={{width, maxWidth: '100%', flexShrink: 0}}>
    {tokens ? <ConfigProvider theme={{components: {Form: tokens}}}>{component}</ConfigProvider> : component}
  </div>;
}
function Body() {
  const {token} = theme.useToken();
  (window as any).__formTokens = token;
  return <main style={{boxSizing: 'border-box', width: sample.viewport.width, height: sample.viewport.height,
    padding: 20, display: 'flex', flexDirection: 'column', alignItems: 'flex-start', gap: 24,
    background: sample.background??token.colorBgContainer, color: token.colorText, fontFamily: token.fontFamily,
    fontSize: token.fontSize, lineHeight: token.lineHeight, direction}}>
    {sample.forms.map((config: any) => <SampleForm key={config.id} config={config}/>)}
  </main>;
}
function bounds(element: Element | null) {
  if (!element) return null;
  const r = element.getBoundingClientRect(), s = getComputedStyle(element);
  return {x:r.x,y:r.y,width:r.width,height:r.height,color:s.color,background:s.backgroundColor,
    fontFamily:s.fontFamily,fontSize:parseFloat(s.fontSize),lineHeight:parseFloat(s.lineHeight),
    fontWeight:s.fontWeight,padding:[s.paddingTop,s.paddingRight,s.paddingBottom,s.paddingLeft].map(parseFloat),
    margin:[s.marginTop,s.marginRight,s.marginBottom,s.marginLeft].map(parseFloat),
    border:[s.borderTopWidth,s.borderRightWidth,s.borderBottomWidth,s.borderLeftWidth].map(parseFloat),
    borderColor:s.borderColor,borderRadius:s.borderRadius,opacity:parseFloat(s.opacity),text:element.textContent,
    display:s.display,alignItems:s.alignItems,textAlign:s.textAlign};
}
function textRects(element: Element) {
  const rectangles: any[] = [];
  const walker = document.createTreeWalker(element, NodeFilter.SHOW_TEXT);
  let text: Node | null;
  while ((text = walker.nextNode())) {
    const range = document.createRange(); range.selectNodeContents(text);
    for (const r of range.getClientRects()) rectangles.push({x:r.x,y:r.y,width:r.width,height:r.height});
  }
  return rectangles;
}
function pseudo(element: Element | null, name: string) {
  if (!element) return null;
  const s = getComputedStyle(element, name);
  return {content:s.content,display:s.display,visibility:s.visibility,color:s.color,
    fontSize:parseFloat(s.fontSize),lineHeight:s.lineHeight,
    margin:[s.marginTop,s.marginRight,s.marginBottom,s.marginLeft].map(parseFloat)};
}
function editorTextRect(element: Element | null) {
  if (!(element instanceof HTMLInputElement || element instanceof HTMLTextAreaElement)) return null;
  let text = element.value || element.placeholder;
  if (!text || ['checkbox','radio'].includes(element.type)) return null;
  if (element instanceof HTMLInputElement && element.type === 'password' && element.value) text='•'.repeat(element.value.length);
  const r=element.getBoundingClientRect(),s=getComputedStyle(element),canvas=document.createElement('canvas');
  const ctx=canvas.getContext('2d')!;ctx.font=`${s.fontStyle} ${s.fontWeight} ${s.fontSize} ${s.fontFamily}`;
  const left=parseFloat(s.borderLeftWidth)+parseFloat(s.paddingLeft),right=parseFloat(s.borderRightWidth)+parseFloat(s.paddingRight);
  const width=Math.min(ctx.measureText(text).width,r.width-left-right),height=parseFloat(s.lineHeight);
  return {x:s.direction==='rtl'?r.right-right-width:r.left+left,y:r.top+(r.height-height)/2,width,height};
}
function describeItem(item:Element,id?:string):any {
  const label=item.querySelector(':scope > .ant-row > .ant-form-item-label');
  const labelText=label?.querySelector(':scope > label')??null;
  const content=item.querySelector(':scope > .ant-row > .ant-form-item-control');
  const visual=content?.querySelector('.ant-input-affix-wrapper,.ant-checkbox-wrapper,.ant-switch,.ant-btn,textarea.ant-input,input.ant-input')??null;
  const editor=content?.querySelector('input:not([type=checkbox]),textarea')??null;
  const group=content?.querySelector(':scope > .ant-form-item-control-input > .ant-form-item-control-input-content > [data-form-group]');
  const help=content?.querySelector(':scope > .ant-form-item-additional > .ant-form-item-explain')??null;
  return {id:id??item.querySelector(':scope > .ant-row')?.getAttribute('data-item'),root:bounds(item),
    row:bounds(item.querySelector(':scope > .ant-row')),label:bounds(label),label_text:bounds(labelText),
    control:bounds(content),input:bounds(visual),help:bounds(help),help_items:[...(help?.children??[])].map(bounds),
    extra:bounds(content?.querySelector(':scope > .ant-form-item-additional > .ant-form-item-extra')??null),
    feedback:bounds(item.querySelector('.ant-form-item-feedback-icon svg')),
    feedback_wrapper:bounds(item.querySelector('.ant-form-item-feedback-icon')),
    tooltip:bounds(item.querySelector('.ant-form-item-tooltip')),icons:[...item.querySelectorAll('.anticon')].map(bounds),
    required_mark:pseudo(labelText,'::before'),colon:pseudo(labelText,'::after'),textRects:textRects(item),
    editor_text_rect:editorTextRect(editor),attributes:{required:(editor??visual)?.getAttribute('aria-required'),
      invalid:(editor??visual)?.getAttribute('aria-invalid'),describedby:(editor??visual)?.getAttribute('aria-describedby')},
    children:[...(group?.children??[])].map(child=>child.classList.contains('fixture-child-item')?describeItem(child):
      ({id:child.getAttribute('data-child'),root:null,label:null,control:null,input:bounds(child),help:null,extra:null,feedback:null,
        editor_text_rect:editorTextRect(child),children:[]})),
  };
}
(window as any).captureFormMetrics = () => {
  const tokens = (window as any).__formTokens;
  return {scene,theme:themeName,direction,scale:devicePixelRatio,viewport:sample.viewport,
    background:sample.background??tokens.colorBgContainer,actual_viewport:{width:innerWidth,height:innerHeight},
    tokens:Object.fromEntries(['fontSize','lineHeight','controlHeight','controlHeightSM','controlHeightLG',
      'margin','marginXXS','marginXS','marginLG','paddingXS','colorText','colorTextHeading',
      'colorTextDescription','colorError','colorWarning','colorSuccess','colorPrimary',
      'colorBorder','colorBgContainer','screenXSMax'].map(name => [name,tokens[name]])),
    forms:[...document.querySelectorAll('.fixture-form')].map(wrapper => {
      const form = wrapper.querySelector('.ant-form')!;
      return {id:wrapper.getAttribute('data-form'),root:bounds(form),wrapper:bounds(wrapper),
        items:[...form.querySelectorAll(':scope > .fixture-form-item')].map(item=>describeItem(item))};
    })};
};
flushSync(() => createRoot(document.getElementById('root')!).render(
  <ConfigProvider direction={direction as any} locale={enUS} theme={{algorithm,token:{motion:false}}}><Body/></ConfigProvider>));
// Freeze the reference loading glyph at the same initial phase for repeatability.
const freeze = document.createElement('style');freeze.textContent='*,*::before,*::after{animation-play-state:paused!important}';document.head.append(freeze);
document.fonts.ready.then(() => requestAnimationFrame(() => requestAnimationFrame(() => {
  const ready = document.createElement('script');ready.id='visual-ready';ready.type='application/json';
  ready.textContent=JSON.stringify((window as any).captureFormMetrics());document.body.append(ready);
})));
