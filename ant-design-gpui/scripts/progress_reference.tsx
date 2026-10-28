// Copyright (C) 2026 mg-chao
// SPDX-License-Identifier: Apache-2.0
import React from 'react';
import {createRoot} from 'react-dom/client';
import {flushSync} from 'react-dom';
import Progress from 'antd-reference/progress';
import ConfigProvider from 'antd-reference/config-provider';
import theme from 'antd-reference/theme';
import cases from '../fixtures/progress/visual-cases.json';

const query=new URLSearchParams(location.search);
const scene=query.get('scene')??'line',themeName=query.get('theme')??'default',direction=query.get('direction')??'ltr';
const algorithm=themeName==='dark'?theme.darkAlgorithm:themeName==='compact'?theme.compactAlgorithm:themeName==='dark-compact'?[theme.darkAlgorithm,theme.compactAlgorithm]:theme.defaultAlgorithm;
const overrides={defaultColor:'#bae0ff',remainingColor:'#f5222d',circleTextColor:'#52c41a',lineBorderRadius:3,circleTextFontSize:'0.8em',circleIconFontSize:'1.25em'};
const formatters={release:n=>`In progress, ${n}% complete`,done:n=>`${n} done`,days:()=> '3 Days',badge:()=> <span style={{display:'block',width:80,height:10,background:'#ffffff'}}/>};
function Body(){
  const {token}=theme.useToken(); (window as any).__progressTokens=token;
  return <main style={{boxSizing:'border-box',width:740,height:1000,padding:20,display:'flex',flexDirection:'column',alignItems:'flex-start',gap:12,background:token.colorBgContainer,color:token.colorText,fontFamily:token.fontFamily,fontSize:token.fontSize,lineHeight:token.lineHeight,direction}}>
    {cases[scene].map((sample,index)=>{
      const circular=sample.type==='circle'||sample.type==='dashboard';
      const {width=360,height=circular?132:52,format,rounding,tokenCase,semantic,railOpacity,trackOpacity,...props}=sample;
      const styles=semantic?{rail:{backgroundColor:'#fff1f0',stroke:'#fff1f0',opacity:railOpacity},track:{backgroundColor:'#9254de',stroke:'#9254de',opacity:trackOpacity},indicator:{color:'#d4380d'}}:undefined;
      const progress=<Progress {...props} styles={styles} format={formatters[format]} rounding={rounding==='floor'?Math.floor:rounding==='ceil'?Math.ceil:undefined}/>;
      return <div className="progress-case" data-options={JSON.stringify(sample)} key={index} style={{width,height,display:'flex',alignItems:'center',flexShrink:0}}>
        {tokenCase?<ConfigProvider theme={{components:{Progress:overrides}}}>{progress}</ConfigProvider>:progress}
      </div>;
    })}
  </main>;
}
function bounds(element){
  if(!element)return null;
  const r=element.getBoundingClientRect(),s=getComputedStyle(element);
  return {x:r.x,y:r.y,width:r.width,height:r.height,color:s.color,background:s.backgroundColor,backgroundImage:s.backgroundImage,fontSize:parseFloat(s.fontSize),lineHeight:s.lineHeight,borderRadius:s.borderRadius,stroke:s.stroke,strokeWidth:s.strokeWidth,strokeLinecap:s.strokeLinecap,strokeDasharray:s.strokeDasharray,strokeDashoffset:s.strokeDashoffset,transform:s.transform,opacity:s.opacity,text:element.textContent};
}
(window as any).captureProgressMetrics=()=>({scene,theme:themeName,direction,scale:devicePixelRatio,background:(window as any).__progressTokens.colorBgContainer,
  rows:[...document.querySelectorAll('.progress-case')].map(row=>{
    const root=row.querySelector('.ant-progress');return {...bounds(row),options:JSON.parse(row.getAttribute('data-options')!),progress:bounds(root),body:bounds(root?.querySelector('.ant-progress-body,.ant-progress-steps-body')),rail:bounds(root?.querySelector('.ant-progress-rail,.ant-progress-circle-rail')),indicator:bounds(root?.querySelector('.ant-progress-indicator')),tracks:[...root!.querySelectorAll('.ant-progress-track,.ant-progress-circle-path,.ant-progress-steps-item')].map(bounds),status:[...root!.classList].find(n=>n.startsWith('ant-progress-status-'))?.replace('ant-progress-status-',''),aria:{role:root?.getAttribute('role'),value:root?.getAttribute('aria-valuenow'),min:root?.getAttribute('aria-valuemin'),max:root?.getAttribute('aria-valuemax')}};
  })});
(window as any).setProgressPhase=(ms)=>{for(const animation of document.getAnimations()){animation.pause();animation.currentTime=ms;}return (window as any).captureProgressMetrics();};
const reactRoot=createRoot(document.getElementById('root')!);
flushSync(()=>reactRoot.render(<ConfigProvider direction={direction as any} theme={{algorithm,token:{motion:false}}}><Body/></ConfigProvider>));
requestAnimationFrame(()=>requestAnimationFrame(()=>{(window as any).setProgressPhase(0);const ready=document.createElement('script');ready.id='visual-ready';ready.type='application/json';ready.textContent=JSON.stringify((window as any).captureProgressMetrics());document.body.append(ready);}));

// Deterministic samples of the browser's actual CSS transitions, not copied curves.
(window as any).probeProgressMotion=async(mode)=>{
  const frame=()=>new Promise(resolve=>requestAnimationFrame(resolve));
  const render=(percent,status='normal')=>flushSync(()=>reactRoot.render(
    <ConfigProvider theme={{token:{motion:true}}}><div style={{width:300}}>
      <Progress key={mode} type={mode==='circle'?'circle':'line'} percent={percent} status={status as any} showInfo={false} steps={mode==='steps'?5:undefined}/>
    </div></ConfigProvider>));
  render(mode==='active'?50:20,mode==='active'?'active':'normal');
  await frame();await frame();await new Promise(resolve=>setTimeout(resolve,130));
  if(mode!=='active')render(80);
  await frame();await frame();
  const animations=document.getAnimations();for(const a of animations)a.pause();
  const samples=[];
  for(const ms of (mode==='active'?[0,240,480,960,1440,1920,2399]:[0,75,150,225,300])){
    for(const a of animations)a.currentTime=ms;
    const rail=document.querySelector('.ant-progress-rail'),track=document.querySelector('.ant-progress-track,.ant-progress-circle-path');
    const s=track?getComputedStyle(track):null,overlay=track?getComputedStyle(track,'::after'):null;
    samples.push({ms,rail:bounds(rail),track:bounds(track),dash:s?.strokeDashoffset,
      steps:[...document.querySelectorAll('.ant-progress-steps-item')].map(bounds),
      overlay:{transform:overlay?.transform,opacity:overlay?.opacity,width:overlay?.width}});
  }
  return {mode,animations:animations.map(a=>({type:a.constructor.name,property:(a as any).transitionProperty,timing:a.effect?.getTiming()})),samples};
};
