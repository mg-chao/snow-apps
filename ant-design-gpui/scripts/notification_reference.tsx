// Copyright (C) 2026 mg-chao
// SPDX-License-Identifier: Apache-2.0
import 'antd-reference/style/reset.css';
import React, {useEffect} from 'react';
import {createRoot} from 'react-dom/client';
import notification from 'antd-reference/notification';
import ConfigProvider from 'antd-reference/config-provider';
import Button from 'antd-reference/button';
import theme from 'antd-reference/theme';
import SmileOutlined from '@ant-design/icons/SmileOutlined';
import MinusOutlined from '@ant-design/icons/MinusOutlined';
import scenes from '../fixtures/notification/visual-cases.json';

const q=new URLSearchParams(location.search);
const scene=scenes[q.get('scene')??'placement-topRight'];
const algorithm=q.get('theme')??'default';
const direction=q.get('direction')==='rtl'?'rtl':'ltr';
const styles={root:{background:'#f6ffed',border:'2px solid #95de64',borderRadius:16,boxShadow:'4px 4px 0 #d9f7be'},
  title:{color:'#237804',fontWeight:600},description:{color:'#3f6600'},icon:{color:'#237804'}};
function Scene(){
  const [api,holder]=notification.useNotification({stack:scene.stack?{threshold:scene.threshold??3}:false,
    placement:scene.placement??'topRight',top:scene.top,bottom:scene.bottom,maxCount:scene.maxCount,
    styles:scene.holderStyles?{list:{padding:31,background:"#fafafa"},listContent:{gap:37,background:"#f0f5ff"}}:undefined});
  useEffect(()=>{
    for(const item of scene.items){
      api.open({...item,title:item.title??undefined,description:item.description??undefined,
        props:{'data-fixture-key':item.key},duration:item.showProgress?86400:0,
        icon:item.icon==='smile'?<SmileOutlined style={{color:'#108ee9'}}/>:undefined,
        closeIcon:item.closeIcon==='minus'?<MinusOutlined/>:undefined,
        closable:item.closeDisabled?{disabled:true}:item.closable,
        actions:item.actions?<div style={{display:'flex',gap:8}}><Button type="link" size="small">Destroy All</Button><Button type="primary" size="small">Confirm</Button></div>:undefined,
        styles:item.semantic?styles:item.icon==='none'?{icon:{display:'none'}}:undefined,
      });
    }
    (window as any).__notificationApi=api;
  },[]);
  return holder;
}
const custom=scene.tokenCase?{Notification:{width:420,progressBg:'linear-gradient(135deg,#6253e1,#04befe)',
  colorSuccessBg:'linear-gradient(30deg,#d9f7be,#f6ffed)',colorInfoBg:'linear-gradient(30deg,#bae0ff,#e6f4ff)',
  colorWarningBg:'linear-gradient(30deg,#ffffb8,#feffe6)',colorErrorBg:'linear-gradient(30deg,#ffccc7,#fff1f0)'}}:undefined;
createRoot(document.getElementById('root')!).render(<ConfigProvider direction={direction} theme={{
  algorithm:algorithm==='dark'?theme.darkAlgorithm:algorithm==='compact'?theme.compactAlgorithm:algorithm==='dark-compact'?[theme.darkAlgorithm,theme.compactAlgorithm]:theme.defaultAlgorithm,
  token:{motion:false},components:custom,
}}><main style={{width:'100vw',height:'100vh',background:algorithm.startsWith('dark')?'#141414':'#fff'}}><Scene/></main></ConfigProvider>);

function measure(){
  const rect=(element:Element)=>{const r=element.getBoundingClientRect();return{x:r.x,y:r.y,width:r.width,height:r.height};};
  const rows=[...document.querySelectorAll('.ant-notification-notice')].map(element=>{
    const s=getComputedStyle(element);const bounds=rect(element);
    return {key:element.getAttribute('data-fixture-key'),root:bounds,visible:Number(s.opacity)>0&&s.display!=='none',
      styles:{padding:s.padding,fontSize:s.fontSize,lineHeight:s.lineHeight,background:s.background,borderRadius:s.borderRadius,boxShadow:s.boxShadow},
      parts:Object.fromEntries(['title','description','icon','close','actions','progress'].map(part=>{
        const child=element.querySelector(`.ant-notification-notice-${part}`);return[part,child?rect(child):null];}))};
  });
  return {scale:devicePixelRatio,viewport:[innerWidth,innerHeight],rows};
}
setTimeout(()=>{
  for(const animation of document.getAnimations()){animation.finish();}
  (document.activeElement as HTMLElement)?.blur();
  if(scene.hover){
    const list=document.querySelector('.ant-notification-list-content');
    list?.dispatchEvent(new MouseEvent('mouseover',{bubbles:true,relatedTarget:document.body}));
  }
  setTimeout(()=>{
    const script=document.createElement('script');script.id='visual-metrics';script.type='application/json';script.textContent=JSON.stringify(measure());document.body.append(script);
    (window as any).__notificationReference={measure};
  },80);
},200);
