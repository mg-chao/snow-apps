#!/usr/bin/env python3
"""Offscreen build-script contract tests; no compiler, Qt, or network required."""
import hashlib
import importlib.util
import json
import os
import platform
import plistlib
from pathlib import Path
import signal
import shutil
import struct
import subprocess
import sys
import tempfile
import threading
import unittest
from unittest import mock
import xml.etree.ElementTree as ET

ROOT = Path(__file__).resolve().parents[1]
QT_POLICY = json.loads((ROOT / 'scripts/qt-toolchain.json').read_bytes())
QT_VERSION = QT_POLICY['qtVersion']
QT_MACOS_DEPLOYMENT_TARGET = QT_POLICY['macosQtDeploymentTarget']


def bash_executable():
    if os.name == 'nt':
        candidates = [Path(os.environ.get('ProgramFiles', 'C:/Program Files')) / 'Git/bin/bash.exe',
                      Path(os.environ.get('LOCALAPPDATA', '')) / 'Programs/Git/bin/bash.exe']
        return next((str(path) for path in candidates if path.is_file()), None)
    return shutil.which('bash')


def write_qt_kit(prefix, static=True, architectures=('arm64', 'x64')):
    translations = prefix / 'translations'
    translations.mkdir(parents=True, exist_ok=True)
    for language in ('zh_CN', 'zh_TW'):
        (translations / f'qtbase_{language}.qm').touch()
    for module in ('Qt6', 'Qt6Core'):
        directory = prefix / 'lib/cmake' / module
        directory.mkdir(parents=True, exist_ok=True)
        (directory / (module + 'Config.cmake')).touch()
        (directory / (module + 'ConfigVersionImpl.cmake')).write_text(
            f'set(PACKAGE_VERSION "{QT_VERSION}")\n'
            'if(NOT CMAKE_SIZEOF_VOID_P STREQUAL "8")\n'
            '  math(EXPR installedBits "8 * 8")\n'
            '  set(PACKAGE_VERSION "${PACKAGE_VERSION} (${installedBits}bit)")\n'
            '  set(PACKAGE_VERSION_UNSUITABLE TRUE)\n'
            'endif()\n')
    core = prefix / 'lib/cmake/Qt6Core'
    linkage = 'STATIC' if static else 'SHARED'
    (core / 'Qt6CoreTargets.cmake').write_text(f'add_library(Qt6::Core {linkage} IMPORTED)\n')
    (core / 'Qt6CoreTargets-release.cmake').write_text(
        'IMPORTED_LOCATION_RELEASE "${_IMPORT_PREFIX}/lib/libQt6Core.a"\n')
    cpus = {'arm64': 0x0100000c, 'x64': 0x01000007}
    objects = [struct.pack('<8I', 0xfeedfacf, cpus[arch], 0, 1, 0, 0, 0, 0)
               for arch in architectures]
    if len(objects) == 1:
        binary = objects[0]
    else:
        offset = 8 + len(objects) * 20
        entries = []
        for arch, obj in zip(architectures, objects):
            entries.append(struct.pack('>5I', cpus[arch], 0, offset, len(obj), 0))
            offset += len(obj)
        binary = struct.pack('>2I', 0xcafebabe, len(objects)) + b''.join(entries + objects)
    (prefix / 'lib/libQt6Core.a').write_bytes(binary)


def write_static_qt_feature_targets(prefix):
    write_qt_kit(prefix)
    core = prefix / 'lib/cmake/Qt6Core/Qt6CoreTargets.cmake'
    core.parent.mkdir(parents=True, exist_ok=True)
    core.write_text('add_library(Qt6::Core STATIC IMPORTED)\n'
                    'QT_ENABLED_PUBLIC_FEATURES "timezone;static"\n'
                    'QT_ENABLED_PRIVATE_FEATURES "ltcg;system_zlib"\n'
                    'QT_DISABLED_PRIVATE_FEATURES "timezone_locale"\n')
    gui = prefix / 'lib/cmake/Qt6Gui/Qt6GuiTargets.cmake'
    gui.parent.mkdir(parents=True, exist_ok=True)
    gui.write_text('QT_ENABLED_PRIVATE_FEATURES "system_png"\n')


def static_qt_feature_fingerprint():
    policy_bytes = (ROOT / 'scripts/static-qt-features.json').read_bytes()
    hashes = [hashlib.sha256(policy_bytes).hexdigest()]
    hashes.extend(hashlib.sha256((ROOT / 'scripts' / name).read_bytes()).hexdigest()
                  for name in json.loads(policy_bytes)['windowsSourcePatches'])
    return hashlib.sha256('|'.join(hashes).encode('utf-8')).hexdigest()


class QtKitValidation(unittest.TestCase):
    """Qt upgrade guards runnable on every host without a compiler or shell."""

    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix='snow Qt validation ')
        self.addCleanup(self.temp.cleanup)
        self.prefix = Path(self.temp.name) / 'Qt kit'
        write_static_qt_feature_targets(self.prefix)
        stamp = self.prefix / 'share/snow-apps/static-qt-build.json'
        stamp.parent.mkdir(parents=True)
        stamp.write_text(json.dumps({
            'SchemaVersion': 3, 'QtVersion': QT_VERSION, 'Architecture': 'arm64',
            'SourceArchiveSha256': QT_POLICY['sourceArchiveSha256'],
            'Configuration': 'Release', 'DeploymentTarget': QT_MACOS_DEPLOYMENT_TARGET,
            'Dup3': False,
            'Ltcg': True, 'SystemPng': True, 'SystemZlib': True,
            'Timezone': True, 'TimezoneLocale': False,
            'FeatureFingerprint': static_qt_feature_fingerprint(),
        }))
        (self.prefix / 'share/snow-apps/qt-licenses').mkdir()
        spec = importlib.util.spec_from_file_location(
            'snow_qt_validator', ROOT / 'scripts/validate-static-qt.py')
        self.validator = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(self.validator)

    def run_validator(self, *args, success=True):
        result = subprocess.run([sys.executable, str(ROOT / 'scripts/validate-static-qt.py'),
                                 '--prefix', str(self.prefix), '--arch', 'arm64', *args],
                                text=True, capture_output=True)
        if success:
            self.assertEqual(result.returncode, 0, result.stderr)
        else:
            self.assertNotEqual(result.returncode, 0, result.stdout)
        return result

    def test_static_and_shared_kits_use_the_pinned_version(self):
        self.run_validator()
        self.run_validator('--features-only')
        write_qt_kit(self.prefix, static=False, architectures=('arm64',))
        self.run_validator('--kit-only')
        self.run_validator(success=False)
        self.assertEqual(self.validator.toolchain_policy()['qtVersion'], QT_VERSION)

    def test_stale_installed_version_cannot_be_hidden_by_a_current_stamp(self):
        for module in ('Qt6', 'Qt6Core'):
            path = self.prefix / 'lib/cmake' / module / (module + 'ConfigVersionImpl.cmake')
            original = path.read_text()
            path.write_text('set(PACKAGE_VERSION "6.11.1")\n')
            for mode in ((), ('--kit-only',), ('--features-only',)):
                with self.subTest(module=module, mode=mode):
                    result = self.run_validator(*mode, success=False)
                    self.assertIn(f'The installed {module} package must be Qt', result.stderr)
            path.write_text(original)

    def test_cmake_version_diagnostics_require_a_literal_package_version(self):
        for module in ('Qt6', 'Qt6Core'):
            config = self.prefix / 'lib/cmake' / module / (module + 'ConfigVersionImpl.cmake')
            original = config.read_text()
            wrapper = config.with_name(module + 'ConfigVersion.cmake')
            wrapper.write_text(
                f'include("${{CMAKE_CURRENT_LIST_DIR}}/{config.name}")\n'
                '# set(PACKAGE_VERSION "6.11.1")\n')
            for mode in ((), ('--kit-only',), ('--features-only',)):
                with self.subTest(module=module, mode=mode, literal=True):
                    self.run_validator(*mode)
            config.write_text(
                f'# set(PACKAGE_VERSION "{QT_VERSION}")\n'
                'set(PACKAGE_VERSION "${PACKAGE_VERSION} (${installedBits}bit)")\n')
            for mode in ((), ('--kit-only',), ('--features-only',)):
                with self.subTest(module=module, mode=mode, literal=False):
                    result = self.run_validator(*mode, success=False)
                    self.assertIn(f'The installed {module} package must be Qt', result.stderr)
            config.write_text(original)

    def test_stock_dialog_catalogs_are_required_without_an_english_catalog(self):
        self.assertFalse((self.prefix / 'translations/qtbase_en_US.qm').exists())
        self.run_validator('--kit-only')
        for language in ('zh_CN', 'zh_TW'):
            path = self.prefix / 'translations' / f'qtbase_{language}.qm'
            path.unlink()
            result = self.run_validator('--kit-only', success=False)
            self.assertIn('stock-dialog translation catalog is missing', result.stderr)
            path.touch()

    def test_missing_or_conflicting_package_metadata_is_rejected(self):
        config = self.prefix / 'lib/cmake/Qt6/Qt6ConfigVersionImpl.cmake'
        config.unlink()
        self.run_validator('--kit-only', success=False)
        config.write_text(f'set(PACKAGE_VERSION "{QT_VERSION}")\n')
        wrapper = config.with_name('Qt6ConfigVersion.cmake')
        wrapper.write_text('set(PACKAGE_VERSION "6.11.1")\n')
        self.run_validator('--kit-only', success=False)

    def test_release_binary_architecture_is_verified_independently_of_stamp(self):
        path = self.prefix / 'lib/libQt6Core.a'
        path.write_bytes(struct.pack('<8I', 0xfeedfacf, 0x01000007, 0, 1, 0, 0, 0, 0))
        result = self.run_validator(success=False)
        self.assertIn('does not support arm64', result.stderr)
        result = self.run_validator('--kit-only', success=False)
        self.assertIn('does not support arm64', result.stderr)

    def test_shared_framework_relwithdebinfo_artifact_is_validated(self):
        write_qt_kit(self.prefix, static=False, architectures=('arm64',))
        core = self.prefix / 'lib/cmake/Qt6Core'
        release = core / 'Qt6CoreTargets-release.cmake'
        release.unlink()
        configuration = core / 'Qt6CoreTargets-relwithdebinfo.cmake'
        framework = self.prefix / 'lib/QtCore.framework/Versions/A/QtCore'
        framework.parent.mkdir(parents=True)
        (self.prefix / 'lib/libQt6Core.a').rename(framework)
        configuration.write_text(
            'IMPORTED_LOCATION_RELWITHDEBINFO '
            '"${_IMPORT_PREFIX}/lib/QtCore.framework/Versions/A/QtCore"\n')
        self.run_validator('--kit-only')
        framework.write_bytes(struct.pack('<8I', 0xfeedfacf, 0x01000007, 0, 1, 0, 0, 0, 0))
        result = self.run_validator('--kit-only', success=False)
        self.assertIn('does not support arm64', result.stderr)
        configuration.unlink()
        self.run_validator('--kit-only', success=False)

    def test_static_kit_requires_the_audited_release_artifact(self):
        core = self.prefix / 'lib/cmake/Qt6Core'
        release = core / 'Qt6CoreTargets-release.cmake'
        configuration = core / 'Qt6CoreTargets-relwithdebinfo.cmake'
        configuration.write_text(release.read_text().replace('_RELEASE', '_RELWITHDEBINFO'))
        release.unlink()
        self.run_validator(success=False)
        self.run_validator('--features-only', success=False)

    def test_release_artifact_and_supported_linkage_are_required(self):
        targets = self.prefix / 'lib/cmake/Qt6Core/Qt6CoreTargets.cmake'
        targets.write_text('add_library(Qt6::Core INTERFACE IMPORTED)\n')
        self.run_validator('--kit-only', success=False)
        write_static_qt_feature_targets(self.prefix)
        release = targets.with_name('Qt6CoreTargets-release.cmake')
        release.unlink()
        self.run_validator('--kit-only', success=False)
        release.write_text('IMPORTED_LOCATION_DEBUG "${_IMPORT_PREFIX}/lib/libQt6Core.a"\n')
        self.run_validator('--kit-only', success=False)

    def test_thin_bsd_archive_and_universal_headers_are_supported(self):
        path = self.prefix / 'lib/libQt6Core.a'
        self.assertEqual(self.validator.binary_architectures(path), {'arm64', 'x64'})
        obj = struct.pack('<8I', 0xfeedfacf, 0x0100000c, 0, 1, 0, 0, 0, 0)
        name = b'qcore.o\0'
        member = f"{'#1/' + str(len(name)):<16}{0:<12}{0:<6}{0:<6}{100644:<8}{len(name + obj):<10}`\n".encode()
        path.write_bytes(b'!<arch>\n' + member + name + obj)
        self.assertEqual(self.validator.binary_architectures(path), {'arm64'})
        self.run_validator()

    def test_truncated_or_non_macho_artifacts_are_rejected(self):
        path = self.prefix / 'lib/libQt6Core.a'
        for payload in (b'not a macOS library', struct.pack('>2I', 0xcafebabe, 2),
                        b'!<arch>\n' + b' ' * 60):
            with self.subTest(payload=payload):
                path.write_bytes(payload)
                self.run_validator('--kit-only', success=False)

    def test_darwin_lto_objects_retain_their_actual_architecture(self):
        path = self.prefix / 'lib/libQt6Core.a'
        bitcode = b'BC\xc0\xde'
        wrapper = struct.pack('<5I', 0x0b17c0de, 0, 20, len(bitcode), 0x0100000c)
        member = f"{'qcore.o/':<16}{0:<12}{0:<6}{0:<6}{100644:<8}{len(wrapper + bitcode):<10}`\n".encode()
        path.write_bytes(b'!<arch>\n' + member + wrapper + bitcode)
        self.assertEqual(self.validator.binary_architectures(path), {'arm64'})
        self.run_validator()
        path.write_bytes(struct.pack('<5I', 0x0b17c0de, 0, 20, 1000, 0x0100000c))
        self.run_validator('--kit-only', success=False)

    def test_unspecified_lto_cpu_uses_the_module_target_not_archive_neighbors(self):
        path = self.prefix / 'lib/libQt6Core.a'
        bitcode = b'BC\xc0\xde' + b'fixture payload'
        wrapper = struct.pack('<5I', 0x0b17c0de, 0, 20, len(bitcode), 0xffffffff)
        obj = struct.pack('<8I', 0xfeedfacf, 0x0100000c, 0, 1, 0, 0, 0, 0)

        def member(name, data):
            header = f"{name:<16}{0:<12}{0:<6}{0:<6}{100644:<8}{len(data):<10}`\n".encode()
            return header + data + (b'\n' if len(data) % 2 else b'')

        path.write_bytes(b'!<arch>\n' + member('native.o/', obj) +
                         member('lto.o/', wrapper + bitcode))
        for architecture in ('arm64', 'x64'):
            with self.subTest(architecture=architecture), mock.patch.object(
                    self.validator, 'darwin_lto_architecture', return_value=architecture) as reader:
                self.assertEqual(self.validator.binary_architectures(path),
                                 {'arm64', architecture})
                reader.assert_called_once_with(bitcode)
        with mock.patch.object(self.validator, 'darwin_lto_architecture',
                               return_value='unsupported'):
            with self.assertRaises(ValueError):
                self.validator.binary_architectures(path)
        with mock.patch.object(self.validator, 'darwin_lto_architecture',
                               side_effect=ValueError('invalid bitcode')):
            with self.assertRaisesRegex(ValueError, 'invalid bitcode'):
                self.validator.binary_architectures(path)

    def test_lto_target_triples_are_validated_and_modules_are_disposed(self):
        reader = mock.Mock()
        reader.lto_module_create_from_memory.return_value = 1
        triples = {b'arm64-apple-macosx14.4.0': 'arm64',
                   b'aarch64-apple-darwin': 'arm64',
                   b'x86_64-apple-macosx14.4.0': 'x64',
                   b'arm64-apple-ios18.0.0': 'unsupported',
                   b'x86_64-pc-linux-gnu': 'unsupported',
                   b'powerpc-apple-darwin': 'unsupported'}
        with mock.patch.object(self.validator, 'darwin_lto_reader', return_value=reader):
            for triple, architecture in triples.items():
                with self.subTest(triple=triple):
                    reader.reset_mock()
                    reader.lto_module_get_target_triple.return_value = triple
                    self.assertEqual(self.validator.darwin_lto_architecture(b'bitcode'),
                                     architecture)
                    reader.lto_module_dispose.assert_called_once_with(1)
            reader.lto_module_get_target_triple.return_value = None
            with self.assertRaisesRegex(ValueError, 'no target triple'):
                self.validator.darwin_lto_architecture(b'bitcode')
            reader.reset_mock()
            reader.lto_module_create_from_memory.return_value = None
            with self.assertRaisesRegex(ValueError, 'rejected'):
                self.validator.darwin_lto_architecture(b'bitcode')
            reader.lto_module_dispose.assert_not_called()

    @unittest.skipUnless(sys.platform == 'darwin', 'Darwin LTO requires the Apple toolchain')
    def test_native_apple_lto_objects_report_their_compiled_target(self):
        source = self.prefix / 'lto.c'
        source.write_text('int qt_lto_probe(void) { return 1; }\n')
        for target, architecture in (('arm64', 'arm64'), ('x86_64', 'x64')):
            with self.subTest(target=target):
                obj = self.prefix / (target + '.o')
                subprocess.run(['xcrun', 'clang', '-target', target + '-apple-macosx' +
                                QT_MACOS_DEPLOYMENT_TARGET, '-flto', '-c', str(source),
                                '-o', str(obj)], check=True, capture_output=True)
                self.assertEqual(self.validator.darwin_lto_architecture(obj.read_bytes()),
                                 architecture)
                self.assertEqual(self.validator.binary_architectures(obj), {architecture})

    def test_stamp_cannot_claim_a_different_source_archive(self):
        path = self.prefix / 'share/snow-apps/static-qt-build.json'
        stamp = json.loads(path.read_text())
        for field, value in (('SchemaVersion', 2), ('SourceArchiveSha256', '0' * 64),
                             ('Configuration', 'Debug'), ('Architecture', 'x64'),
                             ('DeploymentTarget', '14.0')):
            with self.subTest(field=field):
                path.write_text(json.dumps(dict(stamp, **{field: value})))
                result = self.run_validator(success=False)
                self.assertIn(f"Static Qt build stamp '{field}'", result.stderr)

    def test_existing_source_tree_must_match_the_toolchain_policy(self):
        source = Path(self.temp.name) / 'Qt source'
        metadata = source / 'qtbase/.cmake.conf'
        metadata.parent.mkdir(parents=True)
        metadata.write_text(f'set(QT_REPO_MODULE_VERSION "{QT_VERSION}")\n'
                            f'set(QT_SUPPORTED_MIN_MACOS_VERSION "{QT_MACOS_DEPLOYMENT_TARGET}")\n')
        self.run_validator('--source-dir', str(source))
        metadata.write_text('set(QT_REPO_MODULE_VERSION "6.11.1")\n')
        result = self.run_validator('--source-dir', str(source), success=False)
        self.assertIn(f'Qt sources must be qtbase {QT_VERSION}', result.stderr)

    def test_source_runtime_floor_cannot_exceed_the_audited_deployment_target(self):
        source = Path(self.temp.name) / 'Qt source'
        metadata = source / 'qtbase/.cmake.conf'
        metadata.parent.mkdir(parents=True)
        for minimum in ('14.0', QT_MACOS_DEPLOYMENT_TARGET, QT_MACOS_DEPLOYMENT_TARGET + '.0'):
            with self.subTest(minimum=minimum):
                metadata.write_text(f'set(QT_REPO_MODULE_VERSION "{QT_VERSION}")\n'
                                    f'set(QT_SUPPORTED_MIN_MACOS_VERSION "{minimum}")\n')
                self.run_validator('--source-dir', str(source))
        metadata.write_text(f'set(QT_REPO_MODULE_VERSION "{QT_VERSION}")\n'
                            'set(QT_SUPPORTED_MIN_MACOS_VERSION "15.0")\n')
        result = self.run_validator('--source-dir', str(source), success=False)
        self.assertIn('Qt sources require macOS 15.0 or newer', result.stderr)
        for declaration in ('', 'set(QT_SUPPORTED_MIN_MACOS_VERSION "unsupported")\n'):
            with self.subTest(declaration=declaration):
                metadata.write_text(f'set(QT_REPO_MODULE_VERSION "{QT_VERSION}")\n' + declaration)
                result = self.run_validator('--source-dir', str(source), success=False)
                self.assertIn('must declare their supported macOS runtime floor', result.stderr)

    def test_source_archive_must_match_its_pinned_digest_before_extraction(self):
        archive = Path(self.temp.name) / 'qt source archive.tar.xz'
        payload = b'Qt archive fixture' * 100000
        archive.write_bytes(payload)
        self.validator.validate_archive(archive, hashlib.sha256(payload).hexdigest())
        with self.assertRaisesRegex(ValueError, 'SHA-256 mismatch'):
            self.validator.validate_archive(archive, '0' * 64)
        result = self.run_validator('--source-archive', str(archive), success=False)
        self.assertIn('SHA-256 mismatch', result.stderr)


