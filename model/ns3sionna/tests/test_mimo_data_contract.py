import unittest

import numpy as np

from common import message_pb2
from mimo_data_contract import (
    LEGACY_RADIO_ORIENTATION_RAD,
    RX_ARRAY_DEFAULTS,
    TX_ARRAY_DEFAULTS,
    append_cfr_to_message,
    append_user_channel_identity,
    extract_link_cfr,
    normalize_link_cfr,
    resolve_array_config,
    resolve_node_orientation,
    validate_multi_user_snapshot,
)


class MimoDataContractTest(unittest.TestCase):
    @staticmethod
    def _make_multi_user_snapshot():
        state = message_pb2.ChannelStateResponse.ChannelState()
        state.start_time = 1234
        state.end_time = 2234
        state.tx_node.id = 42
        for user_id, num_rx, array_config, phase_offset in (
            (9, 2, {**RX_ARRAY_DEFAULTS, "num_cols": 2}, 10),
            (3, 1, RX_ARRAY_DEFAULTS, 0),
        ):
            node = state.rx_nodes.add()
            node.id = user_id
            node.wb_loss = 70.0 + user_id
            phases = np.arange(num_rx * 2 * 2).reshape(num_rx, 2, 2) + phase_offset
            append_cfr_to_message(node, [-10, 10], np.exp(1j * phases))
            append_user_channel_identity(node, user_id, 42, 1234, array_config)
        return state

    def test_legacy_array_defaults_and_explicit_configuration(self):
        self.assertEqual(resolve_array_config(None, TX_ARRAY_DEFAULTS), TX_ARRAY_DEFAULTS)
        self.assertEqual(resolve_array_config(None, RX_ARRAY_DEFAULTS), RX_ARRAY_DEFAULTS)

        wrapper = message_pb2.Wrapper()
        config = wrapper.sim_init_msg.tx_array
        config.num_rows = 2
        config.num_cols = 3
        config.vertical_spacing = 0.25
        config.horizontal_spacing = 0.75
        config.pattern = "dipole"
        config.polarization = "VH"

        self.assertEqual(
            resolve_array_config(config, TX_ARRAY_DEFAULTS),
            {
                "num_rows": 2,
                "num_cols": 3,
                "vertical_spacing": 0.25,
                "horizontal_spacing": 0.75,
                "pattern": "dipole",
                "polarization": "VH",
            },
        )

    def test_node_orientation_preserves_legacy_default_and_converts_degrees(self):
        node = message_pb2.SimInitMessage.NodeInfo()
        self.assertEqual(
            resolve_node_orientation(node),
            LEGACY_RADIO_ORIENTATION_RAD,
        )

        node.orientation.azimuth = 90.0
        node.orientation.elevation = -45.0
        node.orientation.roll = 180.0
        self.assertTrue(
            np.allclose(
                resolve_node_orientation(node),
                [np.pi / 2.0, -np.pi / 4.0, np.pi],
            )
        )

    def test_siso_wire_representation_stays_legacy_compatible(self):
        frequencies = [-1, 1]
        cfr = np.array([[[1.5 + 2.5j, 3.5 + 4.5j]]])

        expected = message_pb2.ChannelStateResponse.ChannelState.RxNodeInfo()
        expected.frequencies.extend(frequencies)
        expected.csi_real.extend([1.5, 3.5])
        expected.csi_imag.extend([2.5, 4.5])

        actual = message_pb2.ChannelStateResponse.ChannelState.RxNodeInfo()
        append_cfr_to_message(actual, frequencies, cfr)

        self.assertEqual(actual.num_rx_antennas, 0)
        self.assertEqual(actual.num_tx_antennas, 0)
        self.assertEqual(actual.SerializeToString(), expected.SerializeToString())

    def test_synthetic_2x2_is_flattened_subcarrier_rx_tx(self):
        raw = np.zeros((1, 2, 1, 2, 1, 2), dtype=np.complex128)
        raw[0, 0, 0, 0, 0, :] = [1 + 11j, 5 + 15j]
        raw[0, 0, 0, 1, 0, :] = [2 + 12j, 6 + 16j]
        raw[0, 1, 0, 0, 0, :] = [3 + 13j, 7 + 17j]
        raw[0, 1, 0, 1, 0, :] = [4 + 14j, 8 + 18j]

        link = extract_link_cfr(raw, 0)
        self.assertEqual(link.shape, (2, 2, 2))

        rx_info = message_pb2.ChannelStateResponse.ChannelState.RxNodeInfo()
        append_cfr_to_message(rx_info, [-10, 10], link)

        self.assertEqual(rx_info.num_rx_antennas, 2)
        self.assertEqual(rx_info.num_tx_antennas, 2)
        self.assertEqual(list(rx_info.csi_real), [1, 2, 3, 4, 5, 6, 7, 8])
        self.assertEqual(list(rx_info.csi_imag), [11, 12, 13, 14, 15, 16, 17, 18])

    def test_normalization_covers_all_subcarriers_and_antenna_pairs(self):
        cfr = np.array(
            [
                [[1 + 0j, 2 + 0j], [3 + 0j, 4 + 0j]],
                [[5 + 0j, 6 + 0j], [7 + 0j, 8 + 0j]],
            ]
        )
        expected_power = np.mean(np.abs(cfr) ** 2)

        wideband_loss_db, normalized = normalize_link_cfr(cfr)

        self.assertAlmostEqual(wideband_loss_db, -10 * np.log10(expected_power))
        self.assertAlmostEqual(np.mean(np.abs(normalized) ** 2), 1.0)
        self.assertEqual(normalized.shape, cfr.shape)

    def test_multi_user_snapshot_round_trip_orders_identity_and_keeps_heterogeneous_rx(self):
        wire = self._make_multi_user_snapshot().SerializeToString()
        parsed = message_pb2.ChannelStateResponse.ChannelState()
        parsed.ParseFromString(wire)

        users = validate_multi_user_snapshot(parsed)

        self.assertEqual([user.user_identity.user_id for user in users], [3, 9])
        self.assertEqual([user.num_rx_antennas or 1 for user in users], [1, 2])
        self.assertEqual([user.num_tx_antennas for user in users], [2, 2])
        self.assertEqual([user.user_identity.rx_array.num_cols for user in users], [1, 2])
        self.assertTrue(
            all(
                user.user_identity.direction == message_pb2.CHANNEL_DIRECTION_DOWNLINK
                and user.user_identity.tx_node_id == 42
                and user.user_identity.rx_node_id == user.id
                and user.user_identity.csi_timestamp == 1234
                for user in users
            )
        )
        self.assertAlmostEqual(users[1].csi_real[2], np.cos(14.0))
        self.assertAlmostEqual(users[1].csi_imag[2], np.sin(14.0))

    def test_multi_user_snapshot_rejects_inconsistent_identity_before_use(self):
        mutations = {
            "duplicate user": lambda state: setattr(
                state.rx_nodes[1].user_identity, "user_id", 9
            ),
            "mixed timestamp": lambda state: setattr(
                state.rx_nodes[1].user_identity, "csi_timestamp", 1235
            ),
            "wrong direction": lambda state: setattr(
                state.rx_nodes[1].user_identity,
                "direction",
                message_pb2.CHANNEL_DIRECTION_UPLINK,
            ),
            "frequency grid": lambda state: state.rx_nodes[1].frequencies.__setitem__(
                1, 11
            ),
            "RX array": lambda state: setattr(
                state.rx_nodes[0].user_identity.rx_array, "num_cols", 3
            ),
        }
        for name, mutate in mutations.items():
            with self.subTest(name=name):
                snapshot = self._make_multi_user_snapshot()
                mutate(snapshot)
                with self.assertRaises(ValueError):
                    validate_multi_user_snapshot(snapshot)


