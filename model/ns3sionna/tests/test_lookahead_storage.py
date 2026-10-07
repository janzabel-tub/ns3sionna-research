"""Full look-ahead orchestration with deterministic frozen path responses."""
from contextlib import ExitStack, redirect_stdout
from copy import deepcopy
import io
import unittest
from unittest.mock import patch

import numpy as np
from common import message_pb2
from mobility import ConstantMobility, RandomWalkMobility
from mimo_data_contract import ResourceLimits, RX_ARRAY_DEFAULTS, TX_ARRAY_DEFAULTS
import ns3sionna_server as server
from test_deterministic_mobility import _NoHitScene


class _Radio:
    def __init__(self, name, position, **kwargs):
        self.name, self.position = name, np.array(position)


class _Scene:
    def __init__(self):
        self.radios = {}
        self.mi_scene = _NoHitScene()

    def add(self, radio):
        self.radios[radio.name] = radio

    def remove(self, name):
        del self.radios[name]


class _Paths:
    def __init__(self, scene):
        self.receivers = [radio for name, radio in scene.radios.items() if name.startswith('rx')]

    def cir(self, **kwargs):
        return None, np.full((len(self.receivers), 1), 1e-8)

    def cfr(self, **kwargs):
        # A frozen fixture encodes placement in magnitude; this is not real RT.
        return np.asarray([np.full((1, 1, 1, 1, 8),
            (1 + np.linalg.norm(radio.position)) * 1e-4 + 1e-4j)
            for radio in self.receivers])