class QtMacOSDeploymentPolicy(unittest.TestCase):
    def test_shell_loader_exports_the_audited_qt_target(self):
        bash = bash_executable()
        if not bash:
            self.skipTest('Bash is required for the Qt deployment policy fixture')
        result = subprocess.run([
            bash, '-c',
            'snow_fixture_python="$2"; python3() { "$snow_fixture_python" "$@"; }; '
            'source "$1"; snow_load_qt_policy; '
            'printf "%s\\n" "$snow_qt_deployment_target" "$snow_qt_version" "$snow_qt_source_sha256"',
            'qt-policy-fixture', (ROOT / 'scripts/snow-build-environment.sh').as_posix(),
            Path(sys.executable).as_posix()], text=True, capture_output=True)
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(result.stdout.splitlines(),
                         [QT_MACOS_DEPLOYMENT_TARGET, QT_VERSION, QT_POLICY['sourceArchiveSha256']])


class QtLicenseMetadata(unittest.TestCase):
    """Test source-license copying on macOS, Linux, or Windows with Git Bash."""

    def setUp(self):
        self.bash = bash_executable()
        if not self.bash:
            self.skipTest('Bash is required for the source-license copying fixture')
        self.temporary = tempfile.TemporaryDirectory(prefix='snow Qt license metadata ')
        self.addCleanup(self.temporary.cleanup)
        self.root = Path(self.temporary.name)
        self.source = self.root / 'Qt source'
        self.destination = self.root / 'license bundle'
        for component in ('root', 'qtbase', 'qtsvg', 'qttools', 'qttranslations'):
            directory = self.source if component == 'root' else self.source / component
            (directory / 'LICENSES').mkdir(parents=True)
            (directory / 'LICENSES/GPL-3.0-only.txt').write_text('upstream license text\n')
            metadata = 'licenseRule.json' if component == 'qttranslations' else 'REUSE.toml'
            (directory / metadata).write_text(f'upstream {component} metadata\n')

    def copy(self):
        return subprocess.run([self.bash, '-c',
                               'source "$1"; snow_install_qt_license_metadata "$2" "$3"',
                               'qt-license-fixture',
                               (ROOT / 'scripts/snow-build-environment.sh').as_posix(),
                               self.source.as_posix(), self.destination.as_posix()],
                              text=True, capture_output=True)

    def test_translation_license_rules_and_license_texts_are_preserved(self):
        result = self.copy()
        self.assertEqual(result.returncode, 0, result.stderr)
        for source in self.source.rglob('*'):
            if source.is_file():
                relative = source.relative_to(self.source)
                if relative.parts[0] not in ('qtbase', 'qtsvg', 'qttools', 'qttranslations'):
                    relative = Path('root') / relative
                self.assertEqual((self.destination / relative).read_bytes(), source.read_bytes())
        self.assertFalse((self.destination / 'qttranslations/REUSE.toml').exists())

    def test_incomplete_source_metadata_is_rejected_before_copying(self):
        (self.source / 'qttranslations/licenseRule.json').unlink()
        result = self.copy()
        self.assertNotEqual(result.returncode, 0)
        self.assertIn('incomplete for qttranslations', result.stderr)
        self.assertFalse(self.destination.exists())


