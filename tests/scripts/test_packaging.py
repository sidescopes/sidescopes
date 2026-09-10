"""Distribution assembly and embedded-notice checks."""

import json
import os
import pathlib
import shutil
import subprocess
import sys
import tempfile
import time
import unittest

ROOT = pathlib.Path(__file__).resolve().parents[2]


@unittest.skipUnless(shutil.which('cmake'), 'requires CMake')
class NoticeCollectionTests(unittest.TestCase):
    def test_embedded_notices_preserve_bytes_and_follow_source_changes(self):
        with tempfile.TemporaryDirectory(prefix='embedded notices ') as directory:
            fixture = pathlib.Path(directory)
            for component in ['imgui', 'nanosvg', 'glfw', 'src/app', 'actual']:
                (fixture / component).mkdir(parents=True)
            shutil.copytree(ROOT / 'licenses', fixture / 'licenses')
            shutil.copyfile(ROOT / 'LICENSE', fixture / 'LICENSE')
            shutil.copyfile(ROOT / 'src/app/license_notices.h', fixture / 'src/app/license_notices.h')
            special = 'Quotes " and \\; percent %; UTF-8 café.\r\n'.encode()
            for source in ['imgui/LICENSE.txt', 'nanosvg/LICENSE.txt', 'glfw/LICENSE.md']:
                (fixture / source).write_bytes(special)
            embedded = b'This software is available under 2 licenses\nCopyright and permission.\n'
            for component in ['rectpack', 'textedit', 'truetype']:
                (fixture / 'imgui' / f'imstb_{component}.h').write_bytes(b'/*\n' + embedded + b'*/\n')
            (fixture / 'unused.cpp').write_text('// Catalog-only fixture.\n')
            (fixture / 'dump.cpp').write_text('''#include "app/license_notices.h"
#include <filesystem>
#include <fstream>
#include <string>
int main(int, char** argv) {
    for (auto notice : sidescopes::licenseNotices()) {
        std::ofstream output(std::filesystem::path(argv[1]) / (std::string(notice.name) + ".txt"),
                             std::ios::binary);
        output.write(notice.text.data(), static_cast<std::streamsize>(notice.text.size()));
        if (!output) return 1;
    }
}
''')
            (fixture / 'CMakeLists.txt').write_text(f'''cmake_minimum_required(VERSION 3.22)
project(NoticeFixture LANGUAGES CXX)
set(CMAKE_CXX_STANDARD 20)
set(imgui_SOURCE_DIR "${{CMAKE_SOURCE_DIR}}/imgui")
set(nanosvg_SOURCE_DIR "${{CMAKE_SOURCE_DIR}}/nanosvg")
set(glfw_SOURCE_DIR "${{CMAKE_SOURCE_DIR}}/glfw")
add_library(sidescopes_app STATIC unused.cpp)
target_include_directories(sidescopes_app PUBLIC "${{CMAKE_SOURCE_DIR}}/src")
add_executable(notice_dump dump.cpp)
target_link_libraries(notice_dump PRIVATE sidescopes_app)
include("{(ROOT / 'cmake/ThirdPartyNotices.cmake').as_posix()}")
sidescopes_add_notices(notice_dump)
''')
            build = fixture / 'build'
            # Keep temporary Visual Studio builds from leaving worker nodes.
            build_environment = {**os.environ, 'MSBUILDDISABLENODEREUSE': '1'}
            configured = subprocess.run(['cmake', '-S', str(fixture), '-B', str(build)],
                                        capture_output=True, text=True, env=build_environment)
            self.assertEqual(configured.returncode, 0, configured.stdout + configured.stderr)
            build_command = ['cmake', '--build', str(build), '--config', 'Release', '--parallel', '2']
            collect = ['cmake', f'-DNOTICE_OUTPUT={fixture / "expected"}', f'-DNOTICE_SOURCE={fixture}',
                       f'-DNOTICE_IMGUI={fixture / "imgui"}', f'-DNOTICE_NANOSVG={fixture / "nanosvg"}',
                       f'-DNOTICE_GLFW={fixture / "glfw"}', '-P', str(ROOT / 'cmake/ThirdPartyNotices.cmake')]
            for change in [None, 'glfw/LICENSE.md', 'imgui/imstb_truetype.h']:
                with self.subTest(change=change):
                    if change:
                        # Some Make versions compare only whole-second mtimes.
                        # Give this edit a distinct timestamp from the build.
                        time.sleep(1.1)
                        source = fixture / change
                        source.write_bytes(source.read_bytes().replace(b'\n', b'\nNew notice line.\n', 1))
                    result = subprocess.run(build_command, capture_output=True, text=True, env=build_environment)
                    self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
                    suffix = '.exe' if os.name == 'nt' else ''
                    binary = build / ('notice_dump' + suffix)
                    if not binary.exists():
                        binary = build / 'Release' / ('notice_dump' + suffix)
                    subprocess.run([str(binary), str(fixture / 'actual')], check=True)
                    subprocess.run(collect, check=True, capture_output=True)
                    expected = {('SideScopes' if p.stem == 'SideScopes-GPL' else p.stem.replace('-', ' ')) + '.txt':
                                p.read_bytes() for p in (fixture / 'expected').glob('*.txt')}
                    actual = {p.name: p.read_bytes() for p in (fixture / 'actual').glob('*.txt')}
                    self.assertEqual(actual, expected)
                    self.assertEqual(len(actual), 10)
            (fixture / 'glfw/LICENSE.md').unlink()
            failed = subprocess.run(build_command, capture_output=True, text=True, env=build_environment)
            self.assertNotEqual(failed.returncode, 0)

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

    def test_rebuild_refreshes_samples_and_removes_unavailable_inputs(self):
        with tempfile.TemporaryDirectory() as directory:
            root = pathlib.Path(directory)
            environment = self.prepare_fixture(root)
            self.prepare_fake_build(root)
            cache = root / 'photo-cache'
            cache.mkdir()
            environment.update(SIDESCOPES_WEB_SKIP_SAMPLES='0', SIDESCOPES_PHOTO_CACHE=str(cache))
            (root / 'scripts/scenarios').mkdir()
            # Cached inputs stand in for the separately tested verifier. The
            # image converter copies bytes so this checks assembly, not codecs.
            (root / 'scripts/scenarios/content.py').write_text('')
            converter = root / 'bin/magick'
            converter.write_text(f'#!{sys.executable}\nimport shutil,sys\nshutil.copyfile(sys.argv[1], sys.argv[-1])\n')
            converter.chmod(0o755)
            source = cache / 'skin-and-colour.jpg'
            packaged = root / 'build-web/samples/skin-and-colour.jpg'
            source.write_bytes(b'first verified image')
            self.run_wrapper(root, environment)
            self.assertEqual(packaged.read_bytes(), source.read_bytes())
            source.write_bytes(b'replacement verified image')
            self.run_wrapper(root, environment)
            self.assertEqual(packaged.read_bytes(), source.read_bytes())
            source.unlink()
            self.run_wrapper(root, environment)
            self.assertFalse(packaged.exists())

    def prepare_fake_build(self, root):
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

    def test_repeated_builds_refresh_both_toolchains_and_ship_font_notices(self):
        with tempfile.TemporaryDirectory() as directory:
            root = pathlib.Path(directory)
            environment = self.prepare_fixture(root)
            self.prepare_fake_build(root)
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
