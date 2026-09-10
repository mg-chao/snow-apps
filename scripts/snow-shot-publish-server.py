"""Fixed-file Snow Shot publisher. Invoked over verified SSH; no shell interpolation.

All staging and backup data lives outside the web root. The only public mutations
are the explicit ALLOWED paths below. Also usable against a temporary fixture root.
"""
import base64
import contextlib
import hashlib
import json
import os
from pathlib import Path
import re
import shutil
import sys
import time

ALLOWED = (
    'setup/snow-shot_windows-x64-offline.exe',
    'setup/snow-shot_windows-x64-online.exe',
    'setup/snow-shot_windows-x64-portable.zip',
    'setup/snow-shot_windows-x64-offline-update.zip',
    'setup/snow-shot_windows-x64-online-update.zip',
    'setup/SHA256SUMS', 'latest-version.json', 'latest-version.txt',
)
VERSION = re.compile(r'(0|[1-9]\d*)\.(0|[1-9]\d*)\.(0|[1-9]\d*)'
                     r'(?:-((?:0|[1-9]\d*|\d*[A-Za-z-][0-9A-Za-z-]*)(?:\.(?:0|[1-9]\d*|\d*[A-Za-z-][0-9A-Za-z-]*))*))?'
                     r'(?:\+[0-9A-Za-z-]+(?:\.[0-9A-Za-z-]+)*)?', re.ASCII)


def version_key(value):
    match = VERSION.fullmatch(value)
    if not match or len(value) > 128:
        raise ValueError('Invalid semantic version')
    pre = match[4]
    identifiers = tuple((0, int(v)) if v.isdigit() else (1, v) for v in pre.split('.')) if pre else ()
    return (int(match[1]), int(match[2]), int(match[3]), not bool(pre), identifiers)


def digest(path):
    with path.open('rb') as stream:
        return hashlib.file_digest(stream, 'sha256').hexdigest()


def atomic_json(path, value):
    temporary = path.with_name(path.name + '.tmp')
    with temporary.open('w', encoding='utf-8', newline='\n') as stream:
        json.dump(value, stream, separators=(',', ':'))
        stream.flush()
        os.fsync(stream.fileno())
    os.replace(temporary, path)
    sync_directory(path.parent)


def sync_directory(path):
    if os.name != 'nt':
        descriptor = os.open(path, os.O_RDONLY | os.O_DIRECTORY)
        try:
            os.fsync(descriptor)
        finally:
            os.close(descriptor)


def no_links(path):
    for part in (path, *path.parents):
        if part.is_symlink():
            raise ValueError('Publish paths must not contain symbolic links')


