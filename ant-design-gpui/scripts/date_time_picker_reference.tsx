// Copyright (C) 2026 mg-chao
// SPDX-License-Identifier: Apache-2.0
// Executes the actual pinned Ant Design source, independently of native GPUI.
import React from 'react';
import {createRoot} from 'react-dom/client';
import {flushSync} from 'react-dom';
import dayjs from 'dayjs';
import 'dayjs/locale/zh-cn';
import 'dayjs/locale/zh-tw';
import DatePicker from 'antd-reference/date-picker';
import TimePicker from 'antd-reference/time-picker';
import ConfigProvider from 'antd-reference/config-provider';
import theme from 'antd-reference/theme';
import enUS from 'antd-reference/locale/en_US';
import zhCN from 'antd-reference/locale/zh_CN';
import zhTW from 'antd-reference/locale/zh_TW';
import manifest from '../fixtures/date_time_picker/visual-cases.json';

const NativeDate = Date;
const epoch = NativeDate.parse(manifest.clock);
class FixedDate extends NativeDate {
  constructor(...args: any[]) { super(...(args.length ? args : [epoch]) as [any]); }
  static now() { return epoch; }
}
(globalThis as any).Date = FixedDate;
const query = new URLSearchParams(location.search);
const scene = query.get('scene') ?? 'date-selected';
const themeName = query.get('theme') ?? 'default';
const direction = query.get('direction') ?? 'ltr';
const originalSample: any = manifest.scenes[scene];
if (!originalSample) throw Error(`Unknown picker scene ${scene}`);
const probeStep=query.get('millisecondStep');
if(probeStep!==null&&(!Number.isInteger(Number(probeStep))||Number(probeStep)<1||Number(probeStep)>1000))throw Error('Invalid probe millisecond step');
const probeEmpty=query.get('emptyMultiple')==='true';
const probeWidth=query.get('triggerWidth');
if(probeWidth!==null&&(!Number.isFinite(Number(probeWidth))||Number(probeWidth)<80))throw Error('Invalid probe trigger width');
const sample: any = {...originalSample,...(probeStep===null?{}:{millisecondStep:Number(probeStep)}),
  ...(probeEmpty?{value:[]}:{}),...(probeWidth===null?{}:{triggerWidth:Number(probeWidth)})};
const algorithm = themeName === 'dark' ? theme.darkAlgorithm : themeName === 'compact' ? theme.compactAlgorithm
  : themeName === 'dark-compact' ? [theme.darkAlgorithm, theme.compactAlgorithm] : theme.defaultAlgorithm;
const value = (v: any): any => Array.isArray(v) ? v.map(value) : v == null ? null : dayjs(v);
const locale = sample.locale === 'zh_CN' ? zhCN : sample.locale === 'zh_TW' ? zhTW : enUS;
dayjs.locale(sample.locale === 'zh_CN' ? 'zh-cn' : sample.locale === 'zh_TW' ? 'zh-tw' : 'en');
const disabledTime = () => ({disabledHours: () => [0, 1, 2, 3, 23], disabledMinutes: () => [0, 1, 2, 58, 59],
  disabledSeconds: () => [0, 1, 2, 58, 59], disabledMilliseconds: () => [0, 20, 980]});
