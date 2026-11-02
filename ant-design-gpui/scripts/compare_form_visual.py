# Copyright (C) 2026 mg-chao
# SPDX-License-Identifier: Apache-2.0
"""Strict original-browser/native Form geometry and solid-paint comparison.

Screenshots are never aligned, resized, or replaced. Text and SVG glyph regions
are excluded from solid-paint gates, whose independent geometry is still checked.
"""
import argparse
import hashlib
import json
from pathlib import Path

import numpy as np
from PIL import Image, ImageFilter


REQUIRED_SLOTS = ('root', 'label', 'control', 'input', 'help', 'extra', 'feedback')


def geometry(expected, actual):
    if expected is None or actual is None:
        return {'passed': expected is None and actual is None,
                'expected': expected, 'actual': actual}
    delta = {key: actual[key] - expected[key] for key in ('x', 'y', 'width', 'height')}
    return {'passed': max(map(abs, delta.values())) <= 1.001, 'delta': delta}


def exclude(mask, rect, scale, margin=2):
    if not rect:
        return
    x = max(0, int(np.floor((rect['x'] - margin) * scale)))
    y = max(0, int(np.floor((rect['y'] - margin) * scale)))
    right = min(mask.shape[1], int(np.ceil((rect['x'] + rect['width'] + margin) * scale)))
    bottom = min(mask.shape[0], int(np.ceil((rect['y'] + rect['height'] + margin) * scale)))
    mask[y:bottom, x:right] = False


def dilate(mask, radius):
    return np.asarray(Image.fromarray(mask.astype('uint8') * 255).filter(
        ImageFilter.MaxFilter(radius * 2 + 1))) > 0


