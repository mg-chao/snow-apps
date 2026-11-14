// Copyright (C) 2026 mg-chao
// SPDX-License-Identifier: Apache-2.0
// Static component structure from the pinned Input/Search/OTP/Space sources.
const sizes = { small: 'sm', medium: '', large: 'lg' };
const icon = '<svg viewBox="0 0 1024 1024" width="1em" height="1em" fill="currentColor"><circle cx="512" cy="512" r="400"/></svg>';

function inputClass(o, affix = false) {
  const base = affix ? 'ant-input-affix-wrapper' : 'ant-input';
  return [base, 'ant-input-css-var', `ant-input-${o.variant}`, sizes[o.size] && `${base}-${sizes[o.size]}`,
    o.direction === 'rtl' && `${base}-rtl`, o.status !== 'none' && `ant-input-status-${o.status}`,
    o.availability === 'disabled' && `${base}-disabled ant-input-disabled`].filter(Boolean).join(' ');
}
function attrs(o) {
  return o.availability === 'disabled' ? 'disabled' : o.availability === 'readonly' ? 'readonly' : '';
}
function clear(o) {
  return `<button data-part="clear" class="ant-input-clear-icon ant-input-clear-icon-has-suffix ${o.availability!=='enabled'?'ant-input-clear-icon-hidden':''}" type="button"><span class="anticon">${icon}</span></button>`;
}
function field(o, extra = '', part = 'field', width = '') {
  return `<input data-part="${part}" class="${inputClass(o)} ${extra}" value="Ant Design" ${attrs(o)} ${width ? `style="width:${width}px"` : ''}>`;
}
function affix(o, password = false) {
  return `<span data-part="frame" class="${inputClass(o,true)}">
    ${password ? '' : '<span data-part="prefix" class="ant-input-prefix">￥</span>'}
    <input data-part="field" class="ant-input ${sizes[o.size]?`ant-input-${sizes[o.size]}`:''} ${o.direction==='rtl'?'ant-input-rtl':''} ${o.availability==='disabled'?'ant-input-disabled':''}" value="Ant Design" ${attrs(o)} ${password ? 'type="password"' : ''}>
    <span data-part="suffix" class="ant-input-suffix">${clear(o)}
    ${password ? `<span data-part="visibility" class="ant-input-password-icon anticon">${icon}</span>` : '<span data-part="count" class="ant-input-show-count-suffix ant-input-show-count-has-suffix">10 / 20</span>元'}</span>
  </span>`;
}
function search(o, label) {
  const text = ['filled','borderless','underlined'].includes(o.variant);
  const buttonVariant = text ? 'text' : label ? 'solid' : 'outlined';
  return `<div data-part="search" class="ant-space-compact ant-input-search ant-input-search-${o.size} ant-input-search-css-var ${o.direction==='rtl'?'ant-space-compact-rtl ant-input-search-rtl':''}">
    ${field(o,'ant-input-compact-item ant-input-compact-first-item')}
    <button data-part="button" class="ant-btn ant-btn-css-var ant-btn-color-${label?'primary':'default'} ant-btn-variant-${buttonVariant} ${sizes[o.size]?`ant-btn-${sizes[o.size]}`:''} ${label?'':'ant-btn-icon-only'} ant-input-search-btn ant-input-search-btn-${o.variant} ant-btn-compact-item ant-btn-compact-last-item" ${o.availability==='disabled'?'disabled':''}>${label?'<span>Search</span>':`<span class="ant-btn-icon"><span class="anticon">${icon}</span></span>`}</button>
  </div>`;
}
function render(o) {
  if(o.kind==='affix') return affix(o);
  if(o.kind==='password') return affix(o,true);
  if(o.kind.startsWith('search')) return search(o,o.kind==='search-label');
  if(o.kind==='textarea-count') return `<span data-part="frame" class="${inputClass(o,true)} ant-input-textarea-affix-wrapper ant-input-textarea-show-count ant-input-textarea-allow-clear"><textarea data-part="field" class="ant-input ${sizes[o.size]?`ant-input-${sizes[o.size]}`:''} ${o.availability==='disabled'?'ant-input-disabled':''}" rows="2" ${attrs(o)}>First line\n第二行</textarea><span class="ant-input-suffix">${clear(o)}<span data-part="count" class="ant-input-data-count">14 / 20</span></span></span>`;
  if(o.kind==='group') return `<div data-part="group" class="ant-space-compact ${o.direction==='rtl'?'ant-space-compact-rtl':''}">${field(o,'ant-input-compact-item ant-input-compact-first-item','first',80)}${field(o,'ant-input-compact-item','middle',100)}${field(o,'ant-input-compact-item ant-input-compact-last-item','last',120)}</div>`;
  return `<div data-part="otp" class="ant-otp ant-otp-css-var ${sizes[o.size]?`ant-otp-${sizes[o.size]}`:''} ${o.direction==='rtl'?'ant-otp-rtl':''}">${Array.from({length:6},(_,i)=>`<span class="ant-otp-input-wrapper"><span data-part="mask-${i}" class="ant-otp-mask-icon">●</span><input data-part="cell-${i}" class="${inputClass(o)} ant-otp-input ant-otp-mask-input" size="1" type="password" value="1" ${attrs(o)}></span>`).join('')}</div>`;
}

