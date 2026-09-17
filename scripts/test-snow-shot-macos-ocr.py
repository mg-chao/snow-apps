#!/usr/bin/env python3
"""Deterministic tests for the macOS OCR staging and integrity contract."""
import importlib.util
import json
from pathlib import Path
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
        self.source = dict(runtime=dict(version='1.0.7'), models=[dict(type='small', id='small-id', files=files)])
        ocr.atomic_json(self.model / '.complete.json', dict(schema=1, component='small-id'))
        self.run = patch.object(ocr, 'run', return_value='snow-ocr-process 1.0.7 macos-aarch64 protocol 3').start()
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

    def test_manifest_pins_final_bytes_and_preserves_models(self):
        ocr.finalize(self.source, self.runtime)
        result = ocr.verify_assets(self.source, self.runtime)
        self.assertEqual(result['schema'], 3)
        self.assertEqual(result['models'], self.source['models'])
        self.assertEqual(result['runtime']['delivery'], 'bundled')
        self.assertNotIn('archive', result['runtime'])
        with (self.runtime / 'snow-ocr-process').open('ab') as binary:
            binary.write(b'new signature')
        with self.assertRaisesRegex(ValueError, 'finalized runtime bytes'):
            ocr.verify_assets(self.source, self.runtime)
        ocr.finalize(self.source, self.runtime)
        ocr.verify_assets(self.source, self.runtime)

    def test_missing_model_or_marker_prevents_release(self):
        ocr.finalize(self.source, self.runtime)
        (self.model / 'det.onnx').write_bytes(b'corrupted')
        with self.assertRaisesRegex(ValueError, 'corrupt bundled model'):
            ocr.verify_assets(self.source, self.runtime)
        (self.model / 'det.onnx').write_bytes(b'det.onnx')
        ocr.atomic_json(self.model / '.complete.json', dict(schema=1, component='wrong'))
        with self.assertRaisesRegex(ValueError, 'completion marker'):
            ocr.verify_assets(self.source, self.runtime)

    def test_wrong_architecture_protocol_and_permissions_are_rejected(self):
        self.run.return_value = 'snow-ocr-process 1.0.7 macos-aarch64 protocol 2'
        with self.assertRaisesRegex(ValueError, 'version/protocol'):
            ocr.finalize(self.source, self.runtime)
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

    def test_development_closure_is_staged_then_removed_after_deployment(self):
        library = self.root / 'upstream/libonnxruntime.dylib'
        library.parent.mkdir()
        library.write_bytes(ocr.ARM64_HEADER + bytes(24))
        dependency = library.parent / 'libcpuinfo.dylib'
        dependency.write_bytes(ocr.ARM64_HEADER + bytes(24))
        def inspect(*args):
            path = Path(args[-1])
            if '-D' in args:
                return f'{path}:\n@rpath/{path.name}'
            loads = '\n\t@rpath/libcpuinfo.dylib (compatibility version 0.0.0)' if path == library.resolve() else ''
            return f'{path}:\n\t@rpath/{path.name} (compatibility version 0.0.0){loads}'
        self.run.side_effect = inspect
        ocr.stage_native_dependencies(library, self.runtime)
        self.assertTrue((self.runtime / dependency.name).is_file())
        ocr.remove_development_libraries(self.runtime)
        self.assertFalse((self.runtime / dependency.name).exists())
        self.assertTrue((self.runtime / 'libonnxruntime.dylib').is_file())

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
            if 'lipo' in args[0]:
                return state['arch']
            if '-l' in args:
                return ('cmd LC_BUILD_VERSION\nminos ' + state['minimum'] + '\ncmd LC_RPATH\ncmdsize 48\npath ' + state['rpath'] + ' (offset 12)')
            if '-L' in args:
                return f'{binary}:\n\t{state["dependency"]} (compatibility version 1.0.0)'
            return str(binary) + ':'
        self.run.side_effect = inspect
        self.assertEqual(ocr.verify_bundle(app), ['Contents/MacOS/snow_shot'])
        for key, value in (('arch', 'x86_64'), ('minimum', '27.0'),
                           ('dependency', '/developer/libexample.dylib'),
                           ('dependency', '@rpath/missing.dylib'), ('rpath', '@loader_path/../../../../outside')):
            previous = state[key]
            state[key] = value
            with self.assertRaises(ValueError):
                ocr.verify_bundle(app)
            state[key] = previous


if __name__ == '__main__':
    unittest.main()
