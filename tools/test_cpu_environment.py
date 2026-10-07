"""Environment report failures are explicit; checks do not install dependencies."""
from pathlib import Path
import tempfile
import unittest
from unittest.mock import patch

import check_cpu_environment as cpu


class TestCpuEnvironment(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory()
        self.addCleanup(self.temporary.cleanup)
        self.module = Path(self.temporary.name)
        self.lock = self.module / 'requirements.lock'
        self.lock.write_text('--extra-index-url https://example.invalid\n' +
                             '\n'.join(f'package{i}==1.0' for i in range(55)))
        proto = self.module / 'model/message.proto'
        proto.parent.mkdir()
        proto.write_text('syntax = "proto3";\n')
        for relative in ('model/message.pb.cc', 'model/message.pb.h',
                         'model/ns3sionna/common/message_pb2.py'):
            path = self.module / relative
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_text('generated binding\n')
        self.patches = [patch.object(cpu.platform, 'python_version', return_value='3.12.12'),
                       patch.object(cpu, 'installed', side_effect=lambda name: '26.0.1' if name == 'pip' else '1.0'),
                       patch.object(cpu.shutil, 'which', side_effect=lambda name: '/usr/bin/' + name),
                       patch.object(cpu, 'llvm_library', return_value={'library': 'libLLVM.so', 'loadable': True}),
                       patch.object(cpu, 'command', side_effect=self.good_command)]
        for mock in self.patches:
            mock.start()
            self.addCleanup(mock.stop)

    @staticmethod
    def good_command(args, **kwargs):
        return {'command': args, 'returncode': 0, 'stderr': '',
                'stdout': '{"variant":"llvm_ad_mono_polarized","llvm_evaluation":[2,3]}'
                if cpu.BACKEND_PROBE in args else 'available'}

    def report(self, strict=False):
        return cpu.check_environment(strict=strict, module=self.module, lock=self.lock)

    def test_exact_environment_passes_and_cpu_probe_uses_fresh_process(self):
        report = self.report(strict=True)
        self.assertEqual(report['status'], 'success')
        self.assertEqual(report['deviations'], [])
        calls = cpu.command.call_args_list
        self.assertFalse(any('install' in call.args[0] for call in calls))
        probe = next(call for call in calls if cpu.BACKEND_PROBE in call.args[0])
        self.assertEqual(probe.kwargs['env']['CUDA_VISIBLE_DEVICES'], '')
        self.assertEqual(probe.args[0][0], cpu.sys.executable)

    def test_pin_deviation_is_reported_and_strict_mode_rejects_it(self):
        with patch.object(cpu, 'installed', side_effect=lambda name: '25.0' if name == 'pip' else '1.0'):
            relaxed = self.report()
            strict = self.report(strict=True)
        self.assertEqual(relaxed['status'], 'success')
        self.assertIn('pip_exact', relaxed['deviations'])
        self.assertEqual(strict['status'], 'failed')
        self.assertIn('pip_exact', strict['failures'])

    def test_missing_package_is_reported_and_strictly_rejected(self):
        with patch.object(cpu, 'installed', side_effect=lambda name: None if name == 'package0' else
                          '26.0.1' if name == 'pip' else '1.0'):
            report = self.report(strict=True)
        self.assertIn('distribution:package0', report['failures'])
        observed = next(check['observed'] for check in report['checks'] if check['name'] == 'distribution:package0')
        self.assertIsNone(observed)

    def test_native_dependency_and_pip_conflicts_are_always_failures(self):
        def failed_command(args, **kwargs):
            if 'check' in args or 'libzmq' in args:
                return {'command': args, 'returncode': 1, 'stdout': '', 'stderr': 'dependency conflict'}
            return self.good_command(args, **kwargs)
        with patch.object(cpu, 'command', side_effect=failed_command):
            report = self.report()
        self.assertIn('pip_check', report['failures'])
        self.assertIn('native_package:libzmq', report['failures'])

    def test_missing_or_stale_bindings_fail_without_claiming_clean_build(self):
        import os
        path = self.module / 'model/message.pb.cc'
        os.utime(path, ns=(1, 1))
        (self.module / 'model/message.pb.h').unlink()
        report = self.report()
        self.assertIn('binding:model/message.pb.cc', report['failures'])
        self.assertIn('binding:model/message.pb.h', report['failures'])

    def test_wrong_or_broken_backend_is_an_explicit_failure(self):
        for stdout, returncode in [('{"variant":"cuda_ad_mono_polarized","llvm_evaluation":null}', 0),
                                   ('import error', 1), ('', 0),
                                   ('{"variant":"llvm_ad_mono_polarized","llvm_evaluation":[9,9]}', 0)]:
            def backend_command(args, **kwargs):
                if cpu.BACKEND_PROBE in args:
                    return {'command': args, 'returncode': returncode, 'stdout': stdout, 'stderr': ''}
                return self.good_command(args, **kwargs)
            with patch.object(cpu, 'command', side_effect=backend_command):
                self.assertIn('actual_cpu_backend', self.report()['failures'])

    def test_invalid_or_missing_lock_still_emits_failed_report(self):
        self.lock.write_text('unlocked>=1\n')
        self.assertIn('cpu_lock', self.report()['failures'])
        self.lock.unlink()
        self.assertIn('cpu_lock', self.report()['failures'])

    def test_missing_compiler_or_llvm_is_required(self):
        with patch.object(cpu.shutil, 'which', return_value=None), patch.object(cpu, 'llvm_library',
                return_value={'library': None, 'loadable': False}):
            report = self.report()
        self.assertIn('tool:gcc', report['failures'])
        self.assertIn('llvm_library', report['failures'])


if __name__ == '__main__':
    unittest.main()
