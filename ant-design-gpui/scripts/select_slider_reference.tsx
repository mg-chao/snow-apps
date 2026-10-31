// Copyright (C) 2026 mg-chao
// SPDX-License-Identifier: Apache-2.0
// Executed against the original pinned Ant Design source by the reference builder.
import React from 'react';
import { createRoot } from 'react-dom/client';
import Select from 'antd-reference/select';
import Slider from 'antd-reference/slider';
import ConfigProvider from 'antd-reference/config-provider';
import theme from 'antd-reference/theme';
import enUS from 'antd-reference/locale/en_US';

const q = new URLSearchParams(location.search);
const kind = q.get('kind') ?? 'basic';
const component = q.get('component') ?? 'select';
const algorithm = q.get('theme') ?? 'default';
const direction = q.get('direction') === 'rtl' ? 'rtl' : 'ltr';
const size = q.get('size') ?? 'middle';
const customTokens = q.get('tokens') === 'true';
const events: unknown[] = [];
function record(type: string, ...args: unknown[]) {
  events.push({ type, args });
  const node = document.getElementById('events');
  if (node) node.textContent = JSON.stringify(events);
}
const options = kind === 'empty' ? [] : [
  { value: 'jack', label: 'Jack' }, { value: 'lucy', label: 'Lucy' },
  { value: 'disabled', label: 'Disabled', disabled: true },
  { value: 'yiminghe', label: 'Yiminghe' },
];
function Fixture() {
  const multiple = kind === 'multiple' || kind === 'tags' || kind === 'responsive';
  const select = <Select
    aria-label="Choose a value" style={{ width: 280 }} options={options}
    size={size as any} variant={(q.get('variant') ?? 'outlined') as any}
    status={q.get('status') as any} disabled={kind === 'disabled'}
    loading={kind === 'loading'} placeholder="Please select" allowClear
    defaultValue={kind === 'placeholder' || kind === 'empty' ? undefined : multiple ? ['lucy', 'jack'] : 'lucy'}
    mode={multiple ? kind === 'tags' ? 'tags' : 'multiple' : undefined}
    showSearch={kind === 'search' || multiple} tokenSeparators={[',', ';']}
    maxTagCount={kind === 'responsive' ? 'responsive' : undefined}
    defaultOpen={q.get('open') === 'true'}
    placement={(q.get('placement') ?? 'bottomLeft') as any}
    popupMatchSelectWidth={q.has('popup_width') ? Number(q.get('popup_width')) : true}
    prefix={kind === 'custom' ? <span>To:</span> : undefined}
    suffixIcon={kind === 'custom' ? <span>+</span> : undefined}
    optionRender={kind === 'custom' ? option => <span>{option.label} / team</span> : undefined}
    labelRender={kind === 'custom' ? value => <span>{value.label} / team</span> : undefined}
    onChange={(...args) => record('change', ...args)}
    onSelect={(...args) => record('select', ...args)}
    onDeselect={(...args) => record('deselect', ...args)}
    onSearch={(...args) => record('search', ...args)}
    onOpenChange={(...args) => record('open', ...args)}
    onClear={() => record('clear')}
  />;
  const marks = kind === 'marks' ? { 0: '0°C', 26: '26°C', 37: '37°C', 100: '100°C' } : undefined;
  const range = ['range', 'editable', 'draggable', 'disabled-handle'].includes(kind);
  const slider = <Slider
    aria-label="Value" style={kind === 'vertical' ? { height: 280 } : { width: 280 }}
    defaultValue={range ? [20, 50, 80] : 30}
    range={kind === 'editable' ? { editable: true, minCount: 1, maxCount: 5 } : kind === 'draggable' ? { draggableTrack: true } : range}
    disabled={kind === 'disabled-handle' ? [false, true, false] : kind === 'disabled'}
    vertical={kind === 'vertical'} reverse={q.get('reverse') === 'true'}
    marks={marks} dots={q.get('dots') === 'true'} step={marks ? null : 1}
    tooltip={{ open: q.get('open') === 'true' ? true : undefined }}
    onChange={(...args) => record('change', ...args)}
    onChangeComplete={(...args) => record('complete', ...args)}
  />;
  return <ConfigProvider direction={direction} locale={enUS} theme={{
    algorithm: algorithm === 'dark' ? theme.darkAlgorithm : algorithm === 'compact' ? theme.compactAlgorithm : algorithm === 'dark-compact' ? [theme.darkAlgorithm, theme.compactAlgorithm] : theme.defaultAlgorithm,
    token: { motion: false },
    components: customTokens ? {
      Select: { selectorBg: '#f4fff8', optionSelectedBg: '#c3f5d2', optionHeight: 40, optionPadding: '9px 16px' },
      Slider: { railSize: 6, handleSize: 14, dotSize: 10, railBg: '#eadfff', trackBg: '#8855cc', handleColor: '#8855cc' },
    } : undefined,
  }}>
    <main style={{ width: 800, height: 500, background: algorithm.startsWith('dark') ? '#141414' : '#ffffff' }}>
      <div id="fixture" style={{ position: 'absolute', left: Number(q.get('anchor_x') ?? 64), top: Number(q.get('anchor_y') ?? 48), width: 280 }}>
        {component === 'select' ? select : slider}
      </div>
    </main>
    <pre id="events" aria-label="Event trace" style={{ margin: 0, whiteSpace: 'pre-wrap' }}>[]</pre>
  </ConfigProvider>;
}
createRoot(document.getElementById('root')!).render(<Fixture />);
if (q.get('focus') === 'true') setTimeout(() => {
  (document.querySelector(component === 'select' ? '.ant-select-input' : '.ant-slider-handle') as HTMLElement)?.focus();
}, 100);

// The standalone regression renderer reads only DOM geometry and computed styles.
// This also makes captures reproducible without a connected browser session.
setTimeout(() => {
  const selectors = component === 'select' ? [
    '.ant-select', '.ant-select-content', '.ant-select-input', '.ant-select-suffix',
    '.ant-select-selection-item', '.ant-select-selection-item-remove',
    '.ant-select-dropdown', '.ant-select-item-option', '.ant-empty-image', '.ant-empty-description',
  ] : ['.ant-slider', '.ant-slider-rail', '.ant-slider-track', '.ant-slider-handle', '.ant-slider-dot', '.ant-slider-mark-text', '.ant-tooltip'];
  const elements = selectors.flatMap(selector => [...document.querySelectorAll(selector)].map(element => {
    const r = element.getBoundingClientRect(); const s = getComputedStyle(element);
    const pseudo = getComputedStyle(element, '::after');
    return { selector, text: element.textContent, x: r.x, y: r.y, w: r.width, h: r.height,
      background: s.backgroundColor, border: s.borderColor, borderWidth: s.borderWidth, radius: s.borderRadius,
      color: s.color, fontSize: s.fontSize, lineHeight: s.lineHeight, padding: s.padding, margin: s.margin,
      boxShadow: s.boxShadow, opacity: s.opacity,
      after: { w: pseudo.width, h: pseudo.height, background: pseudo.backgroundColor, boxShadow: pseudo.boxShadow, radius: pseudo.borderRadius },
    };
  }));
  const script = document.createElement('script');
  script.id = 'visual-metrics'; script.type = 'application/json';
  script.textContent = JSON.stringify({ scale: devicePixelRatio, viewport: [innerWidth, innerHeight], elements });
  document.body.append(script);
}, 300);
