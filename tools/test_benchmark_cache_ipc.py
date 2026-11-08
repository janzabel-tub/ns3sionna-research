"""Phase 5.2.3 policy, real production-cache/IPC and frozen-export validation.

Run in the documented Python environment after building the native example.
CACHE_IPC_BENCHMARK_RESULTS overrides the checked-in reference directory.
It can also point to a --copy-profile export (six processes, 4446 timing rows,
162 channel requests); the historical default export remains unchanged.
CACHE_IPC_WORKLOAD_RESULTS separately enables validation of a local workload
sweep. Native workload and failure/policy regressions run without an export.
"""
import copy
import csv
import hashlib
import itertools
import json
import os
from pathlib import Path
import shutil
import struct
import subprocess
import tempfile
import unittest
from unittest.mock import patch

import numpy as np
import benchmark_cache_ipc as benchmark


class TestHeapAccountingPortability(unittest.TestCase):
    def test_production_helper_compiles_with_new_and_legacy_glibc_interfaces(self):
        source = (benchmark.base.MODULE / "examples/ns3_sionna_cache_ipc_benchmark.cc").read_text()
        helper = source[source.index("void\nRequire("):source.index("uint64_t\nRssBytes(")]
        compiler = shutil.which("c++")
        self.assertIsNotNone(compiler, "A C++ compiler is required for the native helper test")
        cases = (
            (33, "uint64_t", "mallinfo2", "8589934592ULL", "4294967296ULL",
             0, "12884901888 glibc_mallinfo2"),
            (32, "int", "mallinfo", "1234", "5678", 0, "6912 glibc_mallinfo_32bit"),
            (31, "int", "mallinfo", "2147483647", "2147483647",
             0, "4294967294 glibc_mallinfo_32bit"),
            (31, "int", "mallinfo", "-1", "1", 2, "overflowed"),
            (31, "int", "mallinfo", "1", "-1", 2, "overflowed"),
        )
        with tempfile.TemporaryDirectory(prefix="cache-heap-portability-") as temporary:
            root = Path(temporary)
            for index, (minor, field_type, function, arena, mapped, status, expected) in enumerate(cases):
                with self.subTest(glibc_minor=minor, arena=arena, mapped=mapped):
                    path = root / f"case{index}.cc"
                    path.write_text(
                        '#include <cstdint>\n#include <iostream>\n#include <stdexcept>\n'
                        'using std::uint64_t;\n#undef __GLIBC__\n#undef __GLIBC_MINOR__\n'
                        f'#define __GLIBC__ 2\n#define __GLIBC_MINOR__ {minor}\n'
                        f'struct HeapInfo {{ {field_type} uordblks, hblkhd; }};\n'
                        f'HeapInfo {function}() {{ return {{{arena}, {mapped}}}; }}\n' + helper +
                        'int main() { try { auto bytes = HeapBytes(); '
                        'std::cout << bytes << " " << HeapAccounting(); return 0; } '
                        'catch (const std::invalid_argument& error) { '
                        'std::cerr << error.what(); return 2; } }\n')
                    binary = root / f"case{index}"
                    subprocess.run([compiler, "-std=c++17", "-Wall", "-Wextra", "-Werror",
                                    str(path), "-o", str(binary)],
                                   check=True, text=True, capture_output=True, timeout=60)
                    result = subprocess.run([str(binary)], text=True, capture_output=True, timeout=10)
                    self.assertEqual(result.returncode, status, result.stderr)
                    self.assertIn(expected, result.stdout + result.stderr)


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
    def test_workload_time_active_links_depth_and_disabled_retention(self):
        fixture = benchmark.batch(benchmark.template(64, 2, 2), 16)
        for depth, enabled in itertools.product((1, 4, 16), (True, False)):
            workload = {"steps": 32, "depth": depth, "enabled": enabled}
            with self.subTest(**workload):
                first, samples, server, trace = benchmark.measure(benchmark.executable(), fixture, workload=workload)
                second, _, _, repeated = benchmark.measure(benchmark.executable(), fixture, workload=workload)
                self.assertEqual(benchmark.deterministic_fields(first), benchmark.deterministic_fields(second))
                self.assertEqual(benchmark.deterministic_trajectory(trace), benchmark.deterministic_trajectory(repeated))
                for key in ("peak_payload_bytes", "cache_miss_count", "final_capacity_bytes"):
                    changed = copy.deepcopy(first)
                    changed[key] += 1
                    with self.subTest(key=key), self.assertRaises(AssertionError):
                        benchmark.validate_workload(changed, fixture, server, samples, trace, **workload)
                for key in ("snapshots", "payload_bytes", "active_first", "time_ns", "coefficient_digest"):
                    changed = copy.deepcopy(trace)
                    changed[1][key] = "wrong" if key == "coefficient_digest" else changed[1][key] + 1
                    with self.subTest(key=key), self.assertRaises(AssertionError):
                        benchmark.validate_workload(first, fixture, server, samples, changed, **workload)
                changed = copy.deepcopy(server)
                changed["requests"][0]["rx_node"] += 1
                with self.assertRaises(AssertionError):
                    benchmark.validate_workload(first, fixture, changed, samples, trace, **workload)
                with self.assertRaises(AssertionError):
                    benchmark.validate_workload(first, fixture, server, samples[:-1], trace, **workload)

    def test_workload_invalid_parameters_rejected_before_transport(self):
        fixture = benchmark.batch(benchmark.template(64, 2, 2), 16)
        with tempfile.TemporaryDirectory() as temporary:
            path = Path(temporary)
            (path / "input.pb").write_bytes(fixture.SerializeToString(deterministic=True))
            for extra in ({"lookAhead": 0}, {"lookAhead": 3}, {"workloadSteps": 17},
                          {"workloadSteps": 4097}, {"copyProfile": 1}, {"storageSteps": 1},
                          {"trajectory": path / "input.pb"}):
                with self.subTest(extra=extra), self.assertRaises(subprocess.CalledProcessError):
                    benchmark.base.native(benchmark.executable(), **{
                        "input": path / "input.pb", "output": path / "result.json",
                        "timings": path / "times.csv", "trajectory": path / "trace.csv", "links": 16,
                        "endpoint": "ipc://" + str(path / "absent"), "workloadSteps": 32,
                        "lookAhead": 16, **extra})

    def test_optional_copy_profile_all_getters_and_strict_counts(self):
        fixture = benchmark.batch(benchmark.template(64, 2, 2), 1)
        record, samples, server = benchmark.measure(benchmark.executable(), fixture, copy_profile=True)
        self.assertEqual(record["cache_hit_count"], 656)
        self.assertEqual(record["cache_miss_count"], 2)
        self.assertEqual(len(samples), 741)
        self.assertEqual(server["channel_requests"], 27)
        for key in ("copy_profile", "verified_csi_links", "cache_hit_count", "memory_validation_hit_ratio"):
            changed = copy.deepcopy(record)
            changed[key] += 1
            with self.subTest(key=key), self.assertRaises(AssertionError):
                benchmark.validate_record(changed, fixture, server, samples, copy_profile=True)
        for operation in benchmark.COPY_PROFILE_OPERATIONS:
            incomplete = [row for row in samples if row["operation"] != operation]
            with self.subTest(operation=operation), self.assertRaises(AssertionError):
                benchmark.validate_record(record, fixture, server, incomplete, copy_profile=True)
        hidden_rpc = copy.deepcopy(server)
        hidden_rpc["channel_requests"] += 1
        hidden_rpc["requests"].insert(0, hidden_rpc["requests"][0])
        with self.assertRaises(AssertionError):
            benchmark.validate_record(record, fixture, hidden_rpc, samples, copy_profile=True)

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
        cls.copy_profile = cls.manifest.get("copy_profile", False)
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
        expected_policy = json.loads(json.dumps(benchmark.plan(self.copy_profile)))
        for key, value in expected_policy.items():
            self.assertEqual(self.policy[key], value)
            self.assertEqual(self.manifest[key], value)
        self.assertEqual(benchmark.base.sha256(self.output / "plan.json"), self.manifest["plan_sha256"])
        cases = (benchmark.COPY_PROFILE_CASES if self.copy_profile else
                 tuple(itertools.product(benchmark.SUBCARRIERS, benchmark.ANTENNAS, benchmark.LINKS)))
        expected = {(nsc, antennas, links, repetition) for nsc, antennas, links in cases
                    for repetition in range(3)}
        actual = {(int(row["subcarriers"]), (int(row["num_tx_antennas"]), int(row["num_rx_antennas"])),
                   int(row["links"]), int(row["repetition"])) for row in self.summaries}
        self.assertEqual(actual, expected)
        self.assertEqual(len(self.summaries), len(cases) * 3)
        self.assertEqual(len(self.samples), len(cases) * 3 * (741 if self.copy_profile else 229))
        self.assertEqual(len(self.requests), len(cases) * 3 * 27)
        self.assertEqual(len(self.manifest["configurations"]), len(cases))
        if self.copy_profile:
            self.assertTrue((self.output / "run.json").is_file())
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
                benchmark.validate_record(record, fixture, server, samples, copy_profile=self.copy_profile)
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
        cases = (benchmark.COPY_PROFILE_CASES if self.copy_profile else
                 ((64, (1, 1), 1), (256, (2, 4), 16), (1024, (8, 8), 64)))
        for nsc, antennas, links in cases:
            configuration = next(row for row in self.manifest["configurations"]
                if row["subcarriers"] == nsc and (row["num_tx_antennas"], row["num_rx_antennas"]) == antennas
                and row["links"] == links)
            result, _, _ = benchmark.measure(benchmark.executable(), self.fixture(configuration),
                                           copy_profile=self.copy_profile)
            expected = next(row for row in self.summaries if row["case_id"] == configuration["case_id"])
            self.assertEqual(benchmark.deterministic_fields(result, include_environment=False),
                             benchmark.deterministic_fields(self.native_record(expected),
                                                            include_environment=False))

    def test_historical_replay_accepts_library_versions_but_rejects_changed_behavior(self):
        from unittest.mock import patch

        measure = benchmark.measure
        observed = []

        def different_environment(*args, **kwargs):
            result, samples, server = measure(*args, **kwargs)
            original = result.copy()
            result["cpp_protobuf_version"] = 6033005 if original["cpp_protobuf_version"] != 6033005 else 3021012
            result["cpp_zmq_version"] = "different-replay-host"
            # Same-host repeatability still includes native library identity.
            self.assertNotEqual(benchmark.deterministic_fields(original),
                                benchmark.deterministic_fields(result))
            observed.append(result.copy())
            return result, samples, server

        with patch.object(benchmark, "measure", side_effect=different_environment):
            self.test_fresh_process_replay_across_shapes_and_link_counts()

        self.assertEqual(len(observed), 2 if self.copy_profile else 3)
        # Every remaining deterministic field must still affect acceptance.
        for key in benchmark.deterministic_fields(observed[0], include_environment=False):
            result = observed[0].copy()
            result[key] = "changed" if isinstance(result[key], str) else result[key] + 1
            with self.subTest(field=key), patch.object(benchmark, "measure",
                    return_value=(result, {}, [])), self.assertRaises(AssertionError):
                self.test_fresh_process_replay_across_shapes_and_link_counts()