export function compositePage(css, globals, theme, profile) {
  const rows=[];
  for(const kind of ['affix','password','textarea-count','search-icon','search-label','group','masked-otp'])
    for(const direction of ['ltr','rtl']) for(const size of Object.keys(sizes))
      for(const variant of ['outlined','filled','borderless','underlined'])
        for(const [status,availability] of [['none','enabled'],['error','enabled'],['warning','enabled'],['none','disabled'],['none','readonly']]) {
          const o={kind,direction,size,variant,status,availability};
          rows.push(`<section class="sample" dir="${direction}"><label>${Object.values(o).join(' · ')}</label><div data-control='${JSON.stringify(o)}'>${render(o)}</div></section>`);
        }
  return `<!doctype html><meta charset="utf-8"><title>Input composite reference · ${theme} · ${profile}</title><style>${css}
body{margin:24px;font-family:${globals.fontFamily};font-size:${globals.fontSize}px;line-height:${globals.lineHeight};background:${globals.colorBgContainer};color:${globals.colorText};pointer-events:none}.sample{margin:16px 0 32px;width:480px}.sample>label{display:block;font-size:12px;color:${globals.colorTextSecondary};margin-bottom:8px}[data-control]{width:300px}.anticon{display:inline-flex;align-items:center;line-height:0}*{transition:none!important;animation:none!important}
</style>${rows.join('')}<script>
const state=new URLSearchParams(location.search).get('state')||'rest';
if(state!=='rest') {
  const style=document.createElement('style'); style.textContent=document.querySelector('style').textContent.replaceAll(':hover','[data-hover]').replaceAll(':focus-within','[data-focus-within]').replaceAll(':focus-visible','[data-focus-visible]').replaceAll(':focus','[data-focus]');document.head.append(style);
  document.querySelectorAll('[data-part]').forEach(el=>{
    const disabled=el.closest('[data-control]').dataset.control.includes('"availability":"disabled"');
    if(state==='hover')el.setAttribute('data-hover','');
    if(state==='focus'&&!disabled){el.setAttribute('data-focus','');el.setAttribute('data-focus-visible','');el.setAttribute('data-focus-within','');if(el.classList.contains('ant-input-affix-wrapper'))el.classList.add('ant-input-affix-wrapper-focused');}
  });
}
function measure(el,origin){const s=getComputedStyle(el),b=el.getBoundingClientRect();const r={x:b.x-origin.x,y:b.y-origin.y,width:b.width,height:b.height};for(const key of ['paddingTop','paddingRight','paddingBottom','paddingLeft','marginLeft','marginRight','fontSize','lineHeight','borderTopWidth','borderRightWidth','borderBottomWidth','borderLeftWidth','borderTopLeftRadius','borderTopRightRadius','borderBottomLeftRadius','borderBottomRightRadius','minHeight','minWidth'])r[key]=parseFloat(s[key])||0;for(const key of ['backgroundColor','borderTopColor','borderBottomColor','color','boxShadow','direction','columnGap','zIndex'])r[key]=s[key];return r;}
document.fonts.ready.then(()=>{const m=document.createElement('script');m.type='application/json';m.id='composite-metrics';m.textContent=JSON.stringify({theme:'${theme}',profile:'${profile}',state,scale:devicePixelRatio,controls:[...document.querySelectorAll('[data-control]')].map(root=>({options:JSON.parse(root.dataset.control),parts:Object.fromEntries([...root.querySelectorAll('[data-part]')].map(el=>[el.dataset.part,measure(el,root.getBoundingClientRect())]))}))});document.body.append(m);});
</script>`;
}