function Body() {
  const {token} = theme.useToken();
  (window as any).__pickerTokens = token;
  const {component, range, value: raw, hoverDate, hoverTrigger, focusInput, disabledDates, disabledTime: disableTime,
    tokenCase, presets, extraFooter, cellRender, triggerWidth, locale: _, ...rest} = sample;
  const Component: any = component === 'time' ? range ? TimePicker.RangePicker : TimePicker : range ? DatePicker.RangePicker : DatePicker;
  const props: any = {...rest, style:triggerWidth?{width:triggerWidth}:undefined,value: value(raw), defaultPickerValue: dayjs('2024-02-15T13:24:35.120'),
    allowClear: true,
    disabledDate: disabledDates ? (date: any) => date.day() === 0 || date.day() === 6 : undefined,
    disabledTime: disableTime ? disabledTime : undefined,
    minDate: sample.minDate ? dayjs(sample.minDate) : undefined, maxDate: sample.maxDate ? dayjs(sample.maxDate) : undefined,
    presets: presets ? range ? [{label:'This week', value:[dayjs('2024-02-12'),dayjs('2024-02-18')]},
      {label:'This month',value:[dayjs('2024-02-01'),dayjs('2024-02-29')]}] : [{label:'Today',value:dayjs(manifest.clock)},
      {label:'Tomorrow',value:dayjs('2024-02-16')}] : undefined,
    renderExtraFooter: extraFooter ? () => 'Choose an available date' : undefined,
    cellRender: cellRender ? (current: any, info: any) => info.type === 'date' && [1,15].includes(current.date())
      ? <div className="ant-picker-cell-inner" style={{border:'1px solid #1677ff',borderRadius:4}}>{current.date()}</div> : info.originNode : undefined};
  return <main style={{boxSizing:'border-box',width:manifest.canvas.width,height:manifest.canvas.height,
    padding:manifest.canvas.padding,background:token.colorBgContainer,color:token.colorText,fontFamily:token.fontFamily,
    fontSize:token.fontSize,lineHeight:token.lineHeight}}><Component {...props}/></main>;
}
const q = (selector: string) => document.querySelector(selector);
const qa = (selector: string) => [...document.querySelectorAll(selector)];
function bounds(element: Element | null) {
  if (!element) return null;
  const r = element.getBoundingClientRect(), s = getComputedStyle(element);
  return {x:r.x,y:r.y,width:r.width,height:r.height,color:s.color,background:s.backgroundColor,
    borderColor:s.borderColor,borderWidth:s.borderWidth,borderRadius:s.borderRadius,boxShadow:s.boxShadow,
    fontSize:parseFloat(s.fontSize),lineHeight:s.lineHeight,text:element.textContent,visibility:s.visibility,opacity:s.opacity};
}
function pseudo(element: Element, side: string) {
  const style = getComputedStyle(element, side);
  return {content:style.content,display:style.display,position:style.position,top:style.top,left:style.left,right:style.right,
    bottom:style.bottom,width:style.width,height:style.height,background:style.backgroundColor,borderColor:style.borderColor,
    borderWidth:style.borderWidth,borderStyle:style.borderStyle,borderRadius:style.borderRadius};
}
function has(element: Element, suffix: string) { return element.classList.contains(`ant-picker-cell-${suffix}`); }
function weekRowState(element: Element) {
  const row=element.closest('tr.ant-picker-week-panel-row');
  return row ? Object.fromEntries([['weekSelected','selected'],['weekRangeStart','range-start'],['weekRangeEnd','range-end'],
    ['weekRangeHover','range-hover'],['weekHover','hover']].map(([key,suffix])=>[key,row.classList.contains(`ant-picker-week-panel-row-${suffix}`)])) : {};
}
function visible(element: Element) { const r = element.getBoundingClientRect(); return r.width > 0 && r.height > 0; }
function textBounds(element: Element | null) {
  if(!element)return null;const range=document.createRange();range.selectNodeContents(element);const r=range.getBoundingClientRect();
  return {x:r.x,y:r.y,width:r.width,height:r.height};
}
function timeColumnGeometry(element: Element, index: number) {
  const e=element as HTMLElement,b=e.getBoundingClientRect(),style=getComputedStyle(e);
  const borderLeft=parseFloat(style.borderLeftWidth),borderRight=parseFloat(style.borderRightWidth);
  const scrollbarWidth=e.offsetWidth-e.clientWidth-borderLeft-borderRight;
  const side=e.clientLeft-borderLeft>scrollbarWidth/2?'left':'right';
  return {index,...bounds(e),scrollTop:e.scrollTop,scrollHeight:e.scrollHeight,clientHeight:e.clientHeight,
    clientWidth:e.clientWidth,clientLeft:e.clientLeft,scrollbarColor:style.scrollbarColor,
    viewport:{x:b.x+e.clientLeft,y:b.y+e.clientTop,width:e.clientWidth,height:e.clientHeight},
    scrollbar:scrollbarWidth>0&&e.scrollHeight>e.clientHeight?{x:side==='left'?b.x+borderLeft:b.right-borderRight-scrollbarWidth,
      y:b.y+e.clientTop,width:scrollbarWidth,height:e.clientHeight,side}:null};
}
(window as any).captureDateTimePickerMetrics = () => ({scene,theme:themeName,direction,scale:devicePixelRatio,
  clock:manifest.clock,timezone:manifest.timezone,canvas:manifest.canvas,options:sample,
  tokens:Object.fromEntries(['colorBgContainer','colorBgElevated','colorBorder','colorPrimary','colorPrimaryBg','colorText',
    'colorTextDisabled','controlHeight','controlHeightSM','controlHeightLG','fontSize','borderRadius','borderRadiusLG']
    .map(key => [key,(window as any).__pickerTokens[key]])),
  geometry:{trigger:bounds(q('main .ant-picker')),inputs:qa('main .ant-picker input').map(e=>({...bounds(e),value:(e as HTMLInputElement).value,
    auxiliary:e.classList.contains('ant-picker-multiple-input')})),
    prefix:bounds(q('main .ant-picker-prefix')),prefixText:textBounds(q('main .ant-picker-prefix')),
    placeholder:bounds(q('.ant-picker-selection-placeholder')),placeholderText:textBounds(q('.ant-picker-selection-placeholder')),
    popup:bounds(q('.ant-picker-dropdown:not(.ant-picker-dropdown-hidden) .ant-picker-panel-container')),
    rangeArrow:bounds(q('.ant-picker-dropdown:not(.ant-picker-dropdown-hidden) .ant-picker-range-arrow')),
    panels:qa('.ant-picker-panel').filter(visible).map(bounds),headers:qa('.ant-picker-header').filter(visible).map(bounds),
    tables:qa('.ant-picker-content').filter(visible).map(bounds),
    cells:qa('td.ant-picker-cell').filter(visible).map((e,index)=>({key:e.getAttribute('title')??`${index}:${e.textContent}`,
      title:e.getAttribute('title'),text:e.textContent,selected:has(e,'selected'),today:has(e,'today'),disabled:has(e,'disabled'),
      inView:has(e,'in-view'),inRange:has(e,'in-range'),rangeStart:has(e,'range-start'),rangeEnd:has(e,'range-end'),
      rangeHover:has(e,'range-hover')||has(e,'range-hover-start')||has(e,'range-hover-end'),
      ...weekRowState(e),
      rect:bounds(e),inner:bounds(e.querySelector('.ant-picker-cell-inner')),before:pseudo(e,'::before'),
      innerBefore:e.querySelector('.ant-picker-cell-inner')?pseudo(e.querySelector('.ant-picker-cell-inner')!,'::before'):null})),
    timeColumns:qa('.ant-picker-time-panel-column').filter(visible).map(timeColumnGeometry),
    timeViewports:qa('.ant-picker-time-panel-column').filter(visible).map((e,index)=>timeColumnGeometry(e,index).viewport),
    scrollbars:qa('.ant-picker-time-panel-column').filter(visible).map((e,column)=>({column,...timeColumnGeometry(e,column)}))
      .filter(column=>column.scrollbar).map(column=>({column:column.column,track:column.scrollbar,side:column.scrollbar!.side,
        scrollTop:column.scrollTop,scrollHeight:column.scrollHeight,clientHeight:column.clientHeight})),
    timeCells:qa('.ant-picker-time-panel-column').filter(visible).flatMap((column,col)=>[...column.querySelectorAll('li')].filter(e=>{
      const a=e.getBoundingClientRect(),b=column.getBoundingClientRect();return a.bottom>b.top&&a.top<b.bottom;
    }).map(e=>({key:`${col}:${e.getAttribute('data-value')??e.textContent}`,column:col,value:e.getAttribute('data-value'),
      text:e.textContent,selected:e.classList.contains('ant-picker-time-panel-cell-selected'),
      disabled:e.classList.contains('ant-picker-time-panel-cell-disabled'),rect:bounds(e),inner:bounds(e.firstElementChild)}))),
    footer:bounds(q('.ant-picker-footer')),presets:bounds(q('.ant-picker-presets')),activeBar:bounds(q('.ant-picker-active-bar')),
    footerActions:qa('.ant-picker-footer button.ant-btn').filter(visible).map(e=>({key:'ok',label:e.textContent,rect:bounds(e)})),
    tags:qa('.ant-picker-selection-item').filter(visible).map((e,index)=>({key:index,label:e.querySelector('.ant-picker-selection-item-content')?.textContent,
      rect:bounds(e),content:bounds(e.querySelector('.ant-picker-selection-item-content')),remove:bounds(e.querySelector('.ant-picker-selection-item-remove svg'))}))},
  elements:qa('.ant-picker,.ant-picker-panel-container,.ant-picker-header,.ant-picker-cell-inner,.ant-picker-footer,.ant-picker-time-panel-cell-inner')
    .filter(visible).map(e=>({className:e.className,...bounds(e)})),
  activeElement:document.activeElement?.className,
  dropdown:{className:q('.ant-picker-dropdown:not(.ant-picker-dropdown-hidden)')?.className,
    style:q('.ant-picker-dropdown:not(.ant-picker-dropdown-hidden)')?.getAttribute('style')}});
flushSync(()=>createRoot(document.getElementById('root')!).render(<ConfigProvider direction={direction as any} locale={locale}
  theme={{algorithm,token:{motion:false},components:sample.tokenCase?{DatePicker:{cellWidth:40,cellHeight:28,cellActiveWithRangeBg:'#efdbff',
    colorPrimary:'#722ed1',timeColumnWidth:64,timeCellHeight:32}}:undefined}}><Body/></ConfigProvider>));
requestAnimationFrame(()=>requestAnimationFrame(()=>setTimeout(()=>{
  if(sample.focusInput!==undefined)(qa('main .ant-picker input')[sample.focusInput] as HTMLElement)?.focus();
  requestAnimationFrame(()=>{
    const ready=document.createElement('script');ready.id='visual-ready';ready.type='application/json';
    ready.textContent=JSON.stringify((window as any).captureDateTimePickerMetrics());document.body.append(ready);
  });
},180)));
