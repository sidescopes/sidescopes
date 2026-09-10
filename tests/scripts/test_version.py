"""Build identity must describe the application checkout, never its parent."""

import pathlib
import shutil
import subprocess
import tempfile
import unittest

ROOT = pathlib.Path(__file__).resolve().parents[2]


@unittest.skipUnless(shutil.which('cmake') and shutil.which('git'), 'requires CMake and Git')
class VersionStampTests(unittest.TestCase):
    def generate(self, source, output):
        result = subprocess.run(
            ['cmake', '-DSIDESCOPES_VERSION=1.2.3', f'-DSIDESCOPES_SOURCE_DIR={source}',
             f'-DSIDESCOPES_OUTPUT={output}', '-P', str(ROOT / 'cmake/GenerateVersion.cmake')],
            capture_output=True, text=True, timeout=30)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        return output.read_text()

    def test_source_archive_inside_another_checkout_has_no_git_identity(self):
        with tempfile.TemporaryDirectory(prefix='source archive ', dir=ROOT) as directory:
            source = pathlib.Path(directory)
            header = self.generate(source, source / 'version.h')
            self.assertIn('#define SIDESCOPES_VERSION "1.2.3"', header)
            self.assertIn('#define SIDESCOPES_GIT_DESCRIBE ""', header)

    def test_checkout_uses_its_own_identity_and_preserves_unchanged_header(self):
        checkout = subprocess.run(['git', '-C', str(ROOT), 'rev-parse', '--show-toplevel'],
                                  capture_output=True, text=True, timeout=30)
        if checkout.returncode or pathlib.Path(checkout.stdout.strip()).resolve() != ROOT:
            self.skipTest('source archives have no application checkout identity')
        expected = subprocess.run(['git', '-C', str(ROOT), 'describe', '--tags', '--always', '--dirty'],
                                  capture_output=True, text=True, check=True, timeout=30).stdout.strip()
        with tempfile.TemporaryDirectory() as directory:
            output = pathlib.Path(directory) / 'version.h'
            header = self.generate(ROOT, output)
            self.assertIn(f'#define SIDESCOPES_GIT_DESCRIBE "{expected}"', header)
            written = output.stat().st_mtime_ns
            self.assertEqual(self.generate(ROOT, output), header)
            self.assertEqual(output.stat().st_mtime_ns, written)


if __name__ == '__main__':
    unittest.main()