class MacOSBundleMetadata(unittest.TestCase):
    def test_native_deployment_fixture_has_no_mcp_bridge_requirement(self):
        fixture = (ROOT / 'test-support/macos-build/CMakeLists.txt').read_text()
        setup = fixture.split('if(NOT SNOW_FFMPEG_ROOT)', 1)[0]
        setup = '\n'.join(line for line in setup.splitlines()
                          if not line.startswith(('cmake_minimum_required(', 'project(')))
        deployment = (ROOT / 'cmake/DeploySnowShotMacOS.cmake.in').read_text()
        preconditions = deployment.split('set(_snow_codesign_identity ', 1)[0]
        managed_cmake = ROOT / '.tools/macos-dev/bin/cmake'
        cmake = str(managed_cmake) if managed_cmake.is_file() else shutil.which('cmake')
        self.assertIsNotNone(cmake)
        with tempfile.TemporaryDirectory(prefix='snow fixture deployment ') as directory:
            root = Path(directory)
            template = root / 'deploy.cmake.in'
            template.write_text(preconditions)
            script = root / 'check.cmake'
            script.write_text(
                'cmake_minimum_required(VERSION 4.2)\n' + setup + '\n'
                f'set(CMAKE_INSTALL_PREFIX [==[{root}]==])\n'
                f'configure_file([==[{template}]==] [==[{root / "deploy.cmake"}]==] @ONLY)\n'
                f'include([==[{root / "deploy.cmake"}]==])\n'
                'if(_snow_mcp_deploy_arguments)\n'
                '  message(FATAL_ERROR "The native fixture does not build an MCP bridge")\n'
                'endif()\n')
            result = subprocess.run([cmake, '-P', str(script)], text=True,
                                    capture_output=True, env=dict(os.environ, DESTDIR=''))
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)

    def test_mini_product_metadata_and_native_translations(self):
        resources = ROOT / 'snow_shot/packaging/macos'
        plist = plistlib.loads((resources / 'Info-mini.plist.in').read_bytes())
        self.assertEqual(plist['CFBundleName'], 'Snow Shot Mini')
        self.assertEqual(plist['CFBundleDisplayName'], 'Snow Shot Mini')
        self.assertEqual(plist['CFBundleIdentifier'], 'com.snowshot.snow_shot_mini')
        self.assertEqual(plist['CFBundleExecutable'], '${MACOSX_BUNDLE_EXECUTABLE_NAME}')
        for language in plist['CFBundleLocalizations']:
            strings = (resources / 'mini' / (language + '.lproj') / 'InfoPlist.strings').read_text(encoding='utf-8')
            self.assertIn('Snow Shot Mini', strings)
        instructions = (resources / 'dmg-mini-background.svg').read_text(encoding='utf-8')
        self.assertIn('Snow Shot Mini', instructions)
        self.assertIn('Snow Shot Mini.app', (resources / 'dmg-mini-layout.applescript').read_text(encoding='utf-8'))

    def test_dmg_instructions_cover_all_bundle_languages(self):
        resources = ROOT / 'snow_shot/packaging/macos'
        plist = plistlib.loads((resources / 'Info.plist.in').read_bytes())
        background = ET.parse(resources / 'dmg-background.svg').getroot()
        background_source = (resources / 'dmg-background.svg').read_text()
        package_source = (ROOT / 'cmake/SnowShotMacOSPackage.cmake').read_text()
        self.assertIn('@SNOW_DMG_WORDMARK@', background_source)
        self.assertNotIn('>Snow Shot</', background_source)
        self.assertIn('icons/resources/snow-shot-logo.svg', package_source)
        self.assertIn('configure_file(', package_source)
        language_key = '{http://www.w3.org/XML/1998/namespace}lang'
        groups = {element.attrib[language_key]: element
                  for element in background.iter('{http://www.w3.org/2000/svg}g')
                  if language_key in element.attrib}
        self.assertEqual(set(groups), set(plist['CFBundleLocalizations']))
        for language, group in groups.items():
            with self.subTest(language=language):
                lines = [element.text for element in group]
                self.assertEqual(len(lines), 2, 'Both install and launch instructions are required')
                self.assertTrue(all(line and 'Snow Shot' in line for line in lines))
        self.assertIn('应用程序', ''.join(groups['zh-Hans'].itertext()))
        self.assertIn('應用程式', ''.join(groups['zh-Hant'].itertext()))

    def test_product_metadata_and_native_translations(self):
        resources = ROOT / 'snow_shot/packaging/macos'
        plist = plistlib.loads((resources / 'Info.plist.in').read_bytes())
        self.assertEqual(plist['CFBundleInfoDictionaryVersion'], '6.0')
        self.assertEqual(plist['CFBundleName'], 'Snow Shot')
        self.assertEqual(plist['CFBundleDisplayName'], 'Snow Shot')
        self.assertEqual(plist['CFBundleIdentifier'], 'com.snowshot.snow_shot')
        self.assertEqual(plist['LSApplicationCategoryType'], 'public.app-category.productivity')
        self.assertIs(plist['LSUIElement'], True)
        self.assertEqual(plist['NSHumanReadableCopyright'], '${SNOW_SHOT_COPYRIGHT}')
        for language in plist['CFBundleLocalizations']:
            strings = (resources / (language + '.lproj') / 'InfoPlist.strings').read_text()
            for key in ('CFBundleName', 'CFBundleDisplayName', 'NSAppleEventsUsageDescription',
                        'NSMicrophoneUsageDescription', 'NSAudioCaptureUsageDescription'):
                self.assertIn('"' + key + '" = "', strings)
        main = (ROOT / 'snow_shot/src/app/main.cpp').read_text()
        self.assertIn('setApplicationDisplayName(', main)
        self.assertIn('QString applicationName = snow_shot::app::edition::applicationName()', main)
        edition = (ROOT / 'snow_shot/include/snow_shot/app/edition.h').read_text(encoding='utf-8')
        self.assertIn('QStringLiteral("snow_shot_mini") : QStringLiteral("snow_shot")', edition)

    def test_dmg_staging_preserves_bundle_contents(self):
        cmake = ROOT / '.tools/macos-dev/bin/cmake'
        cmake = str(cmake) if cmake.is_file() else shutil.which('cmake')
        self.assertIsNotNone(cmake)
        for target, product in [('snow_shot', 'Snow Shot'), ('snow_shot_mini', 'Snow Shot Mini')]:
            with self.subTest(product=product), tempfile.TemporaryDirectory(prefix='snow dmg staging ') as directory:
                stage = Path(directory)
                bundle = stage / (target + '.app')
                files = {'Contents/Info.plist': b'plist',
                         'Contents/MacOS/' + target: b'executable',
                         'Contents/_CodeSignature/CodeResources': b'signature'}
                for name, content in files.items():
                    path = bundle / name
                    path.parent.mkdir(parents=True, exist_ok=True)
                    path.write_bytes(content)
                binary = bundle / 'Contents/MacOS' / target
                binary.chmod(0o755)
                result = subprocess.run([cmake, '-DCPACK_TEMPORARY_DIRECTORY=' + str(stage),
                                         '-DCPACK_SNOW_SHOT_BUNDLE_NAME=' + target,
                                         '-DCPACK_SNOW_SHOT_PRODUCT_NAME=' + product,
                                         '-P', str(ROOT / 'cmake/PrepareSnowShotMacOSDmg.cmake')],
                                        text=True, capture_output=True)
                self.assertEqual(result.returncode, 0, result.stderr)
                self.assertFalse(bundle.exists())
                for name, content in files.items():
                    self.assertEqual((stage / (product + '.app') / name).read_bytes(), content)
                self.assertTrue(os.access(stage / (product + '.app') / 'Contents/MacOS' / target, os.X_OK))

    def test_mini_payload_contains_only_its_helpers_without_models(self):
        cmake = shutil.which('cmake')
        self.assertIsNotNone(cmake)
        manifest = ROOT / 'snow_shot/packaging/snow-shot-ocr-asset-manifest.json'
        model = next(item for item in json.loads(manifest.read_text())['models']
                     if item['type'] == 'small')
        with tempfile.TemporaryDirectory(prefix='snow mini payload ') as directory:
            bundle = Path(directory) / 'snow_shot_mini.app'
            runtime = bundle / 'Contents/MacOS'
            assets = bundle / 'Contents/Resources/assets'
            files = [runtime / name for name in ('snow_shot_mini', 'snow-shot-mini-mcp',
                                                'snow-ocr-process', 'crashpad_handler')]
            files.extend([assets / 'ocr/asset-manifest.json',
                          bundle / 'Contents/Resources/audios/camera_shutter.mp3',
                          bundle / 'Contents/Resources/snow-shot.icns'])
            files.extend(bundle / ('Contents/Resources/' + language + '.lproj/InfoPlist.strings')
                         for language in ('en', 'zh-Hans', 'zh-Hant'))
            files.extend(bundle / ('Contents/Resources/snow-shot-mini/licenses/' + relative)
                         for relative in ('LICENSE', 'components/snow_rust_ffi/COPYRIGHT',
                                          'third-party/qt/LICENSES/Qt-GPL-exception-1.0.txt',
                                          'third-party/vcpkg/zlib/copyright'))
            for path in files:
                path.parent.mkdir(parents=True, exist_ok=True)
                path.write_text('fixture')

            def verify(static=True):
                return subprocess.run([
                    cmake, '-DSNOW_SHOT_MINI_APP=' + str(bundle),
                    '-DSNOW_SHOT_MINI_STATIC=' + ('ON' if static else 'OFF'),
                    '-DSNOW_SHOT_MINI_MCP=ON', '-P',
                    str(ROOT / 'cmake/AssertSnowShotMiniMacOSPayload.cmake')],
                    text=True, capture_output=True)

            result = verify()
            self.assertEqual(result.returncode, 0, result.stderr)
            for relative in ('Contents/MacOS/snow_shot', 'Contents/MacOS/snow-shot-mcp',
                             'Contents/MacOS/libonnxruntime.dylib',
                             'Contents/Frameworks/Unused.framework/Unused',
                             'Contents/PlugIns/imageformats/unused.dylib',
                             'Contents/Resources/assets/qrcode/detect.prototxt',
                             'Contents/Resources/assets/ocr/models/' + model['id'] + '/' + model['files'][0]['name'],
                             'Contents/Resources/assets/ocr/models/unused/engine.onnx',
                             'Contents/Resources/assets/ocr/development-libraries.json',
                             'Contents/Resources/audios/unused.mp3',
                             'Contents/Resources/unused-model.zip',
                             'Contents/Resources/snow_shot_en_US.qm',
                             'Contents/Resources/en.lproj/snow_shot_en_US.qm',
                             'Contents/Resources/snow-shot/licenses/LICENSE',
                             'Contents/Resources/snow-shot-mini/unused-model.zip'):
                with self.subTest(relative=relative):
                    forbidden = bundle / relative
                    forbidden.parent.mkdir(parents=True, exist_ok=True)
                    forbidden.write_text('forbidden')
                    self.assertNotEqual(verify().returncode, 0)
                    forbidden.unlink()
                    parent = forbidden.parent
                    while parent != bundle:
                        try:
                            parent.rmdir()
                        except OSError:
                            break
                        parent = parent.parent
                    self.assertEqual(verify().returncode, 0)
            for relative in ('Contents/Resources/models', 'Contents/Resources/assets/ocr/models'):
                unused_directory = bundle / relative
                unused_directory.mkdir()
                self.assertNotEqual(verify().returncode, 0)
                unused_directory.rmdir()
            qt_config = bundle / 'Contents/Resources/qt.conf'
            qt_config.write_text('[Paths]\nPlugins = PlugIns\n')
            self.assertNotEqual(verify().returncode, 0)
            result = verify(static=False)
            self.assertEqual(result.returncode, 0, result.stderr)
            qt_config.unlink()
            self.assertEqual(verify().returncode, 0)
            files[4].unlink()
            self.assertNotEqual(verify().returncode, 0)

    def test_finder_automation_has_usage_description(self):
        plist = plistlib.loads((ROOT / 'snow_shot/packaging/macos/Info.plist.in').read_bytes())
        self.assertEqual(plist['NSAppleEventsUsageDescription'],
                         'Snow Shot reads selected image files from Finder to pin them to the screen.')

    def test_native_languages_match_the_application_catalogs(self):
        plist = plistlib.loads((ROOT / 'snow_shot/packaging/macos/Info.plist.in').read_bytes())
        native_languages = {'en_US': 'en', 'zh_CN': 'zh-Hans', 'zh_TW': 'zh-Hant'}
        catalog_languages = {ET.parse(path).getroot().attrib['language']
                             for path in (ROOT / 'snow_shot/i18n').rglob('*.ts')}
        self.assertEqual(set(plist['CFBundleLocalizations']),
                         {native_languages[language] for language in catalog_languages})
        self.assertEqual(plist['CFBundleDevelopmentRegion'], native_languages['en_US'])

    def test_pkg_config_apple_framework_options_are_removed_as_pairs(self):
        module = ROOT / 'cmake/SnowPkgConfigAppleFrameworks.cmake'
        managed_cmake = ROOT / '.tools/macos-dev/bin/cmake'
        cmake = str(managed_cmake) if managed_cmake.is_file() else shutil.which('cmake')
        self.assertIsNotNone(cmake, 'CMake is required for the framework option contract test')
        with tempfile.TemporaryDirectory(prefix='snow cmake test ') as directory:
            script = Path(directory) / 'test.cmake'
            script.write_text(f'''include([[{module.as_posix()}]])
snow_strip_pkg_config_apple_framework_options(result
    -pthread -framework VideoToolbox -framework CoreMedia -Wl,-dead_strip)
if(NOT result STREQUAL "-pthread;-Wl,-dead_strip")
    message(FATAL_ERROR "Unexpected sanitized options: ${{result}}")
endif()
''')
            result = subprocess.run([cmake, '-P', str(script)], text=True,
                                    capture_output=True)
            self.assertEqual(result.returncode, 0, result.stderr)


class MacOSRuntimeTestContracts(unittest.TestCase):
    def test_focused_runtime_targets_share_general_contracts_without_qttest(self):
        managed_cmake = ROOT / '.tools/macos-dev/bin/cmake'
        cmake = str(managed_cmake) if managed_cmake.is_file() else shutil.which('cmake')
        self.assertIsNotNone(cmake, 'CMake is required for the runtime test contract')
        helper = ROOT / 'cmake/SnowShotRuntimeTests.cmake'
        expected_targets = {'snow-shot-ocr-assets-tests',
                            'snow-shot-ocr-recognition-service-tests',
                            'snow-shot-diagnostics-crash-tests'}
        expected_tests = {'snow-shot-ocr-assets-tests', 'snow-shot-ocr-managed-runtime-tests',
                          'snow-shot-ocr-managed-directml-runtime-tests',
                          'snow-shot-ocr-cpu-recognition-service-tests',
                          'snow-shot-ocr-directml-recognition-service-tests',
                          'snow-shot-ocr-storage-relocation-tests',
                          'snow-shot-ocr-process-lifecycle-tests',
                          'snow-shot-diagnostics-crash-tests'}
        for platform, general, focused in (('macos', False, False), ('macos', False, True),
                                           ('macos', True, False), ('macos', True, True),
                                           ('windows', False, True), ('windows', True, False)):
            with self.subTest(platform=platform, general=general, focused=focused), \
                    tempfile.TemporaryDirectory(prefix='snow runtime contract ') as directory:
                root = Path(directory)
                records = root / 'commands.txt'
                script = root / 'test.cmake'
                # Run the actual shared declarations while recording target/test APIs.
                # No compiler, Qt package, worker, or dependency build is required.
                commands = ('enable_testing', 'target_include_directories',
                            'target_compile_definitions', 'target_link_libraries',
                            'set_tests_properties', 'set_property', 'add_dependencies',
                            'snow_add_qt_test_cli_guard', 'snow_add_rust_static_library')
                mocks = ''.join(f'''function({command})
    if("${{ARGN}}" MATCHES "Qt6::Test")
        message(FATAL_ERROR "Focused runtime validation must not require QtTest")
    endif()
    file(APPEND [[{records.as_posix()}]] "{command}:${{ARGN}}\\n")
endfunction()
''' for command in commands)
                script.write_text(f'''
set(APPLE {'ON' if platform == 'macos' else 'OFF'})
set(WIN32 {'ON' if platform == 'windows' else 'OFF'})
set(SNOW_WINDOWS_ARCHITECTURE x64)
set(SNOW_SHOT_BUILD_TESTS {'ON' if general else 'OFF'})
set(SNOW_SHOT_BUILD_RUNTIME_TESTS {'ON' if focused else 'OFF'})
set(SNOW_SHOT_OCR_STATIC_ONNXRUNTIME ON)
set(CMAKE_CURRENT_BINARY_DIR [[{root.as_posix()}]])
set(CMAKE_CURRENT_SOURCE_DIR [[{(ROOT / 'snow_shot').as_posix()}]])
{mocks}
function(add_executable name)
    if(NOT CMAKE_RUNTIME_OUTPUT_DIRECTORY STREQUAL "${{CMAKE_CURRENT_BINARY_DIR}}/test-bin")
        message(FATAL_ERROR "Runtime checks must retain their test-bin output directory")
    endif()
    file(APPEND [[{records.as_posix()}]] "target:${{name}}\\n")
endfunction()
function(add_test)
    cmake_parse_arguments(test "" "NAME" "COMMAND" ${{ARGN}})
    file(APPEND [[{records.as_posix()}]] "test:${{test_NAME}}\\n")
endfunction()
include([[{helper.as_posix()}]])
if(SNOW_SHOT_BUILD_TESTS)
    snow_shot_add_diagnostics_crash_tests()
    snow_shot_add_ocr_runtime_tests()
endif()
''')
                result = subprocess.run([cmake, '-P', str(script)], text=True,
                                        capture_output=True)
                self.assertEqual(result.returncode, 0, result.stderr)
                lines = records.read_text().splitlines() if records.exists() else []
                targets = [line.removeprefix('target:') for line in lines
                           if line.startswith('target:')]
                tests = [line.removeprefix('test:') for line in lines
                         if line.startswith('test:')]
                if not general and not focused:
                    self.assertEqual(targets, [])
                    self.assertEqual(tests, [])
                    continue
                windows_targets = {'snow-shot-ocr-assets-arm64-contract-tests'} \
                    if platform == 'windows' else set()
                windows_tests = windows_targets | {'snow-shot-ocr-crash-tests'} \
                    if platform == 'windows' else set()
                self.assertEqual(set(targets), expected_targets | windows_targets)
                self.assertEqual(len(targets), len(set(targets)))
                self.assertEqual(set(tests), expected_tests | windows_tests)
                self.assertEqual(len(tests), len(set(tests)))
                static_contract = ('target_compile_definitions:'
                                   'snow-shot-ocr-recognition-service-tests;PRIVATE;'
                                   'SNOW_SHOT_OCR_STATIC_ONNXRUNTIME=1')
                self.assertEqual(static_contract in lines, platform == 'macos')
                if platform == 'windows':
                    self.assertIn('set_tests_properties:snow-shot-ocr-crash-tests;PROPERTIES;'
                                  'TIMEOUT;60;LABELS;unit;windows', lines)
                else:
                    self.assertIn('set_tests_properties:snow-shot-ocr-managed-runtime-tests;'
                                  'PROPERTIES;LABELS;unit', lines)
                    self.assertIn('set_property:TARGET;snow-shot-diagnostics-crash-tests;'
                                  'PROPERTY;SNOW_RUST_BUNDLE_OVERRIDE;snow_diagnostics_test', lines)


