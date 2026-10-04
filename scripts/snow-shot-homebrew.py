#!/usr/bin/env python3
"""Build and publish stable and beta casks from immutable GitHub release assets."""
import argparse
import gzip
import hashlib
import io
import json
from pathlib import Path
import re
import subprocess
import tarfile
import tempfile

REPOSITORY = 'mg-chao/snow-apps'
STABLE = r'(0|[1-9][0-9]*)\.(0|[1-9][0-9]*)\.(0|[1-9][0-9]*)'
VERSION = STABLE + r'(?:-beta(?:\.(?:0|[1-9][0-9]*))?)?'


PRODUCTS = {
    'full': ('snow-shot', 'Snow Shot'),
    'mini': ('snow-shot-mini', 'Snow Shot Mini'),
}
ARCHITECTURES = {'arm64': 'arm64', 'x64': 'x86_64'}


def products_for_release(release):
    names = [asset['name'] for asset in release.get('assets', [])]
    version = release_version(release)
    architectures_for_release(release, version)
    # Windows Mini assets and its update feed do not imply a macOS Mini build.
    if any(name.startswith('snow-shot-mini-') and '-macos-' in name for name in names):
        architectures_for_release(release, version, 'mini')
        return ['full', 'mini']
    return ['full']


def tag_version(tag):
    match = re.fullmatch('v(' + VERSION + r')(?:_snow-shot)?', tag)
    if not match:
        raise ValueError('Expected a stable or beta Snow Shot tag (v<version>[_snow-shot]).')
    return match[1]


def cask_name(version, edition='full'):
    slug = PRODUCTS[edition][0]
    return slug + '@beta' if '-beta' in version else slug


def version_key(version):
    base, _, beta = version.partition('-beta')
    return (*map(int, base.split('.')), int(beta[1:]) if beta else -1)


def digest(path):
    result = hashlib.sha256()
    with path.open('rb') as stream:
        for chunk in iter(lambda: stream.read(1024 * 1024), b''):
            result.update(chunk)
    return result.hexdigest()


def release_version(release):
    tag = release.get('tag_name', '')
    version = tag_version(tag)
    if release.get('draft') is not False or release.get('prerelease') not in (True, False):
        raise ValueError('Homebrew requires a published release.')
    if release['prerelease'] and cask_name(version) != 'snow-shot@beta':
        raise ValueError('A stable version cannot be marked as a prerelease.')
    return version


def check_current(current, version, generated=None, edition='full'):
    if not current.exists():
        return
    text = current.read_text(encoding='utf-8')
    match = re.search(r'^  version "(' + VERSION + r')"$', text, re.MULTILINE)
    if not match:
        raise ValueError('The existing tap cask has no recognized version.')
    previous = match[1]
    if cask_name(previous, edition) != cask_name(version, edition):
        raise ValueError('Refusing to change the release channel of a cask.')
    if version_key(previous) > version_key(version):
        raise ValueError('Refusing to downgrade the Homebrew tap.')
    if previous == version and generated is not None and text != generated:
        raise ValueError('Refusing to change the contents of an already published cask version.')


def required_assets(release, version, edition='full', architecture='arm64'):
    if architecture not in ARCHITECTURES:
        raise ValueError('Unsupported macOS architecture.')
    if edition == 'mini' and architecture != 'arm64':
        raise ValueError('Snow Shot Mini is available only for macOS ARM64.')
    dmg = f'{PRODUCTS[edition][0]}-{version}-macos-{ARCHITECTURES[architecture]}.dmg'
    names = [asset['name'] for asset in release.get('assets', [])]
    if names.count(dmg) != 1 or names.count(dmg + '.sha256') > 1:
        raise ValueError(f'Missing or duplicate {dmg} or checksum asset.')
    expected = github_digest(release, dmg)
    if dmg + '.sha256' not in names and expected is None:
        raise ValueError(f'Missing checksum for {dmg}. Upload a SHA-256 sidecar or use an asset with a GitHub SHA-256 digest.')
    return dmg