class TestWorkloadPolicy(unittest.TestCase):
    def test_failed_native_process_keeps_diagnostics_and_cannot_overwrite(self):
        with tempfile.TemporaryDirectory() as temporary:
            output = Path(temporary) / "failed"
            failure = subprocess.CalledProcessError(2, ["injected-native-failure"],
                                                   output="native output", stderr="native error")
            with patch.object(benchmark.base, "native", side_effect=failure):
                with self.assertRaises(subprocess.CalledProcessError):
                    benchmark.run_workload(output, benchmark.executable(), 16)
            run = json.loads((output / "run.json").read_text())
            self.assertEqual(run["status"], "failed")
            self.assertIsNotNone(run["finished_utc"])
            log = (output / run["error_log"]).read_text()
            self.assertIn("native output", log)
            self.assertIn("native error", log)
            self.assertIn("Frozen REP peer log", log)
            self.assertTrue((output / "failed_process/response.pb").is_file())
            self.assertTrue((output / "failed_process/peer.log").is_file())
            previous = (output / "run.json").read_bytes()
            with self.assertRaises(ValueError):
                benchmark.run_workload(output, benchmark.executable(), 16)
            self.assertEqual((output / "run.json").read_bytes(), previous)

    def test_future_timestamps_active_rotation_and_channel_evolution(self):
        fixture = benchmark.batch(benchmark.template(64, 2, 2), 64)
        original = fixture.channel_state_response.csi[0]
        for step, depth in ((0, 16), (17, 16), (127, 16), (17, 4), (17, 1)):
            reply = benchmark.workload_batch(fixture, step, depth)
            states = reply.channel_state_response.csi
            self.assertEqual(len(states), depth)
            for offset, state in enumerate(states):
                epoch = step - step % depth + offset
                self.assertEqual(state.start_time, epoch * 5000001)
                self.assertEqual(state.end_time, (epoch + 1) * 5000001 - 1)
                first = epoch // depth * 8 % 64 + 1
                self.assertEqual([radio.id for radio in state.rx_nodes], list(range(first, first + 8)))
                for radio in state.rx_nodes:
                    source = original.rx_nodes[radio.id - 1]
                    self.assertEqual(radio.wb_loss, source.wb_loss + epoch / 8)
                    self.assertEqual(radio.delay, source.delay + epoch)
                    self.assertEqual(radio.end_time2, state.end_time)
                    self.assertEqual(list(radio.frequencies), list(source.frequencies))
                    for i, (real, imag) in enumerate(zip(radio.csi_real, radio.csi_imag)):
                        self.assertEqual(complex(real, imag),
                            complex(source.csi_real[i], source.csi_imag[i]) * 1j ** (epoch % 4))

    def test_invalid_step_plan_and_nonempty_output_preserve_existing_files(self):
        for steps in (0, 15, 17, 4097):
            with self.subTest(steps=steps), self.assertRaises(ValueError):
                benchmark.workload_plan(steps)
        with tempfile.TemporaryDirectory() as temporary:
            path = Path(temporary)
            (path / "keep.txt").write_text("existing output")
            with self.assertRaises(ValueError):
                benchmark.run_workload(path, None)
            self.assertEqual((path / "keep.txt").read_text(), "existing output")


