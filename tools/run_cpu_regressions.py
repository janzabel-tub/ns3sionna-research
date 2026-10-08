#!/usr/bin/env python3
"""Reuse existing CPU tests; exclude only the explicitly expensive RT checks."""
import os
from pathlib import Path
import subprocess
import sys
import unittest

MODULE = Path(__file__).resolve().parents[1]
NS3 = MODULE.parents[1]
SERVER = MODULE / 'model/ns3sionna'
os.environ['CUDA_VISIBLE_DEVICES'] = ''
sys.path.insert(0, str(SERVER))
sys.path.insert(0, str(SERVER / 'tests'))


def flatten(suite):
    for item in suite:
        if isinstance(item, unittest.TestSuite):
            yield from flatten(item)
        else:
            yield item


class DiagnosticResult(unittest.TextTestResult):
    def addError(self, test, error):
        super().addError(test, error)
        exception = error[1]
        if isinstance(exception, subprocess.SubprocessError):
            self.stream.writeln(f'Native stdout: {getattr(exception, "stdout", None)}')
            self.stream.writeln(f'Native stderr: {getattr(exception, "stderr", None)}')


def main():
    runner = list((NS3 / 'build/utils').glob('*test-runner-*'))
    if len(runner) != 1:
        raise RuntimeError('Build test-runner and sionna-test first')
    for suite in ('sionna', 'sionna-mimo-csi-feedback', 'dl-mu-mimo-runtime',
                  'dl-mu-mimo-ru', 'he-dl-mu-spectrum-ru', 'ul-mu-mimo',
                  'he-ul-mu-mimo-phy-adapter'):
        subprocess.run([str(runner[0]), '--suite=' + suite, '--verbose'], check=True, cwd=NS3)
    subprocess.run([sys.executable, str(SERVER / 'tests/test_imports.py')], check=True)
    # Import checks must establish the actual RT backend in this process too.
    import benchmark_sumimo
    benchmark_sumimo.runtime()
    modules = ('test_cpu_environment', 'test_random_streams', 'test_experiment_bundle', 'test_provenance', 'test_calibrate_eesm', 'test_benchmark_sumimo',
               'test_benchmark_rt_convergence', 'test_benchmark_cache_ipc', 'test_benchmark_mobility_storage', 'test_benchmark_live_scaling', 'test_benchmark_physical_aging', 'test_benchmark_backends',
               'test_mimo_data_contract', 'test_mobility_storage', 'test_lookahead_storage',
               'test_deterministic_mobility', 'test_coherence_time_units')
    tests = unittest.TestLoader().loadTestsFromNames(modules)
    selected = unittest.TestSuite()
    for test in flatten(tests):
        if (type(test).__name__ == 'TestServerGeometry' or
                test._testMethodName == 'test_fresh_process_real_traces_match_all_budgets_and_seeds'):
            print('Separate RT validation:', test.id(), flush=True)
        else:
            selected.addTest(test)
    return 0 if unittest.TextTestRunner(verbosity=2, resultclass=DiagnosticResult).run(selected).wasSuccessful() else 1


if __name__ == '__main__':
    raise SystemExit(main())
