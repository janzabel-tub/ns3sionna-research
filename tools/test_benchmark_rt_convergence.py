"""Policy, repeatability, independent reference and real RT replay validation.

SUMIMO_CONVERGENCE_RESULTS overrides the checked-in CPU reference export.
"""
import copy
import csv
import itertools
import json
import os
from pathlib import Path
import tempfile
import unittest

import numpy as np
import benchmark_rt_convergence as convergence
import benchmark_sumimo as base


class TestConvergencePolicy(unittest.TestCase):
    def reference(self):
        return [{"subcarrier": i, "frequency_offset_hz": i * 78125, "rank": 2,
                 "sigma0": 1.5, "sigma1": 0.1, "sinr0_linear": 100.0,
                 "sinr1_linear": 10.0, "effective_sinr0_linear": 100.0,
                 "effective_sinr1_linear": 10.0, "wideband_loss_db": 55.0,
                 "spectral_efficiency_bps_hz": 10.0} for i in range(4)]

    def test_identical_channels_and_each_failed_criterion(self):
        reference = self.reference()
        self.assertTrue(convergence.compare(reference, reference)["passed"])
        changes = (("wideband_loss_db", 55.501, "wideband_loss_db"),
                   ("rank", 1, "rank_changed_tones"),
                   ("sigma1", 0.106, "singular_value_scaled_error"),
                   ("sinr1_linear", 10 * 10 ** 0.101, "per_tone_sinr_db"),
                   ("effective_sinr1_linear", 10 * 10 ** 0.051, "effective_sinr_db"),
                   ("spectral_efficiency_bps_hz", 10.201, "spectral_efficiency_bps_hz"))
        for field, value, criterion in changes:
            with self.subTest(field=field):
                changed = copy.deepcopy(reference)
                changed[0 if field in ("wideband_loss_db", "spectral_efficiency_bps_hz",
                                      "effective_sinr1_linear") else 2][field] = value
                result = convergence.compare(changed, reference)
                self.assertFalse(result["passed"])
                self.assertFalse(result[criterion + "_passed"])

    def test_zero_mode_absolute_floor_and_invalid_comparisons(self):
        reference = self.reference()
        reference[1].update(sigma1=0, sinr1_linear=0)
        changed = copy.deepcopy(reference)
        changed[1]["sigma1"] = convergence.TOLERANCES["singular_value_atol"]
        self.assertTrue(convergence.compare(changed, reference)["passed"])
        changed[1]["sigma1"] *= 1.01
        self.assertFalse(convergence.compare(changed, reference)["passed"])
        for field, value in (("frequency_offset_hz", -1), ("subcarrier", 3),
                             ("sinr0_linear", float("nan"))):
            broken = copy.deepcopy(reference)
            broken[0][field] = value
            with self.subTest(field=field), self.assertRaises(ValueError):
                convergence.compare(broken, reference)
        for broken in ([], reference[:-1]):
            with self.assertRaises(ValueError):
                convergence.compare(broken, reference)

    def test_all_seed_case_decision_and_incomplete_matrix(self):
        reference = self.reference()
        metrics = {(case[0], seed, budget): reference for case in convergence.CASES
                   for seed in convergence.SEEDS for budget in convergence.BUDGETS}
        rows = convergence.comparisons(metrics)
        self.assertEqual(convergence.verdict(rows)["minimum_supported_samples_per_src"], 10000)
        rows[0].update(passed=False, per_tone_sinr_db_passed=False)
        self.assertEqual(convergence.verdict(rows)["minimum_supported_samples_per_src"], 100000)
        rows[1].update(passed=False, effective_sinr_db_passed=False)
        self.assertFalse(convergence.verdict(rows)["convergence_achieved_in_tested_range"])
        self.assertIsNone(convergence.verdict(rows)["minimum_supported_samples_per_src"])
        for broken in (rows[:-1], rows + [rows[0]]):
            with self.assertRaises(ValueError):
                convergence.verdict(broken)

    def test_native_seed_export_and_invalid_controls(self):
        # The optional 5.2.4 mode keeps a small, predefined matrix and existing policy.
        policy = convergence.plan(independent_spacing=True)
        self.assertEqual(policy["subphase"], "5.2.4")
        self.assertEqual(len(policy["sweep"]["cases"]), 5)
        self.assertEqual(policy["sweep"]["samples_per_src"], 100000)
        self.assertEqual(policy["spot_checks"]["case_ids"], ("case_03", "case_04"))
        self.assertEqual(policy["tolerances"], convergence.TOLERANCES)
        with tempfile.TemporaryDirectory(prefix="sumimo-seeds-") as temporary:
            path = Path(temporary)
            for seed in convergence.SEEDS:
                config = base.export_configuration(base.executable(), path / f"{seed}.pb", 0.5,
                                                   (0, 0, 0), (90, 0, 0), seed=seed)
                self.assertEqual(config.seed, seed)
                base.export_configuration(base.executable(), path / "repeat.pb", 0.5,
                                          (0, 0, 0), (90, 0, 0), seed=seed)
                self.assertEqual((path / f"{seed}.pb").read_bytes(), (path / "repeat.pb").read_bytes())
            with self.assertRaises(ValueError):
                convergence.run(path, base.executable())
            for budget in (0, -1, 1.5):
                with self.subTest(budget=budget), self.assertRaises(ValueError):
                    base.trace(config, None, budget)
            with self.assertRaises(ValueError):
                convergence.run(path, base.executable(), repetitions=1)


