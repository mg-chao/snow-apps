// Copyright (C) 2026 mg-chao
// SPDX-License-Identifier: Apache-2.0
// Browser-side timing harness. Expected values come only from original CSS.

function measureMotion() {
  const properties = ['color', 'backgroundColor', 'borderTopColor', 'borderBottomColor', 'boxShadow',
    'width', 'height', 'opacity', 'fontSize', 'transitionDuration', 'transitionTimingFunction'];
  const measure = element => Object.fromEntries(properties.map(key => [key, getComputedStyle(element)[key]]));
  const output = { theme: document.body.dataset.theme, scale: devicePixelRatio, cases: [] };
  async function run() {
    for (const sample of document.querySelectorAll('[data-case]')) {
      const root = sample.firstElementChild;
      const target = root.querySelector('[data-target]') || root;
      const trigger = root.querySelector('[data-trigger]') || root;
      const attribute = sample.dataset.attribute || 'data-hover';
      // Reading style before mutation establishes the CSS before-change style.
      const before = measure(target);
      trigger.setAttribute(attribute, '');
      measure(target);
      const animations = root.getAnimations({ subtree: true });
      animations.forEach(animation => animation.pause());
      await Promise.all(animations.map(animation => animation.ready));
      const transitions = animations.map(animation => ({
        property: animation.transitionProperty,
        duration: animation.effect.getTiming().duration,
        easing: animation.effect.getTiming().easing,
        target: animation.effect.target === target,
      }));
      const samples = [];
      for (const time of [0, 50, 100, 150, 200, 250, 300]) {
        animations.forEach(animation => { animation.currentTime = time; });
        samples.push({ time, style: measure(target) });
      }
      // A partial reversal distinguishes CSS reversing-shortening from a new
      // full-duration interpolation. Finish-state sampling above stays isolated.
      animations.forEach(animation => { animation.currentTime = 50; });
      const turning = measure(target);
      trigger.removeAttribute(attribute);
      measure(target);
      const reverse = root.getAnimations({ subtree: true });
      reverse.forEach(animation => animation.pause());
      await Promise.all(reverse.map(animation => animation.ready));
      const reverseTransitions = reverse.map(animation => ({
        property: animation.transitionProperty,
        duration: animation.effect.getTiming().duration,
        easing: animation.effect.getTiming().easing,
        target: animation.effect.target === target,
      }));
      const reverseSamples = [];
      for (const time of [0, 25, 50, 75, 100, 150, 200, 300]) {
        reverse.forEach(animation => { animation.currentTime = time; });
        reverseSamples.push({ time, style: measure(target) });
      }
      output.cases.push({ name: sample.dataset.case, before, transitions, samples,
        turning, reverseTransitions, reverseSamples });
      reverse.forEach(animation => animation.finish());
    }
    const metrics = document.createElement('script');
    metrics.id = 'motion-metrics'; metrics.type = 'application/json';
    metrics.textContent = JSON.stringify(output);
    document.body.append(metrics);
    document.title += ' · complete';
  }
  run().catch(error => { document.body.dataset.error = String(error.stack || error); });
}

export function motionPage(css, globals, theme) {
  const states = css.replaceAll(':hover', '[data-hover]')
    .replaceAll(':focus-within', '[data-focus-within]')
    .replaceAll(':focus-visible', '[data-focus-visible]')
    .replaceAll(':focus', '[data-focus]').replaceAll(':active', '[data-active]');
  const cases = [];
  const add = (name, content, attribute = 'data-hover') =>
    cases.push(`<section data-case="${name}" data-attribute="${attribute}">${content}</section>`);
  for (const variant of ['outlined', 'filled', 'borderless', 'underlined']) {
    for (const state of ['hover', 'focus']) {
      add(`input-${variant}-${state}`,
        `<input class="ant-input ant-input-${variant} ant-input-css-var" value="Ant Design">`, `data-${state}`);
    }
  }
  for (const [action, tag] of [['clear-icon', 'button'], ['password-icon', 'span']]) {
    for (const state of action === 'clear-icon' ? ['hover', 'active', 'focus-visible'] : ['hover']) {
      add(`${action}-${state}`, `<span class="ant-input-affix-wrapper ant-input-outlined ant-input-css-var"><input class="ant-input" value="Value"><span class="ant-input-suffix"><${tag} data-trigger data-target class="ant-input-${action}">×</${tag}></span></span>`, `data-${state}`);
    }
  }
  for (const variant of ['outlined', 'filled', 'borderless', 'underlined']) {
    const number = (target, rootHover, actionState = '') =>
      `<div class="ant-input-number ant-input-number-mode-input ant-input-number-${variant} ant-input-number-css-var" ${rootHover ? 'data-hover' : ''}><div class="ant-input-number-input-wrap"><input class="ant-input-number-input" value="1234.5"></div><div class="ant-input-number-actions" ${target === 'actions' ? 'data-target' : ''}><span class="ant-input-number-action ant-input-number-action-up" ${target === 'up' ? 'data-target data-trigger' : ''} ${actionState}><span class="ant-input-number-action-up-inner">⌃</span></span><span class="ant-input-number-action ant-input-number-action-down"><span class="ant-input-number-action-down-inner">⌄</span></span></div></div>`;
    add(`numeric-${variant}-reveal`, number('actions', false));
    add(`numeric-${variant}-up-hover`, number('up', true));
    add(`numeric-${variant}-up-active`, number('up', true, 'data-hover'), 'data-active');
  }
  return `<!doctype html><html lang="en"><meta charset="utf-8"><title>Input motion · ${theme}</title><style>${css}\n${states}
body{margin:24px;font-family:${globals.fontFamily};color:${globals.colorText};background:${globals.colorBgContainer};pointer-events:none}
section{padding:10px;width:340px}section>input{width:300px}
</style><body data-theme="${theme}"><h1>Original CSS motion · ${theme}</h1>${cases.join('')}<script>(${measureMotion.toString()})()</script></body></html>`;
}
