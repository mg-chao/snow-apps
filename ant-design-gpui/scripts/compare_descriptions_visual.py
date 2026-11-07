# Copyright (C) 2026 mg-chao
# SPDX-License-Identifier: Apache-2.0
"""Compare original-source Chromium and native GPU Descriptions captures.

Geometry is gated independently of pixels. Glyph boxes and a one-logical-pixel
edge band are excluded from flat-color gates; complete raw errors are retained.
Images are never aligned, resized, or substituted for one another.
"""
import argparse
import hashlib
import json
from pathlib import Path

import numpy as np
from PIL import Image, ImageFilter


def geometry(expected, actual):
    if expected is None or actual is None:
        return {'present_pass': expected is None and actual is None,
                'geometry_pass': expected is None and actual is None,
                'expected': expected, 'actual': actual}
    delta = {key: actual[key] - expected[key] for key in ('x', 'y', 'width', 'height')}
    return {'present_pass': True, 'geometry_pass': max(map(abs, delta.values())) <= 1.01,
            'delta': delta, 'expected': {k: expected[k] for k in delta}, 'actual': actual}


def exclude(mask, rectangle, origin, scale, margin=2):
    x = int(np.floor((rectangle['x'] - origin['x'] - margin) * scale))
    y = int(np.floor((rectangle['y'] - origin['y'] - margin) * scale))
    right = int(np.ceil((rectangle['x'] + rectangle['width'] - origin['x'] + margin) * scale))
    bottom = int(np.ceil((rectangle['y'] + rectangle['height'] - origin['y'] + margin) * scale))
    mask[max(0, y):min(mask.shape[0], bottom), max(0, x):min(mask.shape[1], right)] = False


