"""Independent paired-channel R5 checks; no RT trace is needed for the fixtures.

PHYSICAL_AGING_RESULTS optionally selects a complete saved physical aging study.
Build ns3_wifi_sumimo_benchmark before running the native fixture checks.
"""

import copy
import itertools
import json
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

import numpy as np

import benchmark_physical_aging as aging
import benchmark_sumimo as base


REJECTED_REFERENCE = (ValueError, AssertionError)
TX_TIME_NS = 50_000_000


def synthetic_response(h, timestamp, loss=95.0):
    """Serialize a production-normalized complex RX-major 256x2x2 CFR."""
    h = np.asarray(h, dtype=np.complex128)
    if h.shape != (256, 2, 2):
        raise ValueError("Synthetic fixture must have 256 2x2 matrices")
    h = h / np.sqrt(np.mean(np.abs(h) ** 2))
    response = base.message_pb2.ChannelStateResponse()
    snapshot = response.csi.add(start_time=timestamp, end_time=timestamp + 5_000_000)
    snapshot.tx_node.id = 0
    for field, value in zip(("x", "y", "z"), base.POSITIONS[0]):
        setattr(snapshot.tx_node.position, field, value)
    rx = snapshot.rx_nodes.add(id=1, num_rx_antennas=2, num_tx_antennas=2,
                              wb_loss=loss)
    # Both snapshots describe the actual linear trajectory at their own times.
    for field, value in zip(("x", "y", "z"),
                            (4 + timestamp / 1e9, 3.0, 1.0)):
        setattr(rx.position, field, value)
    rx.frequencies.extend((np.arange(256) - 128) * 78125)
    rx.csi_real.extend(h.real.reshape(-1))
    rx.csi_imag.extend(h.imag.reshape(-1))
    return response


def synthetic_pair(age_ns=20_000_000):
    """Full-rank unequal modes with complex, frequency-dependent coefficients."""
    tone = np.arange(256)
    theta = 0.18 + 0.13 * np.sin(tone / 37)
    phase = np.exp(1j * (0.2 + tone / 43))
    u = np.zeros((256, 2, 2), dtype=np.complex128)
    u[:, 0, 0] = np.cos(theta)
    u[:, 0, 1] = -np.sin(theta) * phase
    u[:, 1, 0] = np.sin(theta) * phase.conj()
    u[:, 1, 1] = np.cos(theta)
    modes = np.zeros_like(u)
    modes[:, 0, 0] = 1.6 + 0.2 * np.cos(tone / 29)
    modes[:, 1, 1] = 0.22 + 0.03 * np.sin(tone / 31)
    v = np.zeros_like(u)
    v[:, 0, 0] = np.exp(1j * tone / 53)
    v[:, 1, 1] = np.exp(-1j * tone / 47)
    old = u @ modes @ v.conj().transpose(0, 2, 1)
    angle = 0.63
    mixing = np.array([[np.cos(angle), -1j * np.sin(angle)],
                       [-1j * np.sin(angle), np.cos(angle)]])
    current = old @ mixing
    current *= (1 + 0.08 * np.sin(tone / 23))[:, None, None]
    tx = synthetic_response(current, TX_TIME_NS, loss=95)
    csi = synthetic_response(old, TX_TIME_NS - age_ns, loss=62)
    return tx, csi