class TestResourceLimits(unittest.TestCase):
    def test_boundaries_and_overflow(self):
        from mimo_data_contract import ResourceLimits
        limits = ResourceLimits(max_subcarriers=2, max_rx_antennas=2,
                                max_tx_antennas=2, max_coefficients_per_channel=8)
        self.assertEqual(limits.channel(2, 2, 2), 8)
        for shape in ((3, 2, 2), (2, 3, 2), (2, 2, 3), (0, 1, 1)):
            with self.subTest(shape=shape), self.assertRaises(ValueError):
                limits.channel(*shape)
        huge = ResourceLimits(max_subcarriers=2**64, max_rx_antennas=2**64,
                              max_tx_antennas=2**64, max_coefficients_per_channel=2**200)
        with self.assertRaisesRegex(ValueError, "overflow"):
            huge.channel(2**64, 2**64, 2**64)
        with self.assertRaises(ValueError):
            ResourceLimits(max_reply_bytes=0)
        message = message_pb2.ChannelStateResponse.ChannelState.RxNodeInfo()
        original = message.SerializeToString()
        with self.assertRaises(ValueError):
            append_cfr_to_message(message, [0, 1], np.ones((2, 2, 2)),
                                  ResourceLimits(max_coefficients_per_channel=7))
        self.assertEqual(message.SerializeToString(), original)
        append_cfr_to_message(message, [0, 1], np.ones((2, 2, 2)), limits)
        self.assertEqual(len(message.csi_real), 8)

    def test_batch_inclusive_limits(self):
        from dataclasses import replace
        from mimo_data_contract import ResourceLimits
        base = ResourceLimits()
        bound = base.plan(2, 2, 2, 2, 4)
        limits = replace(base, max_batch_coefficients=64, max_snapshots=8,
                         max_payload_bytes=1088, max_reply_bytes=bound)
        self.assertEqual(limits.plan(2, 2, 2, 2, 4), bound)
        for name in ("max_batch_coefficients", "max_snapshots", "max_payload_bytes", "max_reply_bytes"):
            with self.subTest(name=name), self.assertRaises(ValueError):
                replace(limits, **{name: getattr(limits, name) - 1}).plan(2, 2, 2, 2, 4)

    def test_actual_lookahead_depth(self):
        from mimo_data_contract import lookahead_depth
        for depth in (1, 4, 16):
            self.assertEqual(lookahead_depth(3 * depth + 2, 3), depth)
        for sub_mode, receivers in ((0, 3), (2, 3), (4, 0)):
            with self.assertRaisesRegex(ValueError, "[Ll]ook-ahead"):
                lookahead_depth(sub_mode, receivers)

    def test_server_preflight_precedes_mobility_and_tracing(self):
        from unittest.mock import Mock, patch
        from mimo_data_contract import ResourceLimits, RX_ARRAY_DEFAULTS, TX_ARRAY_DEFAULTS
        from ns3sionna_server import SionnaEnv
        env = SionnaEnv(resource_limits=ResourceLimits(max_batch_coefficients=1), VERBOSE=False)
        env.fft_size = 2
        env.rx_array_config = RX_ARRAY_DEFAULTS.copy()
        env.tx_array_config = TX_ARRAY_DEFAULTS.copy()
        env.node_info = {0: object(), 1: object(), 2: object()}
        env.time_evo_model = "position"
        env.sub_mode = 8
        env._update_nodes_to_time = Mock()
        request = message_pb2.ChannelStateRequest(tx_node=0, rx_node=1)
        reply = message_pb2.Wrapper()
        for operation in (lambda: env.compute_cfr_with_lookahead(request, reply),
                          lambda: env._compute_cfr_via_position(0, 0, 1, SionnaEnv.MODE_P2P)):
            with patch("ns3sionna_server.PathSolver") as solver:
                with self.assertRaisesRegex(ValueError, "batch_coefficients"):
                    operation()
                solver.assert_not_called()
            env._update_nodes_to_time.assert_not_called()
            self.assertFalse(reply.HasField("channel_state_response"))
        env.sub_mode = 1
        with self.assertRaisesRegex(ValueError, "Zero look-ahead depth"):
            env.compute_cfr_with_lookahead(request, reply)
        env._update_nodes_to_time.assert_not_called()
        env.resource_limits = ResourceLimits()
        self.assertGreater(env._check_batch_budget(2, 4), 0)  # Reconfiguration recovers.


if __name__ == "__main__":
    unittest.main()
