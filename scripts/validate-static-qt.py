#!/usr/bin/env python3
"""Validate the audited macOS Qt kit without loading or executing its libraries."""
import argparse
import hashlib
import json
from pathlib import Path
import re
import sys


def feature_fingerprint():
    directory = Path(__file__).parent
    policy_bytes = (directory / 'static-qt-features.json').read_bytes()
    policy = json.loads(policy_bytes)
    hashes = [hashlib.sha256(policy_bytes).hexdigest()]
    hashes.extend(hashlib.sha256((directory / name).read_bytes()).hexdigest()
                  for name in policy['windowsSourcePatches'])
    return hashlib.sha256('|'.join(hashes).encode('utf-8')).hexdigest()


def validate_features(prefix, policy):
    for feature, enabled in policy['features'].items():
        module = 'Gui' if feature == 'system_png' else 'Core'
        visibility = 'PUBLIC' if feature == 'timezone' else 'PRIVATE'
        targets = prefix / f'lib/cmake/Qt6{module}/Qt6{module}Targets.cmake'
        text = targets.read_text(encoding='utf-8')
        state = 'ENABLED' if enabled else 'DISABLED'
        opposite_state = 'DISABLED' if enabled else 'ENABLED'
        match = re.search(rf'QT_{state}_{visibility}_FEATURES "([^"]*)"', text)
        opposite = re.search(rf'QT_{opposite_state}_{visibility}_FEATURES "([^"]*)"', text)
        if (not match or feature not in match[1].split(';') or
                (opposite and feature in opposite[1].split(';'))):
            raise ValueError(f'The installed Qt targets do not have {feature}={enabled}.')


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--prefix', type=Path)
    parser.add_argument('--arch', choices=('arm64', 'x64'))
    parser.add_argument('--dependency-fingerprint')
    parser.add_argument('--features-only', action='store_true')
    parser.add_argument('--print-feature-fingerprint', action='store_true')
    args = parser.parse_args()
    if args.print_feature_fingerprint:
        print(feature_fingerprint())
        return
    if not args.prefix:
        parser.error('--prefix is required when validating a kit')
    policy_bytes = Path(__file__).with_name('static-qt-features.json').read_bytes()
    policy = json.loads(policy_bytes)
    validate_features(args.prefix, policy)
    if args.features_only:
        return
    if not args.arch:
        parser.error('--arch is required when validating the build stamp')
    stamp = json.loads((args.prefix / 'share/snow-apps/static-qt-build.json').read_bytes())
    expected = {
        'SchemaVersion': 2, 'QtVersion': '6.11.1', 'Architecture': args.arch,
        'Configuration': 'Release', 'DeploymentTarget': '14.0', 'Dup3': False,
        'FeatureFingerprint': feature_fingerprint(),
        'Ltcg': True, 'SystemPng': True, 'SystemZlib': True,
        'Timezone': True, 'TimezoneLocale': False,
    }
    if args.dependency_fingerprint:
        expected['DependencyFingerprint'] = args.dependency_fingerprint
    for name, value in expected.items():
        if type(stamp.get(name)) is not type(value) or stamp[name] != value:
            raise ValueError(f"Static Qt build stamp '{name}' must be {value!r}.")
    if not (args.prefix / 'share/snow-apps/qt-licenses').is_dir():
        raise ValueError('The static Qt source-license bundle is missing.')


if __name__ == '__main__':
    try:
        main()
    except (OSError, ValueError, KeyError) as error:
        print(f'Invalid static Qt kit: {error}', file=sys.stderr)
        sys.exit(1)