def compare_case(expected, actual, reference_image, native_image):
    assert expected['scale'] == actual['scale'], 'Device scales differ'
    assert expected['viewport'] == actual['viewport'], 'Logical viewports differ'
    scale = expected['scale']
    viewport = expected['viewport']
    size = (int(np.floor(viewport['width'] * scale + .5)), int(np.floor(viewport['height'] * scale + .5)))
    assert reference_image.size == native_image.size == size, 'PNG dimensions differ'
    assert [f['id'] for f in expected['forms']] == [f['id'] for f in actual['forms']], 'Form identity/order differs'
    e = np.asarray(reference_image.convert('RGB')).astype('int16')
    a = np.asarray(native_image.convert('RGB')).astype('int16')
    valid = np.ones(e.shape[:2], dtype=bool)
    forms = []
    for original, measured in zip(expected['forms'], actual['forms']):
        assert [i['id'] for i in original['items']] == [i['id'] for i in measured['items']], 'Field identity/order differs'
        slots = {'root': geometry(original['root'], measured['root'])}
        if 'wrapper' in measured:
            slots['wrapper'] = geometry(original['wrapper'], measured['wrapper'])
        def visit(field, candidate, prefix=''):
            assert all(slot in candidate for slot in REQUIRED_SLOTS), 'Missing mandatory measured field slot'
            identity = f'{prefix}{field["id"]}'
            for slot in REQUIRED_SLOTS:
                slots[f'{identity}.{slot}'] = geometry(field[slot], candidate[slot])
            for slot in ('row', 'label_text', 'tooltip'):
                if slot in candidate:
                    slots[f'{identity}.{slot}'] = geometry(field.get(slot), candidate[slot])
            for rect in field.get('textRects', []):
                exclude(valid, rect, scale)
            # Label pseudo-glyphs (required star and colon) are not DOM text nodes.
            for slot in ('label_text', 'editor_text_rect', 'feedback', 'tooltip'):
                exclude(valid, field.get(slot), scale)
            # Original SVG suffixes have independent component tests; the Form
            # gate still checks the input shell and mandatory feedback box.
            for icon in field.get('icons', []):
                exclude(valid, icon, scale)
            children, candidates = field.get('children', []), candidate.get('children', [])
            assert [child['id'] for child in children] == [child['id'] for child in candidates], 'Nested field identity/order differs'
            for child, child_candidate in zip(children, candidates):
                visit(child, child_candidate, f'{identity}.')
        for field, candidate in zip(original['items'], measured['items']):
            visit(field, candidate)
        forms.append({'id': original['id'], 'geometry_pass': all(v['passed'] for v in slots.values()), 'slots': slots})
    radius = max(1, int(np.ceil(scale)))
    image = Image.fromarray(e.astype('uint8'))
    high = np.asarray(image.filter(ImageFilter.MaxFilter(radius * 2 + 1))).astype('int16')
    low = np.asarray(image.filter(ImageFilter.MinFilter(radius * 2 + 1))).astype('int16')
    # Gate every independently observed flat interior, including the background;
    # a misplaced native surface cannot hide outside a component-only crop.
    flat = valid & (np.max(high - low, axis=2) == 0)
    flat[:radius] = flat[-radius:] = False
    flat[:, :radius] = flat[:, -radius:] = False
    difference = np.max(abs(e - a), axis=2)
    values = difference[flat]
    paint_errors = int((values > 1).sum())
    background = e[0, 0]
    em = np.max(abs(e - background), axis=2) > 3
    am = np.max(abs(a - background), axis=2) > 3
    coverage_valid = valid & ~dilate(~valid, radius)
    coverage_valid[:radius] = coverage_valid[-radius:] = False
    coverage_valid[:, :radius] = coverage_valid[:, -radius:] = False
    # Foreground binarization must respect the same one-byte color budget.
    # E.g. background240/reference243/native244 crosses the 3/4 foreground
    # boundary but differs by only one accepted byte, not a missing surface.
    outside_color_budget = difference > 1
    missing = em & ~dilate(am, radius) & coverage_valid & outside_color_budget
    extra = am & ~dilate(em, radius) & coverage_valid & outside_color_budget
    coverage_errors = int(missing.sum() + extra.sum())
    return {'name': expected['name'], 'forms': forms,
            'geometry_pass': all(form['geometry_pass'] for form in forms),
            'paint_pass': paint_errors == 0 and coverage_errors == 0,
            'flat_interior_pixels': int(values.size),
            'flat_interior_max_error': int(values.max()) if values.size else 0,
            'flat_interior_error_pixels': paint_errors,
            'coverage_error_pixels': coverage_errors,
            'raw_mean_channel_error': float(abs(e-a).mean())}


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--browser', type=Path, required=True)
    parser.add_argument('--native', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    for key in ('scene', 'theme', 'direction'):
        parser.add_argument(f'--{key}')
    parser.add_argument('--scale', type=float)
    args = parser.parse_args()
    browser = json.loads((args.browser / 'browser-metrics.json').read_text(encoding='utf-8'))
    native = json.loads((args.native / 'native-metrics.json').read_text(encoding='utf-8'))
    if isinstance(native, dict):
        native = native['cases']
    def wanted(case):
        return all(getattr(args, key) is None or case[key] == getattr(args, key)
                   for key in ('scene', 'theme', 'direction', 'scale'))
    source_cases = [case for case in browser['cases'] if wanted(case)]
    native_cases = [case for case in native if wanted(case)]
    expected = {case['name']: case for case in source_cases}
    actual = {case['name']: case for case in native_cases}
    assert len(expected) == len(source_cases) and len(actual) == len(native_cases), 'Duplicate scenario identity'
    assert expected and expected.keys() == actual.keys(), ('Scenario sets must match exactly',
        sorted(expected.keys()-actual.keys()), sorted(actual.keys()-expected.keys()))
    records = []
    for name, case in expected.items():
        reference_path, native_path = args.browser / f'{name}.png', args.native / f'{name}.png'
        expected_hash = hashlib.sha256(reference_path.read_bytes()).hexdigest()
        assert case.get('png_sha256', expected_hash) == expected_hash, 'Reference PNG hash changed'
        result = compare_case(case, actual[name], Image.open(reference_path), Image.open(native_path))
        result.update(reference_sha256=expected_hash, native_sha256=hashlib.sha256(native_path.read_bytes()).hexdigest())
        records.append(result)
    failures = [record for record in records if not record['geometry_pass'] or not record['paint_pass']]
    report = {'schema_version': 1, 'passed': not failures, 'comparisons': len(records),
              'provenance': browser['provenance'], 'renderer': browser['renderer'],
              'gates': {'geometry_logical_px': 1, 'solid_channel_error_bytes': 1},
              'limits': 'Glyph rasterization, reference text bounds and SVG glyph bounds are excluded from solid-paint gates. Mandatory field/input/help/extra/feedback geometry is always gated. Optional extra measured geometry is gated when provided. Raw raster error is reported without claiming identical glyph pixels.',
              'failures': [record['name'] for record in failures], 'cases': records}
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(json.dumps(report, indent=2)+'\n', encoding='utf-8', newline='\n')
    print(json.dumps({'comparisons':len(records), 'passed':not failures, 'failed_cases':len(failures)}))
    for case in failures[:15]:
        bad = {form['id']:{slot:value for slot,value in form['slots'].items() if not value['passed']}
               for form in case['forms'] if not form['geometry_pass']}
        print(case['name'], json.dumps(bad), 'paint_pixels', case['flat_interior_error_pixels'])
    if failures:
        raise SystemExit(1)


if __name__ == '__main__':
    main()
