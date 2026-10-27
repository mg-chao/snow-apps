#!/usr/bin/env python3
"""Deterministic tests for the macOS OCR staging and integrity contract."""
import importlib.util
import json
from pathlib import Path
import subprocess
import tempfile
import unittest
from unittest.mock import patch

spec = importlib.util.spec_from_file_location('ocr', Path(__file__).with_name('snow-shot-macos-ocr.py'))
ocr = importlib.util.module_from_spec(spec)
spec.loader.exec_module(ocr)


class MacOSOcrAssets(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix='snow OCR 空间 ')
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        self.runtime = self.root / 'runtime'
        self.runtime.mkdir()
        for name in ocr.RUNTIME_FILES:
            path = self.runtime / name
            path.write_bytes(ocr.ARM64_HEADER + bytes(24) + name.encode())
            path.chmod(0o755)
        self.model = self.runtime / 'assets/ocr/models/small-id'
        self.model.mkdir(parents=True)
        files = []
        for name in ('det.onnx', 'rec.onnx', 'dict.txt'):
            path = self.model / name
            path.write_bytes(name.encode())
            files.append(dict(ocr.descriptor(path), url=f'https://example.invalid/{name}'))
        self.source = dict(runtime=dict(version='1.2.0'), models=[dict(type='small', id='small-id', files=files)])
        ocr.atomic_json(self.model / '.complete.json', dict(schema=1, component='small-id'))
        self.run = patch.object(ocr, 'run', return_value='snow-ocr-process 1.2.0 macos-aarch64 protocol 7').start()
        self.addCleanup(patch.stopall)

    def test_bundle_data_is_sealed_as_resources_with_stable_lookup_paths(self):
        app = self.root / 'Snow Shot.app'
        data = app / 'Contents/MacOS/assets/ocr/model.onnx'
        data.parent.mkdir(parents=True)
        data.write_bytes(b'pinned model')
        ocr.prepare_bundle(app)
        self.assertTrue((app / 'Contents/MacOS/assets').is_symlink())
        self.assertEqual(data.read_bytes(), b'pinned model')
        self.assertEqual((app / 'Contents/Resources/assets/ocr/model.onnx').read_bytes(), b'pinned model')
        ocr.prepare_bundle(app)
        (app / 'Contents/MacOS/assets').unlink()
        (app / 'Contents/MacOS/assets').symlink_to(self.root)
        with self.assertRaisesRegex(ValueError, 'Invalid bundled resource link'):
            ocr.prepare_bundle(app)

    def test_bundle_preparation_removes_reused_loose_audio_and_preserves_ocr(self):
        app = self.root / 'Snow Shot.app'
        runtime = app / 'Contents/MacOS'
        resources = app / 'Contents/Resources'
        for directory in (runtime, resources):
            audio = directory / 'audios/camera_shutter.mp3'
            audio.parent.mkdir(parents=True)
            audio.write_bytes(b'obsolete loose resource')
            model = directory / 'assets/ocr/models' / directory.name / 'rec.onnx'
            model.parent.mkdir(parents=True)
            model.write_bytes(directory.name.encode())

        for unused in range(2):
            ocr.prepare_bundle(app)
            self.assertFalse((runtime / 'audios').exists())
            self.assertFalse((runtime / 'audios').is_symlink())
            self.assertFalse((resources / 'audios').exists())
            for name in ('MacOS', 'Resources'):
                self.assertEqual(
                    (resources / 'assets/ocr/models' / name / 'rec.onnx').read_bytes(),
                    name.encode())
            # Exercise an already-normalized resource link on the second run.
            if unused == 0:
                audio = resources / 'audios/camera_shutter.mp3'
                audio.parent.mkdir()
                audio.write_bytes(b'previous installed audio')
                (runtime / 'audios').symlink_to('../Resources/audios')

    def test_bundle_audio_cleanup_preserves_unrelated_files_and_external_link_targets(self):
        app = self.root / 'Snow Shot.app'
        runtime = app / 'Contents/MacOS'
        resources = app / 'Contents/Resources'
        external = self.root / 'external.mp3'
        external.write_bytes(b'outside package')
        for directory in (runtime, resources):
            audio = directory / 'audios'
            audio.mkdir(parents=True)
            (audio / 'camera_shutter.mp3').symlink_to(external)
            (audio / (directory.name + '.mp3')).write_bytes(directory.name.encode())

        ocr.prepare_bundle(app)
        self.assertEqual(external.read_bytes(), b'outside package')
        self.assertTrue((runtime / 'audios').is_symlink())
        self.assertFalse((resources / 'audios/camera_shutter.mp3').exists())
        self.assertFalse((resources / 'audios/camera_shutter.mp3').is_symlink())
        for name in ('MacOS', 'Resources'):
            self.assertEqual((resources / 'audios' / (name + '.mp3')).read_bytes(), name.encode())

    def test_bundle_audio_cleanup_removes_destination_only_files_and_empty_directories(self):
        app = self.root / 'Snow Shot.app'
        runtime = app / 'Contents/MacOS'
        resources = app / 'Contents/Resources'
        runtime.mkdir(parents=True)
        audio = resources / 'audios/camera_shutter.mp3'
        audio.parent.mkdir(parents=True)
        audio.write_bytes(b'previous installed audio')
        ocr.prepare_bundle(app)
        self.assertFalse(audio.parent.exists())
        for directory in (runtime, resources):
            (directory / 'audios').mkdir()
        ocr.prepare_bundle(app)
        self.assertFalse((runtime / 'audios').exists())
        self.assertFalse((runtime / 'audios').is_symlink())
        self.assertFalse((resources / 'audios').exists())

    def test_bundle_audio_cleanup_rejects_external_parent_links(self):
        external = self.root / 'external-audios'
        external.mkdir()
        shutter = external / 'camera_shutter.mp3'
        shutter.write_bytes(b'outside package')
        for name in ('MacOS', 'Resources'):
            with self.subTest(directory=name):
                app = self.root / (name + '.app')
                runtime = app / 'Contents/MacOS'
                resources = app / 'Contents/Resources'
                runtime.mkdir(parents=True)
                resources.mkdir()
                (app / 'Contents' / name / 'audios').symlink_to(external)
                with self.assertRaisesRegex(ValueError, 'Invalid bundled resource'):
                    ocr.prepare_bundle(app)
                self.assertEqual(shutter.read_bytes(), b'outside package')

    def test_manifest_pins_final_bytes_and_preserves_models(self):
        ocr.finalize(self.source, self.runtime)
        result = ocr.verify_assets(self.source, self.runtime)
        self.assertEqual(result['schema'], 3)
        self.assertEqual(result['models'], self.source['models'])
        self.assertEqual(result['runtime']['delivery'], 'bundled')
        self.assertEqual(result['runtime']['protocol'], 7)
        self.assertNotIn('archive', result['runtime'])
        with (self.runtime / 'snow-ocr-process').open('ab') as binary:
            binary.write(b'new signature')
        with self.assertRaisesRegex(ValueError, 'finalized runtime bytes'):
            ocr.verify_assets(self.source, self.runtime)
        ocr.finalize(self.source, self.runtime)
        ocr.verify_assets(self.source, self.runtime)

    def test_packaging_rejects_same_size_runtime_and_model_corruption(self):
        ocr.finalize(self.source, self.runtime)
        for path in (self.runtime / 'snow-ocr-process', self.model / 'det.onnx',
                     self.model / 'rec.onnx', self.model / 'dict.txt'):
            with self.subTest(file=path.name):
                original = path.read_bytes()
                path.write_bytes(original[:-1] + bytes([original[-1] ^ 1]))
                with self.assertRaisesRegex(
                        ValueError, 'finalized runtime bytes|Missing or corrupt bundled model'):
                    ocr.verify_assets(self.source, self.runtime)
                path.write_bytes(original)
        ocr.verify_assets(self.source, self.runtime)

    def test_x64_stage_finalization_and_report_use_the_target_architecture(self):
        for name in ocr.RUNTIME_FILES:
            (self.runtime / name).write_bytes(ocr.X64_HEADER + bytes(24) + name.encode())
        self.run.return_value = 'snow-ocr-process 1.2.0 macos-x86_64 protocol 7'
        with patch.object(ocr, 'stage_models'):
            self.command('stage', '--arch', 'x64',
                         '--worker', self.runtime / 'snow-ocr-process')
        result = ocr.verify_assets(self.source, self.runtime, arch='x64')
        self.assertEqual(result['runtime']['platform'], 'macos-x64')
        self.assertEqual(result['runtime']['protocol'], 7)
        self.assertEqual(result['models'], self.source['models'])
        self.assertEqual([item['name'] for item in result['runtime']['files']], ['snow-ocr-process'])
        report = self.root / 'x64-report.json'
        self.command('verify', '--arch', 'x64', '--report', report)
        self.assertEqual(json.loads(report.read_text())['platform'], 'macos-x64')
        self.assertEqual(json.loads(report.read_text())['bundled_model'], 'small')
        with self.assertRaisesRegex(ValueError, 'thin ARM64'):
            ocr.verify_assets(self.source, self.runtime)

    def test_x64_rejects_wrong_architecture_and_worker_identity(self):
        with self.assertRaisesRegex(ValueError, 'thin x64'):
            ocr.runtime_manifest(self.source, self.runtime, arch='x64')
        for name in ocr.RUNTIME_FILES:
            (self.runtime / name).write_bytes(ocr.X64_HEADER + bytes(24))
        with self.assertRaisesRegex(ValueError, 'version/protocol'):
            ocr.runtime_manifest(self.source, self.runtime, arch='x64')
        self.run.return_value = 'snow-ocr-process 1.2.0 macos-x86_64 protocol 7'
        ocr.finalize(self.source, self.runtime, arch='x64')
        manifest = self.runtime / 'assets/ocr/asset-manifest.json'
        value = json.loads(manifest.read_text())
        value['runtime']['platform'] = 'macos-arm64'
        ocr.atomic_json(manifest, value)
        with self.assertRaisesRegex(ValueError, 'finalized runtime bytes'):
            ocr.verify_assets(self.source, self.runtime, arch='x64')
        (self.runtime / 'snow-ocr-process').write_bytes(ocr.ARM64_HEADER + bytes(24))
        with self.assertRaisesRegex(ValueError, 'thin x64'):
            ocr.runtime_manifest(self.source, self.runtime, arch='x64')

    def test_x64_bundle_audit_inspects_the_x86_64_slice(self):
        app = self.root / 'Snow Shot.app'
        binary = app / 'Contents/MacOS/snow_shot'
        binary.parent.mkdir(parents=True)
        binary.write_bytes(ocr.X64_HEADER + bytes(24))

        def inspect(*args):
            if 'lipo' in args[0]:
                return 'x86_64'
            if 'otool' in args[0]:
                self.assertEqual(args[1:3], ('-arch', 'x86_64'))
                if '-l' in args:
                    return 'cmd LC_BUILD_VERSION\nminos 15.0'
                if '-L' in args:
                    return f'{binary}:\n\t/usr/lib/libSystem.B.dylib (compatibility version 1.0.0)'
                return str(binary)
            return ''

        self.run.side_effect = inspect
        self.assertEqual(ocr.verify_bundle(app, arch='x64'), ['Contents/MacOS/snow_shot'])
        self.run.assert_called_with('/usr/bin/codesign', '--verify', '--deep', '--strict', str(app))
        self.assertEqual(sum(call.args[0] == '/usr/bin/codesign'
                             for call in self.run.call_args_list), 1)
        self.run.side_effect = lambda *args: 'arm64'
        with self.assertRaisesRegex(ValueError, 'Non-x64'):
            ocr.verify_bundle(app, arch='x64')

    def command(self, command, *arguments):
        manifest = self.root / 'source-manifest.json'
        ocr.atomic_json(manifest, self.source)
        argv = ['snow-shot-macos-ocr.py', command, '--manifest', str(manifest),
                '--runtime-dir', str(self.runtime), *map(str, arguments)]
        with patch.object(ocr.sys, 'argv', argv):
            ocr.main()

    def test_runtime_only_stage_removes_stale_models_and_retains_download_metadata(self):
        cache = self.root / 'cache'
        with patch.object(ocr, 'stage_models', side_effect=AssertionError('models must not stage')):
            self.command('stage', '--worker', self.runtime / 'snow-ocr-process',
                         '--runtime-only', '--cache', cache)
        self.assertFalse((self.runtime / 'assets/ocr/models').exists())
        self.assertFalse(cache.exists())
        result = ocr.verify_assets(self.source, self.runtime, runtime_only=True)
        self.assertEqual(result['default_model'], 'small')
        self.assertEqual(result['models'], self.source['models'])
        self.assertEqual([item['name'] for item in result['runtime']['files']], ['snow-ocr-process'])
        self.command('finalize', '--runtime-only')
        report = self.root / 'runtime-only-report.json'
        self.command('verify', '--runtime-only', '--report', report)
        self.assertIsNone(json.loads(report.read_text())['bundled_model'])
        with self.assertRaisesRegex(ValueError, 'Missing or corrupt bundled model'):
            ocr.verify_assets(self.source, self.runtime)

    def test_runtime_only_finalization_and_verification_reject_model_payload(self):
        ocr.finalize(self.source, self.runtime)
        for operation in (ocr.finalize, ocr.verify_assets):
            with self.assertRaisesRegex(ValueError, 'must not contain bundled models'):
                operation(self.source, self.runtime, runtime_only=True)
        self.assertTrue((self.model / 'det.onnx').exists())
        ocr.remove_bundled_models(self.runtime)
        ocr.finalize(self.source, self.runtime, runtime_only=True)
        result = ocr.verify_assets(self.source, self.runtime, runtime_only=True)
        self.assertEqual([item['name'] for item in result['runtime']['files']],
                         ['snow-ocr-process'])
        models = self.runtime / 'assets/ocr/models'
        models.symlink_to(self.root / 'missing-model-directory')
        with self.assertRaisesRegex(ValueError, 'must not contain bundled models'):
            ocr.verify_assets(self.source, self.runtime, runtime_only=True)

    def test_runtime_only_staging_removes_model_links_without_deleting_cache(self):
        cached = self.root / 'cache/ocr-models-small-id'
        cached.parent.mkdir()
        self.model.rename(cached)
        (self.runtime / 'assets/ocr/models').rmdir()
        (self.runtime / 'assets/ocr/models').symlink_to(cached.parent)
        self.command('stage', '--worker', self.runtime / 'snow-ocr-process',
                     '--runtime-only')
        self.assertFalse((self.runtime / 'assets/ocr/models').is_symlink())
        self.assertTrue((cached / 'det.onnx').exists())

    def test_runtime_only_cleanup_rejects_ocr_parent_links_without_deleting_cache(self):
        cached = self.root / 'cache'
        (self.runtime / 'assets/ocr').rename(cached)
        (self.runtime / 'assets/ocr').symlink_to(cached)
        for operation in (ocr.remove_bundled_models, ocr.verify_no_bundled_models):
            with self.assertRaisesRegex(ValueError, 'Invalid bundled resource directory'):
                operation(self.runtime)
            self.assertEqual((cached / 'models/small-id/det.onnx').read_bytes(), b'det.onnx')
        with self.assertRaisesRegex(ValueError, 'Invalid bundled resource directory'):
            self.command('stage', '--worker', self.runtime / 'snow-ocr-process',
                         '--runtime-only')
        self.assertTrue((cached / 'models/small-id/rec.onnx').is_file())

    def test_runtime_only_cleanup_rejects_external_asset_parent_links(self):
        cached = self.root / 'cache'
        (self.runtime / 'assets').rename(cached)
        (self.runtime / 'assets').symlink_to(cached)
        with self.assertRaisesRegex(ValueError, 'Invalid bundled resource link'):
            ocr.remove_bundled_models(self.runtime)
        self.assertTrue((cached / 'ocr/models/small-id/det.onnx').is_file())

    def test_runtime_only_cli_rejects_external_runtime_root_before_resolving_it(self):
        cached = self.root / 'cache-runtime'
        self.runtime.rename(cached)
        self.runtime.symlink_to(cached)
        with self.assertRaisesRegex(ValueError, 'Invalid bundled resource directory'):
            self.command('stage', '--worker', self.runtime / 'snow-ocr-process',
                         '--runtime-only')
        self.assertEqual((cached / 'assets/ocr/models/small-id/det.onnx').read_bytes(), b'det.onnx')

    def test_full_staging_rebundles_small_after_runtime_only_staging(self):
        cached = self.root / 'cache/ocr-models-small-id'
        cached.mkdir(parents=True)
        for item in self.source['models'][0]['files']:
            ocr.copy_changed(self.model / item['name'], cached / item['name'])
        arguments = ('--worker', self.runtime / 'snow-ocr-process',
                     '--cache', cached.parent)
        self.command('stage', *arguments, '--runtime-only')
        with patch.object(ocr.urllib.request, 'urlopen', side_effect=AssertionError('network')):
            self.command('stage', *arguments)
        ocr.verify_assets(self.source, self.runtime)
        for item in self.source['models'][0]['files']:
            self.assertTrue(ocr.valid(self.model / item['name'], item))

    def test_runtime_only_bundle_preparation_cleans_reused_resource_models(self):
        app = self.root / 'Snow Shot Mini.app'
        resources = app / 'Contents/Resources/assets'
        stale = resources / 'ocr/models/old-small-id/rec.onnx'
        stale.parent.mkdir(parents=True)
        stale.write_bytes(b'previous bundled model')
        incoming = app / 'Contents/MacOS/assets/ocr/asset-manifest.json'
        incoming.parent.mkdir(parents=True)
        incoming.write_text(json.dumps(self.source))
        with patch.object(ocr.sys, 'argv', ['snow-shot-macos-ocr.py', 'prepare-bundle',
                                         '--app', str(app), '--runtime-only']):
            ocr.main()
        self.assertTrue((app / 'Contents/MacOS/assets').is_symlink())
        self.assertFalse((resources / 'ocr/models').exists())
        self.assertEqual(json.loads(incoming.read_text()), self.source)
        # An existing resource link must also clean up a package left by an
        # earlier install that bundled models before the Mini policy changed.
        stale.parent.mkdir(parents=True)
        stale.write_bytes(b'previous bundled model')
        ocr.prepare_bundle(app, runtime_only=True)
        self.assertFalse((resources / 'ocr/models').exists())
        ocr.prepare_bundle(app, runtime_only=True)
        self.assertTrue((resources / 'ocr/asset-manifest.json').is_file())

    def test_runtime_only_bundle_preparation_rejects_external_resource_links(self):
        app = self.root / 'Snow Shot Mini.app'
        runtime = app / 'Contents/MacOS'
        resources = app / 'Contents/Resources'
        runtime.mkdir(parents=True)
        resources.mkdir(parents=True)
        (runtime / 'assets').symlink_to('../Resources/assets')
        (resources / 'assets').symlink_to(self.runtime / 'assets')
        with self.assertRaisesRegex(ValueError, 'Invalid bundled resource link'):
            ocr.prepare_bundle(app, runtime_only=True)
        self.assertTrue((self.model / 'det.onnx').exists())

    def test_runtime_only_resource_merge_never_overwrites_destination_model_cache(self):
        app = self.root / 'Snow Shot Mini.app'
        source = app / 'Contents/MacOS/assets/ocr'
        resources = app / 'Contents/Resources/assets/ocr'
        incoming = source / 'models/small-id/rec.onnx'
        incoming.parent.mkdir(parents=True)
        incoming.write_bytes(b'stale bundled model')
        (source / 'asset-manifest.json').write_text(json.dumps(self.source))
        resources.mkdir(parents=True)
        cached = self.root / 'cache/models/small-id/rec.onnx'
        cached.parent.mkdir(parents=True)
        cached.write_bytes(b'previously downloaded model')
        (resources / 'models').symlink_to(cached.parent.parent)

        ocr.prepare_bundle(app, runtime_only=True)

        self.assertEqual(cached.read_bytes(), b'previously downloaded model')
        self.assertFalse((resources / 'models').exists())
        self.assertFalse((resources / 'models').is_symlink())
        self.assertTrue((app / 'Contents/MacOS/assets').is_symlink())
        self.assertEqual(json.loads((resources / 'asset-manifest.json').read_text()), self.source)

    def test_runtime_only_bundle_without_source_rejects_external_resources(self):
        app = self.root / 'Snow Shot Mini.app'
        resources = app / 'Contents/Resources'
        resources.mkdir(parents=True)
        (resources / 'assets').symlink_to(self.runtime / 'assets')
        self.assertFalse((app / 'Contents/MacOS/assets').exists())
        with self.assertRaisesRegex(ValueError, 'Invalid bundled resource link'):
            ocr.prepare_bundle(app, runtime_only=True)
        self.assertEqual((self.model / 'det.onnx').read_bytes(), b'det.onnx')

    def test_runtime_only_bundle_rejects_external_ocr_and_resource_ancestors(self):
        app = self.root / 'Snow Shot Mini.app'
        resources = app / 'Contents/Resources'
        assets = resources / 'assets'
        assets.mkdir(parents=True)
        (assets / 'ocr').symlink_to(self.runtime / 'assets/ocr')
        with self.assertRaisesRegex(ValueError, 'Invalid bundled resource directory'):
            ocr.prepare_bundle(app, runtime_only=True)
        self.assertEqual((self.model / 'rec.onnx').read_bytes(), b'rec.onnx')

        (assets / 'ocr').unlink()
        assets.rmdir()
        resources.rmdir()
        resources.symlink_to(self.runtime)
        with self.assertRaisesRegex(ValueError, 'Invalid bundled resource directory'):
            ocr.prepare_bundle(app, runtime_only=True)
        self.assertEqual((self.model / 'rec.onnx').read_bytes(), b'rec.onnx')

    def test_runtime_only_cleanup_accepts_valid_executable_resource_link(self):
        app = self.root / 'Snow Shot Mini.app'
        runtime = app / 'Contents/MacOS'
        resources = app / 'Contents/Resources'
        runtime.mkdir(parents=True)
        resources.mkdir(parents=True)
        (self.runtime / 'assets').rename(resources / 'assets')
        (runtime / 'assets').symlink_to('../Resources/assets')
        ocr.remove_bundled_models(runtime)
        self.assertTrue((runtime / 'assets').is_symlink())
        self.assertFalse((resources / 'assets/ocr/models').exists())
        ocr.verify_no_bundled_models(runtime)

    def test_fetch_models_still_requests_all_downloadable_model_variants(self):
        with patch.object(ocr, 'stage_models') as stage:
            self.command('fetch-models', '--model-root', self.root / 'fetched')
        stage.assert_called_once_with(self.source, ocr.ROOT / 'artifacts',
                                      self.root / 'fetched', all_models=True)

    def test_cpu_runtime_manifest_contains_only_the_worker(self):
        ocr.finalize(self.source, self.runtime)
        result = ocr.verify_assets(self.source, self.runtime)
        self.assertEqual([item['name'] for item in result['runtime']['files']],
                         ['snow-ocr-process'])

    def test_missing_model_or_marker_prevents_release(self):
        ocr.finalize(self.source, self.runtime)
        (self.model / 'det.onnx').write_bytes(b'corrupted')
        with self.assertRaisesRegex(ValueError, 'corrupt bundled model'):
            ocr.verify_assets(self.source, self.runtime)
        (self.model / 'det.onnx').write_bytes(b'det.onnx')
        ocr.atomic_json(self.model / '.complete.json', dict(schema=1, component='wrong'))
        with self.assertRaisesRegex(ValueError, 'completion marker'):
            ocr.verify_assets(self.source, self.runtime)

    def test_previous_protocol_is_rejected(self):
        self.run.return_value = 'snow-ocr-process 1.2.0 macos-aarch64 protocol 6'
        with self.assertRaisesRegex(ValueError, 'version/protocol'):
            ocr.finalize(self.source, self.runtime)

    def test_wrong_architecture_and_permissions_are_rejected(self):
        binary = self.runtime / 'snow-ocr-process'
        binary.chmod(0o644)
        with self.assertRaisesRegex(ValueError, 'not executable'):
            ocr.finalize(self.source, self.runtime)
        binary.chmod(0o755)
        binary.write_bytes(bytes.fromhex('cffaedfe07000001') + bytes(24))
        with self.assertRaisesRegex(ValueError, 'thin ARM64'):
            ocr.finalize(self.source, self.runtime)

    def test_cached_models_work_without_network(self):
        cached = self.root / 'cache/ocr-models-small-id'
        cached.mkdir(parents=True)
        for item in self.source['models'][0]['files']:
            ocr.copy_changed(self.model / item['name'], cached / item['name'])
        with patch.object(ocr.urllib.request, 'urlopen', side_effect=AssertionError('network')):
            ocr.stage_models(self.source, self.root / 'cache', self.root / 'staged')
        for item in self.source['models'][0]['files']:
            self.assertTrue(ocr.valid(self.root / 'staged/small-id' / item['name'], item))

    def test_failed_or_corrupt_download_never_promotes(self):
        item = self.source['models'][0]['files'][0]
        destination = self.root / 'downloads/model'
        with patch.object(ocr.urllib.request, 'urlopen', side_effect=OSError('interrupted')):
            with self.assertRaises(OSError):
                ocr.fetch(item, destination)
        self.assertFalse(destination.exists())
        self.assertEqual(list(destination.parent.iterdir()), [])
        class Response:
            url = item['url']
            def __enter__(self):
                return self
            def __exit__(self, *args):
                pass
            def read(self, size):
                if getattr(self, 'sent', False):
                    return b''
                self.sent = True
                return b'wrong bytes'
        with patch.object(ocr.urllib.request, 'urlopen', return_value=Response()):
            with self.assertRaisesRegex(ValueError, 'integrity'):
                ocr.fetch(item, destination)
        self.assertEqual(list(destination.parent.iterdir()), [])

    def test_http_and_symlink_assets_are_rejected(self):
        item = dict(self.source['models'][0]['files'][0], url='http://example.invalid/model')
        with self.assertRaisesRegex(ValueError, 'HTTPS'):
            ocr.fetch(item, self.root / 'model')
        link = self.root / 'link'
        link.symlink_to(self.model / item['name'])
        self.assertFalse(ocr.valid(link, item))

    def test_unchanged_staging_does_not_rewrite_files(self):
        path = self.root / 'manifest.json'
        ocr.atomic_json(path, self.source)
        before = path.stat().st_mtime_ns
        ocr.atomic_json(path, self.source)
        self.assertEqual(before, path.stat().st_mtime_ns)



    def test_worker_crashpad_dependency_stages_without_inference_libraries(self):
        libraries = self.root / 'upstream'
        libraries.mkdir()
        dependency = libraries / 'libz.dylib'
        dependency.write_bytes(ocr.ARM64_HEADER + bytes(24))
        worker = self.runtime / 'snow-ocr-process'
        def inspect(*args):
            path = Path(args[-1])
            if '-D' in args:
                return f'{path}:\n@rpath/{path.name}'
            loads = '\n\t@rpath/libz.dylib (compatibility version 1.0.0)' if path == worker.resolve() else ''
            return f'{path}:\n\t@rpath/{path.name} (compatibility version 1.0.0){loads}'
        self.run.side_effect = inspect
        ocr.stage_native_dependencies(worker, libraries, self.runtime)
        self.assertEqual((self.runtime / dependency.name).read_bytes(), dependency.read_bytes())
        self.assertEqual(sorted(p.name for p in self.runtime.glob('*.dylib')), ['libz.dylib'])
        inventory = self.runtime / 'assets/ocr/development-libraries.json'
        retained_times = {path: path.stat().st_mtime_ns
                          for path in (self.runtime / dependency.name, inventory)}
        ocr.stage_native_dependencies(worker, libraries, self.runtime)
        self.assertEqual({path: path.stat().st_mtime_ns for path in retained_times}, retained_times)
        ocr.remove_development_libraries(self.runtime)
        self.assertFalse((self.runtime / dependency.name).exists())
        self.assertTrue(worker.is_file())

    def test_cpu_staging_prunes_the_previous_runtime_dependency_closure(self):
        libraries = self.root / 'upstream'
        libraries.mkdir()
        (libraries / 'libz.dylib').write_bytes(ocr.ARM64_HEADER + bytes(24))
        cache = self.root / 'cache'
        for item in self.source['models'][0]['files']:
            ocr.copy_changed(self.model / item['name'], cache / 'ocr-models-small-id' / item['name'])
        marker = self.runtime / 'assets/ocr/development-libraries.json'
        worker = self.runtime / 'snow-ocr-process'
        unrelated = self.runtime / 'unrelated.dylib'
        unrelated.write_bytes(b'unrelated application library')

        def inspect(*args):
            if '--version' in args:
                return 'snow-ocr-process 1.2.0 macos-aarch64 protocol 7'
            path = Path(args[-1])
            if '-D' in args:
                return f'{path}:\n@rpath/{path.name}'
            dependency = '\n\t@rpath/libz.dylib (compatibility version 1.0.0)' if path == worker.resolve() else ''
            return f'{path}:{dependency}'

        self.run.side_effect = inspect
        for dynamic in (True, False):
            for runtime_only in (False, True):
                with self.subTest(dynamic=dynamic, runtime_only=runtime_only):
                    for name in ('libonnxruntime.dylib', 'libretired.dylib', 'libz.dylib'):
                        (self.runtime / name).write_bytes(b'old runtime dependency')
                    ocr.atomic_json(marker, ['libretired.dylib', 'libz.dylib'])
                    arguments = ['--worker', worker, '--cache', cache]
                    if dynamic:
                        arguments += ['--library-dir', libraries]
                    if runtime_only:
                        arguments += ['--runtime-only']
                    self.command('stage', *arguments)
                    self.assertFalse((self.runtime / 'libonnxruntime.dylib').exists())
                    self.assertFalse((self.runtime / 'libretired.dylib').exists())
                    self.assertTrue(worker.is_file())
                    self.assertEqual(unrelated.read_bytes(), b'unrelated application library')
                    if dynamic:
                        self.assertEqual(json.loads(marker.read_text()), ['libz.dylib'])
                        self.assertEqual((self.runtime / 'libz.dylib').read_bytes(),
                                         (libraries / 'libz.dylib').read_bytes())
                    else:
                        self.assertFalse(marker.exists())
                        self.assertFalse((self.runtime / 'libz.dylib').exists())
                    ocr.verify_assets(self.source, self.runtime, runtime_only=runtime_only)

    def test_cpu_staging_removes_the_retired_runtime_without_an_inventory(self):
        retired = self.runtime / 'libonnxruntime.dylib'
        retired.write_bytes(b'old inference runtime')
        for attempt in range(2):
            self.command('stage', '--worker', self.runtime / 'snow-ocr-process', '--runtime-only')
            self.assertFalse(retired.exists())
            self.assertFalse((self.runtime / 'assets/ocr/development-libraries.json').exists())
            ocr.verify_assets(self.source, self.runtime, runtime_only=True)

    def test_bundle_preparation_removes_the_untracked_retired_runtime(self):
        app = self.root / 'Reused App.app'
        runtime = app / 'Contents/MacOS'
        runtime.mkdir(parents=True)
        worker = runtime / 'snow-ocr-process'
        worker.write_bytes(ocr.ARM64_HEADER + bytes(24))
        libraries = self.root / 'upstream'
        libraries.mkdir()
        self.run.side_effect = lambda *args: f'{args[-1]}:'
        for dynamic in (True, False):
            with self.subTest(dynamic=dynamic):
                retired = runtime / 'libonnxruntime.dylib'
                retired.write_bytes(ocr.ARM64_HEADER + bytes(24))
                ocr.prepare_bundle(app, libraries if dynamic else None)
                self.assertFalse(retired.exists())
                self.assertTrue(worker.is_file())

    def test_deployment_prunes_retired_libraries_before_scanning_the_bundle(self):
        app = self.root / 'Reused App.app'
        runtime = app / 'Contents/MacOS'
        runtime.mkdir(parents=True)
        libraries = self.root / 'upstream'
        libraries.mkdir()
        worker = runtime / 'snow-ocr-process'
        worker.write_bytes(ocr.ARM64_HEADER + bytes(24))
        for directory, names in ((runtime, ('libonnxruntime.dylib', 'libretired.dylib', 'libz.dylib')),
                                 (libraries, ('libz.dylib',))):
            for name in names:
                (directory / name).write_bytes(ocr.ARM64_HEADER + bytes(24))
        marker = runtime / 'assets/ocr/development-libraries.json'
        ocr.atomic_json(marker, ['libretired.dylib', 'libz.dylib'])

        def inspect(*args):
            if 'install_name_tool' in args[0]:
                return ''
            path = Path(args[-1])
            self.assertNotIn(path.name, ('libonnxruntime.dylib', 'libretired.dylib'),
                             'obsolete libraries must not become deployment roots')
            if '-D' in args:
                return f'{path}:\n@rpath/{path.name}'
            if '-l' in args:
                return ''
            dependency = '\n\t@rpath/libz.dylib (compatibility version 1.0.0)' if path == worker else ''
            return f'{path}:{dependency}'

        self.run.side_effect = inspect
        ocr.deploy_native_libraries(app, libraries)
        self.assertFalse((runtime / 'libonnxruntime.dylib').exists())
        self.assertFalse((runtime / 'libretired.dylib').exists())
        self.assertFalse((runtime / 'libz.dylib').exists())
        self.assertFalse(marker.exists())
        self.assertTrue(worker.is_file())
        self.assertEqual((app / 'Contents/Frameworks/libz.dylib').read_bytes(),
                         (libraries / 'libz.dylib').read_bytes())

    def test_development_cleanup_removes_versioned_library_symlink_chain(self):
        versioned = self.runtime / 'libz.1.3.1.zlib-ng.dylib'
        versioned.write_bytes(b'temporary library')
        (self.runtime / 'libz.1.dylib').symlink_to(versioned.name)
        (self.runtime / 'libz.dylib').symlink_to('libz.1.dylib')
        retained = self.runtime / 'unrelated-link.dylib'
        retained.symlink_to('unrelated.dylib')
        marker = self.runtime / 'assets/ocr/development-libraries.json'
        ocr.atomic_json(marker, [versioned.name])

        ocr.remove_development_libraries(self.runtime)

        for name in (versioned.name, 'libz.1.dylib', 'libz.dylib'):
            self.assertFalse((self.runtime / name).is_symlink())
            self.assertFalse((self.runtime / name).exists())
        self.assertTrue(retained.is_symlink())
        self.assertFalse(marker.exists())

    def test_native_closure_is_copied_and_rewritten_without_build_rpaths(self):
        app = self.root / 'Native App.app'
        worker = app / 'Contents/MacOS/snow_shot'
        worker.parent.mkdir(parents=True)
        worker.write_bytes(ocr.ARM64_HEADER + bytes(24))
        libraries = self.root / 'native'
        libraries.mkdir()
        for name in ('libfirst.dylib', 'libsecond.dylib'):
            (libraries / name).write_bytes(ocr.ARM64_HEADER + bytes(24))
        changes = []
        def inspect(*args):
            if 'install_name_tool' in args[0]:
                changes.append(args)
                return ''
            path = Path(args[-1])
            if '-D' in args:
                return f'{path}:\n@rpath/{path.name}'
            if '-l' in args:
                return 'cmd LC_RPATH\ncmdsize 48\npath /developer/build/lib (offset 12)'
            dep = {'snow_shot': 'libfirst.dylib', 'libfirst.dylib': 'libsecond.dylib'}.get(path.name)
            return f'{path}:' + (f'\n\t@rpath/{dep} (compatibility version 1.0.0)' if dep else '')
        self.run.side_effect = inspect
        ocr.deploy_native_libraries(app, libraries)
        for name in ('libfirst.dylib', 'libsecond.dylib'):
            self.assertTrue((app / 'Contents/Frameworks' / name).is_file())
        self.assertTrue(any('-change' in call and '@loader_path/../Frameworks/libfirst.dylib' in call for call in changes))
        self.assertTrue(any('-change' in call and '@loader_path/libsecond.dylib' in call for call in changes))
        self.assertTrue(any('-delete_rpath' in call for call in changes))

    def test_development_inventory_cannot_escape_runtime_directory(self):
        marker = self.runtime / 'assets/ocr/development-libraries.json'
        ocr.atomic_json(marker, ['../outside.dylib'])
        with self.assertRaisesRegex(ValueError, 'Invalid development'):
            ocr.remove_development_libraries(self.runtime)

    def test_bundle_rejects_wrong_arch_newer_os_and_unresolved_dependencies(self):
        app = self.root / 'Snow Shot.app'
        binary = app / 'Contents/MacOS/snow_shot'
        binary.parent.mkdir(parents=True)
        binary.write_bytes(ocr.ARM64_HEADER + bytes(24))
        state = dict(arch='arm64', minimum='15.0', dependency='/usr/lib/libSystem.B.dylib', rpath='@loader_path/../Frameworks')
        def inspect(*args):
            if args[0] == '/usr/bin/codesign' and state.get('signature_failure'):
                raise subprocess.CalledProcessError(1, args)
            if 'lipo' in args[0]:
                return state['arch']
            if '-l' in args:
                return ('cmd LC_BUILD_VERSION\nminos ' + state['minimum'] + '\ncmd LC_RPATH\ncmdsize 48\npath ' + state['rpath'] + ' (offset 12)')
            if '-L' in args:
                return f'{binary}:\n\t{state["dependency"]} (compatibility version 1.0.0)'
            return str(binary) + ':'
        self.run.side_effect = inspect
        self.assertEqual(ocr.verify_bundle(app), ['Contents/MacOS/snow_shot'])
        self.run.assert_called_with('/usr/bin/codesign', '--verify', '--deep', '--strict', str(app))
        for key, value in (('arch', 'x86_64'), ('minimum', '27.0'),
                           ('dependency', '/developer/libexample.dylib'),
                           ('dependency', '@rpath/missing.dylib'), ('rpath', '@loader_path/../../../../outside')):
            previous = state[key]
            state[key] = value
            with self.assertRaises(ValueError):
                ocr.verify_bundle(app)
            state[key] = previous
        self.assertEqual(sum(call.args[0] == '/usr/bin/codesign'
                             for call in self.run.call_args_list), 1)
        state['signature_failure'] = True
        with self.assertRaises(subprocess.CalledProcessError):
            ocr.verify_bundle(app)


if __name__ == '__main__':
    unittest.main()