def independent_rows(tx, csi):
    """NumPy reference independent of the new orchestration/validation module."""
    h = base.channel_array(tx)
    old = base.channel_array(csi)
    _, _, vh = np.linalg.svd(old)
    w = vh.conj().transpose(0, 2, 1)
    effective_channel = h @ w
    noise = 1.3803e-23 * 290 * 20e6 * 10 ** 0.7
    loss = tx.csi[0].rx_nodes[0].wb_loss
    power = 10 ** ((20 - loss - 30) / 10)
    rho = power / noise
    error = np.linalg.inv(np.eye(2) + rho / 2 * (
        effective_channel.conj().transpose(0, 2, 1) @ effective_channel))
    sinr = 1 / np.diagonal(error, axis1=1, axis2=2).real - 1
    singular = np.linalg.svd(h, compute_uv=False)
    effective = np.expm1(np.mean(np.log1p(sinr), axis=0))
    mi = np.mean(np.sum(np.log2(1 + sinr), axis=1))
    current_norm = np.sum(abs(h) ** 2, axis=(1, 2))
    old_norm = np.sum(abs(old) ** 2, axis=(1, 2))
    correlation = abs(np.sum(old.conj() * h, axis=(1, 2))) / np.sqrt(
        current_norm * old_norm)
    relative_change = np.sqrt(np.sum(abs(h - old) ** 2, axis=(1, 2)) / current_norm)
    timestamp = tx.csi[0].start_time
    sounding = csi.csi[0].start_time
    rows = []
    for index in range(256):
        rows.append({
            "subcarrier": index,
            "frequency_offset_hz": tx.csi[0].rx_nodes[0].frequencies[index],
            "rank": int(np.sum(singular[index] > 1e-12 * singular[index, 0])),
            "sigma0": singular[index, 0], "sigma1": singular[index, 1],
            "sinr0_linear": sinr[index, 0], "sinr1_linear": sinr[index, 1],
            "effective_sinr0_linear": effective[0],
            "effective_sinr1_linear": effective[1],
            "spectral_efficiency_bps_hz": mi, "wideband_loss_db": loss,
            "reference_snr_linear": rho, "received_power_w": power,
            "noise_power_w": noise, "analysis_seconds": 1.0,
            "initial_time_ns": sounding, "csi_time_ns": sounding,
            "transmission_time_ns": timestamp, "csi_age_ns": timestamp - sounding,
            "update_interval_ns": 0, "feedback_delay_ns": timestamp - sounding,
            "rotation_rate_deg_per_ms": 0, "precoder_power": 2,
            "channel_correlation": correlation[index],
            "channel_relative_change": relative_change[index],
        })
    return rows


def malformed_pairs():
    """Corrupt each boundary separately, including sounding-only fields."""
    tx, csi = synthetic_pair()
    for endpoint in ("transmission", "sounding"):
        for defect in ("timestamp_overflow", "end_overflow", "negative_validity",
                       "tx_id", "rx_id", "missing_tx_position", "missing_rx_position",
                       "position_nan", "dimension", "implicit_dimension", "short_real",
                       "short_imag", "short_grid", "duplicate_frequency", "reverse_grid",
                       "nan_real", "inf_imag", "nan_loss", "unnormalized", "zero_power",
                       "extra_receiver", "extra_snapshot", "no_snapshot"):
            a, b = copy.deepcopy(tx), copy.deepcopy(csi)
            response = a if endpoint == "transmission" else b
            snapshot = response.csi[0]
            rx = snapshot.rx_nodes[0]
            if defect == "timestamp_overflow":
                snapshot.start_time = 2 ** 63
                snapshot.end_time = 2 ** 63 + 1
            elif defect == "end_overflow":
                snapshot.end_time = 2 ** 63
            elif defect == "negative_validity":
                snapshot.end_time = snapshot.start_time - 1
            elif defect == "tx_id":
                snapshot.tx_node.id = 2
            elif defect == "rx_id":
                rx.id = 0
            elif defect == "missing_tx_position":
                snapshot.tx_node.ClearField("position")
            elif defect == "missing_rx_position":
                rx.ClearField("position")
            elif defect == "position_nan":
                rx.position.x = float("nan")
            elif defect == "dimension":
                rx.num_rx_antennas = 1
            elif defect == "implicit_dimension":
                rx.ClearField("num_tx_antennas")
            elif defect == "short_real":
                del rx.csi_real[-1]
            elif defect == "short_imag":
                del rx.csi_imag[-1]
            elif defect == "short_grid":
                del rx.frequencies[-1]
            elif defect == "duplicate_frequency":
                rx.frequencies[1] = rx.frequencies[0]
            elif defect == "reverse_grid":
                rx.frequencies.reverse()
            elif defect == "nan_real":
                rx.csi_real[0] = float("nan")
            elif defect == "inf_imag":
                rx.csi_imag[0] = float("inf")
            elif defect == "nan_loss":
                rx.wb_loss = float("nan")
            elif defect in ("unnormalized", "zero_power"):
                factor = 2 if defect == "unnormalized" else 0
                rx.csi_real[:] = [value * factor for value in rx.csi_real]
                rx.csi_imag[:] = [value * factor for value in rx.csi_imag]
            elif defect == "extra_receiver":
                snapshot.rx_nodes.add().CopyFrom(rx)
            elif defect == "extra_snapshot":
                response.csi.add().CopyFrom(snapshot)
            elif defect == "no_snapshot":
                response.ClearField("csi")
            yield endpoint + "_" + defect, a, b
    future = copy.deepcopy(csi)
    future.csi[0].start_time = TX_TIME_NS + 1
    future.csi[0].end_time = TX_TIME_NS + 5_000_000
    yield "future_sounding", tx, future
    mismatched = copy.deepcopy(csi)
    mismatched.csi[0].rx_nodes[0].frequencies[:] = [
        value + 1 for value in mismatched.csi[0].rx_nodes[0].frequencies]
    yield "mismatched_grids", tx, mismatched