@unittest.skipUnless(os.environ.get("CACHE_IPC_WORKLOAD_RESULTS"), "optional local workload export")
class TestWorkloadExport(unittest.TestCase):
    def test_full_matrix_hashes_transcripts_trajectories_and_process_repeatability(self):
        output = Path(os.environ["CACHE_IPC_WORKLOAD_RESULTS"])
        manifest = json.loads((output / "manifest.json").read_text())
        plan = json.loads((output / "plan.json").read_text())
        steps = plan["controls"]["steps"]
        for key, value in json.loads(json.dumps(benchmark.workload_plan(steps))).items():
            self.assertEqual(plan[key], value)
            self.assertEqual(manifest[key], value)
        self.assertEqual(benchmark.base.sha256(output / "plan.json"), manifest["plan_sha256"])
        run = json.loads((output / "run.json").read_text())
        self.assertEqual(run["status"], "success")
        self.assertEqual(run["validation"]["status"], "passed")
        for name, digest in run["artifacts"].items():
            self.assertTrue(benchmark.base.provenance.verify_digest(output / name, digest))
        tables = {}
        for filename in ("summary", "timings", "requests", "trajectory"):
            with (output / (filename + ".csv")).open(newline="") as stream:
                tables[filename] = list(csv.DictReader(stream))
        cases = manifest["configurations"]
        self.assertEqual({(case["links"], case["depth"], case["enabled"]) for case in cases},
            set(itertools.product((1, 16, 64), (1, 4, 16), (True, False))))
        self.assertEqual(len(cases), 18)
        self.assertEqual(len(tables["summary"]), 54)
        self.assertEqual(len(tables["timings"]), 54 * steps)
        self.assertEqual(len(tables["trajectory"]), 54 * steps)
        self.assertEqual(len(tables["requests"]), manifest["validation"]["channel_requests"])
        for case in cases:
            self.assertEqual(benchmark.base.sha256(output / case["template"]), case["template_sha256"])
            fixture = benchmark.batch(benchmark.base.message_pb2.Wrapper.FromString(
                (output / case["template"]).read_bytes()), case["links"])
            self.assertEqual(hashlib.sha256(fixture.SerializeToString(deterministic=True)).hexdigest(),
                             case["response_sha256"])
            first = None
            for repetition in range(3):
                rows = {name: [row for row in table if row["case_id"] == case["case_id"] and
                               int(row["repetition"]) == repetition] for name, table in tables.items()}
                self.assertEqual(len(rows["summary"]), 1)
                record = {key: float(value) for key, value in rows["summary"][0].items()
                          if key not in ("case_id", "repetition")}
                samples = [{"operation": row["operation"], "iteration": int(row["iteration"]),
                    "nanoseconds": int(row["nanoseconds"])} for row in rows["timings"]]
                trace = [{key: value if key == "coefficient_digest" else int(value)
                          for key, value in row.items() if key not in ("case_id", "repetition")}
                         for row in rows["trajectory"]]
                self.assertEqual([int(row["request_index"]) for row in rows["requests"]],
                                 list(range(int(record["server_channel_requests"]))))
                server = {"channel_requests": record["server_channel_requests"],
                    "close_requests": record["server_close_requests"], "peak_rss_bytes": record["rep_server_peak_rss_bytes"],
                    "requests": [{key: int(row[key]) for key in ("tx_node", "rx_node", "time_ns")}
                                 for row in rows["requests"]]}
                benchmark.validate_workload(record, fixture, server, samples, trace, steps, case["depth"], case["enabled"])
                deterministic = benchmark.deterministic_fields(record), benchmark.deterministic_trajectory(trace)
                if first is None:
                    first = deterministic
                else:
                    self.assertEqual(deterministic, first)
                values = [sample["nanoseconds"] for sample in samples]
                self.assertEqual(record["workload_epoch_median_ns"], np.median(values))
                self.assertEqual(record["workload_epoch_p95_ns"], np.percentile(values, 95))


if __name__ == "__main__":
    unittest.main()
