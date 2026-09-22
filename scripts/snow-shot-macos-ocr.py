#!/usr/bin/env python3
"""Stage and verify the bundled macOS ARM64 OCR contract (standard library only)."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import urllib.request

ROOT = Path(__file__).resolve().parents[1]
MANIFEST = ROOT / 'snow_shot/packaging/snow-shot-ocr-asset-manifest.json'
RUNTIME_FILES = ('snow-ocr-process', 'libonnxruntime.dylib')
ARM64_HEADER = bytes.fromhex('cffaedfe0c000001')


def run(*args):
    return subprocess.check_output(args, text=True, stderr=subprocess.STDOUT, timeout=60).strip()


def descriptor(path):
    digest = hashlib.sha256()
    with path.open('rb') as source:
        for chunk in iter(lambda: source.read(1024 * 1024), b''):
            digest.update(chunk)
    return dict(name=path.name, size=path.stat().st_size, sha256=digest.hexdigest())


def valid(path, expected):
    return (path.is_file() and not path.is_symlink()
            and path.stat().st_size == expected['size']
            and descriptor(path)['sha256'] == expected['sha256'])


def atomic_json(path, value):
    path.parent.mkdir(parents=True, exist_ok=True)
    data = (json.dumps(value, indent=2, ensure_ascii=False) + '\n').encode()
    if path.exists() and path.read_bytes() == data:
        return
    with tempfile.NamedTemporaryFile(dir=path.parent, delete=False) as output:
        temporary = Path(output.name)
        try:
            output.write(data)
            output.close()
            temporary.chmod(0o644)
            temporary.replace(path)
        finally:
            temporary.unlink(missing_ok=True)


def fetch(expected, destination):
    if valid(destination, expected):
        return
    url = expected['url']
    if not url.startswith('https://'):
        raise ValueError('OCR models require HTTPS')
    destination.parent.mkdir(parents=True, exist_ok=True)
    # Unique partial files let independent build trees share the artifact cache.
    with tempfile.NamedTemporaryFile(dir=destination.parent, delete=False) as output:
        partial = Path(output.name)
        try:
            print(f'Downloading {expected["name"]}', flush=True)
            with urllib.request.urlopen(url, timeout=120) as response:
                if not response.url.startswith('https://'):
                    raise ValueError('OCR model redirect must use HTTPS')
                shutil.copyfileobj(response, output)
            output.close()
            if not valid(partial, expected):
                raise ValueError(f'OCR model integrity check failed: {expected["name"]}')
            partial.replace(destination)
        finally:
            partial.unlink(missing_ok=True)


def copy_changed(source, destination):
    destination.parent.mkdir(parents=True, exist_ok=True)
    if source.resolve() == destination.resolve():
        return
    if not valid(destination, descriptor(source)):
        shutil.copy2(source, destination)


def stage_native_dependencies(library, runtime):
    """Supply @rpath dependencies beside ORT for build-tree execution.

    macdeployqt moves the used closure to Frameworks during installation; these
    development copies are removed before the final bundle is sealed.
    """
    pending = [library.resolve()]
    visited = set()
    names = set()
    while pending:
        current = pending.pop()
        if current in visited:
            continue
        visited.add(current)
        identities = run('/usr/bin/otool', '-D', str(current)).splitlines()[1:]
        for line in run('/usr/bin/otool', '-L', str(current)).splitlines()[1:]:
            dependency = line.strip().split(' (compatibility')[0]
            if dependency in identities or dependency.startswith(('/usr/lib/', '/System/Library/')):
                continue
            if dependency.startswith('@rpath/'):
                source = library.parent / dependency[len('@rpath/'):]
            elif dependency.startswith('@loader_path/'):
                source = current.parent / dependency[len('@loader_path/'):]
            else:
                source = Path(dependency)
            if not source.is_file() or source.suffix != '.dylib':
                raise ValueError(f'Cannot stage OCR native dependency: {dependency}')
            copy_changed(source, runtime / source.name)
            names.add(source.name)
            pending.append(source.resolve())
    inventory = runtime / 'assets/ocr/development-libraries.json'
    if inventory.exists():
        names.update(json.loads(inventory.read_text()))
    atomic_json(inventory, sorted(names))


def remove_development_libraries(runtime):
    marker = runtime / 'assets/ocr/development-libraries.json'
    if not marker.exists():
        return
    for name in json.loads(marker.read_text()):
        if Path(name).name != name or not name.endswith('.dylib') or name in RUNTIME_FILES:
            raise ValueError('Invalid development library inventory')
        (runtime / name).unlink(missing_ok=True)
    marker.unlink()


def deploy_native_libraries(app, library_directory):
    """Resolve the pinned native closure before macdeployqt handles Qt itself.

    CMake removes build rpaths on install. macdeployqt's -libpath search does
    not resolve every third-party @rpath load, so make those loads explicit and
    relocatable before invoking it.
    """
    runtime = app / 'Contents/MacOS'
    frameworks = app / 'Contents/Frameworks'
    frameworks.mkdir(parents=True, exist_ok=True)
    pending = [p for directory in (runtime, frameworks) for p in directory.iterdir()
               if p.is_file() and not p.is_symlink()
               and p.read_bytes()[:4] == bytes.fromhex('cffaedfe')]
    visited = set()
    copied = set()
    while pending:
        current = pending.pop()
        if current in visited:
            continue
        visited.add(current)
        identities = run('/usr/bin/otool', '-D', str(current)).splitlines()[1:]
        for line in run('/usr/bin/otool', '-L', str(current)).splitlines()[1:]:
            dependency = line.strip().split(' (compatibility')[0]
            if dependency in identities or dependency.startswith(('/usr/lib/', '/System/Library/')):
                continue
            # Qt deployment owns its framework/plugin closure.
            if '.framework/' in dependency:
                continue
            name = Path(dependency).name
            source = library_directory / name
            destination = frameworks / name
            if source.is_file():
                if name not in copied:
                    copy_changed(source, destination)
                    run('/usr/bin/install_name_tool', '-id', '@rpath/' + name, str(destination))
                    copied.add(name)
                    pending.append(destination)
            elif not destination.is_file():
                raise ValueError(f'Cannot deploy native dependency of {current}: {dependency}')
            relative = '@loader_path/' + os.path.relpath(destination, current.parent)
            if dependency != relative:
                run('/usr/bin/install_name_tool', '-change', dependency, relative, str(current))
        loads = run('/usr/bin/otool', '-l', str(current)).splitlines()
        for i, line in enumerate(loads):
            if line.strip() == 'cmd LC_RPATH':
                value = loads[i + 2].strip().split(' (offset')[0].removeprefix('path ')
                if not value.startswith(('@loader_path', '@executable_path', '/usr/lib/', '/System/Library/')):
                    run('/usr/bin/install_name_tool', '-delete_rpath', value, str(current))
    remove_development_libraries(runtime)


def prepare_bundle(app, library_directory=None):
    """Seal data as resources while preserving executable-relative lookup paths.

    codesign interprets regular files inside MacOS as nested code. Directory
    symlinks preserve the existing assets/ocr and audios contracts without
    signing model files or JSON as executable code.
    """
    for name in ('assets', 'audios'):
        source = app / 'Contents/MacOS' / name
        destination = app / 'Contents/Resources' / name
        if source.is_symlink():
            if source.readlink() != Path('../Resources') / name or not destination.is_dir():
                raise ValueError(f'Invalid bundled resource link: {source}')
            continue
        if not source.exists():
            continue
        if not source.is_dir() or destination.is_symlink():
            raise ValueError(f'Invalid bundled resource directory: {source}')
        destination.parent.mkdir(parents=True, exist_ok=True)
        if destination.exists():
            shutil.copytree(source, destination, dirs_exist_ok=True)
            shutil.rmtree(source)
        else:
            source.rename(destination)
        source.symlink_to(Path('../Resources') / name, target_is_directory=True)
    if library_directory is not None:
        deploy_native_libraries(app, library_directory)


def stage_models(manifest, cache, destination, all_models=False):
    for model in manifest['models']:
        if not all_models and model['type'] != 'small':
            continue
        for item in model['files']:
            cached = cache / ('ocr-models-' + model['id']) / item['name']
            fetch(item, cached)
            bundled = destination / model['id'] / item['name']
            copy_changed(cached, bundled)
            bundled.chmod(0o644)
        atomic_json(destination / model['id'] / '.complete.json',
                    dict(schema=1, component=model['id']))


def runtime_manifest(source, runtime, static_runtime=False):
    version = source['runtime']['version']
    runtime_files = RUNTIME_FILES[:1] if static_runtime else RUNTIME_FILES
    for name in runtime_files:
        path = runtime / name
        with path.open('rb') as binary:
            header = binary.read(32)
        if len(header) != 32 or header[:8] != ARM64_HEADER:
            raise ValueError(f'Expected a thin ARM64 Mach-O binary: {path}')
    if not os.access(runtime / RUNTIME_FILES[0], os.X_OK):
        raise ValueError('The OCR worker is not executable')
    expected = f'snow-ocr-process {version} macos-aarch64 protocol 3'
    if run(str(runtime / RUNTIME_FILES[0]), '--version') != expected:
        raise ValueError('The OCR worker version/protocol does not match the application')
    return dict(schema=3, default_model='small', runtime=dict(
        version=version, platform='macos-arm64', delivery='bundled', protocol=3,
        executable=RUNTIME_FILES[0], static=static_runtime,
        files=[descriptor(runtime / n) for n in runtime_files]),
        models=source['models'])


def finalize(source, runtime, static_runtime=False):
    manifest = runtime_manifest(source, runtime, static_runtime)
    atomic_json(runtime / 'assets/ocr/asset-manifest.json', manifest)


def verify_assets(source, runtime, static_runtime=False):
    actual = json.loads((runtime / 'assets/ocr/asset-manifest.json').read_text())
    if actual != runtime_manifest(source, runtime, static_runtime):
        raise ValueError('Bundled OCR manifest does not match finalized runtime bytes')
    small = next(m for m in source['models'] if m['type'] == 'small')
    directory = runtime / 'assets/ocr/models' / small['id']
    for item in small['files']:
        if not valid(directory / item['name'], item):
            raise ValueError(f'Missing or corrupt bundled model: {item["name"]}')
    if json.loads((directory / '.complete.json').read_text()) != dict(schema=1, component=small['id']):
        raise ValueError('Invalid bundled model completion marker')
    return actual


def verify_bundle(app):
    """Reject unresolved Mach-O loads and build-machine paths, including LC_RPATH."""
    executable_root = app / 'Contents/MacOS'
    checked = []
    for path in sorted(app.rglob('*')):
        if not path.is_file() or path.is_symlink():
            continue
        with path.open('rb') as source:
            magic = source.read(4)
        if magic not in (bytes.fromhex('cffaedfe'), bytes.fromhex('cafebabe'),
                         bytes.fromhex('cafebabf')):
            continue
        if 'arm64' not in run('/usr/bin/lipo', '-archs', str(path)).split():
            raise ValueError(f'Non-ARM64 bundle dependency: {path}')
        loads = run('/usr/bin/otool', '-arch', 'arm64', '-l', str(path)).splitlines()
        rpaths = []
        for i, line in enumerate(loads):
            if line.strip() == 'cmd LC_RPATH':
                rpaths.append(loads[i + 2].strip().split(' (offset')[0].removeprefix('path '))
            if line.strip().startswith('minos '):
                minimum = tuple(int(part) for part in line.split()[1].split('.'))
                if minimum > (15, 0, 0)[:len(minimum)]:
                    raise ValueError(f'Bundle dependency requires newer than macOS 15: {path}')
        def expand(value):
            return Path(value.replace('@loader_path', str(path.parent))
                        .replace('@executable_path', str(executable_root)))
        for value in rpaths:
            if not value.startswith(('@loader_path', '@executable_path', '/usr/lib', '/System/Library')):
                raise ValueError(f'Non-relocatable rpath in {path}: {value}')
            if value.startswith('@') and not expand(value).resolve().is_relative_to(app.resolve()):
                raise ValueError(f'Bundle rpath escapes the app in {path}: {value}')
        dependencies = run('/usr/bin/otool', '-arch', 'arm64', '-L', str(path)).splitlines()[1:]
        ids = run('/usr/bin/otool', '-arch', 'arm64', '-D', str(path)).splitlines()[1:]
        for line in dependencies:
            dependency = line.strip().split(' (compatibility')[0]
            if dependency in ids or dependency.startswith(('/usr/lib/', '/System/Library/')):
                continue
            if dependency.startswith('@rpath/'):
                suffix = dependency[len('@rpath/'):]
                candidates = [expand(p) / suffix for p in rpaths]
                candidates.append(app / 'Contents/Frameworks' / suffix)
            elif dependency.startswith(('@loader_path/', '@executable_path/')):
                candidates = [expand(dependency)]
            else:
                raise ValueError(f'Non-relocatable dependency in {path}: {dependency}')
            if not any(p.is_file() and p.resolve().is_relative_to(app.resolve()) for p in candidates):
                raise ValueError(f'Unresolved dependency in {path}: {dependency}')
        checked.append(str(path.relative_to(app)))
    run('/usr/bin/codesign', '--verify', '--deep', '--strict', str(app))
    return checked


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('command', choices=('stage', 'finalize', 'verify', 'fetch-models', 'prepare-bundle'))
    parser.add_argument('--manifest', type=Path, default=MANIFEST)
    parser.add_argument('--runtime-dir', type=Path)
    parser.add_argument('--cache', type=Path, default=ROOT / 'artifacts')
    parser.add_argument('--worker', type=Path)
    parser.add_argument('--library', type=Path)
    parser.add_argument('--library-dir', type=Path)
    parser.add_argument('--model-root', type=Path)
    parser.add_argument('--app', type=Path)
    parser.add_argument('--report', type=Path)
    parser.add_argument('--deployed', action='store_true',
                        help='Finalize after macdeployqt has moved dependencies into Frameworks')
    parser.add_argument('--static-runtime', action='store_true',
                        help='ONNX Runtime is linked into the worker instead of deployed as a dylib')
    args = parser.parse_args()
    if args.command == 'prepare-bundle':
        if args.app is None:
            parser.error('--app is required')
        prepare_bundle(args.app.resolve(), args.library_dir)
        return
    source = json.loads(args.manifest.read_text())
    if args.command == 'fetch-models':
        if args.model_root is None:
            parser.error('--model-root is required')
        stage_models(source, args.cache, args.model_root, all_models=True)
        return
    if args.runtime_dir is None:
        parser.error('--runtime-dir is required')
    runtime = args.runtime_dir.resolve()
    if args.command == 'stage':
        if args.worker is None or (args.library is None and not args.static_runtime):
            parser.error('--worker is required; --library is also required for a dynamic runtime')
        copy_changed(args.worker, runtime / RUNTIME_FILES[0])
        if not args.static_runtime:
            copy_changed(args.library, runtime / RUNTIME_FILES[1])
            stage_native_dependencies(args.library, runtime)
        else:
            (runtime / RUNTIME_FILES[1]).unlink(missing_ok=True)
        stage_models(source, args.cache, runtime / 'assets/ocr/models')
        finalize(source, runtime, args.static_runtime)
    elif args.command == 'finalize':
        if args.deployed:
            remove_development_libraries(runtime)
        finalize(source, runtime, args.static_runtime)
    else:
        manifest = verify_assets(source, runtime, args.static_runtime)
        binaries = verify_bundle(args.app.resolve()) if args.app else []
        if args.report:
            atomic_json(args.report, dict(platform='macos-arm64', runtime=manifest['runtime'],
                        bundled_model=manifest['default_model'], verified_binaries=binaries,
                        signature='ad-hoc' if args.app else None))
        print('Verified macOS ARM64 OCR assets' + (' and app bundle' if args.app else ''))


if __name__ == '__main__':
    try:
        main()
    except (OSError, ValueError, subprocess.SubprocessError) as error:
        sys.exit(f'macOS OCR packaging failed: {error}')
