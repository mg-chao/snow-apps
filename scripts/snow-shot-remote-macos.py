"""SSH worker: package the configured checkout without changing its source files."""
import base64
import hashlib
import json
import os
from pathlib import Path
import re
import shutil
import subprocess
import sys


def digest(path):
    value = hashlib.sha256()
    with path.open('rb') as stream:
        for block in iter(lambda: stream.read(1024 * 1024), b''):
            value.update(block)
    return value.hexdigest()


def symbols_inventory(build, products):
    names = ['snow_shot', 'snow-ocr-process']
    if 'snow-shot-mini' in products:
        names.append('snow_shot_mini')
    symbols = []
    for name in names:
        path = build / 'symbols' / (name + '.dSYM') / 'Contents/Resources/DWARF' / name
        if name == 'snow-ocr-process':
            # Cargo's packed dSYM retains its hashed executable filename when
            # copied out of deps/. Record that actual file without renaming it.
            files = list(path.parent.glob('*'))
            if len(files) != 1:
                raise ValueError(f'Release diagnostics symbols are missing or ambiguous: {name}')
            path = files[0]
        if not path.is_file() or not path.stat().st_size:
            raise ValueError(f'Release diagnostics symbols are missing: {name}')
        symbols.append({'file': str(path.relative_to(build)), 'sha256': digest(path),
                        'size': path.stat().st_size})
    return symbols


def source_identity(repo):
    def git(*args):
        return subprocess.check_output(['git', '-C', str(repo), *args])
    value = hashlib.sha256(git('diff', 'HEAD', '--binary'))
    for name in sorted(git('ls-files', '--others', '--exclude-standard', '-z').split(b'\0')):
        if name:
            value.update(name)
            value.update(digest(repo / os.fsdecode(name)).encode())
    return {'commit': git('rev-parse', 'HEAD').decode().strip(),
            'workingTreeSha256': value.hexdigest()}


def package(request):
    repo = Path(request['projectDirectory']).resolve(strict=True)
    version = request['version']
    if not re.fullmatch(r'[0-9]+\.[0-9]+\.[0-9]+(?:-[0-9A-Za-z.-]+)?', version):
        raise ValueError('Invalid release version')
    identifier = request['id']
    if not re.fullmatch('[a-f0-9]{32}', identifier):
        raise ValueError('Invalid transaction ID')
    source = (repo / 'CMakeLists.txt').read_text()
    match = re.search(r'set\(SNOW_SHOT_VERSION "([^"]+)"\)', source)
    if not match or match[1] != version:
        raise ValueError('Windows and macOS source versions must match')
    if sys.platform != 'darwin' or os.uname().machine != 'arm64':
        raise ValueError('The release host must be an Apple Silicon Mac')
    parallelism = int(request['parallelism'])
    if not 1 <= parallelism <= 256:
        raise ValueError('Invalid parallelism')
    artifacts = repo / 'artifacts'
    artifacts.mkdir(exist_ok=True)
    lock = artifacts / '.macos-release.lock'
    lock.mkdir()  # Never compete with another coordinated release build.
    try:
        identity = source_identity(repo)
        preset = 'snow-shot-macos-arm64-release'
        build = repo / 'build' / preset
        editions = request.get('editions', ['Full'])
        if editions not in (['Full'], ['Full', 'Mini']):
            raise ValueError('Invalid release editions')
        products = ['snow-shot'] + (['snow-shot-mini'] if 'Mini' in editions else [])
        images = [build / (product + '-' + version + '-macos-arm64.dmg')
                  for product in products]
        receipt = build / 'remote-release-source.json'
        if request.get('skipBuild'):
            previous = json.loads(receipt.read_text())
            if previous['version'] != version or previous['source'] != identity:
                raise ValueError('Cached DMG source receipt differs; rebuild macOS')
            previous_images = previous.get('images', [previous])
            if len(previous_images) != len(images):
                raise ValueError('Cached DMG source receipt has different editions')
            for image, item in zip(images, previous_images):
                if digest(image) != item['sha256']:
                    raise ValueError('Cached DMG differs from its source receipt')
            if previous.get('symbols') != symbols_inventory(build, products):
                raise ValueError('Cached diagnostics symbols differ from their source receipt')
        else:
            env = os.environ.copy()
            env['PATH'] = str(Path.home() / '.cargo/bin') + ':/opt/homebrew/bin:/usr/local/bin:' + env['PATH']
            subprocess.run(['bash', str(repo / 'scripts/package-snow-shot.sh'), preset,
                            '--parallel', str(parallelism)], cwd=repo, env=env,
                           stdout=sys.stderr, check=True)
        descriptions = []
        for image, product in zip(images, products):
            checksum = image.with_suffix('.dmg.sha256').read_text().split()[0].lower()
            if not re.fullmatch('[a-f0-9]{64}', checksum) or digest(image) != checksum:
                raise ValueError('CPack DMG checksum mismatch')
            subprocess.run(['hdiutil', 'verify', str(image)], stdout=sys.stderr, check=True)
            subprocess.run(['codesign', '--verify', '--verbose=2', str(image)], check=True)
            descriptions.append({'product': product, 'sha256': checksum, 'size': image.stat().st_size})
        if source_identity(repo) != identity:
            raise ValueError('macOS source changed during packaging; retry from a stable checkout')
        result = {'version': version, 'source': identity, **descriptions[0], 'images': descriptions,
                  'symbols': symbols_inventory(build, products)}
        receipt.write_text(json.dumps(result))
        staged = artifacts / ('remote-release-' + identifier)
        staged.mkdir()
        for image, item in zip(images, descriptions):
            target = staged / (item['product'] + '_macos-arm64.dmg')
            shutil.copyfile(image, target)
            if digest(target) != item['sha256']:
                raise ValueError('Staged DMG checksum mismatch')
            item['path'] = str(target)
        result['path'] = descriptions[0]['path']
        return result
    finally:
        lock.rmdir()


if __name__ == '__main__':
    print(json.dumps(package(json.loads(base64.b64decode(sys.argv[1], validate=True)))))
