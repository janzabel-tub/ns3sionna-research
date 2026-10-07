"""Storage experiment acceptance, immutable output, and failure provenance."""
import csv
import json
from pathlib import Path
import subprocess
import tempfile
import unittest
from unittest.mock import patch

import benchmark_mobility_storage as benchmark


def read_json(path):
    return json.loads(Path(path).read_text())


class TestStorageExperiment(unittest.TestCase):
    STEPS = 257

    @classmethod
    def setUpClass(cls):
        cls.temporary = tempfile.TemporaryDirectory(prefix='r3-storage-test-')
        cls.addClassCleanup(cls.temporary.cleanup)
        cls.output = Path(cls.temporary.name) / 'study'
        original_advance = benchmark.advance
        observed_plan = []

        def observe_advance(node, timestamp):
            if not observed_plan:
                # The plan must exist before the first measured update.
                observed_plan.append(read_json(cls.output / 'plan.json'))
            original_advance(node, timestamp)

        with patch.object(benchmark, 'advance', side_effect=observe_advance):
            benchmark.run(cls.output, steps=cls.STEPS)
        cls.plan_before_measurement = observed_plan[0]
        cls.plan = read_json(cls.output / 'plan.json')
        cls.manifest = read_json(cls.output / 'manifest.json')
        cls.run_record = read_json(cls.output / 'run.json')
        with (cls.output / 'storage.csv').open(newline='') as stream:
            cls.rows = list(csv.DictReader(stream))

    def test_complete_matrix_predeclared_plan_and_distinct_execution_verdict(self):
        self.assertEqual(self.plan_before_measurement, self.plan)
        self.assertEqual(self.plan['schema_version'], 1)
        self.assertEqual(self.plan['subphase'], 'R3.storage')
        for key, value in self.plan.items():
            self.assertEqual(self.manifest[key], value)
        self.assertEqual(self.manifest['plan_sha256'],
                         benchmark.provenance.digest(self.output / 'plan.json')['raw_sha256'])
        self.assertEqual(self.run_record['configuration'], self.plan)
        self.assertEqual(self.run_record['status'], 'success')
        self.assertIsNotNone(self.run_record['finished_utc'])
        validation = self.manifest['validation']
        self.assertEqual(self.run_record['validation'], validation)
        self.assertEqual(validation['verdict'], 'passed')
        self.assertEqual(validation['cases'], 15)
        self.assertEqual(validation['updates_compared'], 15 * self.STEPS)
        self.assertEqual(validation['checkpoint_rows'], 45)
        self.assertEqual(validation['exact_trajectory_control_rng'], 'passed')
        self.assertEqual(validation['bounded_storage_and_slot_reuse'], 'passed')
        self.assertEqual(validation['native_pool_fixture'], 'not requested')
        self.assertEqual(validation['fresh_rt_traces'], 0)
        self.assertEqual(validation['physical_doppler'], 'not assessed')
        self.assertFalse(self.manifest['controls']['scene_used_for_rt'])
        self.assertEqual({(case['limit'], case['mode']) for case in self.plan['cases']},
                         {(limit, mode) for limit in (0, 1, 8, 64, 1024)
                          for mode in ('WALL', 'TIME', 'DISTANCE')})
        for mode, seed in (('WALL', 42), ('TIME', 43), ('DISTANCE', 44)):
            for case in self.plan['cases']:
                if case['mode'] == mode:
                    self.assertEqual(case['seed'], seed)
                    mapping = case['randomness']
                    self.assertEqual((mapping['mapping_version'], mapping['ns3_seed'],
                                      mapping['ns3_run'], mapping['rt_stream'], mapping['mobility_stream']),
                                     (1, seed, 1, 0, 1))
        self.assertEqual(len(self.run_record['randomness']['configurations']), 3)

    def test_checkpoint_storage_counts_and_hashes_match_raw_artifacts(self):
        self.assertEqual(len(self.rows), 45)
        for case in self.plan['cases']:
            rows = [row for row in self.rows if row['case_id'] == case['case_id']]
            self.assertEqual([int(row['time']) for row in rows], [0, 256, 257])
            for row in rows:
                limit, timestamp = int(row['limit']), int(row['time'])
                self.assertEqual(limit, case['limit'])
                self.assertEqual(row['mode'], case['mode'])
                entries = min(limit, timestamp + 1)
                reuses = max(0, timestamp + 1 - limit) if limit else 0
                self.assertEqual(int(row['entries']), entries)
                self.assertEqual(int(row['bytes']), 48 * entries)
                self.assertEqual(int(row['allocations']), entries)
                self.assertEqual(int(row['reuses']), reuses)
                self.assertEqual(int(row['evictions']), reuses)
                self.assertGreaterEqual(float(row['elapsed_seconds']), 0)
                if case['mode'] == 'TIME' and limit:
                    self.assertEqual(int(row['overwrites']), timestamp // 7)
                if case['mode'] == 'WALL' or limit == 0:
                    self.assertEqual(int(row['overwrites']), 0)
        for result in self.manifest['results']:
            limit = result['limit']
            stats = result['storage']
            self.assertEqual(stats['entries'], min(limit, self.STEPS + 1))
            self.assertEqual(stats['bytes'], stats['entries'] * 48)
            self.assertEqual(stats['allocations'], stats['entries'])
            self.assertEqual(stats['reuses'], max(0, self.STEPS + 1 - limit) if limit else 0)
            self.assertEqual(stats['evictions'], stats['reuses'])
        for name in ('plan.json', 'manifest.json', 'storage.csv'):
            self.assertEqual(self.run_record['artifacts'][name],
                             benchmark.provenance.digest(self.output / name))
        for name in ('mobility.py', 'random_streams.py', 'requirements_cpu_ci.lock'):
            source = benchmark.SERVER / name
            self.assertEqual(self.run_record['inputs'][str(source.resolve())],
                             benchmark.provenance.digest(source))
            self.assertEqual(self.manifest['provenance']['source_sha256'][
                str(source.relative_to(benchmark.MODULE))],
                benchmark.provenance.digest(source)['raw_sha256'])

    def test_final_trajectory_control_and_rng_match_across_all_capacities(self):
        fields = ('final_position_m', 'final_velocity_m_s', 'final_control', 'final_rng_state')
        for mode in ('WALL', 'TIME', 'DISTANCE'):
            results = [result for result in self.manifest['results'] if result['mode'] == mode]
            self.assertEqual(len(results), 5)
            reference = next(result for result in results if result['limit'] == 0)
            for result in results:
                self.assertEqual(result['updates_compared'], self.STEPS)
                for field in fields:
                    self.assertEqual(result[field], reference[field])
            self.assertNotEqual(reference['final_position_m'], [1.0, 2.0, 3.0])
        # Elapsed time is deliberately absent from every numerical comparison.

    def test_historical_outputs_are_refused_without_changing_any_bytes(self):
        before = {path.relative_to(self.output): path.read_bytes()
                  for path in self.output.rglob('*') if path.is_file()}
        with self.assertRaisesRegex(ValueError, 'preserve existing measurements'):
            benchmark.run(self.output, steps=1)
        after = {path.relative_to(self.output): path.read_bytes()
                 for path in self.output.rglob('*') if path.is_file()}
        self.assertEqual(after, before)


class TestStorageFailureAndNativeFixture(unittest.TestCase):
    def test_advance_failure_keeps_plan_raw_partial_data_and_failed_execution(self):
        with tempfile.TemporaryDirectory(prefix='r3-storage-failure-') as temporary:
            output = Path(temporary) / 'study'

            def fail_advance(node, timestamp):
                self.assertTrue((output / 'plan.json').is_file())
                self.assertEqual(read_json(output / 'run.json')['status'], 'incomplete')
                raise RuntimeError('injected movement failure')

            with patch.object(benchmark, 'advance', side_effect=fail_advance):
                with self.assertRaisesRegex(RuntimeError, 'injected movement failure'):
                    benchmark.run(output, steps=1)
            record = read_json(output / 'run.json')
            self.assertEqual(record['status'], 'failed')
            self.assertEqual(record['validation'], {'verdict': 'not completed'})
            self.assertIsNotNone(record['finished_utc'])
            self.assertFalse((output / 'manifest.json').exists())
            self.assertIn('injected movement failure', (output / record['error_log']).read_text())
            with (output / 'storage.csv').open(newline='') as stream:
                rows = list(csv.DictReader(stream))
            self.assertEqual(len(rows), 1)
            self.assertEqual(rows[0]['time'], '0')
            self.assertIsNone(benchmark.provenance.ACTIVE_RUN.get())

    @staticmethod
    def subprocess_writer(text):
        def write(command, **kwargs):
            kwargs['stdout'].write(text)
            kwargs['stdout'].flush()
            return subprocess.CompletedProcess(command, 0)
        return write

    def test_native_summary_is_parsed_from_captured_existing_fixture_output(self):
        summary = {'requests': 512, 'allocations': 1, 'hits': 511,
                   'capacity_bytes': 1536, 'limit_bytes': 1048576}
        text = 'native diagnostic\nR3_POOL_SUMMARY ' + json.dumps(summary) + '\nPASS sionna\n'
        with tempfile.TemporaryDirectory(prefix='r3-native-summary-') as temporary:
            output = Path(temporary)
            binary = output / 'existing-test-runner'
            with patch.object(benchmark.subprocess, 'run', side_effect=self.subprocess_writer(text)) as native:
                self.assertEqual(benchmark.native_check(binary, output), summary)
            self.assertEqual((output / 'native.log').read_text(), text)
            command = native.call_args.args[0]
            self.assertEqual(command, [str(binary), '--suite=sionna', '--verbose'])
            self.assertEqual(native.call_args.kwargs['cwd'], benchmark.NS3)
            self.assertTrue(native.call_args.kwargs['check'])
            self.assertEqual(native.call_args.kwargs['stderr'], subprocess.STDOUT)

    def test_native_summary_rejects_bad_counts_capacity_missing_and_duplicates(self):
        accepted = {'requests': 512, 'allocations': 1, 'hits': 511,
                    'capacity_bytes': 1536, 'limit_bytes': 1048576}
        cases = []
        for key, value in (('requests', 511), ('allocations', 2), ('hits', 510),
                           ('capacity_bytes', -1), ('capacity_bytes', 1048577)):
            cases.append(('bad_' + key, 'R3_POOL_SUMMARY ' + json.dumps({**accepted, key: value}) + '\n'))
        line = 'R3_POOL_SUMMARY ' + json.dumps(accepted) + '\n'
        cases.extend((('missing', 'PASS sionna\n'), ('duplicate', line + line)))
        for label, text in cases:
            with self.subTest(label=label), tempfile.TemporaryDirectory(prefix='r3-native-reject-') as temporary:
                output = Path(temporary)
                with patch.object(benchmark.subprocess, 'run', side_effect=self.subprocess_writer(text)):
                    with self.assertRaises(AssertionError):
                        benchmark.native_check(output / 'existing-test-runner', output)
                self.assertEqual((output / 'native.log').read_text(), text)


if __name__ == '__main__':
    unittest.main()
