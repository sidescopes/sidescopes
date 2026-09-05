"""Performance-harness contract checks; every executed tier uses --quick."""

import json
import math
import os
import pathlib
import subprocess
import tempfile
import unittest


@unittest.skipUnless(os.environ.get('SIDESCOPES_PERF_BINARY'), 'set SIDESCOPES_PERF_BINARY to the built harness')
class PerformanceCliTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.binary = pathlib.Path(os.environ['SIDESCOPES_PERF_BINARY']).resolve()
        if not cls.binary.is_file():
            raise FileNotFoundError(cls.binary)

    def run_harness(self, *arguments, **kwargs):
        return subprocess.run([str(self.binary), '--quick', '--tiers', 'hash', *arguments],
                              capture_output=True, text=True, encoding='utf-8', timeout=20, **kwargs)

    def test_json_roundtrips_labels_and_preserves_hash_metric_identity(self):
        machine = 'Łódź "workstation" \\ ' + ''.join(chr(value) for value in range(1, 32))
        os_name = 'system\nsecond\tline'
        commit = 'quote"and\\slash'
        result = self.run_harness('--machine', machine, '--os', os_name, '--commit', commit)
        self.assertEqual(result.returncode, 0, result.stderr)
        rows = json.loads(result.stdout)
        self.assertEqual({row['metric'] for row in rows},
                         {prefix + size for prefix in ['hash ', 'hash-per-mpx ']
                          for size in ['small', 'medium', 'full']})
        self.assertEqual(len(rows), 6)
        for row in rows:
            self.assertEqual((row['machine'], row['os'], row['commit']), (machine, os_name, commit))
            self.assertEqual(row['unit'], 'ns')
            self.assertTrue(math.isfinite(row['value']) and row['value'] >= 0)

    def test_invalid_arguments_fail_before_touching_existing_output(self):
        invalid = [['--unknown'], ['--machine'], ['--os', '--quick'], ['positional'],
                   ['--tiers', ''], ['--tiers', 'hashed'], ['--tiers', 'hash,'],
                   ['--tiers', 'hash,hash'], ['--tiers', ',hash'], ['--tiers', 'hash,,worker'],
                   ['--tiers', 'hash,unknown']]
        invalid += [['--worker-seconds', value]
                    for value in ['inf', '-inf', 'nan', '1e9999', '0', '0.49', '3601', '1x', '', ' 1', '1 ', '+1']]
        with tempfile.TemporaryDirectory() as directory:
            output = pathlib.Path(directory) / 'keep.json'
            output.write_text('previous result', encoding='utf-8')
            for arguments in invalid:
                with self.subTest(arguments=arguments):
                    result = self.run_harness('--out', str(output), *arguments)
                    self.assertEqual(result.returncode, 2, result.stderr)
                    self.assertEqual(result.stdout, '')
                    self.assertNotIn('perf: hash tier', result.stderr)
                    self.assertEqual(output.read_text(encoding='utf-8'), 'previous result')

    def test_more_processed_frames_are_reported_as_an_improvement(self):
        result = self.run_harness('--tiers', 'worker')
        self.assertEqual(result.returncode, 0, result.stderr)
        rows = json.loads(result.stdout)
        throughput = [row for row in rows if row['metric'].startswith('worker-processed ')]
        self.assertEqual(len(throughput), 18)
        self.assertTrue(all(row['direction'] == 'higher' for row in throughput))

    def test_finite_duration_bounds_and_scientific_notation_are_accepted(self):
        for duration in ['0.5', '3.6e3']:
            with self.subTest(duration=duration):
                result = self.run_harness('--worker-seconds', duration)
                self.assertEqual(result.returncode, 0, result.stderr)
                self.assertEqual(len(json.loads(result.stdout)), 6)

    def test_unicode_output_path_is_written_as_utf8_json(self):
        with tempfile.TemporaryDirectory() as directory:
            output = pathlib.Path(directory) / 'wynik-Łódź-測定.json'
            result = self.run_harness('--out', str(output))
            self.assertEqual(result.returncode, 0, result.stderr)
            self.assertEqual(result.stdout, '')
            self.assertEqual(len(json.loads(output.read_text(encoding='utf-8'))), 6)

    def test_output_open_failure_is_reported(self):
        with tempfile.TemporaryDirectory() as directory:
            result = self.run_harness('--out', directory)
            self.assertEqual(result.returncode, 1)
            self.assertIn('cannot write', result.stderr)
            self.assertNotIn('perf: wrote', result.stderr)

    @unittest.skipUnless(pathlib.Path('/dev/full').exists(), 'requires a sink that rejects writes')
    def test_buffered_output_failure_is_reported(self):
        result = self.run_harness('--out', '/dev/full')
        self.assertEqual(result.returncode, 1)
        self.assertIn('cannot write', result.stderr)
        self.assertNotIn('perf: wrote', result.stderr)

    def test_help_does_not_execute_a_tier(self):
        result = self.run_harness('--help')
        self.assertEqual(result.returncode, 0)
        self.assertEqual(result.stdout, '')
        self.assertIn('usage:', result.stderr)
        self.assertNotIn('perf: hash tier', result.stderr)


if __name__ == '__main__':
    unittest.main()