class Publisher:
    def __init__(self, web):
        self.web = Path(web).absolute()
        no_links(self.web)
        if not self.web.is_dir() or not (self.web / 'setup').is_dir() or len(self.web.parts) < 3:
            raise ValueError('Invalid website root')
        self.private = self.web.parent / '.snow-shot-releases'
        no_links(self.private)
        for name in ALLOWED:
            no_links(self.web / name)

    def release(self, identifier):
        if not re.fullmatch('[a-f0-9]{32}', identifier):
            raise ValueError('Invalid release transaction ID')
        path = self.private / identifier
        no_links(path)
        return path

    @contextlib.contextmanager
    def lock(self):
        self.private.mkdir(mode=0o700, exist_ok=True)
        with (self.private / 'publish.lock').open('a+b') as stream:
            if os.name == 'nt':
                import msvcrt
                stream.seek(0)
                if not stream.read(1):
                    stream.write(b'0')
                    stream.flush()
                stream.seek(0)
                msvcrt.locking(stream.fileno(), msvcrt.LK_NBLCK, 1)
            else:
                import fcntl
                fcntl.flock(stream, fcntl.LOCK_EX | fcntl.LOCK_NB)
            yield

    def read_state(self, name):
        path = self.private / name
        return json.loads(path.read_text(encoding='utf-8')) if path.exists() else None

    def replace(self, source, name):
        destination = self.web / name
        no_links(destination)
        temporary = destination.with_name('.' + destination.name + '.snow-shot-publish')
        no_links(temporary)
        if temporary.exists():
            temporary.unlink()
        os.link(source, temporary)
        os.chmod(temporary, 0o644)
        os.replace(temporary, destination)
        sync_directory(destination.parent)

    def restore(self, identifier):
        directory = self.release(identifier)
        journal = json.loads((directory / 'journal.json').read_text(encoding='utf-8'))
        for name in ALLOWED:
            backup = directory / 'before' / name
            if name in journal['before']:
                if digest(backup) != journal['before'][name]:
                    raise ValueError('Rollback backup checksum mismatch')
        for name in ALLOWED:
            if name in journal['before']:
                self.replace(directory / 'before' / name, name)
            elif (self.web / name).exists():
                (self.web / name).unlink()
                sync_directory((self.web / name).parent)
        atomic_json(self.private / 'current.json', journal['previous'])
        atomic_json(self.private / 'pending.json', None)
        return {'rolledBack': identifier}

    def recover(self):
        pending = self.read_state('pending.json')
        if pending:
            if time.time() - pending.get('createdAt', 0) < 3600:
                raise ValueError('Another deployment is awaiting public verification; retry later or roll it back explicitly')
            self.restore(pending['id'])

    def begin(self, request):
        with self.lock():
            self.recover()
            directory = self.release(request['id'])
            directory.mkdir(mode=0o700)
            (directory / 'files' / 'setup').mkdir(parents=True)
            return {'uploadDirectory': str(directory / 'files'), 'id': request['id']}

    def activate(self, request, checkpoint=lambda _: None):
        with self.lock():
            self.recover()
            identifier = request['id']
            directory = self.release(identifier)
            files = request['files']
            if set(files) != set(ALLOWED):
                raise ValueError('The publish file allowlist does not match')
            for name in ALLOWED:
                path = directory / 'files' / name
                no_links(path)
                if path.stat().st_size != files[name]['size'] or digest(path) != files[name]['sha256']:
                    raise ValueError('Uploaded artifact checksum mismatch: ' + name)
                with path.open('r+b') as stream:
                    os.fsync(stream.fileno())
            version = (directory / 'files/latest-version.txt').read_text(encoding='utf-8').strip()
            key = version_key(version)
            envelope = json.loads((directory / 'files/latest-version.json').read_text(encoding='utf-8'))
            payload = json.loads(base64.b64decode(envelope['payload'], validate=True))
            if payload['version'] != version or request['version'] != version:
                raise ValueError('Release metadata versions disagree')
            if (self.web / 'latest-version.txt').exists():
                old = (self.web / 'latest-version.txt').read_text(encoding='utf-8').strip()
                old_key = version_key(old)
                if key < old_key:
                    raise ValueError('Refusing a release downgrade; use explicit rollback')
                if key == old_key:
                    if all((self.web / n).exists() and digest(self.web / n) == files[n]['sha256'] for n in ALLOWED):
                        shutil.rmtree(directory)
                        return {'idempotent': True, 'version': version}
                    raise ValueError('Refusing different artifacts under the same release version')
            before = {}
            for name in ALLOWED:
                source = self.web / name
                if source.exists():
                    backup = directory / 'before' / name
                    backup.parent.mkdir(parents=True, exist_ok=True)
                    os.link(source, backup)
                    before[name] = digest(backup)
                    sync_directory(backup.parent)
            journal = {'before': before, 'previous': self.read_state('current.json'),
                       'version': version, 'files': files, 'createdAt': time.time()}
            atomic_json(directory / 'journal.json', journal)
            atomic_json(self.private / 'pending.json', {'id': identifier, 'createdAt': time.time()})
            try:
                for name in ALLOWED:
                    self.replace(directory / 'files' / name, name)
                    checkpoint(name)
                for name in ALLOWED:
                    if digest(self.web / name) != files[name]['sha256']:
                        raise ValueError('Published artifact verification failed')
                # Keep pending until the client verifies through public HTTPS and commits.
                return {'version': version, 'id': identifier, 'awaitingCommit': True}
            except Exception:
                self.restore(identifier)
                raise

    def commit(self, request):
        with self.lock():
            pending = self.read_state('pending.json')
            current = self.read_state('current.json')
            if not pending and current and current['id'] == request['id']:
                return {'committed': request['id'], 'version': current['version']}
            if not pending or pending['id'] != request['id']:
                raise ValueError('No matching deployment is awaiting verification')
            directory = self.release(request['id'])
            journal = json.loads((directory / 'journal.json').read_text(encoding='utf-8'))
            atomic_json(self.private / 'current.json', {'id': request['id'], 'version': journal['version']})
            atomic_json(self.private / 'pending.json', None)
            # Follow the rollback chain: current plus two previous successful deployments.
            keep, cursor = set(), self.read_state('current.json')
            for index in range(3):
                if not cursor:
                    break
                keep.add(cursor['id'])
                data = json.loads((self.release(cursor['id']) / 'journal.json').read_text(encoding='utf-8'))
                if index == 2 and data['previous']:
                    # Its backup still restores the older public payload, but that older
                    # deployment is no longer a retained rollback transaction.
                    data['previous'] = None
                    atomic_json(self.release(cursor['id']) / 'journal.json', data)
                cursor = data['previous']
            for candidate in self.private.iterdir():
                if re.fullmatch('[a-f0-9]{32}', candidate.name) and candidate.name not in keep:
                    no_links(candidate)
                    if candidate.is_dir() and (candidate / 'journal.json').exists():
                        shutil.rmtree(candidate)
            return {'committed': request['id'], 'version': journal['version']}

    def rollback(self, request):
        with self.lock():
            pending = self.read_state('pending.json')
            current = self.read_state('current.json')
            selected = pending or current
            if not selected or (request.get('id') and request['id'] != selected['id']):
                raise ValueError('No matching release to roll back')
            return self.restore(selected['id'])

    def verify(self, request):
        if self.read_state('pending.json'):
            raise ValueError('A deployment is awaiting verification or recovery')
        result = {name: {'size': (self.web / name).stat().st_size, 'sha256': digest(self.web / name)}
                  for name in ALLOWED if (self.web / name).exists()}
        for name in ('latest-version.json', 'latest-version.txt'):
            if name in result:
                result[name]['contentBase64'] = base64.b64encode((self.web / name).read_bytes()).decode('ascii')
        return result


def main():
    request = json.loads(base64.b64decode(sys.argv[1], validate=True))
    publisher = Publisher(request['webRoot'])
    operation = request['operation']
    if operation not in ('begin', 'activate', 'commit', 'rollback', 'verify'):
        raise ValueError('Unknown publish operation')
    print(json.dumps(getattr(publisher, operation)(request)))


if __name__ == '__main__':
    main()
