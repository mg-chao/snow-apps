#!/usr/bin/env python3
"""Validate the audited macOS Qt kit without loading or executing its libraries."""
import argparse
import hashlib
import json
from pathlib import Path
import re
import struct
import sys


def toolchain_policy():
    policy = json.loads(Path(__file__).with_name('qt-toolchain.json').read_bytes())
    if (not isinstance(policy, dict) or type(policy.get('schemaVersion')) is not int or
            policy['schemaVersion'] != 1 or not isinstance(policy.get('qtVersion'), str) or
            not re.fullmatch(r'6\.\d+\.\d+', policy['qtVersion'])):
        raise ValueError('Invalid Qt toolchain policy.')
    if (not isinstance(policy.get('sourceArchiveSha256'), str) or
            not re.fullmatch(r'[0-9a-f]{64}', policy['sourceArchiveSha256'])):
        raise ValueError('Invalid Qt source archive SHA-256 in the toolchain policy.')
    if (not isinstance(policy.get('macosQtDeploymentTarget'), str) or
            not re.fullmatch(r'[1-9]\d*\.\d+(?:\.\d+)?', policy['macosQtDeploymentTarget'])):
        raise ValueError('Invalid macOS Qt deployment target in the toolchain policy.')
    return policy


def validate_source(source, version, deployment_target):
    metadata = source / 'qtbase/.cmake.conf'
    text = metadata.read_text(encoding='utf-8')
    match = re.search(r'set\(QT_REPO_MODULE_VERSION\s+"([^"]+)"\)', text)
    if not match or match[1] != version:
        raise ValueError(f'Qt sources must be qtbase {version}: {metadata}')
    minimum = re.search(r'set\(QT_SUPPORTED_MIN_MACOS_VERSION\s+"([^"]+)"\)', text)
    if not minimum or not re.fullmatch(r'[1-9]\d*\.\d+(?:\.\d+)?', minimum[1]):
        raise ValueError(f'The Qt sources must declare their supported macOS runtime floor: {metadata}')
    def version_tuple(value):
        parts = tuple(map(int, value.split('.')))
        return parts + (0,) * (3 - len(parts))

    if version_tuple(deployment_target) < version_tuple(minimum[1]):
        raise ValueError(f'Qt sources require macOS {minimum[1]} or newer; '
                         f'the audited Qt deployment target is {deployment_target}.')


def validate_archive(archive, expected_sha256):
    digest = hashlib.sha256()
    with archive.open('rb') as stream:
        for block in iter(lambda: stream.read(1024 * 1024), b''):
            digest.update(block)
    if digest.hexdigest() != expected_sha256:
        raise ValueError(f'Qt source archive SHA-256 mismatch: {archive}')


def installed_version(prefix, version):
    for module in ('Qt6', 'Qt6Core'):
        directory = prefix / 'lib/cmake' / module
        versions = []
        for suffix in ('ConfigVersion.cmake', 'ConfigVersionImpl.cmake'):
            path = directory / (module + suffix)
            if path.is_file():
                versions.extend(re.findall(r'set\(PACKAGE_VERSION\s+"([^"]+)"\)',
                                           path.read_text(encoding='utf-8')))
        if not versions or set(versions) != {version}:
            raise ValueError(f'The installed {module} package must be Qt '
                             f'{version}: {directory}')


def binary_architectures(path):
    """Read Mach-O, Darwin LTO and archive headers without executing Qt tools."""
    cpus = {0x0100000c: 'arm64', 0x01000007: 'x64'}

    def header_architectures(stream, offset, size):
        stream.seek(offset)
        header = stream.read(8)
        if len(header) < 8:
            return set()
        magic = header[:4]
        if magic in (b'\xcf\xfa\xed\xfe', b'\xce\xfa\xed\xfe'):
            return {cpus.get(struct.unpack('<I', header[4:])[0], 'unsupported')}
        if magic in (b'\xfe\xed\xfa\xcf', b'\xfe\xed\xfa\xce'):
            return {cpus.get(struct.unpack('>I', header[4:])[0], 'unsupported')}
        if magic == b'\xde\xc0\x17\x0b':
            # Clang's Darwin LTO objects use LLVM's little-endian bitcode
            # wrapper, whose fifth word records the Mach-O CPU type.
            # https://llvm.org/docs/BitCodeFormat.html#bitcode-wrapper-format
            remainder = stream.read(12)
            if len(remainder) != 12:
                raise ValueError(f'Truncated Darwin LTO header: {path}')
            start, length, cpu = struct.unpack('<3I', remainder)
            if start < 20 or start + length > size:
                raise ValueError(f'Invalid Darwin LTO header: {path}')
            return {cpus.get(cpu, 'unsupported')}
        if magic in (b'\xca\xfe\xba\xbe', b'\xbe\xba\xfe\xca',
                     b'\xca\xfe\xba\xbf', b'\xbf\xba\xfe\xca'):
            endian = '>' if magic[0] == 0xca else '<'
            count = struct.unpack(endian + 'I', header[4:])[0]
            entry_size = 32 if magic in (b'\xca\xfe\xba\xbf', b'\xbf\xba\xfe\xca') else 20
            if not count or count > 32 or 8 + count * entry_size > size:
                raise ValueError(f'Invalid universal Mach-O header: {path}')
            entries = stream.read(count * entry_size)
            return {cpus.get(struct.unpack_from(endian + 'I', entries, index * entry_size)[0],
                             'unsupported') for index in range(count)}
        if header == b'!<arch>\n':
            architectures = set()
            position = offset + 8
            while position + 60 <= offset + size:
                stream.seek(position)
                member = stream.read(60)
                if member[58:] != b'`\n':
                    raise ValueError(f'Invalid static library archive: {path}')
                length = int(member[48:58].strip())
                data_offset = position + 60
                if data_offset + length > offset + size:
                    raise ValueError(f'Truncated static library archive: {path}')
                # BSD archives store an extended filename before the object.
                name_length = int(member[3:16].strip()) if member.startswith(b'#1/') else 0
                if name_length > length:
                    raise ValueError(f'Invalid static library member name: {path}')
                architectures.update(header_architectures(stream, data_offset + name_length,
                                                         length - name_length))
                position = data_offset + length + length % 2
            return architectures
        return set()

    with path.open('rb') as stream:
        result = header_architectures(stream, 0, path.stat().st_size)
    if not result or 'unsupported' in result:
        raise ValueError(f'Qt Core must contain supported macOS Mach-O binaries: {path}')
    return result


