// Copyright (C) 2026 mg-chao
// SPDX-License-Identifier: Apache-2.0
import React from 'react';
import {createRoot} from 'react-dom/client';
import {flushSync} from 'react-dom';
import Descriptions from 'antd-reference/descriptions';
import ConfigProvider from 'antd-reference/config-provider';
import theme from 'antd-reference/theme';
import cases from '../fixtures/descriptions/visual-cases.json';

const query = new URLSearchParams(location.search);
const scene = query.get('scene') ?? 'horizontal';
const themeName = query.get('theme') ?? 'default';
const direction = query.get('direction') ?? 'ltr';
const algorithm = themeName === 'dark' ? theme.darkAlgorithm
  : themeName === 'compact' ? theme.compactAlgorithm
  : themeName === 'dark-compact' ? [theme.darkAlgorithm, theme.compactAlgorithm]
  : theme.defaultAlgorithm;
const sample = cases[scene];

function node(value: any): React.ReactNode {
  if (value && typeof value === 'object') {
    if (value.descriptions) return component(value.descriptions);
    if (value.block) return <div style={{width: value.width, height: value.height,
      background: value.background, display: 'flex', alignItems: 'center'}}>{value.block}</div>;
  }
  if (typeof value === 'string' && value.includes('\n')) {
    return value.split('\n').map((line, index) => <React.Fragment key={index}>{index > 0 && <br/>}{line}</React.Fragment>);
  }
  return value;
}
function component(props: any) {
  const {id, width, tokens, globalTokens, shrinkToFit, items, ...rest} = props;
  const result = <Descriptions {...rest} title={node(props.title)} extra={node(props.extra)}
    items={items.map((item: any, index: number) => ({...item,
      className: `fixture-item-${index}`,
      label: node(item.label), children: node(item.content),
    }))}/>;
  return tokens || globalTokens ? <ConfigProvider theme={{token:globalTokens,components: {Descriptions: tokens}}}>{result}</ConfigProvider> : result;
}
function Body() {
  const {token} = theme.useToken();
  (window as any).__descriptionsTokens = token;
  return <main style={{boxSizing: 'border-box', width: sample.viewport.width,
    height: sample.viewport.height, padding: 20, display: 'flex', flexDirection: 'column',
    alignItems: direction === 'rtl' ? 'flex-end' : 'flex-start', gap: 24,
    background: token.colorBgContainer, color: token.colorText,
    fontFamily: token.fontFamily, fontSize: token.fontSize, lineHeight: token.lineHeight, direction}}>
    {sample.components.map((props: any) => <div key={props.id} className="descriptions-case"
      data-id={props.id} style={{width: props.width, maxWidth: '100%', flexShrink: 0,
        ...(props.shrinkToFit ? {width: 'fit-content'} : {})}}>{component(props)}</div>)}
  </main>;
}
function bounds(element: Element | null) {
  if (!element) return null;
  const r = element.getBoundingClientRect(), s = getComputedStyle(element);
  return {x:r.x,y:r.y,width:r.width,height:r.height,
    color:s.color,background:s.backgroundColor,fontFamily:s.fontFamily,
    fontSize:parseFloat(s.fontSize),lineHeight:parseFloat(s.lineHeight),fontWeight:s.fontWeight,
    padding:[s.paddingTop,s.paddingRight,s.paddingBottom,s.paddingLeft].map(parseFloat),
    margin:[s.marginTop,s.marginRight,s.marginBottom,s.marginLeft].map(parseFloat),
    border:[s.borderTopWidth,s.borderRightWidth,s.borderBottomWidth,s.borderLeftWidth].map(parseFloat),
    borderColor:s.borderColor,borderRadius:s.borderRadius,verticalAlign:s.verticalAlign,
    display:s.display,tableLayout:s.tableLayout,text:element.textContent};
}
function textRects(element: Element) {
  const rects: any[] = [];
  const walker = document.createTreeWalker(element, NodeFilter.SHOW_TEXT);
  let text: Node | null;
  while ((text = walker.nextNode())) {
    const range = document.createRange(); range.selectNodeContents(text);
    for (const r of range.getClientRects()) rects.push({x:r.x,y:r.y,width:r.width,height:r.height});
  }
  return rects;
}
function describe(root: Element) {
  const direct = (name: string) => root.querySelector(`:scope > .ant-descriptions-${name}`);
  const view = direct('view');
  return {root:bounds(root),header:bounds(direct('header')),
    title:bounds(root.querySelector(':scope > .ant-descriptions-header > .ant-descriptions-title')),
    extra:bounds(root.querySelector(':scope > .ant-descriptions-header > .ant-descriptions-extra')),
    view:bounds(view),table:bounds(view?.querySelector(':scope > table') ?? null),
    rows:[...(view?.querySelectorAll(':scope > table > tbody > tr') ?? [])].map(row => ({
      bounds:bounds(row),cells:[...row.children].map(cell => {
        const label = cell.classList.contains('ant-descriptions-item-label') ? cell
          : cell.querySelector(':scope > .ant-descriptions-item-container > .ant-descriptions-item-label');
        const content = cell.classList.contains('ant-descriptions-item-content') ? cell
          : cell.querySelector(':scope > .ant-descriptions-item-container > .ant-descriptions-item-content');
        const pseudo = label ? getComputedStyle(label, '::after') : null;
        return {bounds:bounds(cell),kind:cell.classList.contains('ant-descriptions-item-label')?'label'
          :cell.classList.contains('ant-descriptions-item-content')?'content':'item',
          item:Number([...cell.classList].find(n => n.startsWith('fixture-item-'))?.replace('fixture-item-','')),
          span:Number(cell.getAttribute('colspan')),label:bounds(label),content:bounds(content),
          colon:pseudo?{content:pseudo.content,marginLeft:pseudo.marginLeft,marginRight:pseudo.marginRight,top:pseudo.top,display:pseudo.display}:null,
          textRects:textRects(cell)};
      }),
    })),
    nested:[...(view?.querySelectorAll('.ant-descriptions') ?? [])].map(nested => ({root:bounds(nested),view:bounds(nested.querySelector(':scope > .ant-descriptions-view'))})),
  };
}
(window as any).captureDescriptionsMetrics = () => ({scene,theme:themeName,direction,
  scale:devicePixelRatio,viewport:sample.viewport,background:(window as any).__descriptionsTokens.colorBgContainer,
  actual_viewport:{width:innerWidth,height:innerHeight,client_width:document.documentElement.clientWidth,
    client_height:document.documentElement.clientHeight,visual_width:visualViewport?.width,
    visual_height:visualViewport?.height},
  media_queries:{
    xs:`(max-width: ${(window as any).__descriptionsTokens.screenXSMax}px)`,
    sm:`(min-width: ${(window as any).__descriptionsTokens.screenSM}px)`,
    xs_matches:matchMedia(`(max-width: ${(window as any).__descriptionsTokens.screenXSMax}px)`).matches,
    sm_matches:matchMedia(`(min-width: ${(window as any).__descriptionsTokens.screenSM}px)`).matches,
  },
  tokens:Object.fromEntries(['fontSize','fontSizeSM','fontSizeLG','lineHeight','lineHeightSM','lineHeightLG',
    'padding','paddingXS','paddingSM','paddingLG','marginXXS','marginXS','borderRadiusLG',
    'colorText','colorTextSecondary','colorTextTertiary','colorFillAlter','colorSplit']
    .map(name => [name,(window as any).__descriptionsTokens[name]])),
  components:[...document.querySelectorAll('.descriptions-case')].map(wrapper => ({
    id:wrapper.getAttribute('data-id'),wrapper:bounds(wrapper),
    ...describe(wrapper.querySelector('.ant-descriptions')!),
  })),
});
flushSync(() => createRoot(document.getElementById('root')!).render(
  <ConfigProvider direction={direction as any} theme={{algorithm,token:{motion:false}}}><Body/></ConfigProvider>));
document.fonts.ready.then(() => requestAnimationFrame(() => requestAnimationFrame(() => {
  const ready = document.createElement('script');ready.id='visual-ready';ready.type='application/json';
  ready.textContent=JSON.stringify((window as any).captureDescriptionsMetrics());document.body.append(ready);
})));
