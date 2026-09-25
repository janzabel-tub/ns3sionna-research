"""Contract and recovery checks for the Phase 5.1 calibration workflow."""

import csv
import json
import math
import tempfile
import unittest
from pathlib import Path

from calibrate_eesm import CalibrationError, calibrate, eesm, load_reference, load_model


PHY = {
    "mcs": 3,
    "coding": "BCC",
    "packet_size_bytes": 1458,
    "nss": 1,
    "num_rx_antennas": 1,
    "channel_width_mhz": 20,
    "guard_interval_ns": 800,
}


class CalibrationTest(unittest.TestCase):
    def setUp(self):
        self.directory = tempfile.TemporaryDirectory()
        self.addCleanup(self.directory.cleanup)
        root = Path(self.directory.name)
        self.model_path = root / "awgn.csv"
        self.reference_path = root / "measurements.json"
        self.beta = 2.0
        with self.model_path.open("w", newline="", encoding="utf-8") as handle:
            writer = csv.DictWriter(handle, fieldnames=[*PHY, "snr_db", "per"])
            writer.writeheader()
            for step in range(-80, 161):
                snr = step / 4
                writer.writerow({**PHY, "snr_db": snr, "per": self.per(snr)})
        points = []
        for case, shape in enumerate(([0.2, 2.0, 0.4, 1.5],
                                      [0.1, 1.0, 2.3, 3.1],
                                      [0.3, 0.8, 1.7, 2.8])):
            for db in (3, 5, 7, 9):
                samples = [value * 10 ** (db / 10) for value in shape]
                effective = eesm(samples, self.beta)
                observed = self.per(10 * math.log10(effective))
                points.append({
                    "case_id": f"channel-{case}",
                    "split": "validation" if case == 2 else "train",
                    "sinr_linear": samples,
                    "trials": 100000,
                    "errors": round(100000 * observed),
                })
        self.reference = {
            "source": "synthetic recovery fixture",
            "source_version": "1",
            "source_kind": "synthetic",
            "phy": PHY,
            "awgn": [{"snr_db": db,
                      "split": "train" if index % 2 == 0 else "validation",
                      "trials": 100000,
                      "errors": round(100000 * self.per(db))}
                     for index, db in enumerate((1, 3, 5, 7, 9))],
            "selective": points,
        }
        self.save()

    @staticmethod
    def per(snr):
        return 1 / (1 + math.exp((snr - 5) / 1.5))

    def save(self):
        self.reference_path.write_text(json.dumps(self.reference), encoding="utf-8")

    def test_recovers_known_beta_without_publishing_synthetic_coefficient(self):
        result = calibrate(self.model_path, self.reference_path)
        self.assertAlmostEqual(result["candidate_beta_linear"], self.beta, delta=0.08)
        self.assertAlmostEqual(result["candidate_awgn_snr_offset_db"], 0.0, delta=0.02)
        self.assertLess(result["validation"]["mae"], 0.005)
        self.assertEqual(result["status"], "rejected")
        self.assertNotIn("beta_linear", result)
        self.assertIn("reference is synthetic", result["reasons"])

    def test_rejects_awgn_mismatch(self):
        for point in self.reference["awgn"]:
            if point["split"] == "validation":
                point["errors"] = round(point["trials"] * 0.5)
        self.reference["source_kind"] = "link_level"
        self.save()
        result = calibrate(self.model_path, self.reference_path)
        self.assertEqual(result["status"], "rejected")
        self.assertIn("held-out AWGN PER error exceeds threshold",
                      result["reasons"])
        self.assertNotIn("beta_linear", result)

    def test_recovers_independent_awgn_offset(self):
        for point in self.reference["awgn"]:
            point["errors"] = round(100000 * self.per(point["snr_db"] - 0.5))
        for point in self.reference["selective"]:
            effective = eesm(point["sinr_linear"], self.beta)
            point["errors"] = round(100000 * self.per(10 * math.log10(effective) - 0.5))
        self.save()
        result = calibrate(self.model_path, self.reference_path)
        self.assertAlmostEqual(result["candidate_awgn_snr_offset_db"], -0.5,
                               delta=0.03)
        self.assertAlmostEqual(result["candidate_beta_linear"], self.beta,
                               delta=0.1)

    def test_validated_result_contains_both_parameters(self):
        self.reference["source_kind"] = "link_level"
        self.save()
        result = calibrate(self.model_path, self.reference_path)
        self.assertEqual(result["status"], "validated")
        self.assertIn("beta_linear", result)
        self.assertIn("awgn_snr_offset_db", result)

    def test_rejects_bad_held_out_prediction(self):
        self.reference["source_kind"] = "link_level"
        for point in self.reference["selective"]:
            if point["split"] == "validation":
                point["errors"] = round(point["trials"] * 0.5)
        self.save()
        result = calibrate(self.model_path, self.reference_path)
        self.assertEqual(result["status"], "rejected")
        self.assertIn("held-out frequency selective PER error exceeds threshold",
                      result["reasons"])
        self.assertNotIn("beta_linear", result)

    def test_rejects_split_leakage(self):
        self.reference["selective"][0]["case_id"] = "channel-2"
        self.save()
        metadata, _, _ = load_model(self.model_path)
        with self.assertRaisesRegex(CalibrationError, "both train and validation"):
            load_reference(self.reference_path, metadata)

    def test_rejects_mismatched_phy(self):
        self.reference["phy"] = {**PHY, "coding": "LDPC"}
        self.save()
        metadata, _, _ = load_model(self.model_path)
        with self.assertRaisesRegex(CalibrationError, "differs from AWGN"):
            load_reference(self.reference_path, metadata)

    def test_rejects_bad_counts(self):
        self.reference["selective"][0]["errors"] = 100001
        self.save()
        metadata, _, _ = load_model(self.model_path)
        with self.assertRaisesRegex(CalibrationError, "exceeds trials"):
            load_reference(self.reference_path, metadata)


if __name__ == "__main__":
    unittest.main()
