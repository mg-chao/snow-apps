// Copyright (C) 2026 mg-chao
// SPDX-License-Identifier: Apache-2.0
// This fixture executes the original Ant Design component and its real styles.
import React from 'react';
import {createRoot} from 'react-dom/client';
import {flushSync} from 'react-dom';
import ColorPicker from 'antd-reference/color-picker';
import ConfigProvider from 'antd-reference/config-provider';
import theme from 'antd-reference/theme';
import Button from 'antd-reference/button';
import {DownOutlined} from '@ant-design/icons';
import {FormItemInputContext} from 'antd-reference/form/context';
import ColorPickerPanel from 'antd-reference/color-picker/ColorPickerPanel';
import {AggregationColor} from 'antd-reference/color-picker/color';
import useColorPickerStyle from 'antd-reference/color-picker/style';
import cases from '../fixtures/color_picker/visual-cases.json';

const query = new URLSearchParams(location.search);
const scene = query.get('scene') ?? 'solid-hex';
const themeName = query.get('theme') ?? 'default';
const direction = query.get('direction') ?? 'ltr';
if (!Object.hasOwn(cases, scene)) throw Error(`Unknown ColorPicker scene: ${scene}`);
const algorithm = themeName === 'dark' ? theme.darkAlgorithm
  : themeName === 'compact' ? theme.compactAlgorithm
  : themeName === 'dark-compact' ? [theme.darkAlgorithm, theme.compactAlgorithm]
  : theme.defaultAlgorithm;