def dilate(mask, radius):
    return np.asarray(Image.fromarray(mask.astype('uint8') * 255).filter(
        ImageFilter.MaxFilter(radius * 2 + 1))) > 0


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--browser', type=Path, required=True)
    parser.add_argument('--native', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--scene', help='Compare this scene only; remaining scenarios must match exactly')
    parser.add_argument('--theme')
    parser.add_argument('--direction')
    parser.add_argument('--scale', type=float)
    args = parser.parse_args()
    browser = json.loads((args.browser / 'browser-metrics.json').read_text(encoding='utf-8'))
    native = json.loads((args.native / 'native-metrics.json').read_text(encoding='utf-8'))
    wanted = lambda case: ((args.scene is None or case['scene'] == args.scene)
                           and (args.theme is None or case['theme'] == args.theme)
                           and (args.direction is None or case['direction'] == args.direction)
                           and (args.scale is None or case['scale'] == args.scale))
    expected = {case['name']: case for case in browser['cases'] if wanted(case)}
    actual = {case['name']: case for case in native if wanted(case)}
    assert expected and actual, 'Empty comparison set'
    assert expected.keys() == actual.keys(), ('Scenario sets must match exactly',
                                             sorted(expected.keys() - actual.keys()),
                                             sorted(actual.keys() - expected.keys()))
    records, failures = [], []
    for name, case in expected.items():
        native_case = actual[name]
        assert case['scale'] == native_case['scale']
        assert [c['id'] for c in case['components']] == [c['id'] for c in native_case['components']]
        expected_path, actual_path = args.browser / f'{name}.png', args.native / f'{name}.png'
        expected_image = Image.open(expected_path).convert('RGB')
        actual_image = Image.open(actual_path).convert('RGB')
        scale = case['scale']
        assert expected_image.size == actual_image.size == (
            int(np.floor(case['viewport']['width'] * scale + .5)),
            int(np.floor(case['viewport']['height'] * scale + .5)))
        background = np.asarray(expected_image.getpixel((0, 0)), dtype='int16')
        components = []
        for reference, measured in zip(case['components'], native_case['components']):
            slots = {}
            for slot in ('root', 'header', 'title', 'extra'):
                rectangle = reference[slot]
                if (slot == 'header' and rectangle is not None and measured[slot] is None
                        and (rectangle['width'] == 0 or rectangle['height'] == 0)):
                    slots[slot] = {'geometry_gated': False, 'geometry_pass': None,
                                   'measurement_limit': 'Zero-area header produces no native GPU quad; root geometry is gated and header presence/height require the independent offscreen test.',
                                   'expected': rectangle, 'actual': None}
                else:
                    slots[slot] = geometry(rectangle, measured[slot])
            reference_items = {}
            for row in reference['rows']:
                for cell in row['cells']:
                    item = reference_items.setdefault(cell['item'], {'label': None, 'content': None})
                    for slot in ('label', 'content'):
                        if cell[slot] is not None:
                            item[slot] = cell[slot]
                    # The first outer borderless cell also exists for empty slots.
                    # In vertical mode this is the label row; the content slot has
                    # its own geometry below it, and shares the same track width.
                    if cell['kind'] == 'item':
                        item.setdefault('cell', cell['bounds'])
            assert set(reference_items) == {i['item'] for i in measured['items']}
            for item in measured['items']:
                for slot, rectangle in reference_items[item['item']].items():
                    slots[f'item.{item["item"]}.{slot}'] = geometry(rectangle, item[slot])
            root = reference['root']
            x, y = max(0, round(root['x'] * scale)), max(0, round(root['y'] * scale))
            right = min(expected_image.width, round((root['x'] + root['width']) * scale))
            bottom = min(expected_image.height, round((root['y'] + root['height']) * scale))
            # Convert only measured component crops; high-DPI blank page areas
            # need neither signed pixel buffers nor repeated color arithmetic.
            e = np.asarray(expected_image.crop((x, y, right, bottom))).astype('int16')
            a = np.asarray(actual_image.crop((x, y, right, bottom))).astype('int16')
            valid = np.ones(e.shape[:2], dtype=bool)
            origin = {'x': x / scale, 'y': y / scale}
            for slot in ('title', 'extra'):
                if reference[slot] is not None:
                    exclude(valid, reference[slot], origin, scale)
            for row in reference['rows']:
                for cell in row['cells']:
                    for rectangle in cell['textRects']:
                        exclude(valid, rectangle, origin, scale)
                    if cell['kind'] == 'item' and cell['label'] is not None:
                        exclude(valid, cell['label'], origin, scale)
            radius = max(1, int(np.ceil(scale)))
            if e.size:
                image = Image.fromarray(e.astype('uint8'))
                high = np.asarray(image.filter(ImageFilter.MaxFilter(radius * 2 + 1))).astype('int16')
                low = np.asarray(image.filter(ImageFilter.MinFilter(radius * 2 + 1))).astype('int16')
                flat = valid & (np.max(high - low, axis=2) == 0)
                flat[:radius] = False
                flat[-radius:] = False
                flat[:, :radius] = False
                flat[:, -radius:] = False
                values = np.max(abs(e - a), axis=2)[flat]
                paint_max = int(values.max()) if values.size else 0
                paint_errors = int((values > 1).sum())
                em = np.max(abs(e - background), axis=2) > 3
                am = np.max(abs(a - background), axis=2) > 3
                coverage_valid = valid & ~dilate(~valid, radius)
                missing = em & ~dilate(am, radius) & coverage_valid
                extra = am & ~dilate(em, radius) & coverage_valid
                coverage_errors = int(missing.sum() + extra.sum())
                raw_mean = float(abs(e - a).mean())
            else:
                paint_max, paint_errors, coverage_errors, raw_mean, values = 0, 0, 0, 0., np.array([])
            record = {'id': reference['id'], 'geometry_pass': all(s['geometry_pass'] for s in slots.values()
                                                                   if s.get('geometry_gated', True)),
                      'slots': slots, 'flat_interior_pixels': int(values.size),
                      'flat_interior_max_error': paint_max, 'flat_interior_error_pixels': paint_errors,
                      'coverage_error_pixels': coverage_errors,
                      'paint_pass': paint_errors == 0 and coverage_errors == 0, 'raw_mean_channel_error': raw_mean}
            components.append(record)
            if not record['geometry_pass'] or not record['paint_pass']:
                failures.append({'case': name, **record})
        records.append({'name': name, 'reference_sha256': hashlib.sha256(expected_path.read_bytes()).hexdigest(),
                        'native_sha256': hashlib.sha256(actual_path.read_bytes()).hexdigest(),
                        'components': components})
    report = {'schema_version': 1, 'passed': not failures, 'comparisons': len(records),
              'provenance': browser['provenance'], 'renderer': browser['renderer'],
              'gates': {'geometry_logical_px': 1, 'flat_interior_channel_error': 1},
              'limits': 'Reference text rectangles, header text slots, and edge antialiasing are excluded from paint gates. Nonzero geometry slots are gated; zero-area headers without GPU quads are explicitly marked ungated and require independent presence/height tests. Raw image error is retained.',
              'failures': failures, 'cases': records}
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(json.dumps(report, indent=2) + '\n', encoding='utf-8', newline='\n')
    print(json.dumps({'comparisons': len(records), 'passed': not failures, 'failed_components': len(failures)}))
    for failure in failures[:20]:
        bad = {name: slot.get('delta', 'presence') for name, slot in failure['slots'].items()
               if slot.get('geometry_gated', True) and not slot['geometry_pass']}
        print(failure['case'], failure['id'], bad, 'paint_pixels', failure['flat_interior_error_pixels'])
    if failures:
        raise SystemExit(1)


if __name__ == '__main__':
    main()
