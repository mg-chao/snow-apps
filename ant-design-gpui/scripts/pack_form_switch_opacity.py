# Copyright (C) 2026 mg-chao
# SPDX-License-Identifier: Apache-2.0
"""Keep independently painted source opacity samples, without synthesizing colors."""
import hashlib
import json
from pathlib import Path

from PIL import Image

ROOT = Path(__file__).resolve().parent.parent
BROWSER = ROOT / '../build/ant-design-gpui/form-opacity-browser'
source = json.loads((BROWSER / 'browser-metrics.json').read_text(encoding='utf-8'))
manifest = json.loads((ROOT / 'fixtures/form/switch-opacity-cases.json').read_text(encoding='utf-8'))
assert len(source['cases']) == 12
for file, expected in source['provenance']['inputs'].items():
    assert hashlib.sha256((ROOT / file).read_bytes()).hexdigest() == expected, file
records = []
for case in source['cases']:
    assert case['scale'] == 1 and case['direction'] == 'ltr'
    file = BROWSER / f'{case["name"]}.png'
    assert hashlib.sha256(file.read_bytes()).hexdigest() == case['png_sha256']
    image = Image.open(file).convert('RGB')
    sample = manifest[case['scene']]
    for item, measured in zip(sample['forms'][0]['items'], case['forms'][0]['items']):
        if item.get('loading'):
            continue  # The independently gated loading SVG covers the handle center.
        assert item['id'] == measured['id']
        control = measured['input']
        handle = 12 if item.get('size') == 'small' else 18
        center_x = control['x'] + (control['width'] - 2 - handle / 2 if item['value'] else 2 + handle / 2)
        center_y = control['y'] + control['height'] / 2
        point = (int(center_x), int(center_y))
        records.append({'scene': case['scene'], 'theme': case['theme'], 'id': item['id'],
                        'backdrop': case['background'], 'checked': item['value'],
                        'small': item.get('size') == 'small', 'disabled': item.get('disabled', False),
                        'authored_opacity': item.get('opacity'), 'effective_opacity': float(control['opacity']) if 'opacity' in control else item.get('opacity', .65),
                        'custom_content': item.get('customContent', False), 'handle_center': list(point),
                        'handle_center_rgb': list(image.getpixel(point)), 'png_sha256': case['png_sha256']})
result = {'schema_version': 1, 'scope': 'Actual original Switch pixels over six uniform backdrops, with both native sizes and checked states; no generated expected colors.',
          'provenance': source['provenance'], 'renderer': source['renderer'],
          'captures': len(source['cases']), 'cases': records}
(ROOT / 'fixtures/form/switch-opacity-reference.json').write_text(json.dumps(result, indent=2) + '\n', encoding='utf-8', newline='\n')
print(json.dumps({'captures': len(source['cases']), 'actual_pixel_probes': len(records)}))
