// Copyright (C) 2026 mg-chao
// SPDX-License-Identifier: Apache-2.0
// Mount the original pinned Image component; do not reconstruct its markup or CSS.
import 'antd-reference/style/reset.css';
import React,{useEffect,useState} from 'react';
import {createRoot} from 'react-dom/client';
import {flushSync} from 'react-dom';
import Image from 'antd-reference/image';
import ConfigProvider from 'antd-reference/config-provider';
import theme from 'antd-reference/theme';
import scenes from '../fixtures/image/visual-cases.json';
import unsupported from '../fixtures/image/unsupported-reference-cases.json';
import motionCases from '../fixtures/image/motion-cases.json';

const query=new URLSearchParams(location.search);
const motionCase=(motionCases.cases as any)[query.get('motioncase')??''];
const scene=(scenes as any)[query.get('scene')??'thumbnails']??(unsupported as any)[query.get('scene')??''];
if(!scene)throw Error('Unknown Image reference scene');
const mode=query.get('theme')??'default',direction=query.get('direction')==='rtl'?'rtl':'ltr';
const source=(name?:string)=>name?`/images/${name}.png`:undefined;
let api:any,transform:any,change:(patch:any)=>void;
const trace:any[]=[];
function thumbnail(item:any,index:number){
  return <Image key={index} alt={`Fixture ${index+1}`} src={source(item.src)} width={item.width} height={item.height} fallback={source(item.fallback)}
    preview={item.cover?{cover:{coverNode:item.cover,placement:item.placement}}:undefined}
    placeholder={item.progress!==undefined?{progress:item.loadingText?{render:()=>item.loadingText}:typeof item.progress==='number'?{percent:item.progress,render:item.customProgress?(bar:any,p:number)=><>{bar}<div style={{marginTop:8}}>Generating {p}%</div></>:undefined}:true}:undefined}/>;
}
function Scene(){
  const [override,setOverride]=useState<any>({});change=patch=>flushSync(()=>setOverride((previous:any)=>({...previous,...patch})));
  const {token}=theme.useToken();
  const active=override.isolated?{}:scene;
  const preview=active.preview;
  const actionsRender=(original:any,info:any)=>{api=info.actions;transform=info.transform;return original;};
  const options:any={open:override.open??motionCase?.transition!=='open',focusTrap:false,mask:preview?.mask,actionsRender,
    onTransform:(event:any)=>trace.push(event),onOpenChange:(open:boolean)=>trace.push({open})};
  useEffect(()=>{Object.assign(document.body.style,{fontFamily:token.fontFamily,fontSize:`${token.fontSize}px`,lineHeight:String(token.lineHeight),color:token.colorText});(window as any).__imageMounted=true;},[token]);
  return <main style={{width:'100vw',height:'100vh',background:mode.startsWith('dark')?'#141414':'#fff',padding:24,boxSizing:'border-box',display:'flex',alignItems:'flex-start',gap:16,direction}}>
    {scene.backdrop&&<div style={{display:'flex',gap:12}}>{['landscape','portrait','transparent'].map(name=><img key={name} width={220} src={source(name)}/>)}</div>}
    {active.images?.map(thumbnail)}
    {preview?.items?<Image.PreviewGroup items={preview.items.map(source)} preview={{...options,current:preview.current??0}}/>:preview?<Image style={{display:'none'}} alt="Preview fixture" src={source(preview.src)} preview={options}/>:null}
    {override.src!==undefined&&<Image alt="Loading fixture" width={override.width===null?undefined:override.width??200} height={override.height===null?undefined:override.height??125} src={override.src} fallback={override.fallback} placeholder={<div data-image-loading style={{width:'100%',height:'100%',background:'#ddeeff'}}>Loading</div>} onError={()=>trace.push({error:true})}/>}
  </main>;
}
createRoot(document.getElementById('root')!).render(<ConfigProvider direction={direction} theme={{algorithm:mode==='dark'?theme.darkAlgorithm:mode==='compact'?theme.compactAlgorithm:mode==='dark-compact'?[theme.darkAlgorithm,theme.compactAlgorithm]:theme.defaultAlgorithm,token:{motion:!!motionCase},components:scene.tokens?{Image:scene.tokens}:undefined}}><Scene/></ConfigProvider>);
const rect=(element:Element|null)=>{if(!element)return null;const box=element.getBoundingClientRect();return{x:box.x,y:box.y,width:box.width,height:box.height};};
const style=(element:Element|null)=>{if(!element)return null;const s=getComputedStyle(element);return{background:s.backgroundColor,color:s.color,fontSize:s.fontSize,lineHeight:s.lineHeight,padding:s.padding,gap:s.gap,radius:s.borderRadius,opacity:s.opacity};};
function measure(){
  const popup=document.querySelector('.ant-image-preview');
  return{scale:devicePixelRatio,viewport:[innerWidth,innerHeight],
    rows:[...document.querySelectorAll('.ant-image')].filter(element=>element.getBoundingClientRect().width>0&&element.getBoundingClientRect().height>0).map((element,index)=>({key:`image-${index}`,root:rect(element),image:rect(element.querySelector('.ant-image-img')),progress:element.querySelector('.ant-image-progress-indicator')?.textContent,cover:rect(element.querySelector('.ant-image-cover')),content:rect(element.querySelector('.ant-image-progress-content')),rail:rect(element.querySelector('.ant-image-progress-rail')),indicator:rect(element.querySelector('.ant-image-progress-indicator')),styles:{root:style(element),cover:style(element.querySelector('.ant-image-cover')),rail:style(element.querySelector('.ant-image-progress-rail')),indicator:style(element.querySelector('.ant-image-progress-indicator'))}})),
    preview:popup?{root:rect(popup),image:rect(popup.querySelector('.ant-image-preview-img')),close:rect(popup.querySelector('.ant-image-preview-close')),footer:rect(popup.querySelector('.ant-image-preview-footer')),actions:rect(popup.querySelector('.ant-image-preview-actions')),previous:rect(popup.querySelector('.ant-image-preview-switch-prev')),next:rect(popup.querySelector('.ant-image-preview-switch-next')),count:popup.querySelector('.ant-image-preview-progress')?.textContent,transform,
      styles:{mask:getComputedStyle(popup.querySelector('.ant-image-preview-mask')!).backgroundColor,opacity:getComputedStyle(popup).opacity,bodyTransform:getComputedStyle(popup.querySelector('.ant-image-preview-body')!).transform,imageTransform:getComputedStyle(popup.querySelector('.ant-image-preview-img')!).transform,operationSize:getComputedStyle(popup.querySelector('.ant-image-preview-actions')!).fontSize,actions:style(popup.querySelector('.ant-image-preview-actions')),close:style(popup.querySelector('.ant-image-preview-close'))},controls:[...popup.querySelectorAll('.ant-image-preview-actions-action')].map(element=>({name:element.getAttribute('aria-label'),disabled:(element as HTMLButtonElement).disabled,bounds:rect(element),style:style(element)}))}:null};
}
const frames=()=>new Promise<void>(resolve=>requestAnimationFrame(()=>requestAnimationFrame(()=>resolve())));
let motionPrepared=false;
async function prepareMotion(){
  if(motionPrepared)return;
  for(const animation of document.getAnimations())if(animation.effect?.getTiming().iterations!==Infinity)animation.finish();
  await frames();await frames();
  for(const name of motionCase?.prepare_actions??[]){
    const command=api?.[`on${name[0].toUpperCase()+name.slice(1)}`];if(!command)throw Error(`Unknown preparation action ${name}`);
    flushSync(command);await frames();
    for(const animation of document.getAnimations())if(animation.effect?.getTiming().iterations!==Infinity)animation.finish();
    await frames();await frames();
  }
  motionPrepared=true;
}
async function sampleMotion(recordedPhase?:number,recordedImagePhase?:number){
  if(!motionCase)throw Error('No selected motion case');
  const phase=recordedPhase??motionCase.phase_ms;
  if(motionCase.transition==='open'){
    change({open:true});await frames();await frames();
  }else if(motionCase.transition!=='cloud'){
    await prepareMotion();
    if(motionCase.transition==='close'){
      change({open:false});await frames();await frames();
    }else if(motionCase.transition!=='hover'){
      const command=api?.[`on${motionCase.transition[0].toUpperCase()+motionCase.transition.slice(1)}`];
      if(!command)throw Error(`Unknown motion action ${motionCase.transition}`);
      flushSync(command);await frames();
    }
  }
  const animations=document.getAnimations();
  if(!animations.length)throw Error('Selected motion case did not create real upstream animations');
  for(const animation of animations){
    animation.pause();
    const target=(animation.effect as KeyframeEffect)?.target;
    animation.currentTime=target?.classList.contains('ant-image-preview-img')?(recordedImagePhase??phase):phase;
  }
  await frames();
  return {...measure(),phase_ms:phase,image_phase_ms:recordedImagePhase??phase,animations:animations.map(animation=>({currentTime:animation.currentTime,replay_clock:(animation.effect as KeyframeEffect)?.target?.classList.contains('ant-image-preview-img')?'image_phase_ms':'phase_ms',timing:animation.effect?.getTiming(),target:(animation.effect as KeyframeEffect)?.target?.className}))};
}
(window as any).__imageReference={measure,trace,prepareMotion,sampleMotion,change(patch:any){change(patch);},async action(name:string){const command=api?.[`on${name[0].toUpperCase()+name.slice(1)}`];if(!command)throw Error(`Unknown action ${name}`);flushSync(command);await frames();},async ready(){for(let pass=0;pass<3;pass++){await frames();for(const image of document.images){if(image.complete)continue;await new Promise(resolve=>{image.addEventListener('load',resolve,{once:true});image.addEventListener('error',resolve,{once:true});});}}for(const animation of document.getAnimations()){animation.pause();animation.currentTime=0;}return measure();}};
