"""Native tuple export, legacy identity, repeatability and draw isolation."""
from pathlib import Path
import tempfile
import unittest
from unittest.mock import patch

import numpy as np
import benchmark_sumimo as base
from random_streams import mobility_rng, resolve_randomness
from common import message_pb2


class RandomStreamsTest(unittest.TestCase):
    def config(self, seed=42, run=1, rt=0, mobility=1):
        config = message_pb2.SimInitMessage(seed=seed)
        config.randomness.mapping_version = 1
        config.randomness.ns3_seed = seed
        config.randomness.ns3_run = run
        config.randomness.rt_stream = rt
        config.randomness.mobility_stream = mobility
        return config

    def test_legacy_and_default_rt_seeds_unchanged(self):
        for seed in (42, 43, 44):
            legacy = resolve_randomness(message_pb2.SimInitMessage(seed=seed))
            current = resolve_randomness(self.config(seed))
            self.assertEqual(legacy['rt_seed'], seed)
            self.assertEqual(current['rt_seed'], seed)
            self.assertIsNone(legacy['ns3_run'])
            np.testing.assert_array_equal(mobility_rng(legacy, 3).random(16),
                np.random.default_rng(np.random.SeedSequence([seed, 3, 0x51A0])).random(16))

    def test_run_stream_and_node_draw_isolation(self):
        default = resolve_randomness(self.config())
        rt_changed = resolve_randomness(self.config(rt=2))
        mobile_changed = resolve_randomness(self.config(mobility=3))
        run_changed = resolve_randomness(self.config(run=2))
        self.assertNotEqual(default['rt_seed'], rt_changed['rt_seed'])
        self.assertEqual(default['mobility_seed'], rt_changed['mobility_seed'])
        self.assertEqual(default['rt_seed'], mobile_changed['rt_seed'])
        self.assertNotEqual(default['mobility_seed'], mobile_changed['mobility_seed'])
        self.assertNotEqual(default['rt_seed'], run_changed['rt_seed'])
        self.assertNotEqual(default['mobility_seed'], run_changed['mobility_seed'])
        first = mobility_rng(default, 3).random(16)
        mobility_rng(default, 9).random(1000)
        np.testing.assert_array_equal(first, mobility_rng(default, 3).random(16))
        self.assertFalse(np.array_equal(first, mobility_rng(default, 9).random(16)))
        # Fixed portable SHA-256 convention independently recomputed.
        import hashlib
        expected = int.from_bytes(hashlib.sha256(b'ns3sionna-rng-v1:42:2:0:rt').digest()[:4], 'big')
        self.assertEqual(run_changed['rt_seed'], expected)

    def test_invalid_and_unknown_mapping_rejected_before_loading_scene(self):
        from ns3sionna_server import SionnaEnv
        for config in (self.config(seed=0), self.config(run=0), self.config(rt=1),
                       self.config(rt=1 << 63)):
            with self.assertRaises(ValueError):
                resolve_randomness(config)
        config = self.config()
        config.randomness.mapping_version = 99
        with patch('ns3sionna_server.load_scene', side_effect=AssertionError('Must reject first')):
            env = SionnaEnv(VERBOSE=False)
            success, reason = env.init_simulation_env(config)
            self.assertFalse(success)
            self.assertIn('version', reason)

    def test_native_tuple_and_overflow_rejection(self):
        with tempfile.TemporaryDirectory() as directory:
            output = Path(directory) / 'config.pb'
            with base.provenance.Run(Path(directory) / 'run.json', {}) as recorded:
                config = base.export_configuration(base.executable(), output, 0.5,
                                                    seed=43, run=7, stream=12)
                mapping = resolve_randomness(config)
                self.assertEqual(recorded.data['randomness']['configurations'], [mapping])
                base.provenance.randomness_selected(config)
                self.assertEqual(recorded.data['randomness']['configurations'], [mapping])
            self.assertEqual((mapping['ns3_seed'], mapping['ns3_run'],
                              mapping['rt_stream'], mapping['mobility_stream']), (43, 7, 12, 13))
            before = output.read_bytes()
            import subprocess
            with self.assertRaises(subprocess.CalledProcessError):
                base.native(base.executable(), action='config', output=output, stream=-1)
            self.assertEqual(before, output.read_bytes())


if __name__ == '__main__':
    unittest.main()
