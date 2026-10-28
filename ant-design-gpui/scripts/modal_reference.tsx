// Copyright (C) 2026 mg-chao
// SPDX-License-Identifier: Apache-2.0
// This imports the original pinned component, including its dialog dependency.
import 'antd-reference/style/reset.css';
import React, {useEffect} from 'react';
import {createRoot} from 'react-dom/client';
import Modal from 'antd-reference/modal';
import ConfigProvider from 'antd-reference/config-provider';
import theme from 'antd-reference/theme';

const q = new URLSearchParams(location.search);
const algorithm = q.get('theme') ?? 'default';
const direction = q.get('direction') === 'rtl' ? 'rtl' : 'ltr';
const kind = q.get('kind') ?? 'standard';
const variant = q.get('variant') ?? 'basic';
const body = <>{Array.from({length: variant === 'tall' ? 30 : 3}, (_, i) => <p key={i}>Some contents...</p>)}</>;
const options = {
  title: kind === 'standard' ? 'Basic Modal' : 'Do you want to delete these items?',
  centered: q.get('centered') === 'true',
  loading: q.get('loading') === 'true',
  ...(variant === 'no-footer' ? {footer: null} : {}),
  ...(variant === 'wide' ? {width: 1000} : {}),
  ...(variant === 'no-title' ? {title: null} : {}),
  ...(variant === 'custom-token' ? {okText:'Continue',cancelText:'Back'} : {}),
  mask: {blur: false}, // The agreed native fallback keeps Ant Design's dimming color.
};
function Scene() {
  const [modal, holder] = Modal.useModal();
  useEffect(() => {
    if (kind !== 'standard') modal[kind as 'confirm']({...options, content: 'Some descriptions'});
  }, []);
  return <>{holder}{kind === 'standard' && <Modal open {...options}>{body}</Modal>}</>;
}
createRoot(document.getElementById('root')!).render(<ConfigProvider direction={direction} theme={{
  algorithm: algorithm === 'dark' ? theme.darkAlgorithm : algorithm === 'compact' ? theme.compactAlgorithm : algorithm === 'dark-compact' ? [theme.darkAlgorithm, theme.compactAlgorithm] : theme.defaultAlgorithm,
  token: {motion:false, wireframe:q.get('wireframe') === 'true'},
  ...(variant === 'custom-token' ? {components:{Modal:{contentBg:'#fffbe6',titleColor:'#ad6800',titleFontSize:20,headerMarginBottom:16,footerMarginTop:20}}} : {}),
}}><main style={{width:'100vw',height:'100vh',background:algorithm.startsWith('dark')?'#141414':'#fff'}}><Scene/></main></ConfigProvider>);
setTimeout(() => {
  // Compare the first animation frame deterministically, including Skeleton active.
  for (const animation of document.getAnimations()) { animation.pause(); animation.currentTime=0; }
  // Remove default autofocus outlines for a stable resting-state comparison.
  (document.activeElement as HTMLElement)?.blur();
  const selectors = ['.ant-modal','.ant-modal-container','.ant-modal-header','.ant-modal-title','.ant-modal-body','.ant-modal-footer','.ant-modal-close','.ant-modal-confirm-body','.ant-modal-confirm-title','.ant-modal-confirm-content','.ant-modal-confirm-btns','.ant-modal-confirm-body > .anticon','.ant-btn','.ant-skeleton-paragraph > li','.ant-modal-mask'];
  const elements = selectors.flatMap(selector => [...document.querySelectorAll(selector)].filter(element => element.getBoundingClientRect().height > 0).map(element => {
    const r = element.getBoundingClientRect(), s = getComputedStyle(element);
    return {selector,text:element.textContent,x:r.x,y:r.y,w:r.width,h:r.height,background:s.backgroundColor,color:s.color,fontFamily:s.fontFamily,fontSize:s.fontSize,lineHeight:s.lineHeight,fontWeight:s.fontWeight,radius:s.borderRadius,padding:s.padding,margin:s.margin,border:s.borderColor,borderWidth:s.borderWidth,boxShadow:s.boxShadow};
  }));
  const script = document.createElement('script');script.id='visual-metrics';script.type='application/json';
  script.textContent=JSON.stringify({scale:devicePixelRatio,viewport:[innerWidth,innerHeight],elements});document.body.append(script);
}, 400);