class LookaheadStorageTest(unittest.TestCase):
    def env(self, limit=1, local_rng=True, absolute=True):
        env = server.SionnaEnv(rt_fast=True, VERBOSE=False, mobility_history_limit=limit,
                              equidistant_walk_steps=1_000_000 if absolute else 0)
        env.scene = _Scene()
        env.node_info = {0: ConstantMobility(0, [3, 2, 2], history_limit=limit),
            1: RandomWalkMobility(1, [4, 3, 1], RandomWalkMobility.MODE_TIME, 5_000_000,
                RandomWalkMobility.SPEED_CONSTANT, (1,), RandomWalkMobility.DIRECTION_UNIFORM,
                (-np.pi, np.pi), rng=np.random.default_rng(73) if local_rng else None,
                history_limit=limit)}
        env.node_orientations = {0: [0, 0, 0], 1: [0, 0, 0]}
        env.rx_array_config, env.tx_array_config = RX_ARRAY_DEFAULTS, TX_ARRAY_DEFAULTS
        env.sim_time, env.sub_mode, env.fc = 0, 16, 5.18e9
        env.fft_size = 8
        env.time_evo_model = 'position'
        env.fixed_coherence_time_ns = 5_000_000
        env.frequencies, env.my_seed = np.arange(8, dtype=int), 42
        return env

    def compute(self, env, time=1_000_000, fail=None):
        response = message_pb2.Wrapper()
        with ExitStack() as stack:
            stack.enter_context(redirect_stdout(io.StringIO()))
            stack.enter_context(patch.object(server, 'Transmitter', _Radio))
            stack.enter_context(patch.object(server, 'Receiver', _Radio))
            factory = lambda: lambda scene, **kwargs: _Paths(scene)
            stack.enter_context(patch.object(server, 'PathSolver', side_effect=factory))
            if fail == 'placement':
                stack.enter_context(patch.object(env, '_place_tx_rx_nodes_with_lah', side_effect=RuntimeError('placement')))
            elif fail == 'cfr':
                stack.enter_context(patch.object(_Paths, 'cfr', side_effect=RuntimeError('CFR')))
            env.compute_cfr_with_lookahead(message_pb2.ChannelStateRequest(tx_node=0, rx_node=1, time=time), response)
        return response

    def test_prediction_exceeds_history_cap_without_losing_any_epoch(self):
        small, large = self.env(1), self.env(1024)
        original = small.node_info
        for time in (1_000_000, 12_000_000, 30_000_000):
            first, second = self.compute(small, time), self.compute(large, time)
            self.assertEqual(first.SerializeToString(deterministic=True), second.SerializeToString(deterministic=True))
            self.assertIs(small.node_info, original)
            self.assertEqual([c.start_time for c in first.channel_state_response.csi],
                             [time + i * 5_000_000 for i in range(16)])
            np.testing.assert_array_equal(small.node_info[1].pos, large.node_info[1].pos)
            self.assertEqual(small.node_info[1].rng.bit_generator.state, large.node_info[1].rng.bit_generator.state)
            stats = small.mobility_storage_statistics()
            self.assertEqual(stats['history_entries'], 2)
            self.assertEqual(stats['history_bytes'], 96)
            self.assertEqual(stats['peak_lookahead_states'], 16)
            self.assertEqual(stats['lookahead_states'], 0)
            self.assertEqual(stats['prediction_nodes'], 0)

    def test_no_history_mode_produces_identical_lookahead(self):
        self.assertEqual(self.compute(self.env(0)).SerializeToString(deterministic=True),
                         self.compute(self.env(1024)).SerializeToString(deterministic=True))

    def test_failure_restores_committed_pose_history_and_rng_and_retry(self):
        for failure in ('placement', 'cfr'):
            env, reference = self.env(2), self.env(2)
            reference._update_nodes_to_time(12_000_000, [0, 1])
            committed = env.node_info
            rng_state = np.random.get_state()
            with self.assertRaises(RuntimeError):
                self.compute(env, 12_000_000, fail=failure)
            self.assertIs(env.node_info, committed)
            self.assertEqual(env.sim_time, 12_000_000)
            np.testing.assert_array_equal(env.node_info[1].pos, reference.node_info[1].pos)
            np.testing.assert_array_equal(env.node_info[1].velocity, reference.node_info[1].velocity)
            self.assertEqual(env.node_info[1].rng.bit_generator.state, reference.node_info[1].rng.bit_generator.state)
            self.assertEqual(env.node_info[1].history_statistics(), reference.node_info[1].history_statistics())
            np.testing.assert_array_equal(np.random.get_state()[1], rng_state[1])
            self.assertEqual(env.mobility_storage_statistics()['lookahead_states'], 0)
            self.assertEqual(self.compute(env, 12_000_000).SerializeToString(deterministic=True),
                             self.compute(reference, 12_000_000).SerializeToString(deterministic=True))

    def test_legacy_global_rng_prediction_draws_are_restored(self):
        original = np.random.get_state()
        self.addCleanup(np.random.set_state, original)
        for absolute in (False, True):
            np.random.seed(42)
            env = self.env(1, local_rng=False, absolute=absolute)
            env._update_nodes_to_time(12_000_000, [0, 1])
            env.sim_time = 12_000_000
            before = np.random.get_state()
            first = self.compute(env, 12_000_000)
            after = np.random.get_state()
            np.testing.assert_array_equal(before[1], after[1])
            self.assertEqual(before[2:], after[2:])
            self.assertEqual(first.SerializeToString(deterministic=True),
                             self.compute(env, 12_000_000).SerializeToString(deterministic=True))

    def test_budget_rejection_precedes_any_committed_motion(self):
        env = self.env(1)
        env.resource_limits = ResourceLimits(max_snapshots=15)
        before = deepcopy(env.node_info[1].rng.bit_generator.state)
        with self.assertRaises(ValueError):
            self.compute(env, 12_000_000)
        self.assertEqual(env.node_info[1].last_update, 0)
        self.assertEqual(env.node_info[1].rng.bit_generator.state, before)
        self.assertEqual(env.mobility_storage_statistics()['peak_lookahead_states'], 0)

    def test_history_export_does_not_alias_reused_slots(self):
        env = self.env(1)
        _, saved = env._get_mobility_history(1)
        original = saved[0].copy()
        env._update_nodes_to_time(20_000_000, [0, 1])
        np.testing.assert_array_equal(saved[0], original)
        saved[0][:] = 999
        self.assertFalse(np.all(env.node_info[1].pos == 999))


if __name__ == '__main__':
    unittest.main()