class MacOSFFmpegBuild(unittest.TestCase):
    def run_build(self, architectures, fail_archive_probe=False, fail_build=False,
                  include_program=True):
        bash = bash_executable()
        if not bash:
            self.skipTest('Bash is required for the FFmpeg build fixture')
        with tempfile.TemporaryDirectory(prefix='snow_ffmpeg_build_') as temp:
            root = Path(temp)
            build, source, package, tools = (root / name for name in
                                            ('build', 'source', 'package', 'tools'))
            for directory in (build, source, tools):
                directory.mkdir()
            log = root / 'calls.jsonl'
            state = root / 'configure.json'
            command = '''#!/usr/bin/env python3
import json, os, pathlib, sys
name = pathlib.Path(sys.argv[0]).name
with open(os.environ['SNOW_TEST_LOG'], 'a') as output:
    output.write(json.dumps([name] + sys.argv[1:]) + '\\n')
state = pathlib.Path(os.environ['SNOW_TEST_STATE'])
if name == 'snow-configure':
    values = dict(argument[2:].split('=', 1) for argument in sys.argv[1:] if '=' in argument)
    state.write_text(json.dumps(values))
elif name == 'make':
    if sys.argv[1] in ('clean', 'distclean'):
        sys.exit(0)
    if os.environ.get('SNOW_TEST_FAIL_BUILD'):
        sys.exit(23)
    if sys.argv[1] == 'install':
        values = json.loads(state.read_text())
        prefix = pathlib.Path(values['prefix'])
        (prefix / 'lib/pkgconfig').mkdir(parents=True, exist_ok=True)
        (prefix / 'lib/libavutil.a').write_bytes(b'!<arch>\\n' + values['arch'].encode())
        if os.environ.get('SNOW_TEST_PROGRAM'):
            (prefix / 'bin').mkdir(exist_ok=True)
            (prefix / 'bin/ffprobe').write_bytes(bytes.fromhex('cffaedfe') + values['arch'].encode())
        (prefix / 'lib/pkgconfig/libavutil.pc').write_text('FFmpeg metadata')
elif name == 'lipo':
    if '-info' in sys.argv:
        binary = pathlib.Path(sys.argv[1])
        if binary.suffix == '.a' and os.environ.get('SNOW_TEST_FAIL_ARCHIVE_PROBE'):
            sys.exit(139)
        if binary.read_bytes().startswith((b'!<arch>\\n', bytes.fromhex('cffaedfe'))):
            sys.exit(0)
        sys.exit(1)
    destination = pathlib.Path(sys.argv[sys.argv.index('-output') + 1])
    inputs = [pathlib.Path(sys.argv[index + 2]) for index, argument in enumerate(sys.argv)
              if argument == '-arch']
    destination.parent.mkdir(parents=True, exist_ok=True)
    destination.write_bytes(b''.join(path.read_bytes() for path in inputs))
'''
            for name in ('make', 'lipo', 'snow-configure'):
                path = tools / name
                path.write_text(command)
                path.chmod(0o755)
            (source / 'configure').write_text('exec snow-configure "$@"\n')
            script = (ROOT / 'cmake/vcpkg-overlay-ports/ffmpeg/build.sh.in').read_text()
            values = {'BUILD_DIR': build, 'SOURCE_PATH': source, 'INST_PREFIX': package,
                      'VCPKG_CONCURRENCY': 2, 'OSX_ARCHS': ' '.join(architectures),
                      'OSX_ARCH_COUNT': len(architectures), 'VCPKG_CMAKE_SYSTEM_NAME': 'Darwin',
                      'CONFIGURE_OPTIONS': '', 'BUILD_ARCH': 'x86_64'}
            for name, value in values.items():
                script = script.replace('@' + name + '@', str(value))
            path = build / 'build.sh'
            path.write_text(script)
            env = dict(os.environ, PATH=str(tools) + os.pathsep + os.environ['PATH'],
                       SNOW_TEST_LOG=str(log), SNOW_TEST_STATE=str(state))
            if fail_archive_probe:
                env['SNOW_TEST_FAIL_ARCHIVE_PROBE'] = '1'
            if fail_build:
                env['SNOW_TEST_FAIL_BUILD'] = '1'
            if include_program:
                env['SNOW_TEST_PROGRAM'] = '1'
            result = subprocess.run([bash, str(path)], env=env, text=True, capture_output=True)
            calls = [json.loads(line) for line in log.read_text().splitlines()]
            outputs = {str(path.relative_to(package)): path.read_bytes()
                       for path in package.rglob('*') if path.is_file()}
            return result, calls, outputs, (build / 'stage').exists()

    def test_single_architecture_preserves_installed_outputs_without_lipo(self):
        for architecture in ('x86_64', 'arm64'):
            with self.subTest(architecture=architecture):
                result, calls, outputs, staged = self.run_build([architecture],
                                                              fail_archive_probe=True)
                self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
                self.assertFalse(any(call[0] == 'lipo' for call in calls))
                self.assertFalse(staged)
                self.assertEqual(outputs['lib/libavutil.a'], b'!<arch>\n' + architecture.encode())
                self.assertEqual(outputs['bin/ffprobe'],
                                 bytes.fromhex('cffaedfe') + architecture.encode())
                self.assertEqual(outputs['lib/pkgconfig/libavutil.pc'], b'FFmpeg metadata')
                configure = next(call for call in calls if call[0] == 'snow-configure')
                self.assertIn('--arch=' + architecture, configure)
                self.assertEqual(configure.count('--extra-cflags=' + architecture), 1)
                self.assertEqual(configure.count('--extra-ldflags=' + architecture), 1)

    def test_static_library_only_build_does_not_require_a_merge_staging_directory(self):
        result, calls, outputs, staged = self.run_build(['x86_64'], fail_archive_probe=True,
                                                      include_program=False)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertFalse(staged)
        self.assertFalse(any(call[0] == 'lipo' for call in calls))
        self.assertEqual(outputs['lib/libavutil.a'], b'!<arch>\nx86_64')
        self.assertNotIn('bin/ffprobe', outputs)

    def test_multiple_architectures_still_collect_and_merge_binaries(self):
        result, calls, outputs, staged = self.run_build(['x86_64', 'arm64'])
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertTrue(staged)
        self.assertEqual(outputs['lib/libavutil.a'], b'!<arch>\nx86_64!<arch>\narm64')
        merges = [call for call in calls if call[:2] == ['lipo', '-create']]
        self.assertEqual(len(merges), 2)
        self.assertTrue(all(call.count('-arch') == 2 for call in merges))
        self.assertEqual(outputs['lib/pkgconfig/libavutil.pc'], b'FFmpeg metadata')

    def test_single_architecture_build_failure_stops_before_installation(self):
        result, calls, outputs, staged = self.run_build(['x86_64'], fail_build=True)
        self.assertEqual(result.returncode, 23)
        self.assertFalse(any(call[:2] == ['make', 'install'] for call in calls))
        self.assertFalse(any(call[0] == 'lipo' for call in calls))
        self.assertFalse(outputs)
        self.assertFalse(staged)


