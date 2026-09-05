"""Regression checks for measurement and browser-packaging tools; no desktop input."""

import contextlib
import hashlib
import importlib.util
import io
import json
import os
import shutil
import pathlib
import subprocess
import sys
import tempfile
import types
import unittest
from unittest import mock

ROOT = pathlib.Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT))
# These tests exercise orchestration with an explicit fake platform. They run
# on Linux and Windows too and never read or move the user's real pointer.
sys.modules['scripts.scenarios.quartz'] = types.ModuleType('quartz')
from scripts.scenarios import catalog, conditions, content, session


def load_script(name, path):
    spec = importlib.util.spec_from_file_location(name, ROOT / path)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


compare = load_script('bench_compare', 'scripts/bench-compare.py')


class ScenarioPreferencesTests(unittest.TestCase):
    def test_stack_is_serialized_as_module_ids(self):
        text = session.preferences_text('WVR', (10, 20, 440, 720))
        fields = dict(line.split('=', 1) for line in text.splitlines())
        self.assertEqual(fields['scope_stack'],
                         '[org.sidescopes.waveform][org.sidescopes.vectorscope][org.sidescopes.parade]')
        self.assertEqual(fields['window_x'], '10')
        self.assertEqual(fields['window_y'], '20')

    def test_duplicate_letters_do_not_duplicate_scopes(self):
        text = session.preferences_text('VVW', (0, 0, 440, 720))
        self.assertIn('scope_stack=[org.sidescopes.vectorscope][org.sidescopes.waveform]\n', text)

    def test_empty_region_is_established_explicitly(self):
        with mock.patch.object(session.quartz, 'press_key', create=True) as press:
            self.assertTrue(session.establish_region(42, 'none', (), (), {}))
        press.assert_called_once_with('escape')

    def test_redraw_requires_a_real_clear_action(self):
        old_profile = next(profile for profile in catalog.PROFILES if profile.name == 'always-scoping')
        self.assertIn('clear-region', catalog.unavailable(
            catalog.scenario_named('region-redraw'), 'V', old_profile, 'V'))


@unittest.skipIf(sys.platform == 'win32', 'the content helper uses macOS/POSIX pipes')
class ContentWindowTests(unittest.TestCase):
    def launch_fake(self, code):
        popen = subprocess.Popen
        process = popen([sys.executable, '-c', code], stdout=subprocess.PIPE,
                        stderr=subprocess.STDOUT, text=True)
        return process

    def test_readiness_deadline_interrupts_an_incomplete_line_and_cleans_up(self):
        process = self.launch_fake("import sys,time; sys.stdout.write('partial'); sys.stdout.flush(); time.sleep(30)")
        with mock.patch.object(content.subprocess, 'Popen', return_value=process), \
                mock.patch.object(content, '_HEADER_TIMEOUT_SECONDS', 0.1):
            with self.assertRaisesRegex(RuntimeError, 'never reported itself ready'):
                content.ContentWindow('unused', (0, 0, 100, 100), content.ContentSet('synthetic'))
        self.assertIsNotNone(process.poll())
        self.assertTrue(process.stdout.closed)

    def test_readiness_parses_geometry_and_motion_before_returning(self):
        process = self.launch_fake("import time; print('content_rect 10,20,640,480\\npan 4,160,24\\nready',flush=True); time.sleep(30)")
        with mock.patch.object(content.subprocess, 'Popen', return_value=process):
            with content.ContentWindow('unused', (0, 0, 100, 100), content.ContentSet('synthetic')) as window:
                self.assertEqual(window.rect, (10, 20, 640, 480))
                self.assertEqual(window.pan['pixels_per_frame'], 4)
        self.assertIsNotNone(process.poll())


class CachedContentTests(unittest.TestCase):
    def test_failed_replacement_does_not_leave_corrupt_cached_content_available(self):
        with tempfile.TemporaryDirectory() as directory:
            cache = pathlib.Path(directory)
            image = cache / 'sample.jpg'
            image.write_bytes(b'wrong image')
            entry = {'name': image.name, 'sha256': hashlib.sha256(b'right image').hexdigest(), 'url': 'unused'}
            with mock.patch.object(content, '_download', side_effect=OSError('offline')):
                with self.assertRaises(OSError):
                    content._fetch(entry, cache)
            self.assertFalse(image.exists())


