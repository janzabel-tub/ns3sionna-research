"""Phase 5.2.3 policy, real production-cache/IPC and frozen-export validation.

Run in the documented Python environment after building the native example.
CACHE_IPC_BENCHMARK_RESULTS overrides the checked-in reference directory.
"""
import copy
import csv
import hashlib
import itertools
import json
import os
from pathlib import Path
import struct
import subprocess
import tempfile
import unittest

import numpy as np
import benchmark_cache_ipc as benchmark


class TestFrozenFixtures(unittest.TestCase):
    def test_all_shapes_against_independent_flat_source_indices(self):
        source = benchmark.base.message_pb2.ChannelStateResponse.FromString(benchmark.ANCHOR.read_bytes())
        original = source.csi[0].rx_nodes[0]
        for nsc, (tx, rx) in itertools.product(benchmark.SUBCARRIERS, benchmark.ANTENNAS):
            wrapper = benchmark.template(nsc, tx, rx)
            radio = wrapper.channel_state_response.csi[0].rx_nodes[0]
            self.assertEqual(len(radio.csi_real), nsc * rx * tx)
            for sc, r, t in itertools.product(range(nsc), range(rx), range(tx)):
                target_index = (sc * rx + r) * tx + t
                source_index = ((sc % 256) * 2 + r % 2) * 2 + t % 2
                self.assertEqual(radio.csi_real[target_index], original.csi_real[source_index])
                self.assertEqual(radio.csi_imag[target_index], original.csi_imag[source_index])
            self.assertEqual(list(radio.frequencies), [(sc - nsc // 2) * 78125 for sc in range(nsc)])

    def test_link_metadata_phase_and_independent_bit_digest(self):
        single = benchmark.template(64, 2, 4)
        fixture = benchmark.batch(single, 16)
        source = single.channel_state_response.csi[0].rx_nodes[0]
        index, digest = 0, 0
        mask = (1 << 64) - 1
        for link, radio in enumerate(fixture.channel_state_response.csi[0].rx_nodes, 1):
            self.assertEqual(radio.id, link)
            self.assertEqual(radio.wb_loss, source.wb_loss + (link - 1) / 8)
            self.assertEqual(radio.delay, source.delay + link - 1)
            for i, (real, imag) in enumerate(zip(radio.csi_real, radio.csi_imag)):
                expected = complex(source.csi_real[i], source.csi_imag[i]) * 1j ** ((42 + link - 1) % 4)
                self.assertEqual(complex(real, imag), expected)
                rb, ib = (struct.unpack("<Q", struct.pack("<d", value))[0] for value in (real, imag))
                digest ^= (rb * (2 * index + 1) & mask) ^ (ib * (2 * index + 3) & mask)
                index += 1
        self.assertEqual(benchmark.coefficient_digest(fixture), f"{digest:016x}")
        with self.assertRaises(ValueError):
            benchmark.batch(single, 0)

    def test_frozen_bytes_repeatability_and_nonempty_output_guard(self):
        for nsc, (tx, rx), links in ((64, (1, 1), 1), (256, (2, 4), 16), (1024, (8, 8), 1)):
            first = benchmark.batch(benchmark.template(nsc, tx, rx), links)
            second = benchmark.batch(benchmark.template(nsc, tx, rx), links)
            self.assertEqual(first.SerializeToString(deterministic=True), second.SerializeToString(deterministic=True))
        with tempfile.TemporaryDirectory() as temporary:
            path = Path(temporary)
            (path / "keep.txt").write_text("existing output")
            with self.assertRaises(ValueError):
                benchmark.run(path, None)
            self.assertEqual((path / "keep.txt").read_text(), "existing output")


class TestNativeCacheIpc(unittest.TestCase):
    def test_actual_native_cache_ipc_repeatability_and_corruption_detection(self):
        fixture = benchmark.batch(benchmark.template(64, 2, 4), 16)
        first, samples, server = benchmark.measure(benchmark.executable(), fixture)
        second, _, _ = benchmark.measure(benchmark.executable(), fixture)
        self.assertEqual(benchmark.deterministic_fields(first), benchmark.deterministic_fields(second))
        # Exports must reject bad data, hidden hit RPCs, wrong expiry, and missing samples.
        for key in ("response_bytes", "cache_cfr_bytes", "verified_links", "cache_hit_count"):
            changed = copy.deepcopy(first)
            changed[key] += 1
            with self.subTest(key=key), self.assertRaises(AssertionError):
                benchmark.validate_record(changed, fixture, server, samples)
        changed = copy.deepcopy(server)
        changed["requests"][-1]["time_ns"] = 5000000
        with self.assertRaises(AssertionError):
            benchmark.validate_record(first, fixture, changed, samples)
        with self.assertRaises(AssertionError):
            benchmark.validate_record(first, fixture, server, samples[:-1])

    def test_invalid_fixture_and_count_rejected_before_transport(self):
        fixture = benchmark.batch(benchmark.template(64, 2, 4), 1)
        with tempfile.TemporaryDirectory(prefix="cache-ipc-invalid-") as temporary:
            path = Path(temporary)
            for kind in ("length", "dimensions", "frequency", "identity", "nan", "wire"):
                broken = copy.deepcopy(fixture)
                radio = broken.channel_state_response.csi[0].rx_nodes[0]
                if kind == "length":
                    del radio.csi_imag[-1]
                elif kind == "dimensions":
                    radio.num_rx_antennas = 3
                elif kind == "frequency":
                    radio.frequencies[0] = 0
                elif kind == "identity":
                    radio.id = 2
                elif kind == "nan":
                    radio.csi_real[0] = float("nan")
                wire = b"\xff" if kind == "wire" else broken.SerializeToString(deterministic=True)
                (path / "input.pb").write_bytes(wire)
                with self.subTest(kind=kind), self.assertRaises(subprocess.CalledProcessError):
                    benchmark.base.native(benchmark.executable(), input=path / "input.pb",
                        output=path / "result.json", timings=path / "times.csv", endpoint="ipc://" + str(path / "absent"))
            with self.assertRaises(subprocess.CalledProcessError):
                benchmark.base.native(benchmark.executable(), links=0)


class TestCacheIpcExport(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.output = Path(os.environ.get("CACHE_IPC_BENCHMARK_RESULTS",
                         benchmark.base.MODULE / "data/phase5_2_cache_ipc"))
        cls.manifest = json.loads((cls.output / "manifest.json").read_text())
        cls.policy = json.loads((cls.output / "plan.json").read_text())
        cls.summaries, cls.samples, cls.requests = [], [], []
        for filename, target in (("summary.csv", cls.summaries), ("timings.csv", cls.samples),
                                 ("requests.csv", cls.requests)):
            with (cls.output / filename).open(newline="") as stream:
                target.extend(csv.DictReader(stream))

    def fixture(self, configuration):
        path = self.output / configuration["template"]
        single = benchmark.base.message_pb2.Wrapper.FromString(path.read_bytes())
        return benchmark.batch(single, configuration["links"])

    def native_record(self, row):
        return {key: value if key in ("coefficient_digest", "cpp_zmq_version") else float(value)
                for key, value in row.items() if key not in ("case_id", "repetition")}

    def test_predefined_policy_complete_matrix_and_exact_memory_scaling(self):
        expected_policy = json.loads(json.dumps(benchmark.plan()))
        for key, value in expected_policy.items():
            self.assertEqual(self.policy[key], value)
            self.assertEqual(self.manifest[key], value)
        self.assertEqual(benchmark.base.sha256(self.output / "plan.json"), self.manifest["plan_sha256"])
        expected = set(itertools.product(benchmark.SUBCARRIERS, benchmark.ANTENNAS, benchmark.LINKS, range(3)))
        actual = {(int(row["subcarriers"]), (int(row["num_tx_antennas"]), int(row["num_rx_antennas"])),
                   int(row["links"]), int(row["repetition"])) for row in self.summaries}
        self.assertEqual(actual, expected)
        self.assertEqual(len(self.summaries), 135)
        self.assertEqual(len(self.samples), 135 * 229)
        self.assertEqual(len(self.requests), 135 * 27)
        self.assertEqual(len(self.manifest["configurations"]), 45)
        for row in self.summaries:
            coefficients = int(row["links"]) * int(row["subcarriers"]) * int(row["num_rx_antennas"]) * int(row["num_tx_antennas"])
            self.assertEqual(float(row["cache_cfr_bytes"]), 16 * coefficients)
            self.assertEqual(float(row["cache_frequency_bytes"]), 4 * int(row["links"]) * int(row["subcarriers"]))

    def test_all_hashes_repeatability_native_reference_and_rpc_transcripts(self):
        for configuration in self.manifest["configurations"]:
            fixture = self.fixture(configuration)
            self.assertEqual(benchmark.base.sha256(self.output / configuration["template"]),
                             configuration["template_sha256"])
            self.assertEqual(hashlib.sha256(fixture.SerializeToString(deterministic=True)).hexdigest(),
                             configuration["response_sha256"])
            self.assertEqual(benchmark.coefficient_digest(fixture), configuration["coefficient_digest"])
            first = None
            for repetition in range(3):
                rows = [row for row in self.summaries if row["case_id"] == configuration["case_id"]
                        and int(row["repetition"]) == repetition]
                self.assertEqual(len(rows), 1)
                record = self.native_record(rows[0])
                samples = [{"operation": row["operation"], "iteration": int(row["iteration"]),
                            "nanoseconds": int(row["nanoseconds"])} for row in self.samples
                           if row["case_id"] == configuration["case_id"] and int(row["repetition"]) == repetition]
                requests = [row for row in self.requests if row["case_id"] == configuration["case_id"]
                            and int(row["repetition"]) == repetition]
                self.assertEqual([int(row["request_index"]) for row in requests], list(range(27)))
                server = {"channel_requests": int(record["server_channel_requests"]),
                    "close_requests": int(record["server_close_requests"]),
                    "peak_rss_bytes": record["rep_server_peak_rss_bytes"],
                    "requests": [{key: int(row[key]) for key in ("tx_node", "rx_node", "time_ns")}
                                 for row in requests]}
                benchmark.validate_record(record, fixture, server, samples)
                if first is None:
                    first = benchmark.deterministic_fields(record)
                else:
                    self.assertEqual(benchmark.deterministic_fields(record), first)

    def test_all_latency_summary_statistics_against_raw_samples(self):
        for row in self.summaries:
            samples = [sample for sample in self.samples if sample["case_id"] == row["case_id"]
                       and sample["repetition"] == row["repetition"]]
            for operation in {sample["operation"] for sample in samples}:
                values = [int(sample["nanoseconds"]) for sample in samples if sample["operation"] == operation]
                for statistic, expected in (("median", np.median(values)), ("p95", np.percentile(values, 95)),
                                            ("min", min(values)), ("max", max(values))):
                    self.assertEqual(float(row[f"{operation}_{statistic}_ns"]), expected)

    def test_fresh_process_replay_across_shapes_and_link_counts(self):
        for nsc, antennas, links in ((64, (1, 1), 1), (256, (2, 4), 16), (1024, (8, 8), 64)):
            configuration = next(row for row in self.manifest["configurations"]
                if row["subcarriers"] == nsc and (row["num_tx_antennas"], row["num_rx_antennas"]) == antennas
                and row["links"] == links)
            result, _, _ = benchmark.measure(benchmark.executable(), self.fixture(configuration))
            expected = next(row for row in self.summaries if row["case_id"] == configuration["case_id"])
            self.assertEqual(benchmark.deterministic_fields(result),
                             benchmark.deterministic_fields(self.native_record(expected)))


if __name__ == "__main__":
    unittest.main()
