import unittest

import numpy as np

from mobility import RandomWalkMobility
from ns3sionna_server import SionnaEnv


class _NoHitInteraction:
    def is_valid(self):
        return False


class _NoHitScene:
    def ray_intersect(self, *args, **kwargs):
        return _NoHitInteraction()


class _Scene:
    def __init__(self):
        self.mi_scene = _NoHitScene()


class DeterministicMobilityTest(unittest.TestCase):
    STEP_NS = 100_000_000

    def _node(self, mode, mode_params, seed=7, position=(0.0, 0.0, 1.0)):
        return RandomWalkMobility(
            1,
            position,
            mode,
            mode_params,
            RandomWalkMobility.SPEED_CONSTANT,
            (1.0,),
            RandomWalkMobility.DIRECTION_UNIFORM,
            (-np.pi, np.pi),
            rng=np.random.default_rng(seed),
        )

    def _env(self, node, bounds=None):
        env = SionnaEnv.__new__(SionnaEnv)
        env.equidistant_walk_steps = self.STEP_NS
        env.absolute_time_mobility = True
        env.mobility_bounds = bounds
        env.sim_time = 0
        env.node_info = {1: node}
        env.scene = _Scene()
        return env

    def test_common_time_is_independent_of_request_schedule(self):
        cases = (
            (RandomWalkMobility.MODE_TIME, 200_000_000),
            (RandomWalkMobility.MODE_DISTANCE, 0.2),
        )
        for mode, mode_params in cases:
            with self.subTest(mode=mode):
                split = self._env(self._node(mode, mode_params))
                direct = self._env(self._node(mode, mode_params))

                for target in (150_000_000, 350_000_000, 500_000_000):
                    split._update_nodes_to_time(target, [1])
                direct._update_nodes_to_time(500_000_000, [1])

                np.testing.assert_allclose(split.node_info[1].pos,
                                           direct.node_info[1].pos,
                                           atol=1e-12)
                np.testing.assert_allclose(split.node_info[1].velocity,
                                           direct.node_info[1].velocity,
                                           atol=1e-12)

    def test_boundary_start_reflects_inward(self):
        node = self._node(RandomWalkMobility.MODE_WALL, True,
                          position=(1.0, 0.5, 1.0))
        node.velocity = np.array([1.0, 0.0, 0.0])
        env = self._env(node, bounds=(0.0, 1.0, 0.0, 1.0))

        env._update_nodes_to_time(self.STEP_NS, [1])

        self.assertLess(node.pos[0], 1.0)
        self.assertGreater(node.pos[0], 0.0)
        self.assertLess(node.velocity[0], 0.0)
        self.assertEqual(node.pos[2], 1.0)


if __name__ == '__main__':
    unittest.main()