const presets = [
  {key: 'recommended', label: 'Recommended', colors: ['#f5222d', '#fa8c16', '#fadb14', '#52c41a', '#13c2c2', '#1677ff', '#722ed1', '#eb2f96']},
  {key: 'recent', label: 'Recent', colors: ['#1677ff80', '#ffffff', '#000000', [{color:'#1677ff',percent:0},{color:'#722ed1',percent:100}]], defaultOpen: false},
];
const overrides = {colorPrimary:'#722ed1',colorPrimaryHover:'#9254de',colorBorder:'#b37feb',controlOutline:'rgba(114, 46, 209, 0.15)',borderRadius:10};
function StandalonePanel({sample}) {
  const {token}=theme.useToken();
  const [hashId,cssVarCls]=useColorPickerStyle('ant-color-picker','ant-color-picker-css-var');
  const noop=()=>{};
  return <div className={`color-picker-standalone ant-color-picker ant-color-picker-css-var ${cssVarCls} ${hashId} ${direction==='rtl'?'ant-color-picker-rtl':''}`}
    style={{padding:token.paddingSM,background:token.colorBgElevated,borderRadius:token.borderRadiusLG}}>
    <ColorPickerPanel prefixCls="ant-color-picker" value={new AggregationColor(sample.value)} mode="single" modeOptions={[{value:'single',label:'Single'}]}
      onChange={noop} onChangeComplete={noop} onModeChange={noop} onActive={noop} activeIndex={0} gradientDragging={false} onGradientDragging={noop}/>
  </div>;
}
function Body() {
  const {token} = theme.useToken();
  (window as any).__colorPickerTokens = token;
  return <main style={{boxSizing:'border-box',width:640,height:720,padding:48,paddingLeft:cases[scene][0]?.anchorX??48,display:'flex',alignItems:'flex-start',gap:12,background:token.colorBgContainer,color:token.colorText,fontFamily:token.fontFamily,fontSize:token.fontSize,lineHeight:token.lineHeight}}>
    {cases[scene].map((sample,index) => {
      const {customPanel,splitPanel,standalone,customTrigger,tokenCase,focus,hover,status,anchorX,semantic,textRender,presets:hasPresets,...props} = sample;
      const split=(_,{components:{Picker,Presets}})=><div style={{display:'flex',flexDirection:direction==='rtl'?'row-reverse':'row',gap:12,width:493}}><div style={{width:234,flexShrink:0}}><Presets/></div><div style={{width:1,flexShrink:0,background:token.colorSplit}}/><div style={{width:234,flexShrink:0}}><Picker/></div></div>;
      const picker = standalone ? <StandalonePanel sample={sample}/> : <ColorPicker placement="bottomLeft" {...props} defaultValue={undefined} autoAdjustOverflow={false} presets={hasPresets ? presets : undefined}
        showText={textRender==='custom'?color=><span>Custom Text ({color.toHexString()})</span>:textRender==='icon'?()=> <DownOutlined rotate={sample.open?180:0} style={{color:'rgba(0, 0, 0, 0.25)'}}/>:props.showText}
        styles={semantic?{body:{width:40,height:40,borderRadius:12,backgroundColor:'#00ff00',border:'3px solid #800080'},content:{width:20,height:20,borderRadius:5,backgroundColor:'#ff0000',border:'2px solid #0000ff'}}:undefined}
        panelRender={splitPanel ? split : customPanel ? panel => <div><div style={{fontSize:12,lineHeight:'20px',marginBottom:8}}>Color Picker</div>{panel}</div> : undefined}>
        {customTrigger?<Button>Choose color</Button>:undefined}
      </ColorPicker>;
      return <div className="color-picker-case" data-index={index} data-options={JSON.stringify(sample)} key={index} style={{direction,display:'inline-flex',flexShrink:0}}>
        <FormItemInputContext.Provider value={{status}}>{tokenCase ? <ConfigProvider theme={{components:{ColorPicker:overrides}}}>{picker}</ConfigProvider> : picker}</FormItemInputContext.Provider>
      </div>;
    })}
  </main>;
}
function bounds(element: Element | null) {
  if (!element) return null;
  const r = element.getBoundingClientRect(), s = getComputedStyle(element);
  return {x:r.x,y:r.y,width:r.width,height:r.height,color:s.color,background:s.backgroundColor,backgroundImage:s.backgroundImage,borderColor:s.borderColor,borderWidth:s.borderWidth,borderRadius:s.borderRadius,boxShadow:s.boxShadow,fontSize:parseFloat(s.fontSize),lineHeight:s.lineHeight,opacity:s.opacity,text:element.textContent};
}
const q = (selector:string) => document.querySelector(selector);
const qa = (selector:string) => [...document.querySelectorAll(selector)];
function railThumb(element:Element,kind:string,index:number) {
  const box=element.getBoundingClientRect(),style=getComputedStyle(element,'::after');
  const transform=style.transform==='none'?new DOMMatrixReadOnly():new DOMMatrixReadOnly(style.transform);
  const width=parseFloat(style.width)*Math.hypot(transform.a,transform.b);
  const height=parseFloat(style.height)*Math.hypot(transform.c,transform.d);
  return {kind,index,x:box.x+box.width/2-width/2,y:box.y+box.height/2-height/2,width,height};
}
function textRectangles() {
  const rectangles=[];
  for(const element of qa('.ant-color-picker-trigger-text,.ant-color-picker-input-container,.ant-color-picker-presets-label,.ant-color-picker-operation,.color-picker-case .ant-btn')) {
    const walker=document.createTreeWalker(element,NodeFilter.SHOW_TEXT);
    for(let node=walker.nextNode();node;node=walker.nextNode()) {
      if(!node.textContent?.trim())continue;
      const range=document.createRange();range.selectNodeContents(node);
      for(const r of range.getClientRects())if(r.width&&r.height)rectangles.push({x:r.x,y:r.y,width:r.width,height:r.height});
    }
  }
  // Native input values have no DOM text nodes. Record their actual viewports separately.
  return [...rectangles,...qa('.ant-color-picker-input-container input').map(bounds)];
}
(window as any).captureColorPickerMetrics = () => {
  const sliders=qa('.ant-color-picker-slider').filter(e=>!e.classList.contains('ant-color-picker-gradient-slider'));
  const root=q('.ant-color-picker');
  const panel=q('.ant-popover-container,.color-picker-standalone');
  return {scene,theme:themeName,direction,scale:devicePixelRatio,canvas:{width:640,height:720},
    tokens:Object.fromEntries(['colorBgContainer','colorBgElevated','colorFillSecondary','colorBorder','colorPrimary','colorPrimaryHover','controlOutline','fontFamily','fontSize','fontSizeSM','controlHeight','controlHeightSM','controlHeightLG','borderRadius','borderRadiusSM','marginSM','marginXS'].map(k=>[k,(window as any).__colorPickerTokens[k]])),
    rows:qa('.color-picker-case').map(row=>({...bounds(row),options:JSON.parse(row.getAttribute('data-options')!),trigger:bounds(row.querySelector('.ant-color-picker-trigger,.ant-btn')),swatch:bounds(row.querySelector('.ant-color-picker-trigger .ant-color-picker-color-block,.ant-color-picker-trigger .ant-color-picker-clear')),description:bounds(row.querySelector('.ant-color-picker-trigger-text'))})),
    geometry:{panel:bounds(panel),palette:bounds(q('.ant-color-picker-palette')),hue:bounds(sliders[0]?.querySelector('.ant-color-picker-slider-rail')),alpha:bounds(sliders[1]?.querySelector('.ant-color-picker-slider-rail')),preview:bounds(q('.ant-color-picker-slider-container .ant-color-picker-color-block')),gradient:bounds(q('.ant-color-picker-gradient-slider .ant-color-picker-slider-rail')),editors:qa('.ant-color-picker-input-container .ant-color-picker-hex-input,.ant-color-picker-input-container .ant-input-number').map(bounds),presets:qa('.ant-color-picker-presets-color').filter(e=>e.getBoundingClientRect().height>0).map(bounds),
      handles:[...qa('.ant-color-picker-handler').map((e,index)=>({kind:'palette',index,...bounds(e)})),...sliders.flatMap((slider,i)=>[...slider.querySelectorAll('.ant-color-picker-slider-handle')].map((e,index)=>railThumb(e,i===0?'hue':'alpha',index))),...qa('.ant-color-picker-gradient-slider .ant-color-picker-slider-handle').map((e,index)=>railThumb(e,'gradient',index))]},
    elements:qa('.ant-color-picker-trigger,.ant-color-picker-color-block,.ant-color-picker-color-block-inner,.ant-color-picker-clear,.ant-color-picker-palette,.ant-color-picker-handler,.ant-color-picker-slider,.ant-color-picker-slider-rail,.ant-color-picker-slider-handle,.ant-color-picker-input-container,.ant-color-picker-format-select,.ant-color-picker-presets-color,.ant-color-picker-operation,.ant-popover-container').map(e=>({className:e.className,...bounds(e),after:{...bounds(e),...(()=>{const s=getComputedStyle(e,'::after');return {width:s.width,height:s.height,background:s.backgroundColor,boxShadow:s.boxShadow,borderColor:s.borderColor,borderWidth:s.borderWidth,transform:s.transform};})()}})),
    inputs:qa('.ant-color-picker-input-container input').map(e=>({value:(e as HTMLInputElement).value,...bounds(e)})),
    raster_regions:{text_and_input_viewports:textRectangles(),arrow:bounds(q('.ant-popover-arrow'))},
    activeElement:document.activeElement?.className,root:bounds(root)};
};
flushSync(()=>createRoot(document.getElementById('root')!).render(<ConfigProvider direction={direction as any} theme={{algorithm,token:{motion:false}}}><Body/></ConfigProvider>));
requestAnimationFrame(()=>requestAnimationFrame(()=>setTimeout(()=>{
  if(cases[scene][0]?.focus==='hex') (q('.ant-color-picker-input-container .ant-input') as HTMLElement)?.focus();
  if(cases[scene][0]?.focus==='hue') (q('.ant-color-picker-slider:not(.ant-color-picker-gradient-slider) .ant-color-picker-slider-handle') as HTMLElement)?.focus();
  requestAnimationFrame(()=>{
    const ready=document.createElement('script');ready.id='visual-ready';ready.type='application/json';ready.textContent=JSON.stringify((window as any).captureColorPickerMetrics());document.body.append(ready);
  });
},80)));
