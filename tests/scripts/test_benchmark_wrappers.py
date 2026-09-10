"""Run measurement wrappers against fixed-output fixtures, never real benchmarks."""

import json
import os
import pathlib
import shlex
import shutil
import subprocess
import sys
import tempfile
import unittest

ROOT = pathlib.Path(__file__).resolve().parents[2]
POWERSHELL = shutil.which('pwsh') or shutil.which('powershell')
BUILD_TOOLS = shutil.which('cmake') and shutil.which('ninja')

FIXTURE = r'''#include <fstream>
#include <string>
int main(int argc, char** argv) {
    std::string path;
    for (int i = 1; i + 1 < argc; ++i)
        if (std::string(argv[i]) == "--out") path = argv[i + 1];
    if (path.empty()) return 2;
    std::ofstream output(path);
#ifdef BENCHMARK_XML
    output << "<Catch2TestRun><BenchmarkResults name=\"fixture metric\">"
              "<mean value=\"123.5\"/></BenchmarkResults>"
              "<BenchmarkResults name=\"no result\"/></Catch2TestRun>";
#else
    output << "[]\n";
    std::ofstream arguments(path + ".args");
    for (int i = 1; i < argc; ++i) arguments << argv[i] << '\n';
#endif
    output.close();
    return output ? 0 : 1;
}
'''


@unittest.skipUnless(BUILD_TOOLS, 'requires CMake and Ninja for the tiny fixture')
class BenchmarkWrapperTests(unittest.TestCase):
    def setUp(self):
        temporary = tempfile.TemporaryDirectory(prefix='benchmark wrapper ')
        self.addCleanup(temporary.cleanup)
        self.fixture = pathlib.Path(temporary.name)
        self.build = self.fixture / 'build-bench'
        scripts = self.fixture / 'scripts'
        scripts.mkdir()
        for name in ['bench.sh', 'bench.ps1', 'perf.sh', 'perf.ps1']:
            shutil.copyfile(ROOT / 'scripts' / name, scripts / name)
        (self.fixture / 'fixture.cpp').write_text(FIXTURE, encoding='utf-8')
        (self.fixture / 'CMakeLists.txt').write_text('''cmake_minimum_required(VERSION 3.24)
project(BenchmarkWrapperFixture LANGUAGES CXX)
option(SIDESCOPES_BENCH "Fixture build flag" OFF)
option(SIDESCOPES_BUILD_TESTS "Fixture test flag" ON)
add_executable(sidescopes_bench fixture.cpp)
target_compile_definitions(sidescopes_bench PRIVATE BENCHMARK_XML)
add_executable(sidescopes_perf fixture.cpp)
set_target_properties(sidescopes_bench sidescopes_perf PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bench")
''', encoding='utf-8')
        commands = self.fixture / 'commands'
        commands.mkdir()
        if os.name == 'nt':
            (commands / 'git.cmd').write_text('@echo fixture-revision\n@exit /b 0\n')
            (commands / 'python3.cmd').write_text(f'@"{sys.executable}" %*\n@exit /b %ERRORLEVEL%\n')
        else:
            for name, body in [('git', "printf '%s\\n' fixture-revision"),
                               ('python3', f'exec {shlex.quote(sys.executable)} "$@"')]:
                command = commands / name
                command.write_text('#!/bin/sh\n' + body + '\n')
                command.chmod(0o755)
        self.environment = {**os.environ, 'PATH': str(commands) + os.pathsep + os.environ['PATH'],
                            'BENCH_MACHINE': 'fixture machine', 'MSBUILDDISABLENODEREUSE': '1'}
        self.run_command(['cmake', '-S', str(self.fixture), '-B', str(self.build), '-G', 'Ninja',
                          '-DCMAKE_BUILD_TYPE=Debug'])

    def run_command(self, command):
        result = subprocess.run(command, env=self.environment, stdin=subprocess.DEVNULL,
                                capture_output=True, text=True, timeout=60)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        return result

    def run_wrapper(self, name, *arguments):
        command = ([POWERSHELL, '-NoProfile', '-NonInteractive', '-File'] if name.endswith('.ps1') else ['sh'])
        result = self.run_command([*command, str(self.fixture / 'scripts' / name), *map(str, arguments)])
        cache = (self.build / 'CMakeCache.txt').read_text(encoding='utf-8')
        self.assertIn('CMAKE_BUILD_TYPE:STRING=Release\n', cache)
        self.assertIn('SIDESCOPES_BENCH:BOOL=ON\n', cache)
        self.assertIn('SIDESCOPES_BUILD_TESTS:BOOL=OFF\n', cache)
        return result.stdout.strip().splitlines()[-1]

    def check_benchmark(self, name):
        output = pathlib.Path(self.run_wrapper(name))
        self.assertTrue(output.samefile(self.fixture / 'bench-results' / 'fixture machine-fixture-revision.json'))
        rows = json.loads(output.read_text(encoding='utf-8'))
        self.assertEqual(len(rows), 1)
        self.assertEqual(rows[0]['machine'], 'fixture machine')
        self.assertEqual(rows[0]['commit'], 'fixture-revision')
        self.assertTrue(rows[0]['os'])
        self.assertEqual((rows[0]['metric'], rows[0]['value'], rows[0]['unit']), ('fixture metric', 123.5, 'ns'))

    def check_performance(self, name):
        first = self.fixture / 'first result.json'
        last = self.fixture / 'final result.json'
        reported = self.run_wrapper(name, '--out', first, '--tiers', 'hash', '--out', last)
        self.assertTrue(pathlib.Path(reported).samefile(last))
        self.assertFalse(first.exists())
        self.assertEqual(json.loads(last.read_text()), [])
        arguments = pathlib.Path(str(last) + '.args').read_text().splitlines()
        self.assertEqual(arguments[-6:], ['--out', str(first), '--tiers', 'hash', '--out', str(last)])

    @unittest.skipIf(os.name == 'nt', 'requires the POSIX shell wrapper')
    def test_shell_benchmark_converts_xml_on_stdin_and_forces_release(self):
        self.check_benchmark('bench.sh')

    @unittest.skipIf(os.name == 'nt', 'requires the POSIX shell wrapper')
    def test_shell_performance_reports_the_last_output_override(self):
        self.check_performance('perf.sh')

    @unittest.skipUnless(os.name == 'nt' and POWERSHELL, 'requires Windows PowerShell')
    def test_powershell_benchmark_converts_xml_on_stdin_and_forces_release(self):
        self.check_benchmark('bench.ps1')

    @unittest.skipUnless(os.name == 'nt' and POWERSHELL, 'requires Windows PowerShell')
    def test_powershell_performance_reports_the_last_output_override(self):
        self.check_performance('perf.ps1')


if __name__ == '__main__':
    unittest.main()
