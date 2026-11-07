// Copyright (C) 2026 mg-chao
// SPDX-License-Identifier: Apache-2.0
// Executes the original pinned Ant Design implementation, without recreated styles.
import React from 'react';
import { createRoot } from 'react-dom/client';
import Pagination from 'antd-reference/pagination';
import InputNumber from 'antd-reference/input-number';
import ConfigProvider from 'antd-reference/config-provider';
import theme from 'antd-reference/theme';
import enUS from 'antd-reference/locale/en_US';
import zhCN from 'antd-reference/locale/zh_CN';
import zhTW from 'antd-reference/locale/zh_TW';

const q = new URLSearchParams(location.search);
const initial = Object.fromEntries([...q].map(([key, value]) => [key,
  value === 'true' ? true : value === 'false' ? false : /^\d+$/.test(value) ? Number(value) : value]));
let events: unknown[] = [];
function record(type: string, ...args: unknown[]) {
  events.push({ type, args });
  document.querySelector('#events')!.textContent = JSON.stringify(events);
}
function CustomChanger({ disabled, value, onChange, className }: any) {
  return <InputNumber aria-label="Page Size" className={className} disabled={disabled}
    min={1} precision={0} style={{ width: 100 }} value={value}
    onChange={next => next !== null && onChange(next)} />;
}
function Fixture() {
  const [scenario, setScenario] = React.useState<any>(initial);
  const [epoch, setEpoch] = React.useState(0);
  const [controlled, setControlled] = React.useState<any>(undefined);
  const kind = scenario.kind ?? 'basic';
  const algorithm = scenario.theme ?? 'default';
  const current = scenario.current ?? 6;
  const pageSize = scenario.page_size ?? 10;
  const total = scenario.total ?? 500;
  const viewportWidth = scenario.viewport_width ?? 1280;
  const width = scenario.width ?? viewportWidth - 128;
  const sizeChanger = scenario.show_size_changer ?? ['changer', 'all', 'custom-changer'].includes(kind);
  const quick = ['quick', 'quick-go', 'all'].includes(kind) || scenario.show_quick_jumper;
  const go = kind === 'quick-go' || scenario.go;
  const simple = kind === 'simple' || kind === 'simple-readonly';
  const custom = kind === 'custom';
  const semantic = kind === 'semantic';
  const isControlled = scenario.controlled === 'frozen' || scenario.controlled === 'echo';
  const itemRender = custom ? (_: number, type: string, original: React.ReactNode) =>
    type === 'prev' ? <a>Previous</a> : type === 'next' ? <a>Next</a> : original : undefined;
  React.useEffect(() => {
    (window as any).paginationHarness = {
      reset(next: any) { events = []; record('reset'); events = []; document.querySelector('#events')!.textContent = '[]';
        setScenario(next); setControlled(undefined); setEpoch(value => value + 1); },
      patch(next: any) { setScenario((old: any) => ({ ...old, ...next })); },
      events: () => events,
    };
  }, []);
  const background = algorithm.startsWith('dark') ? '#141414' : '#ffffff';
  return <ConfigProvider direction={scenario.direction ?? 'ltr'}
    locale={scenario.locale === 'zh_CN' ? zhCN : scenario.locale === 'zh_TW' ? zhTW : enUS}
    componentSize={scenario.provider_size}
    input={{ variant: scenario.variant ?? 'outlined' }}
    theme={{ algorithm: algorithm === 'dark' ? theme.darkAlgorithm : algorithm === 'compact' ? theme.compactAlgorithm :
      algorithm === 'dark-compact' ? [theme.darkAlgorithm, theme.compactAlgorithm] : theme.defaultAlgorithm,
      token: { motion: false, wireframe: !!scenario.wireframe },
      components: scenario.tokens ? { Pagination: {
        itemSize: 36, itemSizeSM: 28, itemSizeLG: 44, itemBg: '#e6f4ff',
        itemActiveBg: '#d6e4ff', itemActiveColor: '#531dab', itemActiveColorHover: '#9254de',
        itemLinkBg: '#fff7e6', itemActiveBgDisabled: '#fff1f0', itemActiveColorDisabled: '#a8071a',
        itemInputBg: '#f9f0ff', miniOptionsSizeChangerTop: 2,
      } } : undefined }}>
    <main style={{ width: viewportWidth, height: 420, background }}>
      <div id="fixture" style={{ position: 'absolute', left: 64, top: 48, width }}>
        <Pagination key={epoch} aria-label="Pagination" total={total}
          defaultCurrent={current} defaultPageSize={pageSize}
          current={isControlled ? controlled?.current ?? current : undefined}
          pageSize={isControlled ? controlled?.pageSize ?? pageSize : undefined}
          size={scenario.size} responsive={scenario.responsive}
          align={scenario.align} disabled={scenario.disabled} showLessItems={scenario.show_less}
          hideOnSinglePage={scenario.hide_single} showTitle={scenario.show_title !== false}
          showPrevNextJumpers={scenario.show_jumpers !== false}
          showSizeChanger={sizeChanger === 'auto' ? undefined : sizeChanger ? { open: !!scenario.open } : false}
          totalBoundaryShowSizeChanger={scenario.size_boundary}
          pageSizeOptions={scenario.page_size_options}
          showQuickJumper={quick ? go ? { goButton: true } : true : false}
          simple={simple ? kind === 'simple-readonly' ? { readOnly: true } : true : false}
          showTotal={kind === 'total' || kind === 'all' ? (total, range) => `${range[0]}-${range[1]} of ${total} items` : undefined}
          components={kind === 'custom-changer' ? { sizeChanger: CustomChanger } : undefined}
          itemRender={itemRender}
          styles={semantic ? { root: { border: '2px dashed #ccc', padding: 8 }, item: { borderRadius: 999,
            ...(scenario.size === 'small' ? { background: 'rgba(200,200,200,.3)', marginInlineEnd: 4 } : {}) } } : undefined}
          onChange={(page, pageSize) => { record('change', page, pageSize);
            if (scenario.controlled === 'echo') setControlled({ current: page, pageSize }); }}
          onShowSizeChange={(...args) => record('size-change', ...args)} />
      </div>
    </main>
    <pre id="events" aria-label="Event trace" style={{ display: 'none' }}>[]</pre>
  </ConfigProvider>;
}
createRoot(document.getElementById('root')!).render(<Fixture />);