class TestIndependentPhysicalReference(unittest.TestCase):
    def test_full_rank_complex_fixture_and_independent_reference(self):
        tx, csi = synthetic_pair()
        for response in (tx, csi):
            h = base.channel_array(response)
            self.assertEqual(h.shape, (256, 2, 2))
            self.assertAlmostEqual(float(np.mean(abs(h) ** 2)), 1, places=12)
            self.assertTrue(np.all(np.linalg.svd(h, compute_uv=False)[:, 1] > 0.1))
            self.assertGreater(float(np.max(abs(h.imag))), 0.1)
        aging.validate_reference(tx, csi, independent_rows(tx, csi))

    def test_reference_rejects_corrupt_metric_including_each_timestamp(self):
        tx, csi = synthetic_pair()
        reference = independent_rows(tx, csi)
        for field in ("subcarrier", "frequency_offset_hz", "rank", "sigma0", "sigma1",
                      "sinr0_linear", "sinr1_linear", "effective_sinr0_linear",
                      "effective_sinr1_linear", "spectral_efficiency_bps_hz",
                      "wideband_loss_db", "reference_snr_linear", "received_power_w",
                      "noise_power_w", "initial_time_ns", "csi_time_ns",
                      "transmission_time_ns", "csi_age_ns", "feedback_delay_ns",
                      "update_interval_ns", "rotation_rate_deg_per_ms", "precoder_power",
                      "channel_correlation", "channel_relative_change"):
            rows = copy.deepcopy(reference)
            rows[73][field] += max(abs(rows[73][field]) * 0.1, 1.0)
            with self.subTest(field=field), self.assertRaises(REJECTED_REFERENCE):
                aging.validate_reference(tx, csi, rows)
        for rows in (reference[:-1], reference + [reference[0]], []):
            with self.assertRaises(REJECTED_REFERENCE):
                aging.validate_reference(tx, csi, rows)
        rows = copy.deepcopy(reference)
        rows[3]["sinr0_linear"] = float("nan")
        with self.assertRaises(REJECTED_REFERENCE):
            aging.validate_reference(tx, csi, rows)

    def test_reference_rejects_invalid_snapshot_contract(self):
        tx, csi = synthetic_pair()
        rows = independent_rows(tx, csi)
        for label, a, b in malformed_pairs():
            with self.subTest(defect=label), self.assertRaises(REJECTED_REFERENCE):
                aging.validate_reference(a, b, rows)