def validate_kit(prefix, arch=None, require_static=False):
    if not (prefix / 'lib/cmake/Qt6/Qt6Config.cmake').is_file():
        raise ValueError(f'The Qt CMake package is missing: {prefix}')
    installed_version(prefix, toolchain_policy()['qtVersion'])
    for language in ('zh_CN', 'zh_TW'):
        catalog = prefix / 'translations' / f'qtbase_{language}.qm'
        if not catalog.is_file():
            raise ValueError(f'The Qt stock-dialog translation catalog is missing: {catalog}')
    directory = prefix / 'lib/cmake/Qt6Core'
    targets = (directory / 'Qt6CoreTargets.cmake').read_text(encoding='utf-8')
    linkage = re.search(r'add_library\(Qt6::Core\s+(STATIC|SHARED)\s+IMPORTED\)', targets)
    if not linkage or (require_static and linkage[1] != 'STATIC'):
        raise ValueError('The installed Qt Core targets must describe the required library linkage.')
    if not arch:
        return
    # A static release kit must expose the audited Release artifact. Qt's shared
    # macOS kit also supplies a Release binary to Debug consumers.
    configuration = directory / 'Qt6CoreTargets-release.cmake'
    release = configuration.read_text(encoding='utf-8')
    location = re.search(r'IMPORTED_LOCATION_RELEASE\s+"([^"]+)"', release)
    if not location:
        raise ValueError(f'The Qt Core Release artifact is missing from {configuration}')
    library = Path(location[1].replace('${_IMPORT_PREFIX}', str(prefix)))
    if '$' in str(library) or not library.is_absolute():
        raise ValueError(f'The Qt Core Release artifact has an unsupported path: {location[1]}')
    if arch not in binary_architectures(library):
        raise ValueError(f'The installed Qt Core Release binary does not support {arch}: {library}')


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
    parser.add_argument('--kit-only', action='store_true')
    parser.add_argument('--source-dir', type=Path)
    parser.add_argument('--source-archive', type=Path)
    parser.add_argument('--print-version', action='store_true')
    parser.add_argument('--print-source-sha256', action='store_true')
    parser.add_argument('--print-macos-deployment-target', action='store_true')
    parser.add_argument('--print-feature-fingerprint', action='store_true')
    args = parser.parse_args()
    toolchain = toolchain_policy()
    if args.print_version:
        print(toolchain['qtVersion'])
        return
    if args.print_source_sha256:
        print(toolchain['sourceArchiveSha256'])
        return
    if args.print_macos_deployment_target:
        print(toolchain['macosQtDeploymentTarget'])
        return
    if args.print_feature_fingerprint:
        print(feature_fingerprint())
        return
    if args.source_dir:
        validate_source(args.source_dir, toolchain['qtVersion'], toolchain['macosQtDeploymentTarget'])
        return
    if args.source_archive:
        validate_archive(args.source_archive, toolchain['sourceArchiveSha256'])
        return
    if not args.prefix:
        parser.error('--prefix is required when validating a kit')
    policy_bytes = Path(__file__).with_name('static-qt-features.json').read_bytes()
    policy = json.loads(policy_bytes)
    validate_kit(args.prefix, args.arch, require_static=not args.kit_only)
    if args.kit_only:
        return
    validate_features(args.prefix, policy)
    if args.features_only:
        return
    if not args.arch:
        parser.error('--arch is required when validating the build stamp')
    stamp = json.loads((args.prefix / 'share/snow-apps/static-qt-build.json').read_bytes())
    expected = {
        'SchemaVersion': 3, 'QtVersion': toolchain['qtVersion'], 'Architecture': args.arch,
        'SourceArchiveSha256': toolchain['sourceArchiveSha256'],
        'Configuration': 'Release', 'DeploymentTarget': toolchain['macosQtDeploymentTarget'],
        'Dup3': False,
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
