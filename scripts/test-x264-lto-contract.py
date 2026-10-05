#!/usr/bin/env python3
"""Focused x264 stack-alignment and static macOS LTO contract tests."""

from pathlib import Path
import platform
import shlex
import shutil
import struct
import subprocess
import tempfile
import unittest


ROOT = Path(__file__).resolve().parents[1]
FLAGS = ('VCPKG_C_FLAGS', 'VCPKG_CXX_FLAGS', 'VCPKG_LINKER_FLAGS',
         'VCPKG_C_FLAGS_DEBUG', 'VCPKG_CXX_FLAGS_DEBUG', 'VCPKG_LINKER_FLAGS_DEBUG',
         'VCPKG_C_FLAGS_RELEASE', 'VCPKG_CXX_FLAGS_RELEASE', 'VCPKG_LINKER_FLAGS_RELEASE')


class X264LtoFixture(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory(prefix='snow-x264-lto-')
        self.addCleanup(self.temporary.cleanup)
        self.root = Path(self.temporary.name)
        self.cmake = shutil.which('cmake')
        self.assertIsNotNone(self.cmake, 'CMake must be available')

    def flags(self, *, architecture='x64', linkage='static', macos=True):
        initial = {
            name: ('-flto -Wl,-dead_strip' if 'LINKER' in name else
                   '-O3 -flto=thin -ffunction-sections -fdata-sections')
            for name in FLAGS
        }
        script = self.root / 'flags.cmake'
        script.write_text(f'''
set(VCPKG_TARGET_IS_OSX {"TRUE" if macos else "FALSE"})
set(VCPKG_TARGET_ARCHITECTURE "{architecture}")
set(VCPKG_LIBRARY_LINKAGE "{linkage}")
''' + '\n'.join(f'set({name} "{value}")' for name, value in initial.items()) + f'''
include("{ROOT.as_posix()}/cmake/vcpkg-overlay-ports/x264/macos-static-lto.cmake")
''' + '\n'.join(f'file(APPEND "{(self.root / "flags.txt").as_posix()}" '
                   f'"{name}=${{{name}}}\\n")' for name in FLAGS), encoding='utf-8')
        result = subprocess.run([self.cmake, '-P', str(script)],
                                capture_output=True, text=True, check=False)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        actual = dict(line.split('=', 1) for line in
                      (self.root / 'flags.txt').read_text(encoding='utf-8').splitlines())
        return initial, actual


class X264LtoContract(X264LtoFixture):
    def test_static_macos_x64_overrides_lto_without_losing_optimization_flags(self):
        initial, actual = self.flags()
        for name in FLAGS:
            self.assertEqual(actual[name], initial[name] + ' -fno-lto')

    def test_arm64_macos_keeps_lto_policy(self):
        initial, actual = self.flags(architecture='arm64')
        self.assertEqual(actual, initial)

    def test_dynamic_macos_keeps_lto_policy(self):
        initial, actual = self.flags(linkage='dynamic')
        self.assertEqual(actual, initial)

    def test_other_platforms_keep_lto_policy(self):
        initial, actual = self.flags(macos=False)
        self.assertEqual(actual, initial)


@unittest.skipUnless(platform.system() == 'Darwin', 'Requires the Apple toolchain')
class X264DarwinLink(X264LtoFixture):
    def run_command(self, *arguments, success=True):
        result = subprocess.run(arguments, capture_output=True, text=True, check=False)
        self.assertEqual(result.returncode == 0, success, result.stdout + result.stderr)
        return result

    def test_native_x264_alignment_links_with_lto_app_and_runs(self):
        compiler = shutil.which('clang')
        ar = shutil.which('ar')
        self.assertIsNotNone(compiler)
        self.assertIsNotNone(ar)
        codec = self.root / 'codec.c'
        codec.write_text('''
__attribute__((force_align_arg_pointer, noinline)) int codec_probe(int value) {
    volatile int words[16] __attribute__((aligned(64)));
    words[0] = value;
    return ((unsigned long)words & 63) == 0 ? words[0] : -1;
}
''', encoding='utf-8')
        app = self.root / 'app.c'
        app.write_text('''
extern int codec_probe(int);
int main(void) { return codec_probe(7) == 7 ? 0 : 1; }
''', encoding='utf-8')
        codec_object = self.root / 'codec.o'
        app_object = self.root / 'app.o'
        archive = self.root / 'libcodec.a'
        executable = self.root / 'app'
        self.run_command(compiler, '-arch', 'x86_64', '-O3', '-flto',
                         '-mstack-alignment=16', '-c', str(app), '-o', str(app_object))
        self.run_command(compiler, '-arch', 'x86_64', '-O3', '-flto',
                         '-mstack-alignment=64', '-c', str(codec), '-o', str(codec_object))
        self.run_command(ar, 'rcs', str(archive), str(codec_object))
        failure = self.run_command(compiler, '-arch', 'x86_64', '-flto', str(app_object),
                                   str(archive), '-o', str(executable), success=False)
        self.assertIn('override-stack-alignment', failure.stderr)
        self.assertIn('conflicting values', failure.stderr)

        _, actual = self.flags()
        codec_flags = shlex.split(actual['VCPKG_C_FLAGS_RELEASE'])
        self.run_command(compiler, '-arch', 'x86_64', *codec_flags,
                         '-mstack-alignment=64', '-c', str(codec), '-o', str(codec_object))
        self.assertEqual(struct.unpack('<II', codec_object.read_bytes()[:8]),
                         (0xfeedfacf, 0x01000007), 'x264 must emit a native x64 Mach-O object')
        self.run_command(ar, 'rcs', str(archive), str(codec_object))
        self.run_command(compiler, '-arch', 'x86_64', '-flto', str(app_object),
                         str(archive), '-o', str(executable))
        self.run_command(str(executable))


if __name__ == '__main__':
    unittest.main()
