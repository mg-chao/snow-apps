"""Focused publisher tests; never accesses the production server."""
import base64
import importlib.util
import json
from pathlib import Path
import tempfile
import unittest
import uuid

spec = importlib.util.spec_from_file_location('publisher', Path(__file__).with_name('snow-shot-publish-server.py'))
module = importlib.util.module_from_spec(spec)
spec.loader.exec_module(module)


class PublisherTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.web = Path(self.temp.name) / 'html'
        (self.web / 'setup').mkdir(parents=True)
        (self.web / 'latest-version.txt').write_text('0.8.1-dev')
        (self.web / 'setup/legacy installer.exe').write_bytes(b'preserve me')
        (self.web / 'index.html').write_bytes(b'preserve website')
        self.publisher = module.Publisher(self.web)

    def stage(self, version='1.0.0-beta', content=b'release'):
        identifier = uuid.uuid4().hex
        response = self.publisher.begin({'id': identifier})
        directory = Path(response['uploadDirectory'])
        files = {}
        for name in module.ALLOWED:
            data = content + name.encode()
            if name == 'latest-version.txt':
                data = version.encode()
            elif name == 'latest-version.json':
                data = json.dumps({'payload': base64.b64encode(json.dumps({'version': version}).encode()).decode()}).encode()
            path = directory / name
            path.write_bytes(data)
            files[name] = {'size': len(data), 'sha256': module.digest(path)}
        return {'id': identifier, 'version': version, 'files': files}

    def untouched(self):
        self.assertEqual((self.web / 'setup/legacy installer.exe').read_bytes(), b'preserve me')
        self.assertEqual((self.web / 'index.html').read_bytes(), b'preserve website')

    def test_publish_order_and_commit(self):
        request = self.stage()
        order = []
        self.publisher.activate(request, order.append)
        self.assertEqual(order, list(module.ALLOWED))
        self.assertEqual(order[-1], 'latest-version.txt')
        with self.assertRaises(ValueError):
            self.publisher.verify({})
        self.publisher.commit(request)
        self.assertEqual(set(self.publisher.verify({})), set(module.ALLOWED))
        self.untouched()

    def test_each_promotion_failure_restores_old_release(self):
        for failing_name in module.ALLOWED:
            with self.subTest(failing_name=failing_name):
                request = self.stage()

                def checkpoint(name):
                    if name == failing_name:
                        raise OSError('injected interruption')

                with self.assertRaises(OSError):
                    self.publisher.activate(request, checkpoint)
                self.assertEqual((self.web / 'latest-version.txt').read_text(), '0.8.1-dev')
                self.assertFalse((self.web / 'latest-version.json').exists())
                self.untouched()

    def test_corruption_and_allowlist_rejected_before_mutation(self):
        request = self.stage()
        upload = self.publisher.release(request['id']) / 'files' / module.ALLOWED[0]
        upload.write_bytes(b'corrupt')
        with self.assertRaises(ValueError):
            self.publisher.activate(request)
        request['files']['index.html'] = request['files'][module.ALLOWED[0]]
        with self.assertRaises(ValueError):
            self.publisher.activate(request)
        self.assertEqual((self.web / 'latest-version.txt').read_text(), '0.8.1-dev')
        self.untouched()

    def test_concurrent_deployment_cannot_recover_live_transaction(self):
        first = self.stage()
        second = self.stage('1.0.0-beta.1')
        self.publisher.activate(first)
        with self.assertRaises(ValueError):
            self.publisher.activate(second)
        with self.assertRaises(ValueError):
            self.publisher.begin({'id': uuid.uuid4().hex})
        self.publisher.commit(first)
        self.assertEqual(self.publisher.commit(first)['committed'], first['id'])
        self.publisher.activate(second)
        self.publisher.commit(second)

    def test_stale_interruption_recovers_on_next_publish(self):
        first = self.stage()
        self.publisher.activate(first)
        pending = self.publisher.read_state('pending.json')
        pending['createdAt'] = 0
        module.atomic_json(self.publisher.private / 'pending.json', pending)
        self.publisher.begin({'id': uuid.uuid4().hex})
        self.assertEqual((self.web / 'latest-version.txt').read_text(), '0.8.1-dev')
        self.untouched()

    def test_idempotency_same_version_conflict_and_downgrade(self):
        first = self.stage()
        self.publisher.activate(first)
        self.publisher.commit(first)
        same = self.stage()
        self.assertTrue(self.publisher.activate(same)['idempotent'])
        for request in (self.stage(content=b'changed'), self.stage('0.7.0')):
            with self.assertRaises(ValueError):
                self.publisher.activate(request)

    def test_explicit_rollback_and_retention(self):
        for number in range(1, 5):
            request = self.stage(f'1.0.{number}')
            self.publisher.activate(request)
            self.publisher.commit(request)
        retained = [p for p in self.publisher.private.iterdir() if p.is_dir() and (p / 'journal.json').exists()]
        self.assertEqual(len(retained), 3)
        self.publisher.rollback({})
        self.assertEqual((self.web / 'latest-version.txt').read_text(), '1.0.3')
        self.publisher.rollback({})
        self.publisher.rollback({})
        self.assertEqual((self.web / 'latest-version.txt').read_text(), '1.0.1')
        self.assertIsNone(self.publisher.read_state('current.json'))
        request = self.stage('1.0.5')
        self.publisher.activate(request)
        self.publisher.commit(request)
        self.assertEqual((self.web / 'latest-version.txt').read_text(), '1.0.5')
        self.untouched()

    def test_path_and_semver_validation(self):
        for path in ('../outside', '', '/tmp', 'g' * 32):
            with self.assertRaises(ValueError):
                self.publisher.release(path)
        versions = ['1.0.0-alpha', '1.0.0-alpha.1', '1.0.0-beta.2', '1.0.0-beta.10', '1.0.0', '1.0.1']
        self.assertEqual(sorted(versions, key=module.version_key), versions)
        for version in ('01.0.0', '1.0', '1.0.0-beta.01', '1.0.0_a', '1.0.0-beta_1'):
            with self.assertRaises(ValueError):
                module.version_key(version)


if __name__ == '__main__':
    unittest.main()
