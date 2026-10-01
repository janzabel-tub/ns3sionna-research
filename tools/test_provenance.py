"""Portable identity, immutable legacy exports, and actual runner failure paths."""
import json
from pathlib import Path
import subprocess
import tempfile
import unittest
from unittest.mock import patch

import provenance as p
import benchmark_sumimo as base
import benchmark_rt_convergence as convergence
import benchmark_cache_ipc as cache


class TestProvenance(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory()
        self.addCleanup(self.temporary.cleanup)
        self.root = Path(self.temporary.name)

    def test_text_only_normalizes_crlf_and_reports_raw_difference(self):
        path = self.root / 'input.json'
        path.write_bytes(b'{"v":1}\n')
        expected = p.digest(path)
        path.write_bytes(b'{"v":1}\r\n')
        self.assertFalse(p.verify_digest(path, expected))
        for raw in (b'{"v":2}\r\n', b'{"v":1} \r\n', b'{"v":1}', b'{"v":1}\r'):
            path.write_bytes(raw)
            with self.assertRaises(ValueError):
                p.verify_digest(path, expected)
        path.write_bytes(b'\xff')
        with self.assertRaises(UnicodeDecodeError):
            p.digest(path)
        with self.assertRaises(ValueError):
            p.verify_digest(path, {**expected, 'text_hash_version': 2})

    def test_binary_is_always_raw_including_line_endings(self):
        path = self.root / 'channel.pb'
        path.write_bytes(b'\xff\n')
        expected = p.digest(path)
        self.assertNotIn('lf_sha256', expected)
        path.write_bytes(b'\xff\r\n')
        with self.assertRaises(ValueError):
            p.verify_digest(path, expected)

    def test_success_records_measurement_sources_inputs_build_and_artifacts(self):
        source = self.root / 'input.csv'
        source.write_text('value\n1\n')
        result = self.root / 'result.json'
        plot = self.root / 'plot.png'
        path = self.root / 'run.json'
        with p.Run(path, {'seed': 42}, [source], binary=source, artifacts=[result, plot]) as run:
            self.assertEqual(json.loads(path.read_text())['status'], 'incomplete')
            p.backend_selected('frozen-protobuf-local-zmq')
            p.native_command(['test-native', '--seed=42'])
            measurement = run.data['measurement_sources']
            result.write_text('{"accepted":false}\n')
            plot.write_bytes(b'\x89PNG\r\n')
        data = json.loads(path.read_text())
        self.assertEqual(data['status'], 'success')
        self.assertEqual(data['measurement_sources'], measurement)
        self.assertEqual(data['artifacts']['result.json'], p.digest(result))
        self.assertEqual(data['artifacts']['plot.png'], p.digest(plot))
        self.assertNotIn('lf_sha256', data['artifacts']['plot.png'])
        self.assertEqual(data['inputs'][str(source.resolve())], p.digest(source))
        self.assertEqual(data['native_commands'], [['test-native', '--seed=42']])
        self.assertIsNotNone(data['finished_utc'])
        self.assertFalse(path.with_suffix('.json.tmp').exists())
        with self.assertRaises(ValueError):
            with p.Run(path, {}):
                pass

    def test_failed_and_interrupted_status_preserve_logs(self):
        for error, status in ((subprocess.CalledProcessError(2, 'native', output='native out',
                              stderr='native diagnostic'), 'failed'), (KeyboardInterrupt(), 'incomplete')):
            path = self.root / (status + '.json')
            with self.assertRaises(type(error)):
                with p.Run(path, {}):
                    raise error
            data = json.loads(path.read_text())
            self.assertEqual(data['status'], status)
            log = (self.root / data['error_log']).read_text()
            self.assertIn(type(error).__name__, log)
            if status == 'failed':
                self.assertIn('native diagnostic', log)
                self.assertIn('native out', log)

    def test_failed_setup_resets_context_and_records_input_error(self):
        path = self.root / 'run.json'
        with self.assertRaises(FileNotFoundError):
            with p.Run(path, {}, inputs=[self.root / 'missing.pb']):
                self.fail('Missing input must fail before execution')
        self.assertEqual(json.loads(path.read_text())['status'], 'failed')
        self.assertIsNone(p.ACTIVE_RUN.get())

    def test_rejected_backend_is_recorded_as_failed_without_fallback(self):
        base.runtime()  # Import/select the real supported CPU backend first.
        path = self.root / 'run.json'
        with self.assertRaises(RuntimeError):
            with p.Run(path, {}, artifacts=[]):
                with patch('mitsuba.variant', return_value='cuda_ad_mono_polarized'):
                    base.runtime()
        data = json.loads(path.read_text())
        self.assertEqual(data['status'], 'failed')
        self.assertEqual(data['backend'], 'cuda_ad_mono_polarized')

    def test_bad_artifact_cannot_leave_a_success_marker(self):
        path = self.root / 'run.json'
        result = self.root / 'bad.json'
        with self.assertRaises(UnicodeDecodeError):
            with p.Run(path, {}, artifacts=[result]):
                result.write_bytes(b'\xff')
        self.assertEqual(json.loads(path.read_text())['status'], 'failed')
        self.assertIsNone(p.ACTIVE_RUN.get())

    def test_all_benchmark_entrypoints_write_failure_and_refuse_resume(self):
        for function, args, dependency in ((base.run, (2,), 'benchmark_sumimo.runtime'),
                (convergence.run, (), 'benchmark_sumimo.runtime'),
                (cache.run, (), 'benchmark_cache_ipc.template')):
            output = self.root / function.__module__
            with patch(dependency, side_effect=RuntimeError('injected backend or fixture failure')):
                with self.assertRaises(RuntimeError):
                    function(output, base.executable(), *args)
            data = json.loads((output / 'run.json').read_text())
            self.assertEqual(data['status'], 'failed')
            self.assertIn('injected', (output / data['error_log']).read_text())
            previous = (output / 'run.json').read_bytes()
            with self.assertRaises(ValueError):
                function(output, base.executable(), *args)
            self.assertEqual((output / 'run.json').read_bytes(), previous)

    def test_legacy_measurement_manifests_are_not_rewritten_by_replay(self):
        # Current replay identity belongs to the sidecar, never old source_sha256.
        for study in ('phase5_2_spacing_orientation', 'phase5_2_sample_convergence', 'phase5_2_cache_ipc'):
            path = base.MODULE / 'data' / study / 'manifest.json'
            raw = path.read_bytes()
            old = json.loads(raw)
            self.assertEqual(old['schema_version'], 1)
            self.assertIn('source_sha256', old['provenance'])
            sidecar = self.root / (study + '.json')
            with p.Run(sidecar, {'replay_of': p.digest(path)}, inputs=[path], artifacts=[]):
                pass
            self.assertEqual(path.read_bytes(), raw)
            self.assertNotEqual(json.loads(sidecar.read_text())['measurement_sources'], old['provenance'])


if __name__ == '__main__':
    unittest.main()
