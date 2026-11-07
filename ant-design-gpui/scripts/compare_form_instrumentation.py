# Copyright (C) 2026 mg-chao
# SPDX-License-Identifier: Apache-2.0
"""Prove passive logical-label inspection leaves real native pixels unchanged."""
import argparse
import hashlib
import json
from pathlib import Path

from PIL import Image


def digest(file):
    return hashlib.sha256(file.read_bytes()).hexdigest()


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--plain', type=Path, required=True)
    parser.add_argument('--instrumented', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    contexts = [json.loads((folder / 'capture-context.json').read_text(encoding='utf-8-sig'))
                for folder in (args.plain, args.instrumented)]
    plain, instrumented = contexts
    assert plain['source_snapshot_matches_build'] and instrumented['source_snapshot_matches_build']
    assert plain['source_hashes'] == instrumented['source_hashes'], 'Source differs between feature builds'
    assert plain['capture_script_sha256'] == instrumented['capture_script_sha256'], 'Capture harness changed between feature builds'
    assert plain['features'] == [] and instrumented['features'] == ['test-support']
    assert plain['pe_stack_reserve_bytes'] == instrumented['pe_stack_reserve_bytes'] == 1048576
    captures = [json.loads((folder / 'native-metrics.json').read_text(encoding='utf-8'))
                for folder in (args.plain, args.instrumented)]
    selected = [{case['name']: case for case in cases if case['theme'] == 'default'
                 and case['direction'] == 'ltr' and case['scale'] == 1}
                for cases in captures]
    assert selected[0].keys() == selected[1].keys() and len(selected[0]) == 19
    records = []
    for name in selected[0]:
        left, right = [folder / f'{name}.png' for folder in (args.plain, args.instrumented)]
        a, b = [Image.open(file).convert('RGBA') for file in (left, right)]
        assert a.size == b.size and a.tobytes() == b.tobytes(), f'Instrumentation changed native pixels: {name}'
        assert selected[0][name]['label_geometry_source'] == 'pixel-snapped-quad'
        assert selected[1][name]['label_geometry_source'] == 'logical-prepaint'
        assert selected[0][name]['gpu'] == selected[1][name]['gpu'], 'GPU changed between feature captures'
        records.append({'name': name, 'identical_pixels': True,
                        'plain_png_sha256': digest(left), 'instrumented_png_sha256': digest(right)})
    for context, folder in zip(contexts, (args.plain, args.instrumented)):
        assert context['executable_sha256'] == digest(folder / 'form_visual-capture.exe')
    result = {'schema_version': 1, 'passed': True, 'cases': len(records),
              'scope': 'The exact same source, built with default features and with test-support, produces identical native RGBA pixels for all 19 default/LTR/100% Form scenes.',
              'source_files_verified': len(plain['source_hashes']),
              'pe_stack_reserve_bytes': 1048576,
              'plain_executable_sha256': plain['executable_sha256'],
              'instrumented_executable_sha256': instrumented['executable_sha256'],
              'plain_context_sha256': digest(args.plain / 'capture-context.json'),
              'instrumented_context_sha256': digest(args.instrumented / 'capture-context.json'),
              'records': records}
    args.output.write_text(json.dumps(result, indent=2) + '\n', encoding='utf-8', newline='\n')
    print(json.dumps({'passed': True, 'identical_native_captures': len(records)}))


if __name__ == '__main__':
    main()
