# Copyright (C) 2026 mg-chao
# SPDX-License-Identifier: Apache-2.0
"""Compare independent browser and native Progress captures at native device scale.

Root geometry and painted interiors are gates. Text rasterization and edge AA are
reported separately, never claimed to be pixel-identical. No image is rescaled.
"""
import argparse
import hashlib
import json
from pathlib import Path

import numpy as np
from PIL import Image, ImageFilter


def dilate(mask, radius):
    return np.asarray(Image.fromarray(mask.astype('uint8') * 255).filter(
        ImageFilter.MaxFilter(radius * 2 + 1))) > 0


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--browser', type=Path, required=True)
    parser.add_argument('--native', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    browser = json.loads((args.browser / 'browser-metrics.json').read_text())
    native_rows = json.loads((args.native / 'native-metrics.json').read_text())
    expected = {c['name']: c for c in browser['cases']}
    actual = {c['name']: c for c in native_rows}
    assert expected and len(expected) == len(browser['cases'])
    assert len(actual) == len(native_rows) and expected.keys() == actual.keys(), 'Scenario sets must match exactly'
    records = []
    failures = []
    for name, case in expected.items():
        native = actual[name]
        assert case['scale'] == native['scale']
        assert len(case['rows']) == len(native['rows'])
        ep = args.browser / f'{name}.png'
        ap = args.native / f'{name}.png'
        e = np.asarray(Image.open(ep).convert('RGB')).astype('int16')
        a = np.asarray(Image.open(ap).convert('RGB')).astype('int16')
        assert e.shape == a.shape == (round(1000*case['scale']), round(740*case['scale']), 3)
        scale = case['scale']
        row_records = []
        for index, (er, ar) in enumerate(zip(case['rows'], native['rows'])):
            errors = {key: ar['progress'][key]-er['progress'][key] for key in ('x','y','width','height')}
            geometry_ok = max(abs(v) for v in errors.values()) <= 1.01
            x0, y0 = round(er['x']*scale), round(er['y']*scale)
            x1, y1 = round((er['x']+er['width'])*scale), round((er['y']+er['height'])*scale)
            ec, ac = e[y0:y1, x0:x1], a[y0:y1, x0:x1]
            valid = np.ones(ec.shape[:2], dtype=bool)
            # Remove only the actual label box and native text-rounding margin.
            label = er['indicator']
            if label:
                lx0=max(0,int(np.floor((label['x']-er['x']-2)*scale)))
                ly0=max(0,int(np.floor((label['y']-er['y']-2)*scale)))
                lx1=min(valid.shape[1],int(np.ceil((label['x']+label['width']-er['x']+2)*scale)))
                ly1=min(valid.shape[0],int(np.ceil((label['y']+label['height']-er['y']+2)*scale)))
                valid[ly0:ly1,lx0:lx1]=False
            background = e[0,0]
            em = np.max(abs(ec-background),axis=2)>3
            am = np.max(abs(ac-background),axis=2)>3
            tolerance = max(1,round(scale))
            # Label exclusion is also expanded so native glyph differences cannot
            # be counted as missing paint in neighboring edge pixels.
            coverage_valid = valid & ~dilate(~valid,tolerance)
            missed = em & ~dilate(am,tolerance) & coverage_valid
            extra = am & ~dilate(em,tolerance) & coverage_valid
            coverage_error = int(missed.sum()+extra.sum())
            # Flat 3x3 neighborhoods identify solid interiors without trusting
            # native colors. Exclude AA, text, gradient seams and gradient regions.
            gradient = isinstance(er['options'].get('strokeColor'),dict)
            # A one-logical-pixel geometry tolerance also applies at 150/200%
            # DPI. Color samples must be farther from a reference edge than that
            # tolerance plus one device pixel of antialiasing.
            radius = 1 if gradient else max(1,int(np.ceil(scale)))
            reference_crop=Image.fromarray(ec.astype('uint8'))
            high=np.asarray(reference_crop.filter(ImageFilter.MaxFilter(radius*2+1))).astype('int16')
            low=np.asarray(reference_crop.filter(ImageFilter.MinFilter(radius*2+1))).astype('int16')
            interior = valid & em & (np.max(high-low,axis=2) <= (10 if gradient else 0))
            interior[:2]=False;interior[-2:]=False;interior[:,:2]=False;interior[:,-2:]=False
            delta=np.max(abs(ec-ac),axis=2)
            values=delta[interior]
            max_error=int(values.max()) if values.size else 0
            mean_error=float(values.mean()) if values.size else 0.
            p99=float(np.quantile(values,.99)) if values.size else 0.
            color_ok=(mean_error<=3. and p99<=8.) if gradient else max_error<=1
            paint_ok=coverage_error==0 and color_ok
            row_records.append({'index':index,'geometry_error':errors,'geometry_pass':geometry_ok,
                'coverage_error_pixels':coverage_error,'interior_pixels':int(values.size),'interior_max_error':max_error,
                'interior_mean_error':mean_error,'interior_p99_error':p99,'paint_pass':paint_ok,
                'raw_row_mean_error':float(abs(ec-ac).mean())})
            if not geometry_ok or not paint_ok: failures.append({'case':name,**row_records[-1]})
        records.append({'name':name,'reference_sha256':hashlib.sha256(ep.read_bytes()).hexdigest(),
            'native_sha256':hashlib.sha256(ap.read_bytes()).hexdigest(),'rows':row_records})
    report={'provenance':browser['provenance'],'renderer':browser['renderer'],
        'native_provenance':json.loads((args.native/'capture-info.json').read_text()) if (args.native/'capture-info.json').exists() else None,
        'comparisons':len(records),'component_rows':sum(len(r['rows']) for r in records),
        'passed':not failures,'failures':failures,'cases':records,
        'gates':{'geometry_logical_px':1,'solid_interior_channel_error':1,'gradient_mean_channel_error':3,'gradient_p99_channel_error':8},
        'limits':'Glyph rasterization and shape edge antialiasing are excluded from color gates; full unaligned row error is recorded.'}
    args.output.parent.mkdir(parents=True,exist_ok=True)
    args.output.write_text(json.dumps(report,indent=2)+'\n', encoding='utf-8', newline='\n')
    print(json.dumps({k:report[k] for k in ('comparisons','component_rows','passed')}))
    for f in failures[:30]: print(f['case'],f['index'],'geometry',f['geometry_error'],'coverage',f['coverage_error_pixels'],'color',f['interior_max_error'],round(f['interior_mean_error'],3),round(f['interior_p99_error'],3))
    if failures: raise SystemExit(1)


if __name__ == '__main__': main()
