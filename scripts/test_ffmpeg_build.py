#!/usr/bin/env python3
"""Check FFmpeg build planning and strict CMake prefix discovery without compiling FFmpeg."""
import contextlib
import importlib.util
import io
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest


ROOT = Path(__file__).resolve().parents[1]
SPEC = importlib.util.spec_from_file_location('packetia_ffmpeg_builder', ROOT / 'scripts/build_ffmpeg.py')
BUILDER = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(BUILDER)


class BuildPlanTest(unittest.TestCase):
    def test_dry_run_does_not_create_output_or_modify_source(self):
        with tempfile.TemporaryDirectory() as temporary:
            output = Path(temporary) / 'not-created'
            args = BUILDER.parser().parse_args(['--dry-run', '--output-dir', str(output)])
            with contextlib.redirect_stdout(io.StringIO()):
                BUILDER.build(args)
            self.assertFalse(output.exists())

    def test_rejects_source_pollution_and_implicit_gpl(self):
        for argv in (['--output-dir', str(ROOT / 'third/ffmpeg/ffmpeg-9.0.1/output')],
                     ['--codec-library', 'x264'], ['--codec-library', 'x265'], ['--jobs', '0']):
            with self.subTest(argv=argv), self.assertRaises(ValueError):
                BUILDER.plan(BUILDER.parser().parse_args(argv))


@unittest.skipUnless(shutil.which('cmake') and shutil.which('pkg-config'), 'CMake/pkg-config required')
class PrefixDiscoveryTest(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory(prefix='packetia-ffmpeg-discovery-')
        self.addCleanup(self.temporary.cleanup)
        self.root = Path(self.temporary.name)
        self.project = self.root / 'project'
        self.project.mkdir()
        module = (ROOT / 'cmake/ffmpeg').as_posix()
        (self.project / 'CMakeLists.txt').write_text(f'''cmake_minimum_required(VERSION 3.16)
project(PrefixCheck NONE)
add_subdirectory("{module}" ffmpeg)
get_target_property(libraries PkgConfig::PACKETIA_FFMPEG INTERFACE_LINK_LIBRARIES)
file(WRITE "${{CMAKE_BINARY_DIR}}/resolved.txt" "${{libraries}}")
''')

    def installation(self, name, missing_pc=None, missing_library=None):
        prefix = self.root / name
        (prefix / 'lib/pkgconfig').mkdir(parents=True)
        (prefix / 'include').mkdir()
        for library in ('avformat', 'avcodec', 'avutil', 'swscale'):
            if library != missing_library:
                (prefix / 'lib' / f'lib{library}.a').touch()
            if library != missing_pc:
                (prefix / 'lib/pkgconfig' / f'lib{library}.pc').write_text(f'''prefix={prefix}
libdir=${{prefix}}/lib
includedir=${{prefix}}/include
Name: lib{library}
Description: isolated FFmpeg discovery fixture
Version: 1.0.0
Libs: -L${{libdir}} -l{library}
Cflags: -I${{includedir}}
''')
        return prefix

    def configure(self, prefix, fallback=None):
        env = dict(os.environ)
        if fallback:
            env['PKG_CONFIG_PATH'] = str(fallback / 'lib/pkgconfig')
        return subprocess.run(['cmake', '-S', str(self.project), '-B', str(self.root / 'build'),
            f'-DPACKETIA_FFMPEG_ROOT={prefix}'], env=env, capture_output=True, text=True)

    def test_switching_prefix_invalidates_cached_library_paths(self):
        first, second = self.installation('first'), self.installation('second')
        for prefix in (first, second):
            result = self.configure(prefix)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            resolved = (self.root / 'build/resolved.txt').read_text()
            self.assertTrue(all(str(prefix) in value for value in resolved.split(';')), resolved)

    def test_missing_package_cannot_fall_back_to_environment(self):
        complete = self.installation('complete')
        incomplete = self.installation('incomplete', missing_pc='swscale')
        result = self.configure(incomplete, complete)
        self.assertNotEqual(result.returncode, 0, result.stdout + result.stderr)

    def test_missing_library_cannot_use_system_binary(self):
        complete = self.installation('complete')
        incomplete = self.installation('incomplete', missing_library='avcodec')
        env = dict(os.environ, CMAKE_LIBRARY_PATH=str(complete / 'lib'))
        result = subprocess.run(['cmake', '-S', str(self.project), '-B', str(self.root / 'build'),
            f'-DPACKETIA_FFMPEG_ROOT={incomplete}'], env=env, capture_output=True, text=True)
        self.assertNotEqual(result.returncode, 0, result.stdout + result.stderr)


if __name__ == '__main__':
    unittest.main()
