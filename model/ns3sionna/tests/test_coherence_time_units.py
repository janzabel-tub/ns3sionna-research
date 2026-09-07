"""Regression tests for the server's fixed/dynamic coherence-time branches.

The server imports Sionna RT and its native runtime dependencies at module
import time.  These tests execute the relevant parsed server statements with
small stubs, so the unit contract is checked without requiring that runtime.
"""

import ast
from pathlib import Path
from types import SimpleNamespace
import unittest


SERVER_PATH = Path(__file__).parents[1] / "ns3sionna_server.py"
MILLISECOND = 1_000_000


def _server_tree():
    return ast.parse(SERVER_PATH.read_text(encoding="utf-8"))


def _coherence_branches():
    branches = []
    for node in ast.walk(_server_tree()):
        if not isinstance(node, ast.If) or not isinstance(node.test, ast.Compare):
            continue
        left = node.test.left
        if (
            isinstance(left, ast.Attribute)
            and isinstance(left.value, ast.Name)
            and left.value.id == "self"
            and left.attr == "fixed_coherence_time_ns"
        ):
            branches.append(node)
    return sorted(branches, key=lambda branch: branch.lineno)


class _Mobility:
    velocity = (1.0, 0.0, 0.0)
    pos = (0.0, 0.0, 0.0)

    def get_velo_at(self, _time):
        return self.velocity

    def get_pos_at(self, _time):
        return self.pos


class CoherenceTimeUnitsTest(unittest.TestCase):
    def _execute_branch(self, branch, fixed_time_ns):
        self_stub = SimpleNamespace(
            fixed_coherence_time_ns=fixed_time_ns,
            node_info={0: _Mobility(), 1: _Mobility()},
            fc=5.21e9,
        )
        rx_node_info = SimpleNamespace(end_time2=None)
        csi_tc_arr = []
        dynamic_calls = []

        def dynamic_coherence(*args, **kwargs):
            dynamic_calls.append((args, kwargs))
            return 7_000_000

        namespace = {
            "self": self_stub,
            "coherence_from_velocities": dynamic_coherence,
            "csi_tc_arr": csi_tc_arr,
            "rx_node_info": rx_node_info,
            "node_id": 1,
            "curr_rx_node": 1,
            "comp_rx_node_id": 1,
            "tx_node_id": 0,
            "lah_time": 0,
        }
        module = ast.fix_missing_locations(ast.Module(body=[branch], type_ignores=[]))
        exec(compile(module, str(SERVER_PATH), "exec"), namespace)
        return namespace, dynamic_calls

    def test_configuration_boundary_converts_milliseconds_once(self):
        init = next(
            node
            for node in ast.walk(_server_tree())
            if isinstance(node, ast.FunctionDef) and node.name == "init_simulation_env"
        )
        assignment = next(
            node
            for node in ast.walk(init)
            if isinstance(node, ast.Assign)
            and any(
                isinstance(target, ast.Attribute)
                and target.attr == "fixed_coherence_time_ns"
                for target in node.targets
            )
        )

        for fixed_time_ms, expected_ns in ((5, 5 * MILLISECOND), (0, 0)):
            with self.subTest(fixed_time_ms=fixed_time_ms):
                self_stub = SimpleNamespace()
                sim_init_msg = SimpleNamespace(fixed_coherence_time_ms=fixed_time_ms)
                module = ast.fix_missing_locations(
                    ast.Module(body=[assignment], type_ignores=[])
                )
                exec(
                    compile(module, str(SERVER_PATH), "exec"),
                    {
                        "self": self_stub,
                        "sim_init_msg": sim_init_msg,
                        "MILLISECOND": MILLISECOND,
                    },
                )
                self.assertEqual(self_stub.fixed_coherence_time_ns, expected_ns)

    def test_all_three_branches_preserve_fixed_ns_and_dynamic_mode(self):
        branches = _coherence_branches()
        self.assertEqual(len(branches), 3)

        for branch in branches:
            with self.subTest(line=branch.lineno, mode="fixed"):
                namespace, dynamic_calls = self._execute_branch(
                    branch, 5 * MILLISECOND
                )
                self.assertEqual(dynamic_calls, [])
                if namespace["csi_tc_arr"]:
                    self.assertEqual(namespace["csi_tc_arr"], [5 * MILLISECOND])
                else:
                    self.assertEqual(namespace["tc"], 5 * MILLISECOND)

            with self.subTest(line=branch.lineno, mode="dynamic"):
                namespace, dynamic_calls = self._execute_branch(branch, 0)
                self.assertTrue(dynamic_calls)
                if namespace["csi_tc_arr"]:
                    self.assertEqual(namespace["csi_tc_arr"], [7 * MILLISECOND])
                else:
                    self.assertEqual(namespace["tc"], 7 * MILLISECOND)


if __name__ == "__main__":
    unittest.main()
