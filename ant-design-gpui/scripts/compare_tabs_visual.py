# Copyright (C) 2026 mg-chao
# SPDX-License-Identifier: Apache-2.0
"""Compare real GPUI GPU frames with original Ant CSS in Chromium, without alignment or resampling."""
import argparse
import json
from pathlib import Path

import numpy as np
from PIL import Image


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--native', type=Path, required=True)
    parser.add_argument('--reference', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    native = json.loads((args.native / 'native-metrics.json').read_text(encoding='utf-8'))
    browser = json.loads((args.reference / 'browser-metrics.json').read_text(encoding='utf-8'))
    reference = {c['scenario']['name']: c['metrics'][0] for c in browser['cases']}
    results = []
    for case in native:
        name = case['name']
        ref = reference[name]
        a = np.asarray(Image.open(args.native / f'{name}.png').convert('RGB'), dtype=np.int16)
        b = np.asarray(Image.open(args.reference / f'{name}.png').convert('RGB'), dtype=np.int16)
        if a.shape != b.shape:
            raise ValueError(f'{name}: mismatched dimensions {a.shape}, {b.shape}')
        scale = case['scale']
        height = max(ref['root']['height'], max(i['y'] + i['height'] - 24 for i in case['items']))
        roi = (slice(round(24 * scale), round((24 + height) * scale)), slice(round(24 * scale), round(504 * scale)))
        delta = np.abs(a - b)[roi]
        foreground = (np.abs(a[roi] - a[0, 0]).max(axis=2) > 8) | (np.abs(b[roi] - b[0, 0]).max(axis=2) > 8)
        results.append({
            'name': name,
            'maximum_item_bounds_delta': {key: max(abs(item[key] - ref['items'][i]['box'][key]) for i, item in enumerate(case['items'])) for key in ('x', 'y', 'width', 'height')},
            'component_mean_absolute_channel_error': float(delta.mean()),
            'component_pixels_above_16': float((delta.max(axis=2) > 16).mean()),
            'foreground_mean_absolute_channel_error': float(delta[foreground].mean()),
        })
    report = {
        'cases': len(results), 'browser': browser['renderer'],
        'scale_factors': sorted({c['scale'] for c in native}),
        'maximum_item_bounds_delta': {key: max(r['maximum_item_bounds_delta'][key] for r in results) for key in ('x', 'y', 'width', 'height')},
        'maximum_component_mean_error': max(r['component_mean_absolute_channel_error'] for r in results),
        'mean_component_error': float(np.mean([r['component_mean_absolute_channel_error'] for r in results])),
        'results': results,
    }
    args.output.write_text(json.dumps(report, indent=2) + '\n', encoding='utf-8')
    print(json.dumps({k: v for k, v in report.items() if k != 'results'}, indent=2))


if __name__ == '__main__':
    main()