class MacOSBuildScripts(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix="snow build tests ")
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        shutil.copytree(ROOT / "scripts", self.root / "scripts")
        self.bin = self.root / ".tools/macos-dev/bin"
        self.bin.mkdir(parents=True)
        self.log = self.root / "commands.jsonl"
        # A mock command logs its argv as JSON, preserving spaces and boundaries.
        mock = """#!/usr/bin/env python3
import json, os, pathlib, sys
name = pathlib.Path(sys.argv[0]).name
with open(os.environ['SNOW_TEST_LOG'], 'a') as log:
    log.write(json.dumps([name] + sys.argv[1:]) + '\\n')
if name == 'uname': print('Darwin' if sys.argv[1] == '-s' else os.environ.get('SNOW_TEST_HOST_ARCH', 'arm64'))
if name == 'sysctl': print(os.environ.get('SNOW_TEST_PHYSICAL_ARM64', '1' if os.environ.get('SNOW_TEST_HOST_ARCH', 'arm64') == 'arm64' else '0'))
if name == 'arch' and os.environ.get('FAIL_ROSETTA'): sys.exit(9)
if name == 'configure': sys.exit(17)
if name == 'xcode-select': print('/mock Xcode')
if name == 'git' and 'rev-parse' in sys.argv: print('4497409a47f19db373a410a0efb84eca4747adbf')
if name == 'cmake' and '--version' in sys.argv: print('cmake version 4.4.3')
if name == 'cmake' and '--preset' in sys.argv and os.environ.get('FAIL_CONFIGURE'): sys.exit(17)
if name == 'cmake' and '--preset' in sys.argv and '--build' not in sys.argv:
    preset = sys.argv[sys.argv.index('--preset') + 1]
    root = pathlib.Path(os.environ['SNOW_TEST_ROOT'])
    build = root / 'build' / preset
    build.mkdir(parents=True, exist_ok=True)
    static = preset.endswith(('-release', '-fast'))
    arch = 'arm64' if '-arm64-' in preset else 'x64'
    triplet = f'{arch}-osx-snow-shot' + ('-static' if static else '')
    installed = root / '.tools/macos/installed' / ('static' if static else 'dynamic')
    entries = [
        f'Qt6_DIR:UNINITIALIZED={os.environ["Qt6_DIR"]}',
        f'VCPKG_TARGET_TRIPLET:STRING={triplet}',
        f'VCPKG_INSTALLED_DIR:PATH={installed}',
        f'CMAKE_HOME_DIRECTORY:INTERNAL={root}', 'CMAKE_GENERATOR:INTERNAL=Ninja',
    ]
    mini = 'ON' if arch == 'arm64' else 'OFF'
    for argument in sys.argv:
        if argument.startswith('-DSNOW_APPS_BUILD_SNOW_SHOT_MINI='):
            mini = argument.split('=', 1)[1]
    entries.append(f'SNOW_APPS_BUILD_SNOW_SHOT_MINI:BOOL={mini}')
    if preset.endswith('-release'):
        entries += ['CMAKE_BUILD_TYPE:STRING=Release', 'SNOW_APPS_BUILD_TESTS:BOOL=OFF',
                    'SNOW_APPS_BUILD_BENCHMARKS:BOOL=OFF', 'SNOW_APPS_RELEASE_STATIC:BOOL=ON',
                    'SNOW_APPS_QT_STATIC:BOOL=ON', 'SNOW_APPS_PACKAGE_SNOW_SHOT:BOOL=ON',
                    'SNOW_SHOT_IMAGE_CODEC_BACKEND_STATIC:INTERNAL=ON',
                    'QT_FEATURE_static:INTERNAL=ON']
    (build / 'CMakeCache.txt').write_text('\\n'.join(entries) + '\\n')
if name == 'cmake' and '--build' in sys.argv:
    preset = sys.argv[sys.argv.index('--preset') + 1].removeprefix('build-')
    for target in ('snow_shot', 'snow_shot_mini'):
        if target in sys.argv:
            binary = pathlib.Path(os.environ['SNOW_TEST_ROOT']) / 'build' / preset / target / f'{target}.app/Contents/MacOS/{target}'
            binary.parent.mkdir(parents=True, exist_ok=True)
            binary.touch()
            binary.chmod(0o755)
if name == 'cmake' and '--install' in sys.argv and '--prefix' in sys.argv:
    prefix = pathlib.Path(sys.argv[sys.argv.index('--prefix') + 1])
    component = sys.argv[sys.argv.index('--component') + 1]
    target = 'snow_shot_mini' if component == 'SnowShotMini' else 'snow_shot'
    binary = prefix / f'{target}.app/Contents/MacOS/{target}'
    binary.parent.mkdir(parents=True, exist_ok=True)
    binary.touch()
    binary.chmod(0o755)
if name == 'ps' and os.environ.get('SNOW_TEST_PS_OUTPUT'): print(os.environ['SNOW_TEST_PS_OUTPUT'])
if name == 'security' and 'import' in sys.argv:
    (pathlib.Path(os.environ['SNOW_TEST_ROOT']) / '.codesign-identity').touch()
if name == 'security' and 'find-identity' in sys.argv:
    identity = os.environ.get('SNOW_TEST_CODESIGN_IDENTITY', 'Snow Shot Development (Local)')
    if not identity and (pathlib.Path(os.environ['SNOW_TEST_ROOT']) /
                         '.codesign-identity').exists():
        identity = 'Snow Shot Development (Local)'
    if identity:
        print(f'  1) 0123456789ABCDEF0123456789ABCDEF01234567 "{identity}" '
              '(CSSMERR_TP_NOT_TRUSTED)')
if name == 'openssl':
    for option in ('-keyout', '-out'):
        if option in sys.argv:
            pathlib.Path(sys.argv[sys.argv.index(option) + 1]).touch()
"""
        tools = ("cmake", "cpack", "ninja", "cargo", "rustup", "pkg-config", "uname",
                 "open", "ps", "xcode-select", "xcrun", "git", "lsregister", "security",
                 "openssl", "arch", "sysctl")
        for name in tools:
            path = self.bin / name
            path.write_text(mock)
            path.chmod(0o755)
        vcpkg = self.root / ".tools/vcpkg"
        vcpkg.mkdir()
        (vcpkg / ".git").mkdir()
        (vcpkg / "bootstrap-vcpkg.sh").touch()
        shutil.copyfile(self.bin / "git", vcpkg / "vcpkg")
        (vcpkg / "vcpkg").chmod(0o755)
        qt = self.root / "Qt kit/lib/cmake/Qt6"
        qt.mkdir(parents=True)
        (qt / "Qt6Config.cmake").touch()
        stamp = self.root / "Qt kit/share/snow-apps/static-qt-build.json"
        stamp.parent.mkdir(parents=True)
        stamp.write_text(json.dumps({"SchemaVersion": 3, "QtVersion": QT_VERSION,
                                     "SourceArchiveSha256": QT_POLICY['sourceArchiveSha256'],
                                     "Architecture": "arm64", "Configuration": "Release",
                                     "DeploymentTarget": QT_MACOS_DEPLOYMENT_TARGET,
                                     "Dup3": False,
                                     "Ltcg": True, "SystemPng": True, "SystemZlib": True,
                                     "Timezone": True, "TimezoneLocale": False,
                                     "FeatureFingerprint": static_qt_feature_fingerprint()}))
        write_static_qt_feature_targets(self.root / 'Qt kit')
        (self.root / "Qt kit/share/snow-apps/qt-licenses").mkdir()
        self.env = dict(os.environ, PATH=f"{self.bin}:{os.environ['PATH']}",
                        Qt6_DIR=str(qt), SNOW_QT_STATIC_DIR=str(qt),
                        SNOW_TEST_LOG=str(self.log),
                        SNOW_TEST_ROOT=str(self.root),
                        SNOW_LAUNCH_SERVICES_REGISTER=str(self.bin / 'lsregister'))

    def run_script(self, script, *args, success=True):
        result = subprocess.run(["/bin/bash", str(self.root / "scripts" / script), *args],
                                env=self.env, text=True, capture_output=True, cwd="/")
        self.last_result = result
        if success:
            self.assertEqual(result.returncode, 0, result.stderr)
        else:
            self.assertNotEqual(result.returncode, 0)
        return [] if not self.log.exists() else [json.loads(line) for line in self.log.read_text().splitlines()]

    def test_build_preserves_arguments_and_clean_target(self):
        calls = self.run_script("build.sh", "snow-shot-macos-x64-debug", "--target", "some-test",
                                "--clean", "--", "-DEXAMPLE=a path with spaces")
        configure, build = [c for c in calls if c[0] == "cmake" and '--version' not in c]
        self.assertIn("-DEXAMPLE=a path with spaces", configure)
        self.assertNotIn('-DQT_NO_HANDLE_APPLE_SINGLE_ARCH_CROSS_COMPILING=ON', configure)
        self.assertEqual(build, ["cmake", "--build", "--preset", "build-snow-shot-macos-x64-debug",
                                 "--target", "some-test", "--parallel"])

    def test_default_build_and_empty_array_on_system_bash(self):
        calls = self.run_script("build.sh")
        self.assertIn(["cmake", "--build", "--preset", "build-snow-shot-macos-arm64-debug",
                       "--target", "snow_shot", "snow_shot_mini", "--parallel"], calls)

    def test_default_build_respects_disabled_mini(self):
        calls = self.run_script("build.sh", "--", "-DSNOW_APPS_BUILD_SNOW_SHOT_MINI=OFF")
        self.assertIn(["cmake", "--build", "--preset", "build-snow-shot-macos-arm64-debug",
                       "--target", "snow_shot", "--parallel"], calls)
        self.assertFalse(any('snow_shot_mini' in call for call in calls))

    def test_stale_cache_is_reconfigured_from_fresh_state(self):
        cache = self.root / 'build/snow-shot-macos-arm64-debug/CMakeCache.txt'
        cache.parent.mkdir(parents=True)
        cache.write_text('Qt6_DIR:PATH=/stale/qt\n')
        calls = self.run_script('build.sh', '--skip-bootstrap')
        configure = next(call for call in calls if call[0] == 'cmake' and '--preset' in call
                         and '--build' not in call)
        self.assertEqual(configure[1], '--fresh')
        self.assertIn('configuring from a fresh cache', self.last_result.stdout)

    def test_configure_failure_stops_build(self):
        self.env['FAIL_CONFIGURE'] = '1'
        calls = self.run_script("build.sh", success=False)
        self.assertFalse(any('--build' in c for c in calls))

    def test_invalid_arguments_do_not_configure(self):
        for args in (("windows-msvc-debug",), ("snow-shot-macos-arm64-other-debug",),
                     ("--target",), ("--unknown",)):
            calls = self.run_script("build.sh", *args, success=False)
            self.assertFalse(any(c[0] == 'cmake' for c in calls))

    def test_bootstrap_selects_matching_rust_target(self):
        stamp = self.root / "Qt kit/share/snow-apps/static-qt-build.json"
        value = json.loads(stamp.read_text())
        value['Architecture'] = 'x64'
        stamp.write_text(json.dumps(value))
        calls = self.run_script("bootstrap-macos.sh", "snow-shot-macos-x64-release")
        rustup = next(c for c in calls if c[:3] == ['rustup', 'toolchain', 'install'])
        self.assertIn('x86_64-apple-darwin', rustup)

    def test_x64_build_requires_rosetta_before_configuring(self):
        self.env['FAIL_ROSETTA'] = '1'
        calls = self.run_script('build.sh', 'snow-shot-macos-x64-debug', success=False)
        self.assertIn(['arch', '-x86_64', '/usr/bin/true'], calls)
        self.assertFalse(any(call[0] in ('cmake', 'rustup') for call in calls))
        self.assertIn('softwareupdate --install-rosetta --agree-to-license',
                      self.last_result.stderr)

    def test_skip_qt_bootstrap_still_requires_rosetta_before_installing(self):
        self.env['FAIL_ROSETTA'] = '1'
        calls = self.run_script('bootstrap-macos.sh', 'snow-shot-macos-x64-release',
                                '--skip-qt-validation', success=False)
        self.assertIn(['arch', '-x86_64', '/usr/bin/true'], calls)
        self.assertFalse(any(call[0] in ('cmake', 'rustup', 'vcpkg') for call in calls))

    def test_native_builds_do_not_require_rosetta(self):
        self.env['FAIL_ROSETTA'] = '1'
        for host, target in (('arm64', 'arm64'), ('x86_64', 'x64')):
            with self.subTest(host=host, target=target):
                self.env['SNOW_TEST_HOST_ARCH'] = host
                calls = self.run_script('build.sh', f'snow-shot-macos-{target}-debug')
                self.assertFalse(any(call[0] == 'arch' for call in calls))
                self.assertTrue(any('--build' in call for call in calls))
                self.log.unlink()

    def static_qt_configure_arguments(self, target, host):
        self.env['SNOW_TEST_HOST_ARCH'] = host
        dependencies = self.root / 'static Qt dependencies'
        for relative in ('include/zlib.h', 'include/png.h', 'share/zlib/vcpkg_abi_info.txt',
                         'share/libpng/vcpkg_abi_info.txt'):
            path = dependencies / relative
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_text('dependency fixture\n')
        source = self.root / 'Qt source'
        metadata = source / 'qtbase/.cmake.conf'
        metadata.parent.mkdir(parents=True, exist_ok=True)
        metadata.write_text(f'set(QT_REPO_MODULE_VERSION "{QT_VERSION}")\n'
                            f'set(QT_SUPPORTED_MIN_MACOS_VERSION "{QT_MACOS_DEPLOYMENT_TARGET}")\n')
        configure = source / 'configure'
        shutil.copyfile(self.bin / 'cmake', configure)
        configure.chmod(0o755)
        build = self.root / 'Qt build'
        build.mkdir(exist_ok=True)
        (build / 'CMakeCache.txt').write_text('CMAKE_SYSTEM_NAME:STRING=Darwin\n'
                                           'QT_FEATURE_cross_compile:INTERNAL=ON\n')
        if self.log.exists():
            self.log.unlink()
        calls = self.run_script('build-static-qt.sh', '--arch', target,
                                '--install-prefix', str(self.root / 'static Qt install'),
                                '--dependency-prefix', str(dependencies),
                                '--source-dir', str(source), '--build-dir', str(build),
                                success=False)
        self.assertEqual(self.last_result.returncode, 17, self.last_result.stderr)
        return calls

    def test_static_x64_qt_uses_rosetta_tools_with_a_fresh_cache(self):
        calls = self.static_qt_configure_arguments('x64', 'arm64')
        configure = next(call for call in calls if call[0] == 'configure')
        self.assertIn(['arch', '-x86_64', '/usr/bin/true'], calls)
        self.assertIn('-DCMAKE_OSX_ARCHITECTURES=x86_64', configure)
        self.assertIn('--fresh', configure[configure.index('--') + 1:])
        self.assertIn('-DQT_NO_HANDLE_APPLE_SINGLE_ARCH_CROSS_COMPILING=ON', configure)

    def test_rosetta_shell_still_handles_native_cmake_building_x64_qt(self):
        self.env['SNOW_TEST_PHYSICAL_ARM64'] = '1'
        calls = self.static_qt_configure_arguments('x64', 'x86_64')
        configure = next(call for call in calls if call[0] == 'configure')
        self.assertIn(['arch', '-x86_64', '/usr/bin/true'], calls)
        self.assertIn('-DQT_NO_HANDLE_APPLE_SINGLE_ARCH_CROSS_COMPILING=ON', configure)

    def test_static_native_qt_keeps_normal_tool_detection(self):
        for target, host in (('arm64', 'arm64'), ('x64', 'x86_64')):
            with self.subTest(target=target, host=host):
                calls = self.static_qt_configure_arguments(target, host)
                configure = next(call for call in calls if call[0] == 'configure')
                self.assertFalse(any(call[0] == 'arch' for call in calls))
                self.assertNotIn('-DQT_NO_HANDLE_APPLE_SINGLE_ARCH_CROSS_COMPILING=ON',
                                 configure)

    def test_static_x64_qt_rejects_missing_rosetta_before_configure(self):
        self.env['FAIL_ROSETTA'] = '1'
        calls = self.run_script('build-static-qt.sh', '--arch', 'x64',
                                '--install-prefix', str(self.root / 'static Qt install'),
                                success=False)
        self.assertIn(['arch', '-x86_64', '/usr/bin/true'], calls)
        self.assertFalse(any(call[0] in ('configure', 'cmake') for call in calls))

    def test_release_build_prefers_static_qt_over_shared_qt_dir(self):
        static_qt = self.env['Qt6_DIR']
        shared_qt = self.root / 'shared Qt/lib/cmake/Qt6'
        shared_qt.mkdir(parents=True)
        (shared_qt / 'Qt6Config.cmake').touch()
        self.env['Qt6_DIR'] = str(shared_qt)
        self.env['SNOW_QT_STATIC_DIR'] = static_qt
        calls = self.run_script('build.sh', 'snow-shot-macos-arm64-release',
                                '--skip-bootstrap')
        configure = next(call for call in calls if call[0] == 'cmake' and '--preset' in call
                         and '--build' not in call)
        self.assertIn(f'Qt6_DIR={static_qt}', configure)

    def test_package_builds_before_cpack(self):
        stale = self.root / 'build/snow-shot-macos-arm64-release/stale-bundle-file'
        stale.parent.mkdir(parents=True)
        stale.touch()
        calls = self.run_script("package-snow-shot.sh")
        build = next(i for i, c in enumerate(calls) if '--build' in c)
        pack = next(i for i, c in enumerate(calls) if c[0] == 'cpack')
        self.assertLess(build, pack)
        symbols = next(i for i, c in enumerate(calls)
                       if any(arg.endswith('GenerateSnowShotDiagnosticsSymbols-Release.cmake') for arg in c))
        self.assertLess(build, symbols)
        self.assertLess(symbols, pack)
        self.assertEqual(calls[pack], ['cpack', '--preset', 'package-snow-shot-macos-arm64-release'])
        self.assertTrue(any(call[0] == 'cpack' and
                            any(arg.endswith('CPackSnowShotMiniConfig.cmake') for arg in call)
                            for call in calls))
        self.assertFalse(stale.exists())

    def test_arm_release_rejects_disabled_mini_before_packaging(self):
        self.run_script("package-snow-shot.sh")
        cache = self.root / 'build/snow-shot-macos-arm64-release/CMakeCache.txt'
        cache.write_text(cache.read_text().replace('SNOW_APPS_BUILD_SNOW_SHOT_MINI:BOOL=ON',
                                                  'SNOW_APPS_BUILD_SNOW_SHOT_MINI:BOOL=OFF'))
        self.log.unlink()
        calls = self.run_script("package-snow-shot.sh", "--skip-build", success=False)
        self.assertFalse(any(call[0] == 'cpack' for call in calls))
        self.assertIn('Coordinated ARM64 packaging requires', self.last_result.stderr)

    def test_skip_build_packages_existing_symbols_without_rebuilding(self):
        # Provision the fixture's release cache, then package it without a build.
        self.run_script("package-snow-shot.sh")
        self.log.unlink()
        calls = self.run_script("package-snow-shot.sh", "--skip-build")
        self.assertFalse(any('--build' in call for call in calls))
        self.assertTrue(any(any(arg.endswith('GenerateSnowShotMiniDiagnosticsSymbols-Release.cmake')
                               for arg in call) for call in calls))
        self.assertTrue(any(any(arg.endswith('GenerateSnowShotDiagnosticsSymbols-Release.cmake')
                                    for arg in call) for call in calls))

    def test_arm_assembler_objects_keep_the_macos_deployment_target(self):
        x264 = (ROOT / 'cmake/vcpkg-overlay-ports/x264/portfile.cmake').read_text()
        x265 = (ROOT / 'cmake/vcpkg-overlay-ports/x265/portfile.cmake').read_text()
        x265_patch = (ROOT / 'cmake/vcpkg-overlay-ports/x265/'
                      'macos-arm64-deployment-target.patch').read_text()
        self.assertIn('--extra-asflags=-mmacosx-version-min=', x264)
        self.assertIn('macos-arm64-deployment-target.patch', x265)
        self.assertIn('-mmacosx-version-min=${CMAKE_OSX_DEPLOYMENT_TARGET}', x265_patch)

    def test_package_rejects_nonrelease(self):
        calls = self.run_script("package-snow-shot.sh", "snow-shot-macos-arm64-debug", success=False)
        self.assertFalse(any(c[0] in ('cmake', 'cpack') for c in calls))

    def test_release_presets_use_static_qt_and_isolated_dependencies(self):
        presets = json.loads((ROOT / 'CMakePresets.json').read_text())['configurePresets']
        by_name = {preset['name']: preset for preset in presets}
        release_base = by_name['macos-release-base']['cacheVariables']
        self.assertEqual(by_name['macos-base']['cacheVariables']
                         ['SNOW_MACOS_CODESIGN_IDENTITY'], 'AUTO')
        self.assertEqual(release_base['SNOW_MACOS_CODESIGN_IDENTITY'], '-')
        self.assertEqual(release_base['SNOW_APPS_RELEASE_STATIC'], 'ON')
        self.assertEqual(release_base['SNOW_APPS_QT_STATIC'], 'ON')
        self.assertTrue(release_base['VCPKG_INSTALLED_DIR'].endswith('/static'))
        for arch in ('arm64', 'x64'):
            release = by_name[f'snow-shot-macos-{arch}-release']
            fast = by_name[f'snow-shot-macos-{arch}-fast']
            self.assertEqual(release['inherits'], 'macos-release-base')
            self.assertEqual(fast['inherits'], 'macos-release-base')
            self.assertEqual(release['cacheVariables']['VCPKG_TARGET_TRIPLET'],
                             f'{arch}-osx-snow-shot-static')

    def test_static_package_contract_avoids_dynamic_deployment(self):
        macos = (ROOT / 'cmake/SnowShotMacOS.cmake').read_text()
        deployment = (ROOT / 'cmake/DeploySnowShotMacOS.cmake.in').read_text()
        self.assertIn('Qt6::QCocoaIntegrationPlugin',
                      (ROOT / 'snow_shot/CMakeLists.txt').read_text())
        self.assertIn('if(NOT SNOW_SHOT_QT_STATIC)', macos)
        self.assertIn('if(NOT @SNOW_SHOT_QT_STATIC@)', deployment)
        self.assertIn('Static package contains a non-system dependency', deployment)
        self.assertIn('--static-runtime', deployment)

    def test_development_builds_provision_and_sign_a_stable_debug_identity(self):
        macos = (ROOT / 'cmake/SnowShotMacOS.cmake').read_text()
        self.assertIn('SNOW_MACOS_CODESIGN_IDENTITY STREQUAL "AUTO"', macos)
        self.assertIn('ensure-macos-codesign-identity.sh', macos)
        self.assertIn('--identifier com.snowshot.snow_shot', macos)
        self.assertIn('$<TARGET_FILE:snow_shot>', macos)
        self.assertIn('$<TARGET_FILE:snow_shot>.snow-signing', macos)

    def test_static_qt_builder_reuses_an_audited_matching_installation(self):
        dependencies = self.root / 'dependencies'
        abi_files = [dependencies / 'share/zlib/vcpkg_abi_info.txt',
                     dependencies / 'share/libpng/vcpkg_abi_info.txt']
        for path in abi_files:
            path.parent.mkdir(parents=True)
            path.write_text(path.parent.name)
        for name in ('include/zlib.h', 'include/png.h'):
            path = dependencies / name
            path.parent.mkdir(parents=True, exist_ok=True)
            path.touch()
        abi_files = [path.resolve() for path in abi_files]
        lines = ''.join(f'{hashlib.sha256(path.read_bytes()).hexdigest()}  {path}\n'
                        for path in abi_files)
        fingerprint = hashlib.sha256(lines.encode()).hexdigest()
        prefix = self.root / 'static Qt'
        config = prefix / 'lib/cmake/Qt6/Qt6Config.cmake'
        config.parent.mkdir(parents=True)
        config.touch()
        (prefix / 'share/snow-apps/qt-licenses').mkdir(parents=True)
        stamp = prefix / 'share/snow-apps/static-qt-build.json'
        stamp.write_text(json.dumps({
            'SchemaVersion': 3, 'QtVersion': QT_VERSION, 'Architecture': 'arm64',
            'SourceArchiveSha256': QT_POLICY['sourceArchiveSha256'],
            'Configuration': 'Release', 'DeploymentTarget': QT_MACOS_DEPLOYMENT_TARGET,
            'Dup3': False,
            'DependencyFingerprint': fingerprint,
            'Ltcg': True, 'SystemPng': True, 'SystemZlib': True,
            'Timezone': True, 'TimezoneLocale': False,
            'FeatureFingerprint': static_qt_feature_fingerprint(),
        }, indent=2))
        write_static_qt_feature_targets(prefix)
        calls = self.run_script('build-static-qt.sh', '--install-prefix', str(prefix),
                                '--dependency-prefix', str(dependencies))
        self.assertFalse(any(call[0] == 'cmake' for call in calls))
        self.assertIn(f'Validated static Qt {QT_VERSION} (arm64)', self.last_result.stdout)

    def test_release_build_rejects_stale_or_untrimmed_static_qt_stamps(self):
        stamp = self.root / 'Qt kit/share/snow-apps/static-qt-build.json'
        original = json.loads(stamp.read_text())
        for name, value in (('SchemaVersion', 2), ('QtVersion', '6.11.1'),
                            ('DeploymentTarget', '14.0'),
                            ('SourceArchiveSha256', 'stale-source'), ('Timezone', False),
                            ('TimezoneLocale', True), ('TimezoneLocale', 'false'),
                            ('FeatureFingerprint', 'stale-policy')):
            with self.subTest(field=name, value=value):
                stamp.write_text(json.dumps(dict(original, **{name: value})))
                if self.log.exists():
                    self.log.unlink()
                calls = self.run_script('build.sh', 'snow-shot-macos-arm64-release',
                                        '--skip-bootstrap', success=False)
                self.assertFalse(any('--preset' in call or '--build' in call for call in calls))
                self.assertIn('Invalid static Qt kit', self.last_result.stderr)

    def test_release_build_verifies_installed_qt_features_independently_of_stamp(self):
        targets = self.root / 'Qt kit/lib/cmake/Qt6Core/Qt6CoreTargets.cmake'
        original = targets.read_text()
        for text in (original.replace('timezone;static', 'static'),
                     original.replace('ltcg;system_zlib', 'ltcg;system_zlib;timezone_locale'),
                     original.replace('system_zlib', 'system_zlib_extra')):
            with self.subTest(targets=text):
                targets.write_text(text)
                if self.log.exists():
                    self.log.unlink()
                calls = self.run_script('build.sh', 'snow-shot-macos-arm64-release',
                                        '--skip-bootstrap', success=False)
                self.assertFalse(any('--preset' in call or '--build' in call for call in calls))
                self.assertIn('The installed Qt targets', self.last_result.stderr)

    def test_static_qt_builder_supports_command_line_tools_without_full_xcode(self):
        builder = (ROOT / 'scripts/build-static-qt.sh').read_text()
        self.assertIn('if ! xcodebuild -version >/dev/null 2>&1; then', builder)
        self.assertIn('xcrun --show-sdk-path', builder)
        self.assertIn('qt_apple_options+=(-DQT_NO_XCODE_MIN_VERSION_CHECK=ON)', builder)
        self.assertIn('"${qt_apple_options[@]}"', builder)
        self.assertIn('qt_deployment_target="$snow_qt_deployment_target"', builder)
        self.assertIn('-DCMAKE_OSX_DEPLOYMENT_TARGET="$qt_deployment_target"', builder)
        self.assertIn('-DFEATURE_dup3=OFF', builder)
        self.assertIn("'FEATURE_dup3:BOOL=OFF' 'QT_FEATURE_dup3:INTERNAL=OFF'", builder)

    def test_launch_bundle_with_arguments(self):
        app = self.root / 'build/snow-shot-macos-arm64-debug/snow_shot/snow_shot.app'
        binary = app / 'Contents/MacOS/snow_shot'
        binary.parent.mkdir(parents=True)
        binary.touch()
        binary.chmod(0o755)
        deployed = app.parent.parent / 'run/snow_shot.app'
        deployed.mkdir(parents=True)
        calls = self.run_script('run-snow-shot.sh', '--', '--example', 'a path')
        self.assertIn(['cmake', '--build', '--preset', 'build-snow-shot-macos-arm64-debug',
                       '--target', 'snow_shot', '--parallel'], calls)
        self.assertEqual(calls[-1], ['open', '-n', str(deployed), '--args', '--example', 'a path'])
        self.assertIn('--install', calls[-3])
        self.assertEqual(calls[-2], ['lsregister', '-f', str(deployed)])

    def test_launch_mini_builds_and_deploys_the_selected_edition(self):
        deployed = self.root / 'build/snow-shot-macos-arm64-debug/run/snow_shot_mini.app'
        calls = self.run_script('run-snow-shot.sh', '--edition', 'mini', '--', '--example', 'a path')
        self.assertIn(['cmake', '--build', '--preset', 'build-snow-shot-macos-arm64-debug',
                       '--target', 'snow_shot_mini', '--parallel'], calls)
        self.assertIn(['cmake', '--install', str(deployed.parent.parent), '--component',
                       'SnowShotMini', '--prefix', str(deployed.parent)], calls)
        self.assertEqual(calls[-2], ['lsregister', '-f', str(deployed)])
        self.assertEqual(calls[-1], ['open', '-n', str(deployed), '--args', '--example', 'a path'])
        self.log.unlink()
        calls = self.run_script('run-snow-shot.sh', '--edition', 'mini', '--no-build')
        self.assertFalse(any(call[0] == 'cmake' for call in calls))
        self.assertEqual(calls[-1], ['open', '-n', str(deployed), '--args'])

    def test_launch_stops_selected_build_instances_before_rebuilding(self):
        app = self.root / 'build/snow-shot-macos-arm64-debug/snow_shot/snow_shot.app'
        binary = app / 'Contents/MacOS/snow_shot'
        binary.parent.mkdir(parents=True)
        binary.touch()
        binary.chmod(0o755)
        running = app.parent.parent / 'run/snow_shot.app/Contents/MacOS/snow_shot'
        running.parent.mkdir(parents=True)
        unrelated = self.root / 'other/snow_shot'
        process = subprocess.Popen(['/bin/sleep', '60'])
        reaper = threading.Thread(target=process.wait)
        reaper.start()
        self.env['SNOW_TEST_PS_OUTPUT'] = f'{process.pid} {running}\n2147483646 {unrelated}'

        try:
            calls = self.run_script('run-snow-shot.sh')
        finally:
            if process.poll() is None:
                process.kill()
            reaper.join(timeout=2)

        inspect = calls.index(['ps', '-axww', '-o', 'pid=', '-o', 'comm='])
        build = next(i for i, call in enumerate(calls) if '--build' in call)
        self.assertLess(inspect, build)
        self.assertIn(f'Stopping the running development instance (PID {process.pid})...',
                      self.last_result.stdout)
        self.assertNotIn('2147483646', self.last_result.stdout)
        self.assertFalse(reaper.is_alive())
        self.assertEqual(process.returncode, -signal.SIGTERM)

    def test_launch_can_skip_or_clean_the_automatic_build(self):
        app = self.root / 'build/snow-shot-macos-arm64-debug/snow_shot/snow_shot.app'
        binary = app / 'Contents/MacOS/snow_shot'
        binary.parent.mkdir(parents=True)
        binary.touch()
        binary.chmod(0o755)

        deployed = app.parent.parent / 'run/snow_shot.app/Contents/MacOS/snow_shot'
        deployed.parent.mkdir(parents=True)
        deployed.touch()
        deployed.chmod(0o755)

        calls = self.run_script('run-snow-shot.sh', '--no-build')
        self.assertFalse(any(c[0] == 'cmake' for c in calls))
        self.assertEqual(calls[-1], ['open', '-n', str(deployed.parents[2]), '--args'])

        self.log.unlink()
        calls = self.run_script('run-snow-shot.sh', '--clean')
        self.assertIn(['cmake', '--build', '--preset', 'build-snow-shot-macos-arm64-debug',
                       '--target', 'snow_shot', '--parallel'], calls)

    def test_launch_can_cache_a_persistent_codesign_identity(self):
        app = self.root / 'build/snow-shot-macos-arm64-debug/snow_shot/snow_shot.app'
        binary = app / 'Contents/MacOS/snow_shot'
        binary.parent.mkdir(parents=True)
        binary.touch()
        binary.chmod(0o755)
        deployed = app.parent.parent / 'run/snow_shot.app'
        deployed.mkdir(parents=True)

        identity = 'Snow Shot Development (Local)'
        calls = self.run_script('run-snow-shot.sh', '--codesign-identity', identity)
        configure = next(c for c in calls if c[0] == 'cmake' and '--preset' in c)
        self.assertIn('-DSNOW_MACOS_CODESIGN_IDENTITY=' + identity, configure)
        self.assertIn(['cmake', '--build', '--preset', 'build-snow-shot-macos-arm64-debug',
                       '--target', 'snow_shot', '--parallel'], calls)

    def test_launch_automatically_reuses_a_stable_local_codesign_identity(self):
        app = self.root / 'build/snow-shot-macos-arm64-debug/snow_shot/snow_shot.app'
        binary = app / 'Contents/MacOS/snow_shot'
        binary.parent.mkdir(parents=True)
        binary.touch()
        binary.chmod(0o755)
        (app.parent.parent / 'run/snow_shot.app').mkdir(parents=True)

        calls = self.run_script('run-snow-shot.sh')
        configure = next(c for c in calls if c[0] == 'cmake' and '--preset' in c)
        self.assertIn('-DSNOW_MACOS_CODESIGN_IDENTITY='
                      '0123456789ABCDEF0123456789ABCDEF01234567', configure)
        self.assertFalse(any(c[0] == 'openssl' for c in calls))

    def test_launch_creates_the_local_codesign_identity_once_when_missing(self):
        app = self.root / 'build/snow-shot-macos-arm64-debug/snow_shot/snow_shot.app'
        binary = app / 'Contents/MacOS/snow_shot'
        binary.parent.mkdir(parents=True)
        binary.touch()
        binary.chmod(0o755)
        (app.parent.parent / 'run/snow_shot.app').mkdir(parents=True)
        self.env['SNOW_TEST_CODESIGN_IDENTITY'] = ''

        calls = self.run_script('run-snow-shot.sh')
        self.assertTrue(any(c[:2] == ['openssl', 'req'] for c in calls))
        self.assertTrue(any(c[:2] == ['openssl', 'pkcs12'] for c in calls))
        self.assertTrue(any(c[:2] == ['security', 'import'] for c in calls))
        configure = next(c for c in calls if c[0] == 'cmake' and '--preset' in c)
        self.assertIn('-DSNOW_MACOS_CODESIGN_IDENTITY='
                      '0123456789ABCDEF0123456789ABCDEF01234567', configure)

    def test_explicit_adhoc_signing_does_not_provision_an_identity(self):
        app = self.root / 'build/snow-shot-macos-arm64-debug/snow_shot/snow_shot.app'
        binary = app / 'Contents/MacOS/snow_shot'
        binary.parent.mkdir(parents=True)
        binary.touch()
        binary.chmod(0o755)
        (app.parent.parent / 'run/snow_shot.app').mkdir(parents=True)

        calls = self.run_script('run-snow-shot.sh', '--codesign-identity', '-')
        configure = next(c for c in calls if c[0] == 'cmake' and '--preset' in c)
        self.assertIn('-DSNOW_MACOS_CODESIGN_IDENTITY=-', configure)
        self.assertFalse(any(c[0] in ('security', 'openssl') for c in calls))

    def test_no_build_requires_deployment_and_rejects_clean(self):
        for args in (('--no-build',), ('--no-build', '--clean'),
                     ('--no-build', '--codesign-identity', 'Development')):
            calls = self.run_script('run-snow-shot.sh', *args, success=False)
            self.assertFalse(any(c[0] in ('cmake', 'open') for c in calls))

    def test_codesign_identity_requires_a_value(self):
        calls = self.run_script('run-snow-shot.sh', '--codesign-identity', success=False)
        self.assertFalse(any(c[0] in ('cmake', 'open') for c in calls))

    def test_deployment_uses_the_matching_vcpkg_library_configuration(self):
        deployment = (ROOT / 'cmake/DeploySnowShotMacOS.cmake.in').read_text()
        self.assertIn('if(_snow_install_config STREQUAL "debug")', deployment)
        self.assertIn('set(_snow_vcpkg_library_dir "@SNOW_FFMPEG_ROOT@/debug/lib")', deployment)
        self.assertIn('set(_snow_vcpkg_library_dir "@SNOW_FFMPEG_ROOT@/lib")', deployment)
        self.assertEqual(deployment.count('"-libpath=${_snow_vcpkg_library_dir}"'), 1)

    def test_macos_icon_uses_native_visual_bounds_and_bundle_metadata(self):
        generator = (ROOT / 'cmake/GenerateMacOSIcon.cmake').read_text()
        bounds = 'x=\\"100\\" y=\\"100\\" width=\\"824\\" height=\\"824\\"'
        self.assertIn(bounds, generator)

        macos = (ROOT / 'cmake/SnowShotMacOS.cmake').read_text()
        self.assertIn('snow_shot/resources/app-icon.svg', macos)
        self.assertNotIn('packaging/macos/app-icon.svg', macos)

        plist = (ROOT / 'snow_shot/packaging/macos/Info.plist.in').read_text()
        self.assertIn('<key>CFBundleIconFile</key>', plist)
        self.assertIn('${MACOSX_BUNDLE_ICON_FILE}', plist)

        main = (ROOT / 'snow_shot/src/app/main.cpp').read_text()
        self.assertNotIn('installApplicationIconFromBundle', main)
        self.assertNotIn('setApplicationIconImage', main)
        self.assertIn('#ifndef Q_OS_MACOS\n    QApplication::setWindowIcon(', main)


