// Copyright (C) 2026 mg-chao
// SPDX-License-Identifier: Apache-2.0
// Independent behavior oracle: all store and validation logic is original rc Form.
import React from 'react';
import {createRoot} from 'react-dom/client';
import {flushSync} from 'react-dom';
import Form from 'antd-reference/form';
import Input from 'antd-reference/input';
import ConfigProvider from 'antd-reference/config-provider';
import enUS from 'antd-reference/locale/en_US';

const host: any = window;
const encode = (value: any): any => value === undefined ? {$undefined:true}
  : Array.isArray(value) ? value.map(encode) : value && typeof value === 'object'
    ? Object.fromEntries(Object.entries(value).map(([key,child])=>[key,encode(child)])) : value;
let events: any[] = [], form: any, setScenario: any, epoch=0;
let pending: any[] = [], result: any, ops: any, watched: any;
const record=(type: string,...args: any[])=>events.push({type,args:encode(args)});
function rule(config: any) {
  if (config.equalTo) return ({getFieldValue}: any)=>({validator:(_: any,value: any)=>
    !value||value===getFieldValue(config.equalTo)?Promise.resolve():Promise.reject(Error(config.message??'Values do not match'))});
  if (config.deferred) return {...config,validator:(_: any,value: any)=>new Promise((resolve,reject)=>{
    pending.push({value,resolve,reject});record('validator-start',value);
  })};
  const {pattern,...rest}=config;
  return {...rest,...pattern?{pattern:new RegExp(pattern)}:{}};
}
function Field({config}: any) {
  const {id,name,visible=true,rules,...props}=config;
  if(!visible)return null;
  return <Form.Item {...props} name={name??id} rules={rules?.map(rule)}>
    <Input data-field={id}/>
  </Form.Item>;
}
function WatchProbe({instance}: any) {
  watched={registered:Form.useWatch('a',instance),
    preserved:Form.useWatch('unregistered',{form:instance,preserve:true}),
    selector:Form.useWatch((values: any)=>String(values.a??'').length,instance)};
  return null;
}
function Fixture({config}: any) {
  const [instance]=Form.useForm();form=instance;
  const {fields=[],list,...props}=config;
  return <Form {...props} form={instance} name="oracle" style={{width:600}}
    onValuesChange={(...args)=>record('values-change',...args)}
    onFieldsChange={(changed,all)=>record('fields-change',changed.map(({name,value,touched,validating,errors,warnings})=>
      ({name,value,touched,validating,errors,warnings})),all.map(({name,value,touched,validating,errors,warnings})=>
      ({name,value,touched,validating,errors,warnings})))}
    onFinish={values=>record('finish',values)} onFinishFailed={error=>record('finish-failed',error)}>
    <WatchProbe instance={instance}/>
    {fields.map((field: any)=><Field key={field.id} config={field}/>)}
    {list&&<Form.List name={list.name??'users'} initialValue={list.initialValue}>
      {(entries,operation)=>{ops=operation;return entries.map(entry=><div key={entry.key}
        data-list-key={entry.key} data-list-index={entry.name}>
        <Form.Item name={[entry.name,'name']} noStyle><Input data-field={`row-${entry.key}`}/></Form.Item>
      </div>);}}
    </Form.List>}
  </Form>;
}
function App() {
  const [scenario,update]=React.useState<any>({fields:[]});setScenario=update;
  return <ConfigProvider locale={enUS} theme={{token:{motion:false}}}><Fixture key={scenario.epoch} config={scenario}/></ConfigProvider>;
}
flushSync(()=>createRoot(document.getElementById('root')!).render(<App/>));
host.formOracle={
  reset(config: any) {events=[];pending=[];result=undefined;ops=undefined;
    flushSync(()=>setScenario({...config,epoch:++epoch}));events=[];},
  patch(config: any) {flushSync(()=>setScenario((old: any)=>({...old,...config})));},
  form:()=>form,
  snapshot() {return encode({values:form.getFieldsValue(),allValues:form.getFieldsValue(true),
    fields:form.getFieldsError().map(({name,errors,warnings}: any)=>({name,value:form.getFieldValue(name),
      touched:form.isFieldTouched(name),validating:form.isFieldValidating(name),errors,warnings})),
    list:[...document.querySelectorAll('[data-list-key]')].map(element=>({
      key:Number(element.getAttribute('data-list-key')),index:Number(element.getAttribute('data-list-index')),
      value:(element.querySelector('input') as HTMLInputElement).value})),
    editors:[...document.querySelectorAll('input[data-field]')].map(element=>({id:element.getAttribute('data-field'),
      value:(element as HTMLInputElement).value})),events,result,
    watched,focused:(document.activeElement as HTMLElement)?.getAttribute('data-field')});},
  set(name: any,value: any){form.setFieldValue(name,value);},
  merge(values: any){form.setFieldsValue(values);},
  fields(fields: any){form.setFields(fields);},
  resetFields(names: any){form.resetFields(names);},
  validate(names: any,options: any){return (names===undefined?form.validateFields(options):form.validateFields(names,options)).then((values: any)=>
    {result={ok:true,values};return encode(result);},(error: any)=>{result={ok:false,...error};return encode(result);});},
  startValidate(names: any,options: any){result=undefined;this.validate(names,options);},
  resolve(value: any,message: any){const request=pending.find(entry=>Object.is(entry.value,value));
    if(!request)throw Error(`No pending validator for ${value}`);
    pending=pending.filter(entry=>entry!==request);message?request.reject(Error(message)):request.resolve();},
  list(operation: string,...args: any[]){ops[operation](...args);},
  submit(){form.submit();},
  clearEvents(){events=[];},
};
document.documentElement.setAttribute('data-behavior-ready','true');