def architectures_for_release(release, version, edition='full'):
    names = [asset['name'] for asset in release.get('assets', [])]
    prefix = f'{PRODUCTS[edition][0]}-{version}-macos-'
    if edition == 'mini' and any(name.startswith(prefix + suffix) for name in names
                                 for suffix in ('x86_64.', 'x86_64-', 'x64.', 'x64-')):
        raise ValueError('Snow Shot Mini is available only for macOS ARM64.')
    architectures = []
    for architecture, suffix in ARCHITECTURES.items():
        name = prefix + suffix + '.dmg'
        if name in names or name + '.sha256' in names:
            required_assets(release, version, edition, architecture)
            architectures.append(architecture)
    if not architectures:
        raise ValueError(f'Missing macOS DMG assets for {PRODUCTS[edition][1]}.')
    return architectures


def github_digest(release, name):
    asset = next(asset for asset in release['assets'] if asset['name'] == name)
    value = asset.get('digest')
    if value is None:
        return None
    if not re.fullmatch(r'sha256:[0-9a-fA-F]{64}', value):
        raise ValueError('Invalid GitHub asset SHA-256 digest.')
    return value.split(':', 1)[1].lower()


def cask(version, sha256, tag=None, edition='full', architecture='arm64'):
    slug, name = PRODUCTS[edition]
    checksums = sha256 if isinstance(sha256, dict) else {architecture: sha256}
    if not checksums or any(arch not in ARCHITECTURES for arch in checksums):
        raise ValueError('Cask requires a supported macOS architecture.')
    if edition == 'mini' and set(checksums) != {'arm64'}:
        raise ValueError('Snow Shot Mini is available only for macOS ARM64.')
    if len(checksums) == 2:
        architecture_stanza = '  arch arm: "arm64", intel: "x86_64"\n\n'
        checksum_stanza = f'  sha256 arm:   "{checksums["arm64"]}",\n         intel: "{checksums["x64"]}"'
        archive_architecture = '#{arch}'
        architecture_requirement = ''
    else:
        architecture = next(iter(checksums))
        architecture_stanza = ''
        checksum_stanza = f'  sha256 "{checksums[architecture]}"'
        archive_architecture = ARCHITECTURES[architecture]
        architecture_requirement = f'  depends_on arch: :{archive_architecture}\n'
    edition_argument = ',\n                                  "mini"' if edition == 'mini' else ''
    tag = tag or f'v{version}_snow-shot'
    if tag_version(tag) != version:
        raise ValueError('Cask version does not match its release tag.')
    url_tag = tag.replace(version, '#{version}', 1)
    conflict = f'  conflicts_with cask: "{slug}"\n' if '-beta' in version else ''
    return f'''cask "{cask_name(version, edition)}" do
{architecture_stanza}  version "{version}"
{checksum_stanza}

  url "https://github.com/{REPOSITORY}/releases/download/{url_tag}/{slug}-#{{version}}-macos-{archive_architecture}-homebrew.tar.gz"
  name "{name}"
  desc "Screenshot and screen recording application"
  homepage "https://snowshot.top/"

{conflict}{architecture_requirement}  depends_on macos: :sequoia

  app "{name}.app"

  # Third-party Ruby hooks preserve the desktop user's HOME and Keychain access.
  preflight do
    system_command "/bin/bash",
                   args:         [staged_path.join("prepare-snow-shot-homebrew.sh"),
                                  staged_path.join("{slug}-#{{version}}-macos-{archive_architecture}.dmg"),
                                  staged_path.join("{name}.app"){edition_argument}],
                   must_succeed: true,
                   print_stdout: true,
                   print_stderr: true
  end

  caveats <<~EOS
    {name} reuses a signing identity in your login Keychain. The first install
    may request Keychain access. Grant Screen Recording and Accessibility when
    macOS requests them. Local signing does not provide Apple notarization.
    Keep ~/Library/Application Support/{name}/Installer and its Keychain
    identity across upgrades and reinstalls. Use the same installing account.
  EOS
end
'''