class MacOSSigningDeployment(unittest.TestCase):
    """Exercise the deployment script with fake external tools, including OCR resealing."""

    def test_identity_survives_deployment_and_ocr_reseal(self):
        for identity in ('-', 'Snow Shot Development (Local)'):
            with self.subTest(identity=identity):
                calls, result = self.deploy(identity)
                self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
                deploy = next(c for c in calls if c[0] == 'macdeployqt')
                self.assertIn('-codesign=' + identity, deploy)
                self.assertTrue(any(arg.endswith('/Contents/MacOS/crashpad_handler') for arg in deploy))
                sign = next(c for c in calls if c[0] == 'codesign' and '--sign' in c)
                self.assertEqual(sign[sign.index('--sign') + 1], identity)
                finalize = next(i for i, c in enumerate(calls) if 'finalize' in c)
                self.assertLess(finalize, calls.index(sign))
                self.assertEqual(calls[-1][0:4], ['codesign', '--verify', '--deep', '--strict'])

    def test_signing_failure_stops_without_ad_hoc_fallback(self):
        calls, result = self.deploy('Unavailable Certificate', fail=True)
        self.assertNotEqual(result.returncode, 0)
        self.assertEqual(sum(c[0] == 'macdeployqt' for c in calls), 1)
        self.assertFalse(any(c[0] == 'codesign' for c in calls))

    def test_static_deployment_skips_macdeployqt_and_marks_the_ocr_runtime(self):
        calls, result = self.deploy('-', static=True)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertFalse(any(call[0] == 'macdeployqt' for call in calls))
        for command in ('finalize', 'verify'):
            call = next(call for call in calls if command in call)
            self.assertIn('--static-runtime', call)
        self.assertEqual(calls[-1][0:4], ['codesign', '--verify', '--deep', '--strict'])

    def test_x64_deployment_passes_architecture_to_finalization_and_verification(self):
        calls, result = self.deploy('-', static=True, config='Release', arch='x64')
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        for command in ('finalize', 'verify'):
            call = next(call for call in calls if command in call)
            self.assertEqual(call[call.index('--arch') + 1], 'x64')
        finalize = next(index for index, call in enumerate(calls) if 'finalize' in call)
        sign = next(index for index, call in enumerate(calls)
                    if call[0] == 'codesign' and call[-1].endswith('snow_shot.app'))
        self.assertLess(finalize, sign)

    def test_release_deployment_strips_both_editions_before_signing_and_ocr_hashing(self):
        for mini in (False, True):
            with self.subTest(mini=mini):
                calls, result = self.deploy('-', static=True, config='Release', mini=mini)
                self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
                strips = [(index, call) for index, call in enumerate(calls) if call[0] == 'strip']
                self.assertEqual(len(strips), 4)
                finalize = next(index for index, call in enumerate(calls) if 'finalize' in call)
                for index, call in strips:
                    self.assertEqual(call[1:3], ['-S', '-x'])
                    self.assertNotIn('helper-link', call[-1])
                    self.assertNotIn('note.txt', call[-1])
                    self.assertEqual(calls[index + 1], ['codesign', '--force', '--sign', '-', call[-1]])
                    self.assertLess(index + 1, finalize)
                for command in ('prepare-bundle', 'finalize', 'verify'):
                    call = next(call for call in calls if command in call)
                    self.assertEqual('--runtime-only' in call, mini)
                product = 'snow_shot_mini.app' if mini else 'snow_shot.app'
                self.assertTrue(all('/' + product + '/Contents/' in call[-1] for _, call in strips))
                outer_sign = next(index for index, call in enumerate(calls)
                                  if call[0] == 'codesign' and call[-1].endswith(product))
                self.assertLess(finalize, outer_sign)

    def test_development_deployment_preserves_symbols(self):
        for static, config, release_static in ((True, 'Debug', True),
                                               (True, 'RelWithDebInfo', True),
                                               (True, 'Release', False),
                                               (False, 'Release', False)):
            with self.subTest(static=static, config=config, release_static=release_static):
                calls, result = self.deploy('-', static=static, config=config,
                                            release_static=release_static)
                self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
                self.assertFalse(any(call[0] == 'strip' for call in calls))

    def test_strip_failure_stops_before_signing_and_ocr_hashing(self):
        calls, result = self.deploy('-', static=True, config='Release', strip_fail=True)
        self.assertNotEqual(result.returncode, 0)
        self.assertEqual(sum(call[0] == 'strip' for call in calls), 1)
        self.assertFalse(any(call[0] == 'codesign' or 'finalize' in call for call in calls))

    def test_repeated_release_deployment_reseals_each_worker(self):
        calls, result = self.deploy('-', static=True, config='Release', repeat=2)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertEqual(sum(call[0] == 'strip' for call in calls), 8)
        self.assertEqual(sum('finalize' in call for call in calls), 2)
        self.assertEqual(sum(call[:4] == ['codesign', '--verify', '--deep', '--strict']
                             for call in calls), 2)

    def deploy(self, identity, fail=False, static=False, config='Debug', release_static=None,
               mini=False, strip_fail=False, repeat=1, arch='arm64'):
        with tempfile.TemporaryDirectory(prefix='snow signing tests ') as temp:
            root = Path(temp)
            log = root / 'calls.jsonl'
            mock = """#!/usr/bin/env python3
import json, os, pathlib, sys
name = pathlib.Path(sys.argv[0]).name
with open(os.environ['SNOW_TEST_LOG'], 'a') as log:
    log.write(json.dumps([name] + sys.argv[1:]) + '\\n')
if name == 'macdeployqt' and os.environ.get('SNOW_TEST_FAIL_SIGN'): sys.exit(1)
if name == 'file':
    print('ASCII text' if sys.argv[-1].endswith('.txt') else 'Mach-O 64-bit executable arm64')
if name == 'otool':
    print(sys.argv[-1] + ':\\n\\t/usr/lib/libSystem.B.dylib (compatibility version 1.0.0)')
if name == 'strip' and os.environ.get('SNOW_TEST_FAIL_STRIP'): sys.exit(37)
"""
            for name in ('macdeployqt', 'codesign', 'install_name_tool', 'ocr', 'file', 'otool', 'strip'):
                tool = root / name
                tool.write_text(mock)
                tool.chmod(0o755)
            product = 'snow_shot_mini' if mini else 'snow_shot'
            bridge = 'snow-shot-mini-mcp' if mini else 'snow-shot-mcp'
            runtime = root / (product + '.app') / 'Contents/MacOS'
            runtime.mkdir(parents=True)
            for name in (product, bridge, 'snow-ocr-process', 'crashpad_handler'):
                (runtime / name).write_text('executable fixture')
            (runtime / 'helper-link').symlink_to('snow-ocr-process')
            resources = runtime.parent / 'Resources'
            resources.mkdir()
            (resources / 'note.txt').write_text('resource fixture')
            script = (ROOT / 'cmake/DeploySnowShotMacOS.cmake.in').read_text()
            if mini:
                script = script.replace('snow_shot.app', 'snow_shot_mini.app')
                script = script.replace('snow-shot-mcp', 'snow-shot-mini-mcp')
            values = {'SNOW_MACOS_CODESIGN_IDENTITY': identity,
                      'SNOW_MACDEPLOYQT': str(root / 'macdeployqt'),
                      'SNOW_MACOS_OCR_ASSETS_ENABLED': 'ON',
                      'SNOW_MACOS_OCR_ARCH': arch,
                      'SNOW_MACOS_OCR_RUNTIME_ONLY': 'ON' if mini else 'OFF',
                      'SNOW_SHOT_ENABLE_MCP': 'ON',
                      'SNOW_SHOT_RELEASE_STATIC': 'ON' if (static if release_static is None
                                                          else release_static) else 'OFF',
                      'SNOW_SHOT_QT_STATIC': 'ON' if static else 'OFF',
                      'SNOW_SHOT_OCR_STATIC_ONNXRUNTIME': 'ON' if static else 'OFF',
                      'Python3_EXECUTABLE': str(root / 'ocr'),
                      'SNOW_MACOS_OCR_TOOL': 'ocr.py', 'SNOW_MACOS_OCR_MANIFEST': 'manifest.json',
                      'SNOW_FFMPEG_ROOT': str(root / 'ffmpeg'), 'CMAKE_BINARY_DIR': str(root)}
            for key, value in values.items():
                script = script.replace('@' + key + '@', value)
            for name in ('codesign', 'install_name_tool', 'file', 'otool', 'strip'):
                script = script.replace('/usr/bin/' + name, '"' + str(root / name) + '"')
            path = root / 'deploy.cmake'
            path.write_text(script)
            cmake = shutil.which('cmake') or str(ROOT / '.tools/macos-dev/bin/cmake')
            env = dict(os.environ, SNOW_TEST_LOG=str(log))
            if fail:
                env['SNOW_TEST_FAIL_SIGN'] = '1'
            if strip_fail:
                env['SNOW_TEST_FAIL_STRIP'] = '1'
            for _ in range(repeat):
                result = subprocess.run([cmake, '-DCMAKE_INSTALL_PREFIX=' + str(root),
                                         '-DCMAKE_INSTALL_CONFIG_NAME=' + config, '-P', str(path)],
                                        env=env, text=True, capture_output=True)
                if result.returncode:
                    break
            return [json.loads(line) for line in log.read_text().splitlines()], result


