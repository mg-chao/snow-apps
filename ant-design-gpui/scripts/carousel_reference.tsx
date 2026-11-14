// Copyright (C) 2026 mg-chao
// SPDX-License-Identifier: Apache-2.0
import React from 'react';
import {createRoot} from 'react-dom/client';
import {flushSync} from 'react-dom';
import Carousel from 'antd-reference/carousel';
import ConfigProvider from 'antd-reference/config-provider';
import theme from 'antd-reference/theme';
import {prepareComponentToken} from 'antd-reference/carousel/style';
import scenes from '../fixtures/carousel/visual-cases.json';

const q=new URLSearchParams(location.search);
const scene=q.get('scene')??'placements';
const algorithm=q.get('theme')??'default';
const direction=q.get('direction')==='rtl'?'rtl':'ltr';
const samples=scenes[scene];
if(!samples)throw Error(`Unknown scene ${scene}`);

// Keep Slick's completion/autoplay timers deterministic while using the original
// component and native CSS transitions. requestAnimationFrame remains a real frame.
let clock=1000000,nextTimer=0;
const tasks=new Map();
const timer=(callback,duration,repeat,args)=>{const id=++nextTimer;tasks.set(id,{callback,due:clock+Math.max(0,Number(duration)||0),repeat,args});return id;};
window.setTimeout=(callback,duration,...args)=>timer(callback,duration,0,args);
window.setInterval=(callback,duration,...args)=>timer(callback,duration,Math.max(1,Number(duration)||0),args);
window.clearTimeout=window.clearInterval=id=>tasks.delete(id);
Date.now=()=>clock;
const advance=duration=>{const end=clock+duration;for(let i=0;i<10000;i++){
  const entry=[...tasks].filter(([,task])=>task.due<=end).sort((a,b)=>a[1].due-b[1].due||a[0]-b[0])[0];
  if(!entry){clock=end;return;}
  const[id,task]=entry;clock=task.due;if(task.repeat)task.due+=task.repeat;else tasks.delete(id);
  flushSync(()=>task.callback(...task.args));
}throw Error('Reference virtual timer bound exceeded');};
const refs=samples.map(()=>React.createRef());
const algorithms={default:theme.defaultAlgorithm,dark:theme.darkAlgorithm,compact:theme.compactAlgorithm,'dark-compact':[theme.darkAlgorithm,theme.compactAlgorithm]};
const custom={Carousel:{dotWidth:50,dotHeight:50,dotActiveWidth:80}};
const contentHeight=(sample,index)=>sample.adaptive?(index%2?100:160):160;
function Row({sample,index}){
  const props={ref:refs[index],arrows:sample.arrows??false,dots:sample.dots??true,dotPlacement:sample.placement??'bottom',effect:sample.effect??'scrollx',
    infinite:sample.infinite??true,initialSlide:sample.initial??0,speed:500,autoplay:sample.progress?{dotDuration:true}:false,autoplaySpeed:3000,
    adaptiveHeight:!!sample.adaptive,vertical:sample.vertical,style:sample.adaptive?undefined:{height:sample.height}};
  return <div className="demo-case" data-options={JSON.stringify(sample)} style={{width:sample.width,height:sample.height,position:'relative',flexShrink:0}}>
    <ConfigProvider theme={sample.tokenCase?{components:custom}:undefined}><Carousel {...props}>
      {Array.from({length:sample.count??4},(_,slide)=><div key={slide}>{sample.layered?
        <div style={{height:160,position:'relative',background:slide%2?'#7aa8cc':'#426e99'}}><div style={{position:'absolute',left:40,top:40,width:200,height:80,lineHeight:'80px',textAlign:'center',background:'#e58d8d',color:'#fff'}}>{slide+1}</div></div>:
        <h3 style={{margin:0,height:contentHeight(sample,slide),lineHeight:`${contentHeight(sample,slide)}px`,textAlign:'center',color:'#fff',background:'#364d79'}}>{slide+1}</h3>}</div>)}
    </Carousel></ConfigProvider>
  </div>;
}
function Scene(){
  const {token}=theme.useToken();
  return <main style={{width:520,height:1000,boxSizing:'border-box',padding:20,display:'flex',flexDirection:'column',alignItems:'flex-start',gap:12,
    fontFamily:token.fontFamily,fontSize:token.fontSize,lineHeight:token.lineHeight,color:token.colorText,background:token.colorBgContainer}}>
    {samples.map((sample,index)=><Row key={index} sample={sample} index={index}/>)}
  </main>;
}
flushSync(()=>createRoot(document.getElementById('root')!).render(<ConfigProvider direction={direction} theme={{algorithm:algorithms[algorithm]}}><Scene/></ConfigProvider>));
function measure(){
  const bounds=node=>{const b=node.getBoundingClientRect();return{x:b.x,y:b.y,width:b.width,height:b.height};};
  const part=node=>{if(!node)return null;const s=getComputedStyle(node);return{...bounds(node),opacity:Number(s.opacity),background:s.backgroundColor,transform:s.transform,label:node.getAttribute('aria-label')};};
  return {scale:devicePixelRatio,viewport:[innerWidth,innerHeight],scene,theme:algorithm,direction,
    rows:[...document.querySelectorAll('.demo-case')].map((row,index)=>{const root=row.firstElementChild;return{index,options:samples[index],...bounds(row),carousel:part(root),
      viewport:part(root.querySelector('.slick-list')),track:part(root.querySelector('.slick-track')),
      arrows:[...root.querySelectorAll('.slick-arrow')].map(part),dots:[...root.querySelectorAll('.slick-dots li')].map(node=>{const after=getComputedStyle(node,'::after');return{...part(node),active:node.classList.contains('slick-active'),button:part(node.querySelector('button')),progress:{width:parseFloat(after.width),height:parseFloat(after.height)}};}),
      slides:[...root.querySelectorAll('.slick-slide:not(.slick-cloned)')].map(node=>({...part(node),index:Number(node.dataset.index),active:node.classList.contains('slick-active'),hidden:node.getAttribute('aria-hidden')}))};})};
}
const frame=()=>new Promise(resolve=>requestAnimationFrame(resolve));
function setPhase(time){for(const animation of document.getAnimations()){animation.pause();animation.currentTime=time;}return measure();}
async function start(){
  await frame();await frame();advance(0);await frame();await frame();advance(500);setPhase(1000);await frame();
  if(scene.startsWith('motion')||scene==='adaptive'){
    for(const[index,ref]of refs.entries()){
      const sample=samples[index],vertical=sample.vertical??['start','end'].includes(sample.placement);
      flushSync(()=>direction==='rtl'&&!vertical?ref.current.prev():ref.current.next());
    }
    advance(0);await frame();await frame();
  }
  for(const ref of refs)ref.current?.innerSlider.pause('paused');
  setPhase(Number(q.get('phase')??1000));
  const componentDefaults=Object.fromEntries(Object.entries(algorithms).map(([name,algorithm])=>[name,{defaults:prepareComponentToken(theme.getDesignToken({algorithm}))}]));
  window.__carouselReference={ready:true,refs,measure,setPhase,advance,componentDefaults};
}
start().catch(error=>{document.body.dataset.error=error.stack;console.error(error);});