def package_source(source, version, edition):
    cmake = (source / 'CMakeLists.txt').read_text(encoding='utf-8')
    if f'set(SNOW_SHOT_VERSION "{version}")' not in cmake:
        raise ValueError('The checked-out release source version does not match its tag.')
    installer = (source / 'scripts/install-snow-shot-macos.sh').read_bytes()
    if b'--prepare-app)' not in installer or b'\r' in installer:
        raise ValueError('The release installer must support --prepare-app and use LF line endings.')
    if edition == 'mini' and b'--edition)' not in installer:
        raise ValueError('Mini requires the edition-aware installer from its release source.')
    preflight = (source / 'scripts/prepare-snow-shot-homebrew.sh').read_bytes()
    if b'\r' in preflight:
        raise ValueError('The Homebrew preflight must use LF line endings.')
    return installer, preflight


def verified_dmg(release, assets, name):
    dmg = assets / name
    checksum = assets / (name + '.sha256')
    actual = digest(dmg)
    expected = github_digest(release, name)
    if expected is not None and actual != expected:
        raise ValueError('DMG checksum mismatch with GitHub asset digest.')
    if any(asset['name'] == checksum.name for asset in release['assets']):
        lines = [line.strip() for line in checksum.read_text(encoding='utf-8').splitlines() if line.strip()]
        if len(lines) != 1 or not re.fullmatch(r'[0-9a-fA-F]{64}(?:\s+.*)?', lines[0]):
            raise ValueError('Invalid DMG checksum sidecar.')
        if actual != lines[0][:64].lower():
            raise ValueError('DMG checksum mismatch.')
    return dmg


def package_archive(version, edition, architecture, name, dmg, installer, preflight, output):
    output.mkdir(parents=True, exist_ok=True)
    archive = output / f'{PRODUCTS[edition][0]}-{version}-macos-{ARCHITECTURES[architecture]}-homebrew.tar.gz'
    # Fixed metadata and gzip header make retries byte-identical across hosts.
    with archive.open('wb') as raw, gzip.GzipFile(filename='', mode='wb', fileobj=raw, mtime=0) as compressed:
        with tarfile.open(fileobj=compressed, mode='w|', format=tarfile.USTAR_FORMAT) as tar:
            for filename, path, content in (
                (name, dmg, None),
                (name + '.sha256', None, (digest(dmg) + '  ' + name + '\n').encode()),
                ('install-snow-shot-macos.sh', None, installer),
                ('prepare-snow-shot-homebrew.sh', None, preflight),
            ):
                info = tarfile.TarInfo(filename)
                info.mode = 0o644
                info.size = path.stat().st_size if path else len(content)
                with path.open('rb') if path else io.BytesIO(content) as stream:
                    tar.addfile(info, stream)
    return archive


def package_all(release, source, assets, output, current, edition='full', architectures=None):
    version = release_version(release)
    check_current(current, version, edition=edition)
    available = architectures_for_release(release, version, edition)
    architectures = available if architectures is None else architectures
    if not architectures or len(set(architectures)) != len(architectures) or any(
            architecture not in available for architecture in architectures):
        raise ValueError('Requested macOS architecture is missing from the release.')
    installer, preflight = package_source(source, version, edition)
    inputs = []
    for architecture in architectures:
        name = required_assets(release, version, edition, architecture)
        inputs.append((architecture, name, verified_dmg(release, assets, name)))
    # Verify every input before creating archives or changing the generated cask.
    archives = [package_archive(version, edition, architecture, name, dmg,
                                installer, preflight, output)
                for architecture, name, dmg in inputs]
    checksums = {architecture: digest(archive)
                 for (architecture, _, _), archive in zip(inputs, archives)}
    generated = cask(version, checksums, release['tag_name'], edition)
    check_current(current, version, generated, edition)
    casks = output / 'Casks'
    casks.mkdir(exist_ok=True)
    (casks / f'{cask_name(version, edition)}.rb').write_text(generated, encoding='utf-8')
    return archives, generated


def package(release, source, assets, output, current, edition='full', architecture='arm64'):
    archives, generated = package_all(release, source, assets, output, current, edition,
                                      [architecture])
    return archives[0], generated


def run(*args, cwd=None):
    return subprocess.run(args, cwd=cwd, check=True, text=True, stdout=subprocess.PIPE).stdout