class ComparisonTests(unittest.TestCase):
    def test_content_identity_and_power_state_affect_comparability(self):
        before = {'conditions': {'content': {'kind': 'synthetic', 'patterns': ['gray']},
                                  'power': {'charging': False, 'cpu_speed_limit': 100}}}
        after = {'conditions': {'content': {'kind': 'synthetic', 'patterns': ['noise']},
                                 'power': {'charging': True, 'cpu_speed_limit': 80}}}
        with contextlib.redirect_stdout(io.StringIO()):
            self.assertEqual(compare.report_conditions(before, after), ['power', 'content'])

    def compare(self, left, right, **kwargs):
        output = io.StringIO()
        with contextlib.redirect_stdout(output):
            regression = compare.compare({'cost': left}, {'cost': right}, 15, **kwargs)
        return regression, output.getvalue()

    def test_a_new_cost_above_zero_is_a_regression(self):
        regression, _ = self.compare({'value': 0, 'unit': 'cores'}, {'value': 0.5, 'unit': 'cores'})
        self.assertTrue(regression)

    def test_different_units_are_not_compared(self):
        regression, output = self.compare({'value': 1, 'unit': 'seconds'}, {'value': 1000, 'unit': 'ms'})
        self.assertFalse(regression)
        self.assertIn('NOT COMPARABLE', output)

    def test_different_conditions_suppress_the_verdict(self):
        regression, output = self.compare({'value': 1}, {'value': 2}, condition_reason='different machines')
        self.assertFalse(regression)
        self.assertIn('NOT COMPARABLE: different machines', output)


class PowerStateTests(unittest.TestCase):
    def test_charging_is_distinguished_from_discharging_and_not_charging(self):
        for status, charging in [('charging', True), ('discharging', False), ('not charging', False)]:
            with self.subTest(status=status), mock.patch.object(
                    conditions, '_command', return_value=f"Now drawing from 'AC Power'\n -InternalBattery-0 80%; {status}; 1:00 remaining"):
                self.assertEqual(conditions.power_state()['charging'], charging)


@unittest.skipUnless(shutil.which('cmake'), 'requires CMake')
class NoticeCollectionTests(unittest.TestCase):
    def test_collection_preserves_dependency_text_and_refuses_missing_notices(self):
        with tempfile.TemporaryDirectory() as directory:
            fixture = pathlib.Path(directory)
            for component in ['imgui', 'nanosvg', 'glfw']:
                (fixture / component).mkdir()
            originals = {
                'imgui/LICENSE.txt': b'Exact ImGui notice.\n',
                'nanosvg/LICENSE.txt': b'Exact NanoSVG notice.\r\n',
                'glfw/LICENSE.md': b'Exact GLFW notice.\n',
            }
            for name, data in originals.items():
                (fixture / name).write_bytes(data)
            embedded = 'This software is available under 2 licenses\nCopyright and permission.\n'
            for component in ['rectpack', 'textedit', 'truetype']:
                (fixture / 'imgui' / f'imstb_{component}.h').write_text('code\n/*\n' + embedded + '*/\n')
            output = fixture / 'notices'
            command = ['cmake', f'-DNOTICE_OUTPUT={output}', f'-DNOTICE_SOURCE={ROOT}',
                       f'-DNOTICE_IMGUI={fixture / "imgui"}', f'-DNOTICE_NANOSVG={fixture / "nanosvg"}',
                       f'-DNOTICE_GLFW={fixture / "glfw"}', '-P', str(ROOT / 'cmake/ThirdPartyNotices.cmake')]
            result = subprocess.run(command, capture_output=True, text=True)
            self.assertEqual(result.returncode, 0, result.stderr)
            for source, packaged in [('imgui/LICENSE.txt', 'Dear-ImGui.txt'),
                                     ('nanosvg/LICENSE.txt', 'NanoSVG.txt'), ('glfw/LICENSE.md', 'GLFW.txt')]:
                self.assertEqual((output / packaged).read_bytes(), originals[source])
            for component in ['rectpack', 'textedit', 'truetype']:
                self.assertEqual((output / f'stb-{component}.txt').read_text(), embedded)
            self.assertEqual((output / 'Lucide.txt').read_bytes(), (ROOT / 'licenses/Lucide.txt').read_bytes())
            self.assertEqual((output / 'SideScopes-GPL.txt').read_bytes(), (ROOT / 'LICENSE').read_bytes())
            (fixture / 'glfw/LICENSE.md').unlink()
            failed = subprocess.run(command, capture_output=True, text=True)
            self.assertNotEqual(failed.returncode, 0)
            self.assertIn('Missing distribution notice', failed.stderr)