class TestNativePhysicalAging(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.binary = base.executable()

    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory(prefix="r5-paired-native-")
        self.addCleanup(self.temporary.cleanup)
        self.directory = Path(self.temporary.name)

    def analyze(self, tx, csi, suffix=""):
        tx_path = self.directory / ("tx" + suffix + ".pb")
        csi_path = self.directory / ("sounding" + suffix + ".pb")
        output = self.directory / ("metrics" + suffix + ".csv")
        tx_path.write_bytes(tx.SerializeToString(deterministic=True))
        csi_path.write_bytes(csi.SerializeToString(deterministic=True))
        base.native(self.binary, action="physical-aging", input=tx_path,
                    csiInput=csi_path, output=output)
        return base.read_metrics(output)

    def assert_reference(self, tx, csi, rows):
        expected = independent_rows(tx, csi)
        self.assertEqual(len(rows), 256)
        for field in expected[0]:
            if field == "analysis_seconds":
                self.assertTrue(all(row[field] > 0 for row in rows))
                continue
            tolerance = 1e-10 if field.startswith("sigma") else 1e-8
            np.testing.assert_allclose([row[field] for row in rows],
                                       [row[field] for row in expected],
                                       rtol=tolerance, atol=tolerance, err_msg=field)
        aging.validate_reference(tx, csi, rows)

    def test_separate_snapshots_sounding_svd_current_mmse_and_timestamps(self):
        tx, csi = synthetic_pair()
        rows = self.analyze(tx, csi)
        self.assert_reference(tx, csi, rows)
        self.assertLess(csi.csi[0].end_time, tx.csi[0].start_time)
        self.assertTrue(all(row["rank"] == 2 for row in rows))
        self.assertTrue(all(row["channel_relative_change"] > 0.1 for row in rows))
        self.assertTrue(all(row["channel_correlation"] < 0.99 for row in rows))
        fresh = self.analyze(tx, tx, "fresh")
        self.assert_reference(tx, tx, fresh)
        singular = np.linalg.svd(base.channel_array(tx), compute_uv=False)
        rho = fresh[0]["reference_snr_linear"]
        bound = float(np.mean(np.sum(np.log2(1 + rho / 2 * singular ** 2), axis=1)))
        self.assertAlmostEqual(fresh[0]["spectral_efficiency_bps_hz"], bound, places=8)
        self.assertLessEqual(rows[0]["spectral_efficiency_bps_hz"], bound + 1e-7)
        self.assertGreater(bound - rows[0]["spectral_efficiency_bps_hz"], 0.01)

    def test_current_loss_controls_power_and_sounding_loss_does_not(self):
        tx, csi = synthetic_pair()
        first = self.analyze(tx, csi)
        changed_csi = copy.deepcopy(csi)
        changed_csi.csi[0].rx_nodes[0].wb_loss += 30
        ignored = self.analyze(tx, changed_csi, "old-loss")
        self.assert_reference(tx, changed_csi, ignored)
        for field in first[0]:
            if field != "analysis_seconds":
                np.testing.assert_array_equal([row[field] for row in first],
                                              [row[field] for row in ignored])
        changed_tx = copy.deepcopy(tx)
        changed_tx.csi[0].rx_nodes[0].wb_loss += 10
        lower = self.analyze(changed_tx, csi, "current-loss")
        self.assert_reference(changed_tx, csi, lower)
        self.assertAlmostEqual(lower[0]["received_power_w"] / first[0]["received_power_w"],
                               0.1, places=12)
        self.assertLess(lower[0]["spectral_efficiency_bps_hz"],
                        first[0]["spectral_efficiency_bps_hz"])

    def test_zero_age_stationary_age_controls_and_exact_native_repeatability(self):
        tx, _ = synthetic_pair()
        fresh = self.analyze(tx, tx, "zero")
        self.assert_reference(tx, tx, fresh)
        for age in (1_000_000, 5_000_000, 20_000_000, 50_000_000):
            stationary = copy.deepcopy(tx)
            stationary.csi[0].start_time = TX_TIME_NS - age
            stationary.csi[0].end_time = TX_TIME_NS - age + 5_000_000
            rows = self.analyze(tx, stationary, str(age))
            self.assert_reference(tx, stationary, rows)
            for field in ("sinr0_linear", "sinr1_linear", "effective_sinr0_linear",
                          "effective_sinr1_linear", "spectral_efficiency_bps_hz",
                          "channel_correlation", "channel_relative_change"):
                np.testing.assert_array_equal([row[field] for row in rows],
                                              [row[field] for row in fresh])
        repeat = self.analyze(tx, tx, "repeat")
        for field in fresh[0]:
            if field != "analysis_seconds":
                np.testing.assert_array_equal([row[field] for row in fresh],
                                              [row[field] for row in repeat])

    def test_invalid_pairs_rejected_before_creating_output(self):
        for label, tx, csi in malformed_pairs():
            with self.subTest(defect=label), self.assertRaises(subprocess.CalledProcessError):
                self.analyze(tx, csi, label)
            self.assertFalse((self.directory / ("metrics" + label + ".csv")).exists())


class TestPhysicalAgingPlanAndExport(unittest.TestCase):
    def test_fixed_plan(self):
        policy = aging.plan()
        self.assertEqual(policy["schema_version"], 1)
        self.assertEqual(policy["subphase"], "R5")
        sweep = policy["sweep"]
        self.assertEqual(set(sweep["modes"]), {"position", "doppler"})
        self.assertEqual(tuple(sweep["seeds"]), (42, 43, 44))
        self.assertEqual(tuple(sweep["ages_ms"]), (0, 1, 5, 20, 50))
        self.assertEqual(policy["controls"]["samples_per_src"], 100000)
        self.assertEqual(sweep["repetitions"], 2)
        self.assertEqual(len(sweep["cases"]), 2)
        self.assertEqual(len({case[0] for case in sweep["cases"]}), 2)
        self.assertEqual(tuple(sweep["speeds_m_s"]), (0, 1))
        self.assertEqual({(case[1], case[2], speed)
                          for case in sweep["cases"] for speed in sweep["speeds_m_s"]},
                         set(itertools.product((90,), (90, 0), (0, 1))))
        self.assertEqual(policy["transmission_ms"], 50)
        self.assertEqual(policy["controls"]["normalization"], "unit_average_pair_power")
        self.assertTrue(policy["controls"]["perfect_rx_csi"])
        self.assertTrue(policy["fixed_acceptance"])
        self.assertFalse(aging.plan(samples=10000)["fixed_acceptance"])
        self.assertFalse(aging.plan(repetitions=3)["fixed_acceptance"])

    def test_existing_output_and_invalid_parameters_do_not_start_measurements(self):
        with tempfile.TemporaryDirectory(prefix="r5-policy-") as temporary:
            directory = Path(temporary)
            existing = directory / "historical"
            existing.mkdir()
            marker = existing / "raw.pb"
            marker.write_bytes(b"immutable existing measurement")
            with self.assertRaises(ValueError):
                aging.run(existing, directory / "unused-binary")
            self.assertEqual(marker.read_bytes(), b"immutable existing measurement")
            self.assertEqual(sorted(path.name for path in existing.iterdir()), ["raw.pb"])
            for field, value in (("samples", 0), ("samples", -1), ("samples", 1.5),
                                 ("repetitions", 0), ("repetitions", 1), ("repetitions", -1),
                                 ("repetitions", 1.5)):
                output = directory / (field + str(value))
                with self.subTest(field=field, value=value), self.assertRaises(ValueError):
                    aging.run(output, directory / "unused-binary", **{field: value})
                self.assertFalse((output / "manifest.json").exists())

    @unittest.skipUnless(os.environ.get("PHYSICAL_AGING_RESULTS"),
                         "optional complete local R5 physical aging export")
    def test_local_export_scientific_validation_and_successful_provenance(self):
        output = Path(os.environ["PHYSICAL_AGING_RESULTS"])
        verdict = aging.validate_export(output)
        self.assertEqual(verdict["status"], "passed")
        self.assertEqual(verdict["evaluations"], 2 * 3 * 2 * 2 * 2 * 5)
        plan = json.loads((output / "plan.json").read_text())
        manifest = json.loads((output / "manifest.json").read_text())
        record = json.loads((output / "run.json").read_text())
        self.assertEqual(record["status"], "success")
        self.assertIsNotNone(record["finished_utc"])
        self.assertEqual(record["configuration"], plan)
        self.assertEqual(manifest["plan_sha256"], base.sha256(output / "plan.json"))
        self.assertEqual(manifest["validation"]["status"], "passed")
        self.assertEqual(record["validation"], manifest["validation"])
        self.assertEqual(record["backend"], "llvm_ad_mono_polarized")
        self.assertTrue(record["native_commands"])
        for name in ("plan.json", "manifest.json", "summary.csv"):
            self.assertEqual(record["artifacts"][name], base.provenance.digest(output / name))
        for evaluation in manifest["evaluations"]:
            for artifact in ("transmission", "sounding", "metrics"):
                name = evaluation[artifact]
                self.assertEqual(record["artifacts"][name], base.provenance.digest(output / name))

    @unittest.skipUnless(os.environ.get("PHYSICAL_AGING_RESULTS"),
                         "optional complete local R5 physical aging export")
    def test_export_rejects_missing_paths_changed_policy_configuration_and_metrics(self):
        source = Path(os.environ["PHYSICAL_AGING_RESULTS"])
        for defect in ("missing_paths", "manifest_policy", "recorded_policy",
                       "configuration", "scientific_metrics"):
            with self.subTest(defect=defect), tempfile.TemporaryDirectory(
                    prefix="r5-export-corruption-") as temporary:
                output = Path(temporary) / "study"
                shutil.copytree(source, output)
                manifest_path = output / "manifest.json"
                manifest = json.loads(manifest_path.read_text())
                if defect == "missing_paths":
                    paths = list(output.rglob("paths.npz"))
                    self.assertTrue(paths)
                    # Removing every path input must not turn the independent
                    # path-sum validation into an empty, successful loop.
                    for path in paths:
                        path.unlink()
                elif defect == "manifest_policy":
                    manifest["controls"]["normalization"] = "corrupted"
                elif defect == "recorded_policy":
                    plan_path = output / "plan.json"
                    policy = json.loads(plan_path.read_text())
                    policy["controls"]["max_depth"] += 1
                    plan_path.write_text(json.dumps(policy))
                    manifest["plan_sha256"] = base.sha256(plan_path)
                elif defect == "configuration":
                    first = manifest["configurations"][0]
                    path = output / first["case_id"] / f"seed_{first['seed']}" / "configuration.pb"
                    config = base.message_pb2.SimInitMessage.FromString(path.read_bytes())
                    config.tx_array.horizontal_spacing = 0.75
                    path.write_bytes(config.SerializeToString(deterministic=True))
                elif defect == "scientific_metrics":
                    first = manifest["evaluations"][0]
                    path = output / first["metrics"]
                    rows = base.read_metrics(path)
                    rows[73]["sinr0_linear"] += max(rows[73]["sinr0_linear"], 1.0)
                    base.write_csv(path, rows)
                    # Rehash the changed CSV so acceptance must reject its
                    # scientific contents as well as accidental byte damage.
                    first["metrics_sha256"] = base.sha256(path)
                manifest_path.write_text(json.dumps(manifest))
                with self.assertRaises((ValueError, AssertionError, FileNotFoundError)):
                    aging.validate_export(output)


if __name__ == "__main__":
    unittest.main()
