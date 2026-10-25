"""Fast native frozen-channel checks and validation of the exported CPU sweep.

Run with the same Python environment as the server after building the native
example. SUMIMO_BENCHMARK_RESULTS can select another complete benchmark export.
"""

import copy
import csv
import itertools
import json
import os
from pathlib import Path
import subprocess
import tempfile
import unittest

import numpy as np

import benchmark_sumimo as benchmark


def frozen_response():
    fixture = json.loads((benchmark.MODULE / "data/sumimo_frozen_reference.json").read_text())
    response = benchmark.message_pb2.ChannelStateResponse()
    snapshot = response.csi.add(start_time=0, end_time=5000000)
    snapshot.tx_node.id = 0
    for name, value in zip(("x", "y", "z"), benchmark.POSITIONS[0]):
        setattr(snapshot.tx_node.position, name, value)
    rx = snapshot.rx_nodes.add(id=1, num_rx_antennas=2, num_tx_antennas=2,
                              wb_loss=fixture["wideband_loss_db"])
    for name, value in zip(("x", "y", "z"), benchmark.POSITIONS[1]):
        setattr(rx.position, name, value)
    for index, entry in enumerate(fixture["matrices"]):
        h = np.array([[complex(*value) for value in row] for row in entry["coefficients"]])
        h *= entry["scale"]
        rx.frequencies.append((index - 2) * 78125)
        rx.csi_real.extend(h.real.reshape(-1))
        rx.csi_imag.extend(h.imag.reshape(-1))
    return response, fixture