@unittest.skipUnless(os.environ.get('SNOW_TEST_MACOS_BUNDLE') == '1',
                     'Set SNOW_TEST_MACOS_BUNDLE=1 for the native stripping fixture')
class MacOSPackageStripping(unittest.TestCase):
    @unittest.skipUnless(platform.system() == 'Darwin', 'Mach-O stripping requires macOS')
    def test_release_copies_preserve_code_exports_uuids_and_external_dsyms(self):
        """Use small native binaries without Qt, OCR models, network, or a running UI."""
        def run(*args):
            result = subprocess.run(args, text=True, capture_output=True)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            return result.stdout

        def uuid(path):
            return run('xcrun', 'dwarfdump', '--uuid', str(path)).split()[1]

        def sections(binary):
            data = binary.read_bytes()
            self.assertEqual(struct.unpack_from('<I', data)[0], 0xfeedfacf)
            hashes = {}
            offset = 32
            for _ in range(struct.unpack_from('<I', data, 16)[0]):
                command, length = struct.unpack_from('<II', data, offset)
                if command == 0x19:  # LC_SEGMENT_64
                    for index in range(struct.unpack_from('<I', data, offset + 64)[0]):
                        section = offset + 72 + index * 80
                        name = data[section:section + 16].split(b'\0')[0].decode()
                        segment = data[section + 16:section + 32].split(b'\0')[0].decode()
                        size, start = struct.unpack_from('<QI', data, section + 40)
                        flags = struct.unpack_from('<I', data, section + 64)[0]
                        if (segment in ('__TEXT', '__DATA', '__DATA_CONST')
                                and (flags & 0xff) not in (1, 12, 18)):
                            hashes[(segment, name)] = hashlib.sha256(data[start:start + size]).hexdigest()
                offset += length
            self.assertTrue(hashes)
            return hashes

        cmake = shutil.which('cmake') or str(ROOT / '.tools/macos-dev/bin/cmake')
        with tempfile.TemporaryDirectory(prefix='snow native strip ') as temp:
            root = Path(temp)
            source = root / 'fixture.c'
            names = [f'snow_fixture_internal_function_with_private_debug_symbol_{index:03}'
                     for index in range(64)]
            expression = 'value'
            for name in names:
                expression = name + '(' + expression + ')'
            source.write_text('\n'.join('static int ' + name + '(int value) { return value + 1; }'
                                        for name in names) + '\n'
                              'int snow_fixture_exported(int value) { return ' + expression + '; }\n'
                              'int main(void) { return snow_fixture_exported(0) == 64 ? 0 : 1; }\n')
            binary = root / 'fixture'
            run('xcrun', 'clang', '-g', '-O0', str(source), '-o', str(binary))
            symbols = root / 'fixture.dSYM'
            run('xcrun', 'dsymutil', str(binary), '-o', str(symbols))
            original_hash = hashlib.sha256(binary.read_bytes()).hexdigest()
            original_sections = sections(binary)
            original_exports = run('/usr/bin/nm', '-gUj', str(binary))
            original_uuid = uuid(binary)
            self.assertEqual(uuid(symbols), original_uuid)
            ocr = root / 'ocr'
            ocr.write_text('#!/usr/bin/env python3\n')
            ocr.chmod(0o755)
            for product in ('snow_shot', 'snow_shot_mini'):
                app = root / (product + '.app')
                runtime = app / 'Contents/MacOS'
                runtime.mkdir(parents=True)
                resources = app / 'Contents/Resources'
                resources.mkdir()
                note = resources / 'note.txt'
                note.write_text('resource fixture')
                (runtime / 'helper-link').symlink_to('snow-ocr-process')
                (app / 'Contents/Info.plist').write_bytes(plistlib.dumps({
                    'CFBundleExecutable': product, 'CFBundleIdentifier': 'com.snowshot.' + product,
                    'CFBundlePackageType': 'APPL', 'CFBundleName': 'Snow Shot Fixture',
                    'CFBundleVersion': '1.0'}))
                script = (ROOT / 'cmake/DeploySnowShotMacOS.cmake.in').read_text()
                script = script.replace('snow_shot.app', product + '.app')
                values = {'SNOW_MACOS_CODESIGN_IDENTITY': '-', 'SNOW_SHOT_ENABLE_MCP': 'OFF',
                          'SNOW_MACDEPLOYQT': '', 'SNOW_MACOS_OCR_ASSETS_ENABLED': 'OFF',
                          'SNOW_MACOS_OCR_RUNTIME_ONLY': 'ON' if product == 'snow_shot_mini' else 'OFF',
                          'SNOW_SHOT_QT_STATIC': 'ON', 'SNOW_SHOT_RELEASE_STATIC': 'ON',
                          'SNOW_SHOT_OCR_STATIC_ONNXRUNTIME': 'ON', 'Python3_EXECUTABLE': str(ocr),
                          'SNOW_MACOS_OCR_TOOL': 'ocr.py', 'SNOW_MACOS_OCR_MANIFEST': 'manifest.json',
                          'SNOW_FFMPEG_ROOT': str(root / 'ffmpeg'), 'CMAKE_BINARY_DIR': str(root)}
                for key, value in values.items():
                    script = script.replace('@' + key + '@', value)
                deployment = root / 'deploy.cmake'
                deployment.write_text(script)
                # Reinstalling replaces stripped staging copies with build bytes.
                for attempt in range(2):
                    with self.subTest(product=product, attempt=attempt):
                        for name in (product, 'snow-ocr-process', 'crashpad_handler'):
                            shutil.copy2(binary, runtime / name)
                        run(cmake, '-DCMAKE_INSTALL_PREFIX=' + str(root),
                            '-DCMAKE_INSTALL_CONFIG_NAME=Release', '-P', str(deployment))
                        run('/usr/bin/codesign', '--verify', '--deep', '--strict', str(app))
                        for name in (product, 'snow-ocr-process', 'crashpad_handler'):
                            deployed = runtime / name
                            self.assertEqual(sections(deployed), original_sections)
                            self.assertEqual(run('/usr/bin/nm', '-gUj', str(deployed)), original_exports)
                            self.assertEqual(uuid(deployed), original_uuid)
                            self.assertLess(deployed.stat().st_size, binary.stat().st_size)
                            run(str(deployed))
                        self.assertEqual(hashlib.sha256(binary.read_bytes()).hexdigest(), original_hash)
                        self.assertEqual(note.read_text(), 'resource fixture')
                        self.assertTrue((runtime / 'helper-link').is_symlink())
                        self.assertEqual(uuid(symbols), original_uuid)


