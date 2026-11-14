# Copyright (C) 2026 mg-chao
# SPDX-License-Identifier: Apache-2.0
"""Verify original-source capture completeness, hashes, dimensions and raster bounds."""
import argparse
import hashlib
import json
import math
from pathlib import Path

import numpy as np
from PIL import Image, ImageColor


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--browser', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    root = Path(__file__).resolve().parent.parent
    matrix = json.loads((args.browser/'browser-metrics.json').read_text(encoding='utf-8'))
    manifest = json.loads((root/'fixtures/form/visual-cases.json').read_text(encoding='utf-8'))
    expected = {f'{scene}-{theme}-{direction}-{scale}' for scene in manifest
                for theme in ('default','dark','compact','dark-compact')
                for direction in ('ltr','rtl') for scale in (100,150,200)}
    actual = {case['name'] for case in matrix['cases']}
    assert expected == actual and len(matrix['cases']) == len(expected), 'Incomplete/duplicate original matrix'
    provenance = matrix['provenance']
    assert provenance['version'] == '6.6.7' and provenance['rc_form'] == '1.8.6'
    assert provenance['cases_sha256'] == hashlib.sha256((root/'fixtures/form/visual-cases.json').read_bytes()).hexdigest()
    assert provenance['capture_generator_sha256'] == hashlib.sha256((root/'scripts/capture_form_reference.mjs').read_bytes()).hexdigest(), 'Capture tool changed'
    for name, digest in provenance['inputs'].items():
        assert hashlib.sha256((root/name).read_bytes()).hexdigest() == digest, f'Source changed: {name}'
    records = []
    for case in matrix['cases']:
        filename = args.browser/f'{case["name"]}.png'
        assert hashlib.sha256(filename.read_bytes()).hexdigest() == case['png_sha256'], f'Changed PNG {filename}'
        image = Image.open(filename).convert('RGB')
        scale, viewport = case['scale'], case['viewport']
        assert case['browser_launch_scale'] == case['browser_launch_device_pixel_ratio'] == scale, 'Emulated-only browser DPI is not an actual native DPI baseline'
        assert image.size == (math.floor(viewport['width']*scale+.5),math.floor(viewport['height']*scale+.5)), f'Wrong dimensions {filename}'
        pixels = np.asarray(image).astype('int16')
        background = np.asarray(ImageColor.getrgb(case['background']))
        assert np.array_equal(pixels[0,0],background), f'Wrong page background {filename}'
        allowed = np.zeros(pixels.shape[:2],dtype=bool)
        # At 575 logical px / 150% the screenshot rounds 862.5 to 863 pixels.
        # Chromium composites its fractional page-edge pixels over the white
        # body with tile-dependent coverage. Preserve the actual sampled bands;
        # only this single physical viewport-edge column is exempted.
        fractional_width = viewport['width']*scale % 1
        edge_bands = []
        if fractional_width:
            edge = pixels[:,-1]
            assert np.all(edge>=background) and np.all(edge<=255), f'Unexpected fractional page edge {filename}'
            assert np.max(np.ptp(edge,axis=1)) <= 1, f'Unexpected colored page-edge content {filename}'
            colors,counts = np.unique(edge,axis=0,return_counts=True)
            edge_bands = [{'rgb':color.tolist(),'pixels':int(count)} for color,count in zip(colors,counts)]
            allowed[:,-1] = True
        forms = manifest[case['scene']]['forms']
        assert [f['id'] for f in case['forms']] == [f['id'] for f in forms]
        for measured, config in zip(case['forms'],forms):
            identities = [i['id'] for i in config['items']]
            if config.get('list'):
                keys = list(range(len(config['list']['initialValue'])))
                if config['list'].get('move'):
                    source, target = config['list']['move'];keys.insert(target,keys.pop(source))
                identities += [f'row-{key}' for key in keys]
            assert [i['id'] for i in measured['items']] == identities, f'Wrong item identity {filename}'
            for item in measured['items']:
                assert item['root'] and item['control'] and item['input'], f'Missing visible field {filename}'
            rect=measured['wrapper'];margin=8
            x=max(0,math.floor((rect['x']-margin)*scale));y=max(0,math.floor((rect['y']-margin)*scale))
            right=min(image.width,math.ceil((rect['x']+rect['width']+margin)*scale))
            bottom=min(image.height,math.ceil((rect['y']+rect['height']+margin)*scale))
            allowed[y:bottom,x:right]=True
        foreground=np.max(abs(pixels-background),axis=2)>3
        outside=int((foreground&~allowed).sum())
        assert outside==0, f'Unexpected raster outside measured forms in {filename}: {outside} pixels'
        assert int(foreground.sum())>0, f'Blank source capture {filename}'
        records.append({'name':case['name'],'png_sha256':case['png_sha256'],
                        'foreground_pixels':int(foreground.sum()),'outside_foreground_pixels':outside,
                        'fractional_viewport_edge':bool(fractional_width),'page_edge_samples':edge_bands})
    report={'schema_version':1,'passed':True,'cases':len(records),'scenes':len(manifest),
            'source_files_checked':len(provenance['inputs']),'renderer':matrix['renderer'],
            'native_browser_dpi_verified':True,
            'metrics_sha256':hashlib.sha256((args.browser/'browser-metrics.json').read_bytes()).hexdigest(),
            'records':records}
    args.output.parent.mkdir(parents=True,exist_ok=True)
    args.output.write_text(json.dumps(report,indent=2)+'\n',encoding='utf-8',newline='\n')
    print(json.dumps({key:report[key] for key in ('passed','cases','scenes','source_files_checked')}))


if __name__=='__main__':
    main()