@unittest.skipIf(sys.platform == 'win32', 'the browser build wrapper is a POSIX shell script')
class BrowserPackagingTests(unittest.TestCase):
    def prepare_fixture(self, root):
        for relative in ['scripts', 'src/web/fonts', 'assets/brand/icons/linux', 'bin']:
            (root / relative).mkdir(parents=True)
        for relative in ['scripts/build-web.sh', 'scripts/web-standalone.py', 'src/web/index.html',
                         'src/web/fonts/Inter-OFL.txt', 'src/web/fonts/RobotoMono-OFL.txt']:
            shutil.copy2(ROOT / relative, root / relative)
        for name in ['sidescopes-32.png', 'sidescopes-256.png']:
            (root / 'assets/brand/icons/linux' / name).write_bytes(b'icon')
        return dict(os.environ, PATH=str(root / 'bin') + os.pathsep + os.environ['PATH'],
                    SIDESCOPES_WEB_SKIP_SAMPLES='1')

    def run_wrapper(self, root, environment):
        result = subprocess.run(['sh', str(root / 'scripts/build-web.sh'), '--standalone'],
                                env=environment, capture_output=True, text=True, timeout=60)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)

    def check_real_generator(self, preconfigured_generator):
        # Real CMake validates the cached generator and builds the artifacts;
        # only Emscripten is replaced, so these tests need no SDK or downloads.
        with tempfile.TemporaryDirectory(prefix='browser packaging ') as directory:
            root = pathlib.Path(directory)
            environment = self.prepare_fixture(root)
            environment['CMAKE_GENERATOR'] = 'Ninja' if preconfigured_generator else 'Unix Makefiles'
            configure = root / 'bin/emcmake'
            configure.write_text('#!/bin/sh\nexec "$@"\n')
            configure.chmod(0o755)
            (root / 'engine.js').write_text('// SINGLE_FILE\n')
            (root / 'engine.wasm').write_bytes(b'module')
            (root / 'CMakeLists.txt').write_text('''cmake_minimum_required(VERSION 3.20)
project(BrowserPackagingFixture LANGUAGES NONE)
file(MAKE_DIRECTORY "${CMAKE_BINARY_DIR}/licenses")
file(WRITE "${CMAKE_BINARY_DIR}/licenses/Dependency.txt" "Dependency notice.\\n")
add_custom_target(lab ALL
    COMMAND "${CMAKE_COMMAND}" -E copy "${CMAKE_SOURCE_DIR}/engine.js" "${CMAKE_BINARY_DIR}/sidescopes-lab.js"
    COMMAND "${CMAKE_COMMAND}" -E copy "${CMAKE_SOURCE_DIR}/engine.wasm" "${CMAKE_BINARY_DIR}/sidescopes-lab.wasm")
''')
            build_dirs = [root / 'build-web-cmake', root / 'build-web-single']
            if preconfigured_generator:
                for build in build_dirs:
                    result = subprocess.run(['cmake', '-S', str(root), '-B', str(build),
                                             '-G', preconfigured_generator, '-DCMAKE_BUILD_TYPE=Debug'],
                                            env=environment, capture_output=True, text=True, timeout=60)
                    self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            for _ in range(2):
                self.run_wrapper(root, environment)
                for build in build_dirs:
                    cache = (build / 'CMakeCache.txt').read_text()
                    generator = preconfigured_generator or 'Ninja'
                    self.assertIn(f'CMAKE_GENERATOR:INTERNAL={generator}\n', cache)
                    self.assertRegex(cache, r'(?m)^CMAKE_BUILD_TYPE:[^=]+=Release$')
                    self.assertRegex(cache, r'(?m)^FETCHCONTENT_UPDATES_DISCONNECTED:[^=]+=ON$')
                self.assertRegex(cache, r'(?m)^SIDESCOPES_WEB_SINGLE_FILE:[^=]+=ON$')
                self.assertEqual((root / 'build-web/sidescopes-lab.wasm').read_bytes(), b'module')
                self.assertIn('window.__STANDALONE = true',
                              (root / 'build-web/sidescopes-lab.html').read_text())

    @unittest.skipUnless(shutil.which('cmake') and shutil.which('make'), 'requires CMake and Make')
    def test_preconfigured_makefiles_builds_keep_their_generator(self):
        self.check_real_generator('Unix Makefiles')

    @unittest.skipUnless(shutil.which('cmake') and shutil.which('ninja'), 'requires CMake and Ninja')
    def test_cold_builds_select_ninja(self):
        self.check_real_generator(None)

    def test_repeated_builds_refresh_both_toolchains_and_ship_font_notices(self):
        with tempfile.TemporaryDirectory() as directory:
            root = pathlib.Path(directory)
            environment = self.prepare_fixture(root)
            configure = root / 'bin/emcmake'
            configure.write_text(f"#!{sys.executable}\n" + r'''import json,pathlib,sys
root = pathlib.Path(__file__).resolve().parents[1]
with (root / 'configured.jsonl').open('a') as output:
    output.write(json.dumps(sys.argv[1:]) + '\n')
build = pathlib.Path(sys.argv[sys.argv.index('-B') + 1])
build.mkdir(exist_ok=True)
(build / 'build.ninja').write_text('configured')
(build / 'CMakeCache.txt').write_text('CMAKE_GENERATOR:INTERNAL=Ninja\n')
(build / 'sidescopes-lab.js').write_text('// SINGLE_FILE\n')
(build / 'sidescopes-lab.wasm').write_bytes(b'module')
(build / 'licenses').mkdir(exist_ok=True)
for notice in (root / 'src/web/fonts').glob('*.txt'):
    (build / 'licenses' / notice.name).write_bytes(notice.read_bytes())
(build / 'licenses/Dependency.txt').write_bytes(b'Exact dependency notice.\r\n')
''')
            configure.chmod(0o755)
            build = root / 'bin/cmake'
            build.write_text('#!/bin/sh\nexit 0\n')
            build.chmod(0o755)
            for _ in range(2):
                self.run_wrapper(root, environment)
            configured = [json.loads(line) for line in (root / 'configured.jsonl').read_text().splitlines()]
            self.assertEqual(len(configured), 4)
            self.assertTrue(all('-DFETCHCONTENT_UPDATES_DISCONNECTED=ON' in args for args in configured))
            self.assertEqual(sum('-DSIDESCOPES_WEB_SINGLE_FILE=ON' in args for args in configured), 2)
            standalone = (root / 'build-web/sidescopes-lab.html').read_text()
            for name in ['Inter-OFL.txt', 'RobotoMono-OFL.txt']:
                notice = (root / 'src/web/fonts' / name).read_text()
                self.assertEqual((root / 'build-web/licenses' / name).read_text(), notice)
                self.assertIn(notice, standalone)
            self.assertIn('window.__STANDALONE = true', standalone)
            self.assertIn('Exact dependency notice.\n', standalone)
            self.assertIn(b'Exact dependency notice.\r\n',
                          (root / 'build-web/sidescopes-lab.html').read_bytes())
            self.assertEqual((root / 'build-web/licenses/Dependency.txt').read_text(),
                             'Exact dependency notice.\n')


if __name__ == '__main__':
    unittest.main()
