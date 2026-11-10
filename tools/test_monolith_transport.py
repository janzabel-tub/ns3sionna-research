#!/usr/bin/env python3
"""Server-free interface checks for TCP/UDP monoliths and both sweep runners."""
import os
from pathlib import Path
import shlex
import subprocess
import tempfile
import unittest

MODULE = Path(__file__).resolve().parents[1]
NS3 = MODULE.parents[1]


class TransportTest(unittest.TestCase):
    def dry_run(self, runner, *options, **env):
        with tempfile.TemporaryDirectory() as tmp:
            settings = dict(os.environ)
            settings.pop('TRANSPORT', None)
            settings.update(OUTPUT_DIR=tmp + '/unused', BUILD='false', **env)
            result = subprocess.run(['bash', str(MODULE / 'examples' / runner), '--dry-run', *options],
                                    env=settings,
                                    cwd=tmp, text=True, capture_output=True, check=True)
            self.assertFalse((Path(tmp) / 'unused').exists())
            commands = [shlex.split(line) for line in result.stdout.splitlines() if './ns3' in line]
            return [{arg.split('=', 1)[0]: arg.split('=', 1)[1]
                     for arg in command if arg.startswith('--') and '=' in arg} for command in commands]

    def test_runner_defaults_and_cli_override(self):
        for family in ('mimo', 'mumimo'):
            runner = f'performance-sionna-{family}-monolith.sh'
            self.assertEqual(self.dry_run(runner)[0]['--transport'], 'tcp')
            self.assertEqual(self.dry_run(runner, TRANSPORT='udp')[0]['--transport'], 'udp')
            self.assertEqual(self.dry_run(runner, '--transport=tcp', TRANSPORT='udp')[0]['--transport'], 'tcp')

    def test_udp_reaches_every_sweep_case(self):
        for runner in ('performance-sionna-sumimo-antenna-sweep.sh',
                       'performance-sionna-mumimo-sta-sweep.sh'):
            commands = self.dry_run(runner, TRANSPORT='udp')
            self.assertEqual(len(commands), 8)
            self.assertTrue(all(command['--transport'] == 'udp' for command in commands))
            for n, command in enumerate(commands, 1):
                if 'antenna-sweep' in runner:
                    self.assertEqual(command['--antennaCount'], str(n))
                    self.assertEqual(command['--streams'], str(n))
                else:
                    self.assertEqual(command['--sim_min_stas'], str(n))
                    self.assertEqual(command['--antennaCount'], '8')

    def test_native_rejects_invalid_transport_before_server_or_output(self):
        for family in ('mimo', 'mumimo'):
            binaries = list((NS3 / 'build/contrib/sionna/examples').glob(
                f'ns3.*-ns3sionna-performance-sionna-{family}-monolith-*'))
            self.assertEqual(len(binaries), 1, 'Build both monolith targets first')
            with tempfile.TemporaryDirectory() as tmp:
                output = Path(tmp) / 'unused'
                result = subprocess.run([str(binaries[0]), '--transport=invalid',
                                         f'--outputDirectory={output}'],
                                        cwd=NS3, text=True, capture_output=True, timeout=10)
                self.assertNotEqual(result.returncode, 0)
                self.assertIn('transport must be tcp or udp', result.stdout + result.stderr)
                self.assertFalse(output.exists())


if __name__ == '__main__':
    unittest.main(verbosity=2)