@unittest.skipUnless(os.environ.get("SNOW_TEST_MACOS_BUNDLE") == "1",
                     "Set SNOW_TEST_MACOS_BUNDLE=1 for the native deployment fixture")
class MacOSBundle(unittest.TestCase):
    @unittest.skipUnless(platform.system() == 'Darwin' and platform.machine() == 'arm64',
                         'Mini requires Apple Silicon macOS')
    def test_mini_deploys_its_codec_backend_and_starts_offscreen(self):
        def run(*args, **kwargs):
            result = subprocess.run(args, text=True, capture_output=True, **kwargs)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            return result.stdout

        preset = 'snow-shot-macos-arm64-debug'
        run(str(ROOT / 'scripts/build.sh'), preset, '--target', 'snow_shot_mini')
        with tempfile.TemporaryDirectory(prefix='snow mini deployment ') as temp:
            stage = Path(temp)
            app = stage / 'snow_shot_mini.app'
            backend = 'libsnow_shot_image_codec_backend.dylib'
            stale_backend = app / 'Contents/MacOS' / backend
            stale_backend.parent.mkdir(parents=True)
            shutil.copy2(ROOT / 'build' / preset / 'snow_shot' / backend, stale_backend)
            run('cmake', '--install', str(ROOT / 'build' / preset), '--component',
                'SnowShotMini', '--prefix', str(stage))
            self.assertTrue((app / 'Contents/Frameworks' / backend).is_file())
            self.assertFalse(stale_backend.exists())
            run('codesign', '--verify', '--deep', '--strict', str(app))
            env = dict(os.environ, QT_QPA_PLATFORM='offscreen')
            for name in ('QT_PLUGIN_PATH', 'QT_QPA_PLATFORM_PLUGIN_PATH', 'DYLD_LIBRARY_PATH',
                         'DYLD_FRAMEWORK_PATH'):
                env.pop(name, None)
            run(str(app / 'Contents/MacOS/snow_shot_mini'), '--startup-probe',
                env=env, cwd=temp, timeout=30)

    def test_deploy_helpers_and_package(self):
        arch = "arm64" if os.uname().machine == "arm64" else "x64"
        prefix = ROOT / f".tools/macos/installed/dynamic/{arch}-osx-snow-shot"
        qt = os.environ.get("Qt6_DIR", str(Path.home() / f"Qt/{QT_VERSION}/macos/lib/cmake/Qt6"))
        with tempfile.TemporaryDirectory(prefix="snow bundle test ") as temp:
            out = Path(temp) / "build"
            stage = Path(temp) / "stage"
            def run(*args, **kwargs):
                result = subprocess.run(args, text=True, capture_output=True, **kwargs)
                self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
                return result.stdout
            run("cmake", "-S", str(ROOT / "test-support/macos-build"), "-B", str(out),
                "-G", "Ninja", f"-DSNOW_ROOT={ROOT}", f"-DSNOW_FFMPEG_ROOT={prefix}",
                f"-DQt6_DIR={qt}", "-DCMAKE_BUILD_TYPE=Release",
                f"-DCMAKE_OSX_ARCHITECTURES={os.uname().machine}")
            run("cmake", "--build", str(out))
            run(str(out / "snow_shot.app/Contents/MacOS/crashpad_handler"), "--version", cwd="/")
            stale_helper = stage / "snow_shot.app/Contents/MacOS/snow-shot-updater"
            stale_helper.parent.mkdir(parents=True, exist_ok=True)
            stale_helper.write_text("obsolete helper")
            run("cmake", "--install", str(out), "--component", "SnowShot", "--prefix", str(stage))
            self.assertFalse(stale_helper.exists())
            app = stage / "snow_shot.app"
            info = run("plutil", "-extract", "CFBundleIconFile", "raw", "-o", "-",
                       str(app / "Contents/Info.plist")).strip()
            self.assertEqual(info, "snow-shot.icns")
            self.assertTrue((app / "Contents/Resources/snow-shot.icns").is_file())
            self.assertTrue((app / "Contents/PlugIns/platforms/libqoffscreen.dylib").is_file())
            collector = app / "Contents/MacOS/crashpad_handler"
            self.assertTrue(collector.is_file())
            run(str(collector), "--version", cwd="/")
            for name in ("snow_shot", "snow-ocr-process"):
                binary = app / "Contents/MacOS" / name
                run(str(binary), cwd="/")
                rpaths = run("otool", "-l", str(binary))
                self.assertNotIn(str(ROOT), rpaths)
                self.assertNotIn(str(out), rpaths)
            run("codesign", "--verify", "--deep", "--strict", str(app))
            run("cpack", "--config", str(out / "CPackConfig.cmake"), cwd=str(out))
            self.assertEqual(len(list(out.glob("*.dmg"))), 1)
            self.assertEqual(len(list(out.glob("*.dmg.sha256"))), 1)
            dmg = next(out.glob("*.dmg"))
            run("codesign", "--verify", "--strict", str(dmg))
            mount = Path(temp) / "mounted"
            mount.mkdir()
            run("hdiutil", "attach", "-readonly", "-nobrowse", "-noautoopen",
                "-mountpoint", str(mount), str(dmg))
            try:
                packaged = mount / "Snow Shot.app"
                self.assertTrue(packaged.is_dir())
                self.assertFalse((mount / "snow_shot.app").exists())
                self.assertEqual(os.readlink(mount / "Applications"), "/Applications")
                self.assertTrue((mount / ".background/background.png").is_file())
                self.assertTrue((mount / ".DS_Store").is_file())
                metadata = plistlib.loads((packaged / "Contents/Info.plist").read_bytes())
                self.assertEqual(metadata['CFBundleDisplayName'], 'Snow Shot')
                self.assertIs(metadata['LSUIElement'], True)
                self.assertEqual(metadata['NSHumanReadableCopyright'],
                                 'Copyright (C) 2025-2026 mg-chao')
                for language in metadata['CFBundleLocalizations']:
                    self.assertTrue((packaged / 'Contents/Resources' /
                                     (language + '.lproj') / 'InfoPlist.strings').is_file())
                run("codesign", "--verify", "--deep", "--strict", str(packaged))
            finally:
                run("hdiutil", "detach", str(mount))
            import hashlib
            checksum = dmg.with_suffix(".dmg.sha256").read_text().split()[0]
            self.assertEqual(checksum, hashlib.sha256(dmg.read_bytes()).hexdigest())


if __name__ == '__main__':
    unittest.main()
