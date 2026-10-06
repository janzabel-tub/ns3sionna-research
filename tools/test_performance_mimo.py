#!/usr/bin/env python3
"""End-to-end checks for the performance runner, with no Sionna server required.

Build ns3sionna-performance-sionna-mimo first, then run this file with Python.
The fixed CFR is a diagnostic fixture, not a ray-traced performance prediction.
"""
import csv
import math
import os
from pathlib import Path
import subprocess
import tempfile
import unittest

MODULE = Path(__file__).resolve().parents[1]
NS3 = MODULE.parents[1]
RUNNER = MODULE / 'examples/performance-sionna-mimo.sh'


class PerformanceMimoTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        binaries = list((NS3 / 'build/contrib/sionna/examples').glob(
            'ns3.*-ns3sionna-performance-sionna-mimo-*'))
        if len(binaries) != 1:
            raise RuntimeError('Build ns3sionna-performance-sionna-mimo first')
        cls.binary = binaries[0]

    def invoke(self, directory, **options):
        settings = dict(channelSource='fixed', mobileSpeedMps=0, mcs=0,
                        eesmBetaLinear=2, simulationTimeSeconds=2.35,
                        outputDirectory=directory)
        settings.update(options)
        return subprocess.run([str(self.binary), *(
            f'--{key}={value}' for key, value in settings.items())],
            cwd=NS3, text=True, capture_output=True, timeout=60)

    def results(self, directory):
        with (directory / 'throughput_results.csv').open() as stream:
            rows = list(csv.DictReader(stream))
        with (directory / 'summary.csv').open() as stream:
            summary = list(csv.DictReader(stream))
        return rows, summary

    def validate_results(self, directory, snr=True):
        rows, summary = self.results(directory)
        self.assertEqual(len(rows), 14 * 3)  # 13 full intervals + 50 ms final interval
        self.assertEqual([int(row['link_id']) for row in summary], [0, 1, -1])
        for row in summary[:2]:
            self.assertGreater(int(row['received_bytes']), 0, f'no TCP delivery: {directory}')
        for group in range(0, len(rows), 3):
            a, b, total = rows[group:group + 3]
            self.assertEqual([int(r['link_id']) for r in (a, b, total)], [0, 1, -1])
            self.assertEqual(int(total['received_bytes']),
                             int(a['received_bytes']) + int(b['received_bytes']))
            self.assertAlmostEqual(float(total['throughput_mbps']),
                                   float(a['throughput_mbps']) + float(b['throughput_mbps']), places=6)
            for row in (a, b, total):
                self.assertAlmostEqual(float(row['throughput_mbps']),
                                       int(row['received_bytes']) * 8 / float(row['interval_s']) / 1e6,
                                       places=6)
                if int(row['snr_samples']):
                    self.assertTrue(math.isfinite(float(row['avg_snr_db'])))
                else:
                    self.assertTrue(math.isnan(float(row['avg_snr_db'])))
        for row in summary:
            link_rows = [r for r in rows if r['link_id'] == row['link_id']]
            self.assertEqual(sum(int(r['received_bytes']) for r in link_rows), int(row['received_bytes']))
            self.assertAlmostEqual(sum(float(r['interval_s']) for r in link_rows), 1.35)
            self.assertAlmostEqual(float(link_rows[-1]['interval_s']), 0.05)
            self.assertAlmostEqual(float(link_rows[-1]['time_s']), 2.35)
            if snr:
                self.assertGreater(sum(int(r['snr_samples']) for r in link_rows), 0)
            else:
                self.assertEqual(sum(int(r['snr_samples']) for r in link_rows), 0)
        return rows

    def test_antenna_width_and_beamforming_matrix(self):
        with tempfile.TemporaryDirectory(prefix='mimo-matrix-') as tmp:
            for antennas in (1, 2, 4):
                for width in (20, 40):
                    precoders = ('open-loop',) if antennas == 1 else ('open-loop', 'svd')
                    for precoding in precoders:
                        with self.subTest(antennas=antennas, width=width, precoding=precoding):
                            out = Path(tmp) / f'{antennas}-{width}-{precoding}'
                            result = self.invoke(out, antennaCount=antennas,
                                                 channelWidthMhz=width, precoding=precoding)
                            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
                            self.validate_results(out)

    def test_svd_multiple_streams_and_planar_array(self):
        with tempfile.TemporaryDirectory(prefix='mimo-svd-') as tmp:
            for streams in (2, 4):
                out = Path(tmp) / str(streams)
                result = self.invoke(out, antennaCount=4, arrayRows=2,
                                     streams=streams, precoding='svd', channelWidthMhz=40)
                self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
                self.validate_results(out)

    def test_repeatability_and_optional_snr(self):
        with tempfile.TemporaryDirectory(prefix='mimo-repeat-') as tmp:
            datasets = []
            for label, enabled in (('a', 'true'), ('b', 'true'), ('c', 'false')):
                out = Path(tmp) / label
                result = self.invoke(out, snr=enabled)
                self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
                datasets.append(self.validate_results(out, snr=enabled == 'true'))
            self.assertEqual(datasets[0], datasets[1])
            self.assertEqual([r['received_bytes'] for r in datasets[0]],
                             [r['received_bytes'] for r in datasets[2]])

    def test_reject_invalid_configuration_and_existing_results(self):
        invalid = [dict(channelWidthMhz=30), dict(antennaCount=4, arrayRows=3),
                   dict(antennaCount=2, eesmBetaLinear=0), dict(streams=1),
                   dict(antennaCount=1, precoding='svd'), dict(reportIntervalMs=0),
                   dict(simulationTimeSeconds=1), dict(numPairs=3),
                   dict(tcpVariant='ns3::Node'), dict(mobileSpeedMps=1)]
        with tempfile.TemporaryDirectory(prefix='mimo-invalid-') as tmp:
            for index, settings in enumerate(invalid):
                out = Path(tmp) / str(index)
                with self.subTest(settings=settings):
                    result = self.invoke(out, **settings)
                    self.assertNotEqual(result.returncode, 0)
                    self.assertFalse(out.exists())
            out = Path(tmp) / 'existing'
            self.assertEqual(self.invoke(out).returncode, 0)
            before = (out / 'throughput_results.csv').read_bytes()
            self.assertNotEqual(self.invoke(out).returncode, 0)
            self.assertEqual((out / 'throughput_results.csv').read_bytes(), before)

    def test_shell_sweep_from_another_directory(self):
        with tempfile.TemporaryDirectory(prefix='mimo-shell-') as tmp:
            env = dict(os.environ, ANTENNAS_LIST='1 2', WIDTHS_MHZ='20',
                       PRECODING_LIST='open-loop svd', SEEDS='1', SPEEDS_MPS='0',
                       TX_POWERS_DBM='20', MCS_LIST='0', EESM_BETA_LINEAR='2',
                       CHANNEL_SOURCE='fixed', BUILD='false', START_SERVER='false',
                       OUTPUT_BASE=str(Path(tmp) / 'outputs with spaces'),
                       SIMULATION_TIME_SECONDS='2.35', NUM_PAIRS='2', STREAMS='0',
                       ARRAY_ROWS='1', ENABLE_SNR='true')
            result = subprocess.run(['bash', str(RUNNER), '--offeredRate=100Mbps'],
                                    env=env, cwd=tmp, text=True, capture_output=True, timeout=90)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            configs = list((Path(tmp) / 'outputs with spaces').glob('*/config.txt'))
            self.assertEqual(len(configs), 3)
            for config in configs:
                self.assertIn('offeredRate=100Mbps', config.read_text())
                self.validate_results(config.parent)


if __name__ == '__main__':
    unittest.main(verbosity=2)