class TestNativeBenchmark(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.binary = benchmark.executable()

    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory(prefix="sumimo-test-")
        self.path = Path(self.temporary.name)
        self.addCleanup(self.temporary.cleanup)

    def analyze(self, response, suffix=""):
        channel_path = self.path / ("channel" + suffix + ".pb")
        metrics_path = self.path / ("metrics" + suffix + ".csv")
        channel_path.write_bytes(response.SerializeToString(deterministic=True))
        benchmark.native(self.binary, action="analyze", input=channel_path, output=metrics_path)
        return benchmark.read_metrics(metrics_path)

    def test_configuration_geometry_and_all_orientation_components(self):
        tx, rx = (90.0, -30.0, 45.0), (-60.0, 20.0, -90.0)
        config = benchmark.export_configuration(self.binary, self.path / "first.pb", 0.25, tx, rx)
        benchmark.export_configuration(self.binary, self.path / "second.pb", 0.25, tx, rx)
        self.assertEqual((self.path / "first.pb").read_bytes(), (self.path / "second.pb").read_bytes())
        self.assertNotEqual(config.nodes[0].orientation, config.nodes[1].orientation)
        # A missing override or stale spacing must be observable at the byte boundary.
        broken = copy.deepcopy(config)
        broken.nodes[1].ClearField("orientation")
        with self.assertRaises(AssertionError):
            benchmark.validate_configuration(broken, 0.25, tx, rx)
        # Independent spacings must survive the same helper/Protobuf boundary.
        for tx_spacing, rx_spacing in ((0.25, 1.0), (1.0, 0.25)):
            first = self.path / "independent.pb"
            independent = benchmark.export_configuration(self.binary, first, tx_spacing, tx, rx,
                                                         rx_spacing=rx_spacing)
            benchmark.export_configuration(self.binary, self.path / "repeat.pb", tx_spacing,
                                           tx, rx, rx_spacing=rx_spacing)
            self.assertEqual(first.read_bytes(), (self.path / "repeat.pb").read_bytes())
            self.assertEqual(independent.tx_array.horizontal_spacing, tx_spacing)
            self.assertEqual(independent.rx_array.horizontal_spacing, rx_spacing)
        broken = copy.deepcopy(config)
        broken.rx_array.horizontal_spacing = 0.5
        with self.assertRaises(AssertionError):
            benchmark.validate_configuration(broken, 0.25, tx, rx)

    def test_invalid_geometry_and_orientation_rejected(self):
        for options in ({"spacingLambda": 0}, {"spacingLambda": "nan"},
                        {"txSpacingLambda": -1}, {"rxSpacingLambda": 0},
                        {"txSpacingLambda": "inf"}, {"rxSpacingLambda": "0.5junk"},
                        {"txElevation": 91}, {"rxAzimuth": 181}, {"rxRoll": "inf"}):
            with self.subTest(options=options), self.assertRaises(subprocess.CalledProcessError):
                benchmark.native(self.binary, action="config", output=self.path / "bad.pb", **options)

    def test_frozen_channel_analytic_rank_singular_values_mmse_and_mi(self):
        response, fixture = frozen_response()
        rows = self.analyze(response)
        # This channel has unit average pair power, a zero tone, rank loss,
        # unequal tone powers, and complex asymmetric coefficients.
        self.assertAlmostEqual(float(np.mean(np.abs(benchmark.channel_array(response)) ** 2)), 1)
        noise = 1.3803e-23 * 290 * 20e6 * 10 ** 0.7
        rho = 1e-7 / noise  # 20 dBm minus 60 dB, converted to W.
        alpha = rho / 2
        expected = np.array([[rho, rho], [rho / (1 + rho)] * 2, [2 * rho] * 2, [0, 0],
                             [1.6 * alpha - 1.28 * alpha ** 2 / (1 + 2.4 * alpha),
                              2.4 * alpha - 1.28 * alpha ** 2 / (1 + 1.6 * alpha)]])
        np.testing.assert_allclose([row["rank"] for row in rows], fixture["expected_rank"], atol=0)
        np.testing.assert_allclose([[row["sigma0"], row["sigma1"]] for row in rows],
                                   fixture["expected_singular_values"], rtol=1e-12, atol=1e-12)
        np.testing.assert_allclose([[row["sinr0_linear"], row["sinr1_linear"]] for row in rows],
                                   expected, rtol=1e-10, atol=1e-10)
        effective = np.expm1(np.mean(np.log1p(expected), axis=0))
        np.testing.assert_allclose([rows[0]["effective_sinr0_linear"], rows[0]["effective_sinr1_linear"]],
                                   effective, rtol=1e-10)
        self.assertAlmostEqual(rows[0]["spectral_efficiency_bps_hz"],
                               float(np.mean(np.sum(np.log2(1 + expected), axis=1))), places=9)
        np.testing.assert_allclose(rows[0]["reference_snr_linear"], rho, rtol=1e-12)
        np.testing.assert_allclose(rows[0]["noise_power_w"], noise, rtol=1e-12)
        for row in rows:
            self.assertGreater(row["analysis_seconds"], 0)

    def test_frozen_channel_repeatability(self):
        response, _ = frozen_response()
        first, second = self.analyze(response, "1"), self.analyze(response, "2")
        benchmark.compare_repetitions(response, response, first, second)
        for key in first[0]:
            if key != "analysis_seconds":
                self.assertEqual([row[key] for row in first], [row[key] for row in second])
        second[0]["sinr0_linear"] *= 1.01
        with self.assertRaises(AssertionError):
            benchmark.compare_repetitions(response, response, first, second)

    def test_invalid_response_rejected(self):
        response, _ = frozen_response()
        for change in ("dimensions", "node", "length", "nan"):
            broken = copy.deepcopy(response)
            rx = broken.csi[0].rx_nodes[0]
            if change == "dimensions":
                rx.num_rx_antennas = 1
            elif change == "node":
                rx.id = 0
            elif change == "length":
                del rx.csi_imag[-1]
            else:
                rx.csi_real[0] = float("nan")
            with self.subTest(change=change), self.assertRaises(subprocess.CalledProcessError):
                self.analyze(broken, change)


class TestServerGeometry(unittest.TestCase):
    def test_seeded_replay_across_rt_threads(self):
        """Real production CFRs stay identical when LLVM worker scheduling changes."""
        env_class, _ = benchmark.runtime()
        import drjit as dr
        original_threads = dr.thread_count()
        try:
            with tempfile.TemporaryDirectory(prefix="sumimo-seeded-replay-") as temporary:
                config = benchmark.export_configuration(benchmark.executable(),
                    Path(temporary) / "configuration.pb", 0.5,
                    (90, 0, 0), (0, 0, 0))
                first, _, _, _ = benchmark.trace(config, env_class, 10000)
                dr.set_thread_count(2)
                second, _, _, _ = benchmark.trace(config, env_class, 10000)
                self.assertLess(first.csi[0].rx_nodes[0].wb_loss, 300)
                self.assertEqual(first.SerializeToString(deterministic=True),
                                 second.SerializeToString(deterministic=True))
        finally:
            dr.set_thread_count(original_threads)

    def test_all_euler_components_on_actual_arrays_and_radios(self):
        env_class, _ = benchmark.runtime()
        with tempfile.TemporaryDirectory(prefix="sumimo-geometry-") as temporary:
            config = benchmark.export_configuration(benchmark.executable(),
                Path(temporary) / "configuration.pb", 0.75,
                (90, -30, 45), (-60, 20, -90), rx_spacing=0.25)
            # trace checks actual PlanarArray coordinates and actual placed radio
            # Euler angles against the independently known degree/radian values.
            _, observed, _, _ = benchmark.trace(config, env_class)
            self.assertEqual(set(observed), {"tx", "rx"})

    def test_fresh_raytraces_match_frozen_reference(self):
        # The reference sweep was created in a different Python process. Check
        # real RT replay as well as the separate native replay of all 12 channels.
        output = Path(os.environ.get("SUMIMO_BENCHMARK_RESULTS",
                                     benchmark.MODULE / "data/phase5_2_spacing_orientation"))
        env_class, _ = benchmark.runtime()
        for case_id in ("case_00", "case_05", "case_10"):
            with self.subTest(case=case_id):
                config = benchmark.message_pb2.SimInitMessage.FromString(
                    (output / case_id / "configuration.pb").read_bytes())
                expected = benchmark.message_pb2.ChannelStateResponse.FromString(
                    (output / case_id / "channel_00.pb").read_bytes())
                actual, _, _, _ = benchmark.trace(config, env_class)
                self.assertEqual(actual.SerializeToString(deterministic=True),
                                 expected.SerializeToString(deterministic=True))


class TestExportedBenchmark(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.output = Path(os.environ.get("SUMIMO_BENCHMARK_RESULTS",
                                        benchmark.MODULE / "data/phase5_2_spacing_orientation"))
        cls.manifest = json.loads((cls.output / "manifest.json").read_text())
        with (cls.output / "summary.csv").open(newline="") as stream:
            cls.summary = list(csv.DictReader(stream))
        with (cls.output / "tones.csv").open(newline="") as stream:
            cls.tones = list(csv.DictReader(stream))

    def test_complete_fixed_control_matrix(self):
        manifest = self.manifest
        self.assertEqual(manifest["controls"], benchmark.CONTROLS)
        self.assertGreaterEqual(manifest["sweep"]["repetitions"], 2)
        expected = set(itertools.product(benchmark.SPACINGS, benchmark.AZIMUTHS, benchmark.AZIMUTHS))
        actual = {(float(row["spacing_lambda"]), float(row["tx_azimuth_deg"]),
                   float(row["rx_azimuth_deg"])) for row in self.summary}
        self.assertEqual(actual, expected)
        traces = 12 * manifest["sweep"]["repetitions"]
        self.assertEqual(len(self.summary), traces)
        self.assertEqual(len(self.tones), traces * 256)
        self.assertEqual(manifest["validation"]["fresh_traces"], traces)
        self.assertEqual(manifest["provenance"]["backend"], "llvm_ad_mono_polarized")
        for row in self.summary:
            for key in ("init_seconds", "cfr_seconds", "analysis_seconds", "native_process_seconds"):
                self.assertGreater(float(row[key]), 0)
        # Generated plots are local artifacts and are not required in reference exports.

    def test_observed_geometry_orientation_and_response_repeatability(self):
        for record in self.manifest["configurations"]:
            with self.subTest(case=record["case_id"]):
                case_dir = self.output / record["case_id"]
                config_path = case_dir / "configuration.pb"
                self.assertEqual(benchmark.sha256(config_path), record["configuration_sha256"])
                config = benchmark.message_pb2.SimInitMessage.FromString(config_path.read_bytes())
                angles = [tuple(getattr(node.orientation, key) for key in
                                ("azimuth", "elevation", "roll")) for node in config.nodes]
                spacing = config.tx_array.horizontal_spacing
                benchmark.validate_configuration(config, spacing, *angles)
                for row in self.summary:
                    if row["case_id"] == record["case_id"]:
                        self.assertEqual(float(row["spacing_lambda"]), spacing)
                        self.assertEqual(float(row["tx_azimuth_deg"]), angles[0][0])
                        self.assertEqual(float(row["rx_azimuth_deg"]), angles[1][0])
                first_response, first_metrics = None, None
                for index, repetition in enumerate(record["repetitions"]):
                    path = case_dir / f"channel_{index:02d}.pb"
                    self.assertEqual(benchmark.sha256(path), repetition["channel_sha256"])
                    response = benchmark.message_pb2.ChannelStateResponse.FromString(path.read_bytes())
                    snapshot = response.csi[0]
                    self.assertEqual((snapshot.start_time, snapshot.end_time, snapshot.tx_node.id),
                                     (0, 5000000, 0))
                    self.assertEqual(snapshot.rx_nodes[0].id, 1)
                    np.testing.assert_allclose(np.mean(np.abs(benchmark.channel_array(response)) ** 2),
                                               1.0, rtol=1e-6, atol=1e-7)
                    metrics = [{key: float(value) for key, value in row.items()
                                if key not in ("case_id", "repetition", "spacing_lambda",
                                               "tx_azimuth_deg", "rx_azimuth_deg")}
                               for row in self.tones if row["case_id"] == record["case_id"]
                               and int(row["repetition"]) == index]
                    self.assertEqual(len(metrics), 256)
                    if index == 0:
                        first_response, first_metrics = response, metrics
                    else:
                        benchmark.compare_repetitions(first_response, response, first_metrics, metrics)
                    for endpoint, orientation in zip(("tx", "rx"), angles):
                        observed = repetition["observed_geometry"][endpoint]
                        np.testing.assert_allclose(observed["orientation_rad"], np.deg2rad(orientation),
                                                   rtol=0, atol=1e-7)
                        np.testing.assert_allclose(observed["position_m"],
                                                   benchmark.POSITIONS[0 if endpoint == "tx" else 1])
                        np.testing.assert_allclose(observed["array_positions_lambda"],
                            [[0, -spacing / 2, 0], [0, spacing / 2, 0]], rtol=0, atol=1e-7)

    def test_exported_metrics_against_independent_numpy_reference(self):
        """Detect serialization/layout/power changes on every frozen real channel."""
        for record in self.manifest["configurations"]:
            path = self.output / record["case_id"] / "channel_00.pb"
            response = benchmark.message_pb2.ChannelStateResponse.FromString(path.read_bytes())
            metrics = [{key: float(value) for key, value in row.items()
                        if key not in ("case_id", "repetition", "spacing_lambda",
                                       "tx_azimuth_deg", "rx_azimuth_deg")}
                       for row in self.tones if row["case_id"] == record["case_id"]
                       and int(row["repetition"]) == 0]
            benchmark.validate_reference_metrics(response, metrics)

    def test_native_replay_of_frozen_real_channel(self):
        with tempfile.TemporaryDirectory(prefix="sumimo-replay-") as temporary:
            path = Path(temporary) / "metrics.csv"
            for record in self.manifest["configurations"]:
                channel_path = self.output / record["case_id"] / "channel_00.pb"
                benchmark.native(benchmark.executable(), action="analyze", output=path,
                                 input=channel_path)
                actual = benchmark.read_metrics(path)
                expected = [{key: float(value) for key, value in row.items()
                             if key not in ("case_id", "repetition", "spacing_lambda",
                                            "tx_azimuth_deg", "rx_azimuth_deg")}
                            for row in self.tones if row["case_id"] == record["case_id"]
                            and row["repetition"] == "0"]
                response = benchmark.message_pb2.ChannelStateResponse.FromString(channel_path.read_bytes())
                benchmark.compare_repetitions(response, response, expected, actual)


if __name__ == "__main__":
    unittest.main()