class TestConvergenceExport(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.output = Path(os.environ.get("SUMIMO_CONVERGENCE_RESULTS",
                                        base.MODULE / "data/phase5_2_sample_convergence"))
        cls.manifest = json.loads((cls.output / "manifest.json").read_text())
        cls.policy = json.loads((cls.output / "plan.json").read_text())
        cls.summaries, cls.tones, cls.errors = [], [], []
        for filename, target in (("summary.csv", cls.summaries), ("tones.csv", cls.tones),
                                 ("convergence.csv", cls.errors)):
            with (cls.output / filename).open(newline="") as stream:
                target.extend(csv.DictReader(stream))
        cls.metrics = {}
        for row in cls.tones:
            identity = (row["case_id"], int(row["seed"]), int(row["samples_per_src"]),
                        int(row["repetition"]))
            cls.metrics.setdefault(identity, []).append(
                {key: float(value) for key, value in row.items() if key not in convergence.IDENTITY_FIELDS})

    def config_dir(self, case_id, seed):
        return self.output / case_id / f"seed_{seed}"

    def channel_path(self, case_id, seed, budget, repetition=0):
        return self.config_dir(case_id, seed) / f"samples_{budget}" / f"channel_{repetition:02d}.pb"

    def response(self, case_id, seed, budget, repetition=0):
        return base.message_pb2.ChannelStateResponse.FromString(
            self.channel_path(case_id, seed, budget, repetition).read_bytes())

    def test_fixed_plan_controls_and_complete_matrix(self):
        policy = json.loads(json.dumps(convergence.plan()))
        for key, value in policy.items():
            self.assertEqual(self.policy[key], value)
            self.assertEqual(self.manifest[key], value)
        self.assertEqual(base.sha256(self.output / "plan.json"), self.manifest["plan_sha256"])
        self.assertIn("defined_before_tracing_utc", self.policy)
        expected = set(itertools.product([case[0] for case in convergence.CASES],
            convergence.SEEDS, convergence.BUDGETS, range(2)))
        self.assertEqual(set(self.metrics), expected)
        self.assertEqual(len(self.summaries), 72)
        self.assertEqual(len(self.tones), 72 * 256)
        self.assertEqual(self.manifest["validation"]["fresh_traces"], 72)
        self.assertEqual(self.manifest["provenance"]["backend"], "llvm_ad_mono_polarized")
        actual = {(row["case_id"], int(row["seed"]), int(row["samples_per_src"]),
                   int(row["repetition"])) for row in self.summaries}
        self.assertEqual(actual, expected)
        for rows in self.metrics.values():
            self.assertEqual(len(rows), 256)
            self.assertEqual([row["subcarrier"] for row in rows], list(range(256)))
            self.assertEqual([row["frequency_offset_hz"] for row in rows],
                             [(index - 128) * 78125 for index in range(256)])
        for row in self.summaries:
            for key in ("init_seconds", "cfr_seconds", "analysis_seconds",
                        "native_process_seconds", "case_seconds"):
                self.assertGreater(float(row[key]), 0)

    def test_export_hashes_geometry_and_repeatability(self):
        expected_records = {(case[0], seed) for case in convergence.CASES for seed in convergence.SEEDS}
        self.assertEqual({(record["case_id"], record["seed"])
                          for record in self.manifest["configurations"]}, expected_records)
        self.assertEqual(len(self.manifest["configurations"]), len(expected_records))
        for record in self.manifest["configurations"]:
            case_id, seed = record["case_id"], record["seed"]
            config_path = self.config_dir(case_id, seed) / "configuration.pb"
            self.assertEqual(base.sha256(config_path), record["configuration_sha256"])
            config = base.message_pb2.SimInitMessage.FromString(config_path.read_bytes())
            case = next(case for case in convergence.CASES if case[0] == case_id)
            base.validate_configuration(config, case[1], (case[2], 0, 0), (case[3], 0, 0), seed)
            self.assertEqual(base.MessageToDict(config, preserving_proto_field_name=True,
                always_print_fields_with_no_presence=True), record["configuration"])
            self.assertEqual(len(record["traces"]), 6)
            for trace in record["traces"]:
                budget, repetition = trace["samples_per_src"], trace["repetition"]
                self.assertEqual(base.sha256(self.channel_path(case_id, seed, budget, repetition)),
                                 trace["channel_sha256"])
                response = self.response(case_id, seed, budget, repetition)
                np.testing.assert_allclose(np.mean(abs(base.channel_array(response)) ** 2),
                                           1, rtol=1e-6, atol=1e-7)
                base.compare_repetitions(self.response(case_id, seed, budget), response,
                    self.metrics[case_id, seed, budget, 0], self.metrics[case_id, seed, budget, repetition])
                for endpoint, angles, position in zip(("tx", "rx"),
                        ((case[2], 0, 0), (case[3], 0, 0)), base.POSITIONS):
                    observed = trace["observed_geometry"][endpoint]
                    np.testing.assert_allclose(observed["orientation_rad"], np.deg2rad(angles),
                                               rtol=0, atol=1e-7)
                    np.testing.assert_allclose(observed["position_m"], position)
                    np.testing.assert_allclose(observed["array_positions_lambda"],
                        [[0, -case[1] / 2, 0], [0, case[1] / 2, 0]], rtol=0, atol=1e-7)

    def test_all_channels_against_independent_numpy_mmse_svd_mi_and_budget(self):
        noise = 1.3803e-23 * 290 * 20e6 * 10 ** 0.7
        for (case_id, seed, budget, repetition), rows in self.metrics.items():
            with self.subTest(case=case_id, seed=seed, budget=budget, repetition=repetition):
                response = self.response(case_id, seed, budget, repetition)
                h = base.channel_array(response)
                loss = response.csi[0].rx_nodes[0].wb_loss
                power = 10 ** ((20 - loss - 30) / 10)
                rho = power / noise
                gram = h.conj().transpose(0, 2, 1) @ h
                error = np.linalg.inv(np.eye(2) + rho / 2 * gram)
                sinr = 1 / np.diagonal(error, axis1=1, axis2=2).real - 1
                singular = np.linalg.svd(h, compute_uv=False)
                ranks = np.sum(singular > 1e-12 * singular[:, :1], axis=1)
                np.testing.assert_allclose([[row["sigma0"], row["sigma1"]] for row in rows],
                                           singular, rtol=1e-10, atol=1e-10)
                np.testing.assert_array_equal([row["rank"] for row in rows], ranks)
                np.testing.assert_allclose([[row["sinr0_linear"], row["sinr1_linear"]] for row in rows],
                                           sinr, rtol=1e-8, atol=1e-8)
                effective = np.expm1(np.mean(np.log1p(sinr), axis=0))
                for row in rows:
                    np.testing.assert_allclose([row[f"effective_sinr{s}_linear"] for s in range(2)],
                                               effective, rtol=1e-8, atol=1e-8)
                    np.testing.assert_allclose([row["wideband_loss_db"], row["noise_power_w"],
                        row["received_power_w"], row["reference_snr_linear"]],
                        [loss, noise, power, rho], rtol=1e-12)
                    self.assertAlmostEqual(row["spectral_efficiency_bps_hz"],
                                           float(np.mean(np.sum(np.log2(1 + sinr), axis=1))), places=8)
                summary = next(row for row in self.summaries if row["case_id"] == case_id
                    and int(row["seed"]) == seed and int(row["samples_per_src"]) == budget
                    and int(row["repetition"]) == repetition)
                for key in ("wideband_loss_db", "noise_power_w", "received_power_w", "reference_snr_linear",
                            "effective_sinr0_linear", "effective_sinr1_linear", "spectral_efficiency_bps_hz"):
                    self.assertEqual(float(summary[key]), rows[0][key])
                self.assertEqual(float(summary["minimum_rank"]), min(ranks))
                np.testing.assert_allclose([float(summary[f"median_sigma{s}"]) for s in range(2)],
                                           np.median(singular, axis=0), rtol=1e-10, atol=1e-10)
                self.assertAlmostEqual(float(summary["median_sigma_ratio"]),
                                       float(np.median(singular[:, 1] / singular[:, 0])), places=10)
                np.testing.assert_allclose([float(summary[f"effective_sinr{s}_db"]) for s in range(2)],
                                           10 * np.log10(effective), rtol=1e-10)

    def test_decision_and_error_exports_are_recomputed(self):
        paired = {identity[:3]: rows for identity, rows in self.metrics.items() if identity[3] == 0}
        expected = convergence.comparisons(paired)
        self.assertEqual(len(self.errors), len(expected))
        for exported, row in zip(self.errors, expected):
            self.assertEqual(set(exported), set(row))
            for key, value in row.items():
                actual = exported[key]
                if isinstance(value, bool):
                    self.assertEqual(actual, str(value))
                elif isinstance(value, str):
                    self.assertEqual(actual, value)
                else:
                    self.assertAlmostEqual(float(actual), value, places=12)
        expected_verdict = convergence.verdict(expected)
        self.assertEqual(json.loads((self.output / "convergence.json").read_text()), expected_verdict)
        self.assertEqual(self.manifest["convergence"], expected_verdict)

    def test_native_replay_of_all_unique_channels_and_previous_reference(self):
        with tempfile.TemporaryDirectory(prefix="sumimo-convergence-replay-") as temporary:
            path = Path(temporary) / "metrics.csv"
            for case_id, seed, budget in itertools.product([case[0] for case in convergence.CASES],
                                                         convergence.SEEDS, convergence.BUDGETS):
                base.native(base.executable(), action="analyze", output=path,
                            input=self.channel_path(case_id, seed, budget))
                response = self.response(case_id, seed, budget)
                base.compare_repetitions(response, response, self.metrics[case_id, seed, budget, 0],
                                         base.read_metrics(path))
                if seed == 42 and budget == 100000:
                    old = base.MODULE / "data/phase5_2_spacing_orientation" / case_id / "channel_00.pb"
                    self.assertEqual(old.read_bytes(), self.channel_path(case_id, seed, budget).read_bytes())

    def test_fresh_process_real_traces_match_all_budgets_and_seeds(self):
        env_class, _ = base.runtime()
        for case_id, seed, budget in (("case_03", 42, 10000), ("case_05", 43, 100000),
                                     ("case_06", 44, 1000000)):
            with self.subTest(case=case_id, seed=seed, budget=budget):
                config = base.message_pb2.SimInitMessage.FromString(
                    (self.config_dir(case_id, seed) / "configuration.pb").read_bytes())
                response, _, _, _ = base.trace(config, env_class, budget)
                self.assertEqual(response.SerializeToString(deterministic=True),
                                 self.channel_path(case_id, seed, budget).read_bytes())


if __name__ == "__main__":
    unittest.main()
