import unittest

import numpy as np

from common import message_pb2
from mimo_data_contract import (
    RX_ARRAY_DEFAULTS,
    TX_ARRAY_DEFAULTS,
    append_cfr_to_message,
    extract_link_cfr,
    normalize_link_cfr,
    resolve_array_config,
)


class MimoDataContractTest(unittest.TestCase):
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


if __name__ == "__main__":
    unittest.main()
