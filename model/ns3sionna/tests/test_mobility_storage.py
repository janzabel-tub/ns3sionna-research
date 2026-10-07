"""Bounded history ownership, reuse, and prediction-state regression tests."""

import json
import unittest
from copy import deepcopy

import numpy as np

from mobility import ConstantMobility, RandomWalkMobility


def _node(limit=1024, collect=True, seed=73, mode=RandomWalkMobility.MODE_TIME,
          mode_params=5, node_class=RandomWalkMobility, rng=None):
    return node_class(
        1, [1, 2, 3], mode, mode_params,
        RandomWalkMobility.SPEED_UNIFORM, [0.5, 1.5],
        RandomWalkMobility.DIRECTION_UNIFORM, [-np.pi, np.pi],
        rng=np.random.default_rng(seed) if rng is None else rng,
        collect_history=collect, history_limit=limit,
    )


class _UnboundedReference(RandomWalkMobility):
    """The original recording behavior, sharing only the motion implementation."""

    def _record_history(self, sim_time):
        self.pos_history[sim_time] = self.pos.copy()
        self.velo_history[sim_time] = self.velocity.copy()


class MobilityHistoryStorageTest(unittest.TestCase):
    def test_default_and_positional_constructor_compatibility(self):
        fixed = ConstantMobility(4, [1, 2, 3], True, 2)
        self.assertEqual(fixed.history_statistics()['limit'], 2)
        mobile = RandomWalkMobility(
            1, [0, 0, 0], RandomWalkMobility.MODE_WALL, True,
            RandomWalkMobility.SPEED_CONSTANT, (1,),
            RandomWalkMobility.DIRECTION_CONSTANT, (0,), None, True, 3,
        )
        self.assertEqual(mobile.history_statistics()['limit'], 3)
        self.assertEqual(_node().history_statistics()['limit'], 1024)
        self.assertEqual(_node(limit=np.int64(2)).history_limit, 2)

    def test_limit_requires_nonnegative_integer(self):
        for invalid in (-1, 1.0, True, False, '2', None, np.nan):
            with self.subTest(invalid=invalid):
                with self.assertRaisesRegex(ValueError, 'nonnegative integer'):
                    _node(limit=invalid)

    def test_wrap_reuses_exact_pairs_and_keeps_histories_synchronized(self):
        node = _node(limit=3, mode=RandomWalkMobility.MODE_WALL)
        for time in (1, 2):
            node.update_pos(time, [time, 2, 3], [4, time, 0], False)
        original_pairs = {
            time: (id(node.pos_history[time]), id(node.velo_history[time]))
            for time in node.pos_history
        }
        pool_ids = set(original_pairs.values())
        for time in range(3, 30):
            oldest = next(iter(node.pos_history))
            pair = (id(node.pos_history[oldest]), id(node.velo_history[oldest]))
            node.update_pos(time, [time, 2, 3], [4, time, 0], False)
            self.assertEqual(pair, (id(node.pos_history[time]), id(node.velo_history[time])))
            self.assertEqual(list(node.pos_history), list(node.velo_history))
            self.assertEqual(list(node.pos_history), list(range(time - 2, time + 1)))
            self.assertEqual(pool_ids, {
                (id(node.pos_history[t]), id(node.velo_history[t])) for t in node.pos_history
            })
            for recorded_time in node.pos_history:
                np.testing.assert_array_equal(node.pos_history[recorded_time], [recorded_time, 2, 3])
                np.testing.assert_array_equal(node.velo_history[recorded_time], [4, recorded_time, 0])
        self.assertEqual(node.history_statistics(), {
            'enabled': True, 'limit': 3, 'entries': 3, 'bytes': 144,
            'allocations': 3, 'reuses': 27, 'overwrites': 0, 'evictions': 27,
        })
        json.dumps(node.history_statistics(), allow_nan=False)

    def test_repeated_time_and_velocity_changes_overwrite_without_allocation(self):
        for mode, period in ((RandomWalkMobility.MODE_TIME, 1),
                             (RandomWalkMobility.MODE_DISTANCE, 0.1)):
            with self.subTest(mode=mode):
                node = _node(limit=2, mode=mode, mode_params=period)
                node.update_pos(1, [2, 3, 4], [1, 0, 0], False)
                pair = (id(node.pos_history[1]), id(node.velo_history[1]))
                node.check_set_new_velocity(1, 0.2)
                np.testing.assert_array_equal(node.velo_history[1], node.velocity)
                for _ in range(20):
                    node.update_pos(1, [4, 5, 6], [0, 1, 0], True)
                self.assertEqual(pair, (id(node.pos_history[1]), id(node.velo_history[1])))
                stats = node.history_statistics()
                self.assertEqual(stats['allocations'], 2)
                self.assertEqual(stats['entries'], 2)
                self.assertEqual(stats['overwrites'], 21)
                self.assertEqual(stats['reuses'], 0)
                self.assertEqual(stats['evictions'], 0)
                self.assertEqual(stats['bytes'], 96)
                np.testing.assert_array_equal(node.pos_history[1], [4, 5, 6])

    def test_disabled_history_has_current_getters_and_no_storage(self):
        for limit, collect in ((0, True), (2, False), (0, False)):
            with self.subTest(limit=limit, collect=collect):
                node = _node(limit=limit, collect=collect, mode_params=1)
                for time in range(1, 20):
                    node.update_pos(time, [time, 2, 3], [1, 2, 0], False)
                    node.check_set_new_velocity(time, 1)
                stats = node.history_statistics()
                self.assertFalse(stats['enabled'])
                for key in ('entries', 'bytes', 'allocations', 'reuses', 'overwrites', 'evictions'):
                    self.assertEqual(stats[key], 0)
                self.assertEqual(node.pos_history, {})
                self.assertEqual(node.velo_history, {})
                np.testing.assert_array_equal(node.get_pos_at(19), node.pos)
                np.testing.assert_array_equal(node.get_velo_at(19), node.velocity)
                for getter in (node.get_pos_at, node.get_velo_at):
                    with self.assertRaises(KeyError):
                        getter(18)
                    with self.assertRaises(KeyError):
                        getter(20)

    def test_getters_and_inputs_do_not_alias_live_or_retained_vectors(self):
        node = _node(limit=2)
        position, velocity = np.array([3., 4., 5.]), np.array([1., 2., 3.])
        node.update_pos(1, position, velocity, False)
        position[:] = 99
        velocity[:] = 99
        node.update_pos(2, [6, 7, 8], [4, 5, 6], False)
        for time in (1, 2):
            for getter, history, live in ((node.get_pos_at, node.pos_history, node.pos),
                                          (node.get_velo_at, node.velo_history, node.velocity)):
                value = getter(time)
                expected = value.copy()
                self.assertFalse(np.shares_memory(value, history[time]))
                self.assertFalse(np.shares_memory(value, live))
                value[:] = -99
                np.testing.assert_array_equal(getter(time), expected)
        with self.assertRaises(KeyError):
            node.get_pos_at(0)
        with self.assertRaises(KeyError):
            node.get_velo_at(0)
        for collect in (True, False):
            fixed = ConstantMobility(2, [1, 2, 3], collect)
            for getter, live in ((fixed.get_pos_at, fixed.pos), (fixed.get_velo_at, fixed.velocity)):
                value = getter(900)
                self.assertFalse(np.shares_memory(value, live))
                value[:] = -99
                self.assertFalse(np.any(live == -99))

    def test_long_trajectories_match_unbounded_reference_in_every_mode(self):
        for mode, period in ((RandomWalkMobility.MODE_WALL, True),
                             (RandomWalkMobility.MODE_TIME, 7),
                             (RandomWalkMobility.MODE_DISTANCE, 0.15)):
            with self.subTest(mode=mode):
                bounded = _node(mode=mode, mode_params=period)
                disabled = _node(collect=False, mode=mode, mode_params=period)
                reference = _node(mode=mode, mode_params=period, node_class=_UnboundedReference)
                for time in range(1, 20_001):
                    for node in (bounded, disabled, reference):
                        distance = np.linalg.norm(node.velocity) * 0.01
                        node.update_pos(time, node.pos + node.velocity * 0.01, node.velocity, False)
                        node.check_set_new_velocity(time, distance)
                    if time % 500 == 0:
                        for node in (bounded, disabled):
                            np.testing.assert_array_equal(node.pos, reference.pos)
                            np.testing.assert_array_equal(node.velocity, reference.velocity)
                            self.assertEqual(node.segment_distance, reference.segment_distance)
                            self.assertEqual(node.last_update_velocity, reference.last_update_velocity)
                            self.assertEqual(node.rng.bit_generator.state, reference.rng.bit_generator.state)
                stats = bounded.history_statistics()
                self.assertEqual(stats['entries'], 1024)
                self.assertEqual(stats['bytes'], 48 * 1024)
                self.assertEqual(stats['allocations'], 1024)
                self.assertEqual(stats['reuses'], 20_001 - 1024)
                self.assertEqual(stats['evictions'], stats['reuses'])
                self.assertEqual(len(reference.pos_history), 20_001)
                self.assertEqual(disabled.history_statistics()['bytes'], 0)