(window as any).capturePaginationMetrics = () => {
  const selectors = ['.ant-pagination', '.ant-pagination > li', '.ant-pagination-item > a',
    '.ant-pagination-item-link', '.ant-pagination-item-link-icon', '.ant-pagination-item-ellipsis',
    '.ant-pagination-simple-pager', '.ant-pagination-slash', '.ant-pagination input',
    '.ant-pagination-options-quick-jumper', '.ant-pagination-options-quick-jumper-button',
    '.ant-select', '.ant-select-input', '.ant-select-suffix', '.ant-select-dropdown',
    '.ant-select-item-option', '.ant-input-number'];
  const elements = selectors.flatMap(selector => [...document.querySelectorAll(selector)].map(element => {
    const r = element.getBoundingClientRect(); const s = getComputedStyle(element);
    return { selector, text: element.textContent, class: element.className,
      x: r.x, y: r.y, w: r.width, h: r.height, display: s.display,
      background: s.backgroundColor, border: s.borderColor, borderWidth: s.borderWidth, radius: s.borderRadius,
      color: s.color, fontSize: s.fontSize, fontWeight: s.fontWeight, lineHeight: s.lineHeight,
      padding: s.padding, margin: s.margin, gap: s.gap, opacity: s.opacity,
      outline: s.outline, outlineOffset: s.outlineOffset, boxShadow: s.boxShadow,
      title: element.getAttribute('title'), label: element.getAttribute('aria-label'),
      disabled: element.getAttribute('aria-disabled') ?? (element as HTMLInputElement).disabled ?? null,
      value: (element as HTMLInputElement).value ?? null,
    };
  }));
  const items = [...document.querySelectorAll('.ant-pagination > li')].map(element => ({
    text: element.textContent, title: element.getAttribute('title'), class: element.className,
    visible: getComputedStyle(element).display !== 'none',
    disabled: element.getAttribute('aria-disabled'), tabIndex: (element as HTMLElement).tabIndex,
  }));
  return { scale: devicePixelRatio, viewport: [innerWidth, innerHeight], elements, items, events,
    activeText: document.querySelector('.ant-pagination-item-active')?.textContent ?? null,
    simpleValue: (document.querySelector('.ant-pagination-simple-pager input') as HTMLInputElement)?.value ?? null,
    quickValue: (document.querySelector('.ant-pagination-options-quick-jumper input') as HTMLInputElement)?.value ?? null,
  };
};
setTimeout(() => {
  const script = document.createElement('script');
  script.id = 'visual-metrics'; script.type = 'application/json';
  script.textContent = JSON.stringify((window as any).capturePaginationMetrics());
  document.body.append(script);
}, 300);