def ensure_asset(release, archive, validate_only=False):
    matches = [asset for asset in release['assets'] if asset['name'] == archive.name]
    if len(matches) > 1:
        raise ValueError('Duplicate Homebrew release assets.')
    if matches:
        with tempfile.TemporaryDirectory(prefix='snow-homebrew-existing-') as directory:
            run('gh', 'release', 'download', release['tag_name'], '--repo', REPOSITORY,
                '--pattern', archive.name, '--dir', directory)
            if digest(Path(directory) / archive.name) != digest(archive):
                raise ValueError('Existing Homebrew archive differs; never replace a published asset.')
    elif not validate_only:
        run('gh', 'release', 'upload', release['tag_name'], str(archive), '--repo', REPOSITORY)


def publish(tag, source, tap, output):
    tag_version(tag)
    release = json.loads(run('gh', 'api', f'repos/{REPOSITORY}/releases/tags/{tag}'))
    if release['tag_name'] != tag:
        raise ValueError('Unexpected release tag returned by GitHub.')
    version = release_version(release)
    editions = products_for_release(release)
    prepared = []
    with tempfile.TemporaryDirectory(prefix='snow-homebrew-assets-') as directory:
        patterns = []
        for edition in editions:
            for architecture in architectures_for_release(release, version, edition):
                name = required_assets(release, version, edition, architecture)
                patterns += ['--pattern', name]
                if any(asset['name'] == name + '.sha256' for asset in release['assets']):
                    patterns += ['--pattern', name + '.sha256']
        run('gh', 'release', 'download', tag, '--repo', REPOSITORY,
            *patterns, '--dir', directory)
        for edition in editions:
            current = tap / f'Casks/{cask_name(version, edition)}.rb'
            archives, generated = package_all(release, source, Path(directory), output, current, edition)
            prepared.append((current, archives, generated))
    # Validate both editions before uploading archives or modifying the tap.
    for _, archives, _ in prepared:
        for archive in archives:
            ensure_asset(release, archive, validate_only=True)
    for _, archives, _ in prepared:
        for archive in archives:
            ensure_asset(release, archive)
    for current, _, generated in prepared:
        current.parent.mkdir(parents=True, exist_ok=True)
        current.write_text(generated, encoding='utf-8')
    readme = tap / 'README.md'
    if not readme.exists():
        readme.write_text((Path(__file__).resolve().parent.parent / 'homebrew/README.md').read_text(encoding='utf-8'), encoding='utf-8')
    run('git', 'add', *(str(current.relative_to(tap)).replace('\\', '/') for current, _, _ in prepared), 'README.md', cwd=tap)
    if run('git', 'diff', '--cached', '--name-only', cwd=tap).strip():
        run('git', '-c', 'user.name=github-actions[bot]', '-c',
            'user.email=41898282+github-actions[bot]@users.noreply.github.com',
            'commit', '-m', f'feat(snow-shot): update editions to {version}', cwd=tap)
        run('git', 'push', 'origin', 'HEAD:main', cwd=tap)



def main():
    parser = argparse.ArgumentParser(description=__doc__)
    commands = parser.add_subparsers(dest='command', required=True)
    local = commands.add_parser('package', help='Generate architecture archives and a cask without publishing')
    local.add_argument('--release-json', type=Path, required=True)
    local.add_argument('--assets', type=Path, required=True)
    local.add_argument('--current-cask', type=Path, required=True)
    local.add_argument('--edition', choices=PRODUCTS, default='full')
    remote = commands.add_parser('publish', help='Publish verified assets and update the checked-out tap')
    remote.add_argument('--tag', required=True)
    remote.add_argument('--tap', type=Path, required=True)
    for command in (local, remote):
        command.add_argument('--source', type=Path, required=True)
        command.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    try:
        if args.command == 'package':
            package_all(json.loads(args.release_json.read_text(encoding='utf-8')), args.source,
                        args.assets, args.output, args.current_cask, args.edition)
        else:
            publish(args.tag, args.source, args.tap, args.output)
    except (ValueError, OSError, subprocess.CalledProcessError) as error:
        parser.exit(1, f'Homebrew release failed: {error}\n')


if __name__ == '__main__':
    main()