class PredictionCloneTest(unittest.TestCase):
    def test_mutable_period_configuration_is_owned_by_clone(self):
        for mode, period_name in ((RandomWalkMobility.MODE_TIME, 'time_period'),
                                  (RandomWalkMobility.MODE_DISTANCE, 'distance_period')):
            with self.subTest(mode=mode):
                node = _node(mode=mode, mode_params=np.array(5.0))
                clone = node.clone_for_prediction()
                self.assertIs(clone.mode_params, getattr(clone, period_name))
                self.assertIsNot(clone.mode_params, node.mode_params)
                clone.mode_params[...] = 99
                self.assertEqual(float(getattr(node, period_name)), 5.0)

    def test_clone_copies_mutable_motion_state_and_rng_without_history(self):
        for rng in (np.random.default_rng(73), np.random.RandomState(73)):
            with self.subTest(rng=type(rng).__name__):
                node = _node(limit=2, rng=rng)
                for time in range(1, 5):
                    node.update_pos(time, [time, 2, 3], node.velocity, time == 4)
                # Fail immediately if cloning tries to deep-copy stored history.
                class NoHistoryCopy(dict):
                    def __deepcopy__(self, memo):
                        raise AssertionError('prediction copied retained history')
                node.pos_history = NoHistoryCopy(node.pos_history)
                node.velo_history = NoHistoryCopy(node.velo_history)
                committed_stats = node.history_statistics()
                expected_rng = deepcopy(node.rng)
                clone = node.clone_for_prediction()
                self.assertIs(type(clone), type(node))
                self.assertEqual(clone.last_update, node.last_update)
                self.assertEqual(clone.last_update_velocity, node.last_update_velocity)
                self.assertEqual(clone.segment_distance, node.segment_distance)
                self.assertEqual(clone.hit_wall, node.hit_wall)
                self.assertFalse(clone.collect_history)
                self.assertEqual(clone.history_limit, 0)
                self.assertEqual(clone.pos_history, {})
                self.assertEqual(clone.velo_history, {})
                self.assertEqual(clone.history_statistics(), {
                    'enabled': False, 'limit': 0, 'entries': 0, 'bytes': 0,
                    'allocations': 0, 'reuses': 0, 'overwrites': 0, 'evictions': 0,
                })
                for name in ('pos', 'velocity', 'speed_params', 'direction_params'):
                    self.assertIsNot(getattr(clone, name), getattr(node, name))
                clone.pos[:] = -1
                clone.velocity[:] = -2
                clone.speed_params[0] = 99
                clone.direction_params[0] = 99
                self.assertFalse(np.any(node.pos == -1))
                self.assertFalse(np.any(node.velocity == -2))
                self.assertEqual(node.speed_params[0], 0.5)
                self.assertEqual(node.direction_params[0], -np.pi)
                clone.rng.uniform(size=100)
                self.assertEqual(node.rng.uniform(), expected_rng.uniform())
                clone.update_pos(5, [5, 6, 7], [1, 2, 0], False)
                self.assertEqual(clone.history_statistics()['entries'], 0)
                self.assertEqual(node.history_statistics(), committed_stats)

    def test_constant_clone_owns_arrays_and_stores_no_history(self):
        node = ConstantMobility(2, [1, 2, 3])
        clone = node.clone_for_prediction()
        clone.update_pos(1, [4, 5, 6], [1, 0, 0], False)
        np.testing.assert_array_equal(node.pos, [1, 2, 3])
        np.testing.assert_array_equal(node.velocity, [0, 0, 0])
        self.assertEqual(clone.history_statistics()['entries'], 0)

    def test_legacy_global_rng_requires_callers_save_and_restore(self):
        outside_state = np.random.get_state()
        try:
            np.random.seed(73)
            node = RandomWalkMobility(
                1, [1, 2, 3], RandomWalkMobility.MODE_TIME, 1,
                RandomWalkMobility.SPEED_UNIFORM, [0.5, 1.5],
                RandomWalkMobility.DIRECTION_UNIFORM, [-np.pi, np.pi],
            )
            clone = node.clone_for_prediction()
            self.assertIsNone(clone.rng)
            committed_state = np.random.get_state()
            reference_rng = np.random.RandomState()
            reference_rng.set_state(committed_state)
            try:
                clone.check_set_new_velocity(1, 1)
                self.assertFalse(np.array_equal(np.random.get_state()[1], committed_state[1])
                                 and np.random.get_state()[2] == committed_state[2])
            finally:
                np.random.set_state(committed_state)
            self.assertEqual(np.random.uniform(), reference_rng.uniform())
        finally:
            np.random.set_state(outside_state)


if __name__ == '__main__':
    unittest.main()
