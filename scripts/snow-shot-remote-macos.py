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
        image = build / ('snow-shot-' + version + '-macos-arm64.dmg')
        receipt = build / 'remote-release-source.json'
        if request.get('skipBuild'):
            previous = json.loads(receipt.read_text())
            if previous['version'] != version or previous['source'] != identity:
                raise ValueError('Cached DMG source receipt differs; rebuild macOS')
            if digest(image) != previous['sha256']:
                raise ValueError('Cached DMG differs from its source receipt')
        else:
            env = os.environ.copy()
            env['PATH'] = str(Path.home() / '.cargo/bin') + ':/opt/homebrew/bin:/usr/local/bin:' + env['PATH']
            subprocess.run(['bash', str(repo / 'scripts/package-snow-shot.sh'), preset,
                            '--parallel', str(parallelism)], cwd=repo, env=env,
                           stdout=sys.stderr, check=True)
        checksum = image.with_suffix('.dmg.sha256').read_text().split()[0].lower()
        if not re.fullmatch('[a-f0-9]{64}', checksum) or digest(image) != checksum:
            raise ValueError('CPack DMG checksum mismatch')
        subprocess.run(['hdiutil', 'verify', str(image)], stdout=sys.stderr, check=True)
        subprocess.run(['codesign', '--verify', '--verbose=2', str(image)], check=True)
        if source_identity(repo) != identity:
            raise ValueError('macOS source changed during packaging; retry from a stable checkout')
        result = {'version': version, 'source': identity, 'sha256': checksum,
                  'size': image.stat().st_size}
        receipt.write_text(json.dumps(result))
        staged = artifacts / ('remote-release-' + identifier)
        staged.mkdir()
        target = staged / 'snow-shot_macos-arm64.dmg'
        shutil.copyfile(image, target)
        if digest(target) != checksum:
            raise ValueError('Staged DMG checksum mismatch')
        result['path'] = str(target)
        return result
    finally:
        lock.rmdir()


if __name__ == '__main__':
    print(json.dumps(package(json.loads(base64.b64decode(sys.argv[1], validate=True)))))
