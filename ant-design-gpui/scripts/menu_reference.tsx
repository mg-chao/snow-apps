// Copyright (C) 2026 mg-chao
// SPDX-License-Identifier: Apache-2.0
// Executed against the actual pinned Ant Design source.
import React from 'react';
import {createRoot} from 'react-dom/client';
import Menu from 'antd-reference/menu';
import ConfigProvider from 'antd-reference/config-provider';
import theme from 'antd-reference/theme';
import {MailOutlined,AppstoreOutlined,SettingOutlined,DesktopOutlined,DeleteOutlined} from '@ant-design/icons';

const q=new URLSearchParams(location.search);
const mode=q.get('mode')??'vertical';
const kind=q.get('kind')??'basic';
const algorithm=q.get('theme')??'default';
const direction=q.get('direction')==='rtl'?'rtl':'ltr';
const noIcon=kind==='no-icon';
const open=q.get('open')==='true';
const icon=(value:React.ReactNode)=>noIcon?undefined:value;
const options=[
  {key:'home',label:'Home',icon:icon(<MailOutlined/>),extra:kind==='extra'?'Ctrl+H':undefined},
  {key:'projects',label:'Projects',icon:icon(<AppstoreOutlined/>)},
  {key:'disabled',label:'Disabled',icon:icon(<DesktopOutlined/>),disabled:true},
  {key:'settings',label:'Settings',icon:icon(<SettingOutlined/>),theme:kind==='submenu-theme'?'light':undefined,children:[
    {key:'general',label:'General'},
    {key:'profile',label:'Profile'},
    {key:'advanced',label:'Advanced',children:[{key:'security',label:'Security'},{key:'privacy',label:'Privacy'}]},
  ]},
  {key:'delete',label:'Delete',icon:icon(<DeleteOutlined/>),danger:true},
];
if(kind==='groups')options.push({type:'divider',key:'divider'} as any,{type:'group',key:'group',label:'Management',children:[{key:'team',label:'Team'},{key:'billing',label:'Billing'}]} as any);
if(kind==='empty')options.splice(0);
if(kind==='empty-group')options.splice(0,options.length,{type:'group',key:'group',label:'Group',children:[]} as any);
if(kind==='only-submenu')options.splice(0,options.length,{key:'settings',label:'Settings',children:[{key:'profile',label:'Profile'}]} as any);
if(kind==='only-divider')options.splice(0,options.length,{type:'divider',key:'divider'} as any);
const selected=q.get('selected')??(open?'profile':'home');
const openKeys=open?kind==='deep'?['settings','advanced']:['settings']:[];
const tokens=q.get('tokens')==='true';
const componentTokens=tokens?{Menu:{itemHeight:44,itemBorderRadius:10,itemMarginBlock:3,itemMarginInline:6,itemSelectedBg:'#fff7e6',itemSelectedColor:'#d46b08',darkItemSelectedBg:'#531dab',activeBarHeight:4}}:undefined;
createRoot(document.getElementById('root')!).render(<ConfigProvider direction={direction} theme={{
  algorithm:algorithm==='dark'?theme.darkAlgorithm:algorithm==='compact'?theme.compactAlgorithm:algorithm==='dark-compact'?[theme.darkAlgorithm,theme.compactAlgorithm]:theme.defaultAlgorithm,
  token:{motion:false},components:componentTokens,
}}><main style={{width:800,height:500,background:algorithm.startsWith('dark')?'#141414':'#fff'}}>
  <div style={{position:'absolute',left:64,top:48,width:Number(q.get('width')??(mode==='horizontal'?640:256))}}>
    <Menu mode={mode==='collapsed'?'inline':mode as any} inlineCollapsed={mode==='collapsed'} theme={(q.get('color_theme')??'light') as any}
      items={options as any} selectedKeys={[selected]} openKeys={openKeys} aria-label="Navigation"
      {...(kind==='custom-expand'?{expandIcon:({isOpen}:any)=><span className="ant-menu-submenu-expand-icon">{isOpen?'−':'+'}</span>}: {})}
      {...(kind==='custom-overflow'?{overflowedIndicator:<span>More</span>}: {})}/>
  </div>
</main></ConfigProvider>);
if(q.get('focus')==='true')setTimeout(()=>{(document.querySelector('.ant-menu-item')as HTMLElement)?.focus();},100);
setTimeout(()=>{
  const selectors=['.ant-menu-root','.ant-menu-item','.ant-menu-submenu-title','.ant-menu-root.ant-menu-horizontal > .ant-menu-submenu','.ant-menu-item-group-title','.ant-menu-submenu-popup > .ant-menu','.ant-menu-sub.ant-menu-inline'];
  const elements=selectors.flatMap(selector=>[...document.querySelectorAll(selector)].filter(element=>{
    const css=getComputedStyle(element);return css.display!=='none'&&css.visibility!=='hidden'&&(selector==='.ant-menu-root'||element.getBoundingClientRect().height>0)&&!element.closest('[aria-hidden="true"]');
  }).map(element=>{
    const r=element.getBoundingClientRect(),s=getComputedStyle(element),after=getComputedStyle(element,'::after');
    const arrow=selector==='.ant-menu-submenu-title'?element.querySelector('.ant-menu-submenu-arrow'):null;
    const arrowStyle=arrow?getComputedStyle(arrow):null;
    const arrowRect=arrow?.getBoundingClientRect();
    const arrowSlot=arrowRect&&arrowRect.width>0&&arrowStyle?.display!=='none'&&Number(arrowStyle?.opacity)>0
      ?{x:arrowRect.x,y:arrowRect.y,w:arrowRect.width,h:arrowRect.height}:null;
    return{selector,text:element.textContent,x:r.x,y:r.y,w:r.width,h:r.height,
      background:s.backgroundColor,color:s.color,fontSize:s.fontSize,lineHeight:s.lineHeight,radius:s.borderRadius,
      border:s.borderColor,borderWidth:s.borderWidth,boxShadow:s.boxShadow,
      outlineStyle:s.outlineStyle,outlineColor:s.outlineColor,outlineWidth:s.outlineWidth,outlineOffset:s.outlineOffset,
      arrow:arrowSlot,
      after:{borderColor:after.borderColor,borderWidth:after.borderWidth,insetInline:after.insetInline,bottom:after.bottom}};
  }));
  const script=document.createElement('script');script.id='visual-metrics';script.type='application/json';
  script.textContent=JSON.stringify({scale:devicePixelRatio,viewport:[innerWidth,innerHeight],elements});document.body.append(script);
},400);
