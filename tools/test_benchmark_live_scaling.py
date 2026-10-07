"""R4 harness validation without ray tracing in the standard regression.

LIVE_SCALING_RESULTS optionally checks a newly measured full acceptance export.
Native option checks use no REP server and must reject input before transport.
"""
import copy
from contextlib import chdir
import csv
import hashlib
import json
import math
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest
from unittest.mock import patch

import benchmark_live_scaling as benchmark


def read_json(path):
    return json.loads(Path(path).read_text())


def response_fixture():
    """Small independent 2-user, 2-epoch, 2x2 tensor with four tones."""
    proto = benchmark.message_pb2
    config = proto.SimInitMessage(mode=3, sub_mode=4, fft_size=4,
                                 subcarrier_spacing=78125, fixed_coherence_time_ms=5)
    for array in (config.tx_array, config.rx_array):
        array.num_rows = 1
        array.num_cols = 2
        array.vertical_spacing = array.horizontal_spacing = 0.5
        array.pattern = "dipole"
        array.polarization = "V"
    for node_id in (0, 1, 2):
        config.nodes.add(id=node_id).constant_position_model.position.x = node_id
    request = proto.ChannelStateRequest(tx_node=0, rx_node=1, time=10_000_000)
    response = proto.Wrapper()
    for epoch in range(2):
        start = request.time + epoch * 5_000_000
        state = response.channel_state_response.csi.add(start_time=start, end_time=start + 4_999_999)
        state.tx_node.id = request.tx_node
        for node_id in (1, 2):
            radio = state.rx_nodes.add(id=node_id, delay=node_id, wb_loss=-60.0,
                                      end_time2=start + 5_000_000,
                                      num_tx_antennas=2, num_rx_antennas=2)
            radio.frequencies.extend((-156250, -78125, 0, 78125))
            radio.position.x = node_id
            values = [complex((i + node_id) / 16, -i / 32) for i in range(16)]
            scale = math.sqrt(sum(abs(value) ** 2 for value in values) / len(values))
            radio.csi_real.extend(value.real / scale for value in values)
            radio.csi_imag.extend(value.imag / scale for value in values)
            identity = radio.user_identity
            identity.user_id = identity.rx_node_id = node_id
            identity.tx_node_id = request.tx_node
            identity.direction = proto.CHANNEL_DIRECTION_DOWNLINK
            identity.csi_timestamp = start
            identity.rx_array.CopyFrom(config.rx_array)
    return request, response, config


class TestLiveResponseValidation(unittest.TestCase):
    def test_small_synthetic_wrapper_and_direct_response_are_valid(self):
        request, response, config = response_fixture()
        before = response.SerializeToString(deterministic=True)
        wrapped = benchmark.validate_response(request, response, config)
        direct = benchmark.validate_response(request, response.channel_state_response, config)
        self.assertIsInstance(wrapped, dict)
        self.assertEqual(wrapped, direct)
        self.assertEqual(response.SerializeToString(deterministic=True), before)

    def test_live_response_resource_limits_are_enforced(self):
        request, response, config = response_fixture()
        for options in ({"max_reply_bytes": 1}, {"max_subcarriers": 3},
                        {"max_coefficients_per_channel": 15}, {"max_batch_coefficients": 63},
                        {"max_snapshots": 3}, {"max_payload_bytes": 1087}):
            with self.subTest(options=options), self.assertRaises(ValueError):
                benchmark.validate_response(request, response, config, benchmark.ResourceLimits(**options))

    def test_corrupt_tensor_identity_time_and_nonfinite_values_are_rejected(self):
        def change(response, kind):
            state = response.channel_state_response.csi[0]
            radio = state.rx_nodes[0]
            if kind == "real_length":
                del radio.csi_real[-1]
            elif kind == "imag_length":
                del radio.csi_imag[-1]
            elif kind == "dimensions":
                radio.num_rx_antennas = 3
            elif kind == "zero_dimensions":
                radio.num_tx_antennas = 0
            elif kind == "frequency_length":
                del radio.frequencies[-1]
            elif kind == "frequency_order":
                radio.frequencies[0] += 1
            elif kind == "tx_identity":
                state.tx_node.id = 99
            elif kind == "rx_identity":
                radio.id = 99
            elif kind == "duplicate_rx":
                state.rx_nodes[1].id = radio.id
            elif kind == "missing_rx":
                del state.rx_nodes[-1]
            elif kind == "user_identity":
                radio.user_identity.rx_node_id = 99
            elif kind == "missing_user_identity":
                radio.ClearField("user_identity")
            elif kind == "user_array":
                radio.user_identity.rx_array.num_cols = 1
            elif kind == "user_time":
                radio.user_identity.csi_timestamp += 1
            elif kind == "direction":
                radio.user_identity.direction = benchmark.message_pb2.CHANNEL_DIRECTION_UPLINK
            elif kind == "start_time":
                state.start_time += 1
            elif kind == "end_time":
                state.end_time = state.start_time - 1
            elif kind == "link_end_time":
                radio.end_time2 = state.start_time - 1
            elif kind == "missing_epoch":
                del response.channel_state_response.csi[-1]
            elif kind == "nan_real":
                radio.csi_real[0] = float("nan")
            elif kind == "inf_imag":
                radio.csi_imag[0] = float("inf")
            elif kind == "nan_loss":
                radio.wb_loss = float("nan")
            elif kind == "inf_position":
                radio.position.x = float("inf")
            elif kind == "static_position":
                radio.position.x += 1
            elif kind == "zero_power":
                for index in range(len(radio.csi_real)):
                    radio.csi_real[index] = radio.csi_imag[index] = 0
            elif kind == "wrong_wrapper":
                response.Clear()
                response.sim_ack.no_error = True

        kinds = ("real_length", "imag_length", "dimensions", "zero_dimensions", "frequency_length",
                 "frequency_order", "tx_identity", "rx_identity", "duplicate_rx", "missing_rx",
                 "user_identity", "missing_user_identity", "user_array", "user_time", "direction",
                 "start_time", "end_time", "link_end_time", "missing_epoch", "nan_real", "inf_imag",
                 "nan_loss", "inf_position", "static_position", "zero_power", "wrong_wrapper")
        for kind in kinds:
            request, response, config = response_fixture()
            change(response, kind)
            with self.subTest(kind=kind), self.assertRaises(ValueError):
                benchmark.validate_response(request, response, config)


class TestLivePolicyAndDiagnostics(unittest.TestCase):
    def test_bad_plan_options_are_rejected(self):
        for arguments in ({"samples": 0}, {"samples": -1}, {"epochs": 0}, {"epochs": -1},
                          {"repeats": 0}, {"repeats": -1}):
            with self.subTest(arguments=arguments), self.assertRaises(ValueError):
                benchmark.plan(**arguments)
        with self.assertRaises(ValueError):
            benchmark.validate_plan({})
        benchmark.validate_plan(benchmark.plan(quick=True))

    def test_mutated_or_duplicated_workloads_are_rejected(self):
        mutations = (("receivers", 2), ("antennas", 3), ("width_mhz", 80),
                     ("lookahead_depth", 2), ("cache_enabled", 1), ("samples", False))
        for key, value in mutations:
            policy = benchmark.plan(quick=True)
            policy["workload"][0][key] = value
            with self.subTest(key=key), self.assertRaises(ValueError):
                benchmark.validate_plan(policy)
        policy = benchmark.plan(quick=True)
        policy["workload"].append(copy.deepcopy(policy["workload"][0]))
        with self.assertRaises(ValueError):
            benchmark.validate_plan(policy)
        policy = benchmark.plan(quick=True)
        policy["controls"]["epochs"] = 0
        with self.assertRaises(ValueError):
            benchmark.validate_plan(policy)

    def test_existing_outputs_are_preserved_byte_for_byte(self):
        with tempfile.TemporaryDirectory(prefix="r4-output-guard-") as temporary:
            output = Path(temporary)
            sentinel = output / "keep.bin"
            sentinel.write_bytes(b"historical measurement\x00\xff")
            before = sentinel.read_bytes()
            with self.assertRaises(ValueError):
                benchmark.ensure_empty_output(output)
            with self.assertRaises(ValueError):
                benchmark.run(output, quick=True, binary=Path("/bin/true"))
            self.assertEqual(sentinel.read_bytes(), before)
            self.assertEqual(list(output.iterdir()), [sentinel])

    def test_failed_process_keeps_plan_diagnostics_and_failed_provenance(self):
        with tempfile.TemporaryDirectory(prefix="r4-failure-") as temporary:
            output = Path(temporary) / "study"
            failure = subprocess.CalledProcessError(2, ["injected-live-client"],
                                                   output="native stdout marker", stderr="native stderr marker")

            def fail_pair(*args, **kwargs):
                self.assertTrue((output / "plan.json").is_file())
                self.assertEqual(read_json(output / "run.json")["status"], "incomplete")
                raise failure

            with patch.object(benchmark, "run_pair", side_effect=fail_pair):
                with self.assertRaises(subprocess.CalledProcessError):
                    benchmark.run(output, quick=True, binary=Path("/bin/true"))
            record = read_json(output / "run.json")
            self.assertEqual(record["status"], "failed")
            self.assertIsNotNone(record["finished_utc"])
            log = (output / record["error_log"]).read_text()
            self.assertIn("native stdout marker", log)
            self.assertIn("native stderr marker", log)
            self.assertEqual(record["configuration"], read_json(output / "plan.json"))
            self.assertIsNone(benchmark.provenance.ACTIVE_RUN.get())
            before = {p.relative_to(output): p.read_bytes() for p in output.rglob("*") if p.is_file()}
            with self.assertRaises(ValueError):
                benchmark.run(output, quick=True, binary=Path("/bin/true"))
            self.assertEqual(before, {p.relative_to(output): p.read_bytes()
                                      for p in output.rglob("*") if p.is_file()})

    def test_relative_output_and_binary_paths_are_absolute_before_process_work(self):
        with tempfile.TemporaryDirectory(prefix="r4-relative-", dir=benchmark.base.MODULE) as temporary:
            root = Path(temporary)
            (root / "bin").mkdir()
            binary = root / "bin/client"
            binary.write_bytes(b"dummy binary for provenance; never executed")
            output = root / "measurements/study"
            observed = []

            def inspect_pair(case, repeat, case_output, executable, timeout):
                self.assertEqual(case_output, output / "case_00/repeat_00")
                self.assertTrue(case_output.is_absolute())
                self.assertEqual(executable, binary)
                self.assertTrue(executable.is_absolute())
                self.assertTrue((output / "plan.json").is_file())
                self.assertEqual(read_json(output / "run.json")["status"], "incomplete")
                observed.append(case_output)
                raise RuntimeError("stop after checking resolved process paths")

            with chdir(root), patch.object(benchmark, "run_pair", side_effect=inspect_pair):
                with self.assertRaisesRegex(RuntimeError, "resolved process paths"):
                    benchmark.run(Path("measurements/study"), quick=True, binary=Path("bin/client"))
            self.assertEqual(len(observed), 1)
            self.assertEqual(read_json(output / "run.json")["status"], "failed")

    def test_failed_fresh_native_process_terminates_its_real_peer(self):
        with tempfile.TemporaryDirectory(prefix="r4-pair-cleanup-") as temporary:
            root = Path(temporary)
            binary = root / "failing-client"
            binary.write_text("#!/bin/sh\necho client-out-marker\necho client-error-marker >&2\nexit 7\n")
            binary.chmod(0o755)
            output = root / "pair"
            original_popen = subprocess.Popen
            peers = []
            worker_script = (
                "import json,pathlib,sys,time; "
                "p=pathlib.Path(sys.argv[1]); p.mkdir(parents=True); "
                "(p/'ready.json').write_text(json.dumps({'endpoint':'ipc://'+str(p/'absent')})); "
                "print('peer-out-marker',flush=True); time.sleep(60)"
            )

            def launch(command, **kwargs):
                if "--worker" in command:
                    peer = original_popen([sys.executable, "-c", worker_script,
                                           str(output / "server")], **kwargs)
                    peers.append(peer)
                    self.addCleanup(benchmark._stop, peer)
                    return peer
                return original_popen(command, **kwargs)

            case = {**benchmark.plan(quick=True)["workload"][0], "epochs": 2}
            with chdir(root), patch.object(benchmark.subprocess, "Popen", side_effect=launch):
                with self.assertRaisesRegex(ValueError, "Native live process failed"):
                    benchmark.run_pair(case, 0, Path("pair"), binary, timeout=5)
            self.assertEqual(len(peers), 1)
            self.assertIsNotNone(peers[0].poll(), "failed native process left its peer running")
            self.assertEqual(read_json(output / "processes.json")["worker_pid"], peers[0].pid)
            self.assertIn("peer-out-marker", (output / "server.stdout.log").read_text())
            self.assertIn("client-out-marker", (output / "native.stdout.log").read_text())
            self.assertIn("client-error-marker", (output / "native.stderr.log").read_text())


class TestLiveGetterTranscript(unittest.TestCase):
    @staticmethod
    def record(index, timestamp, receiver=1):
        return {"kind": "channel", "request_index": index, "request_time_ns": timestamp,
                "tx_node": 0, "rx_node": receiver}

    def test_cache_batches_miss_only_at_known_uncovered_send_times(self):
        # Two packets at 1/2ms, then 7/8ms. A 5ms interval needs two
        # batches; four forecast intervals cover the entire small workload.
        for depth, times in ((1, (1_000_000, 7_000_000)), (4, (1_000_000,))):
            case = {"epochs": 2, "receivers": 4, "cache_enabled": True, "lookahead_depth": depth}
            records = [self.record(index, timestamp) for index, timestamp in enumerate(times)]
            expected_getters = 64  # 4 sends * 4 receivers * 4 public getters
            summary = {"cache_hit_ratio": (expected_getters - len(times)) / expected_getters}
            with self.subTest(depth=depth):
                result = benchmark.validate_transcript(case, records, None, summary)
                self.assertEqual(result, {"getter_calls": 64, "rpc_requests": len(times),
                                         "expected_hit_ratio": summary["cache_hit_ratio"], "valid": True})
                for kind in ("extra_request", "wrong_time", "wrong_tx", "wrong_rx", "wrong_ratio"):
                    changed, native = copy.deepcopy(records), dict(summary)
                    if kind == "extra_request":
                        changed.append(self.record(99, 2_000_000))
                    elif kind == "wrong_time":
                        changed[0]["request_time_ns"] += 1
                    elif kind == "wrong_tx":
                        changed[0]["tx_node"] = 1
                    elif kind == "wrong_rx":
                        changed[0]["rx_node"] = 2
                    else:
                        native["cache_hit_ratio"] = 1.0
                    with self.subTest(depth=depth, kind=kind), self.assertRaises(ValueError):
                        benchmark.validate_transcript(case, changed, None, native)

    def uncached_fixture(self, root):
        # Exactly two getters before propagation and two at the 10ns
        # propagation delay for each of the two packet sends.
        case = {"epochs": 1, "receivers": 1, "cache_enabled": False, "lookahead_depth": 1}
        times = (1_000_000, 1_000_000, 1_000_010, 1_000_010,
                 2_000_000, 2_000_000, 2_000_010, 2_000_010)
        records = [self.record(index, timestamp) for index, timestamp in enumerate(times)]
        responses = root / "responses"
        responses.mkdir()
        for index in range(8):
            response = benchmark.message_pb2.Wrapper()
            response.channel_state_response.csi.add().rx_nodes.add(id=1, delay=10)
            (responses / f"{index:06d}.pb").write_bytes(response.SerializeToString())
        return case, records, {"cache_hit_ratio": 0.0}

    def test_uncached_getter_counts_and_raw_propagation_delay_mapping(self):
        with tempfile.TemporaryDirectory(prefix="r4-transcript-") as temporary:
            root = Path(temporary)
            case, records, summary = self.uncached_fixture(root)
            self.assertEqual(benchmark.validate_transcript(case, records, root, summary),
                             {"getter_calls": 8, "rpc_requests": 8, "expected_hit_ratio": 0.0, "valid": True})
            for kind in ("missing_getter", "duplicate_phase", "unscheduled_time", "wrong_tx", "wrong_ratio"):
                changed, native = copy.deepcopy(records), dict(summary)
                if kind == "missing_getter":
                    changed.pop()
                elif kind == "duplicate_phase":
                    changed[2]["request_time_ns"] = 1_000_000
                elif kind == "unscheduled_time":
                    changed[2]["request_time_ns"] = 1_000_005
                elif kind == "wrong_tx":
                    changed[0]["tx_node"] = 1
                else:
                    native["cache_hit_ratio"] = 0.5
                with self.subTest(kind=kind), self.assertRaises(ValueError):
                    benchmark.validate_transcript(case, changed, root, native)

    def test_malformed_raw_delay_or_receiver_mapping_cannot_pass(self):
        from google.protobuf.message import DecodeError

        for kind in ("delay", "missing_receiver", "wrong_receiver", "malformed_wire", "missing_row_index"):
            with self.subTest(kind=kind), tempfile.TemporaryDirectory(prefix="r4-delay-negative-") as temporary:
                root = Path(temporary)
                case, records, summary = self.uncached_fixture(root)
                target = root / "responses/000002.pb"
                response = benchmark.message_pb2.Wrapper.FromString(target.read_bytes())
                if kind == "delay":
                    response.channel_state_response.csi[0].rx_nodes[0].delay = 20
                    target.write_bytes(response.SerializeToString())
                elif kind == "missing_receiver":
                    del response.channel_state_response.csi[0].rx_nodes[:]
                    target.write_bytes(response.SerializeToString())
                elif kind == "wrong_receiver":
                    records[2]["rx_node"] = 99
                elif kind == "malformed_wire":
                    target.write_bytes(b"\xff")
                else:
                    del records[2]["request_index"]
                with self.assertRaises((ValueError, StopIteration, DecodeError, KeyError)):
                    benchmark.validate_transcript(case, records, root, summary)


class TestLiveLatencySummary(unittest.TestCase):
    @staticmethod
    def record(index, duration, kind="channel"):
        return {"kind": kind, "request_index": index,
                "parse_ns": duration, "compute_ns": duration,
                "serialize_ns": duration, "send_ns": duration,
                "request_bytes": 8, "response_bytes": 64,
                "rss_kib": 100 + index, "hwm_kib": 200 + index}

    def test_cold_and_warm_summary_matches_independent_raw_statistics(self):
        records = [self.record(0, 9000, "init"), self.record(1, 100),
                   self.record(2, 20), self.record(3, 40), self.record(4, 8000, "close")]
        summary = benchmark.summarize(records)
        for stage in ("parse_ns", "compute_ns", "serialize_ns", "send_ns"):
            with self.subTest(stage=stage):
                self.assertEqual(summary["cold"][stage]["count"], 1)
                self.assertEqual(summary["cold"][stage]["median_ns"], 100)
                self.assertEqual(summary["cold"][stage]["p95_ns"], 100)
                self.assertEqual(summary["warm"][stage]["count"], 2)
                self.assertEqual(summary["warm"][stage]["median_ns"], 30)
                self.assertEqual(summary["warm"][stage]["p95_ns"], 39)
        self.assertEqual(summary["rss"]["current_peak_kib"], 104)
        self.assertEqual(summary["rss"]["hwm_peak_kib"], 204)

    def test_missing_warm_observations_are_explicitly_unavailable(self):
        records = [self.record(0, 9000, "init"), self.record(1, 100),
                   self.record(2, 8000, "close")]
        summary = benchmark.summarize(records)
        for stage in ("parse_ns", "compute_ns", "serialize_ns", "send_ns"):
            self.assertEqual(summary["cold"][stage]["count"], 1)
            self.assertEqual(summary["warm"][stage]["count"], 0)
            self.assertIsNone(summary["warm"][stage]["median_ns"])
            self.assertIsNone(summary["warm"][stage]["p95_ns"])
        # An empty group must remain representable in strict JSON exports.
        json.dumps(summary, allow_nan=False)

    def test_negative_and_nonfinite_latency_samples_are_rejected(self):
        for value in (-1, float("nan"), float("inf")):
            record = self.record(1, 10)
            record["compute_ns"] = value
            with self.subTest(value=value), self.assertRaises(ValueError):
                benchmark.summarize([record])


class TestLiveNativeOptionValidation(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        try:
            cls.binary = benchmark.executable()
        except FileNotFoundError as error:
            raise unittest.SkipTest(str(error)) from error

    def test_bad_options_are_rejected_before_output_or_transport(self):
        options = ({"receivers": 0}, {"receivers": 2}, {"antennas": 0}, {"antennas": 3},
                   {"widthMhz": 10}, {"widthMhz": 80}, {"lookaheadDepth": 0},
                   {"lookaheadDepth": 2}, {"epochs": 0}, {"epochs": 100001},
                   {"packetsPerEpoch": 0}, {"packetsPerEpoch": 5}, {"seed": 0}, {"run": 0})
        with tempfile.TemporaryDirectory(prefix="r4-native-negative-") as temporary:
            for index, extra in enumerate(options):
                output = Path(temporary) / f"case-{index}"
                command = [str(self.binary), f"--output={output}",
                           "--endpoint=ipc://" + str(Path(temporary) / "absent-peer"),
                           *(f"--{key}={value}" for key, value in extra.items())]
                with self.subTest(options=extra):
                    result = subprocess.run(command, capture_output=True, text=True, timeout=10)
                    self.assertNotEqual(result.returncode, 0, result.stdout + result.stderr)
                    self.assertFalse(output.exists(), "invalid options published output")

    def test_native_existing_output_is_preserved(self):
        with tempfile.TemporaryDirectory(prefix="r4-native-output-") as temporary:
            output = Path(temporary)
            sentinel = output / "keep.bin"
            sentinel.write_bytes(b"existing result\x00")
            result = subprocess.run([str(self.binary), f"--output={output}",
                                     "--endpoint=ipc://" + str(output / "absent-peer")],
                                    capture_output=True, text=True, timeout=10)
            self.assertNotEqual(result.returncode, 0, result.stdout + result.stderr)
            self.assertEqual(sentinel.read_bytes(), b"existing result\x00")
            self.assertEqual(list(output.iterdir()), [sentinel])


@unittest.skipUnless(os.environ.get("LIVE_SCALING_RESULTS"), "optional newly recorded full R4 acceptance export")
class TestLiveAcceptanceExport(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.output = Path(os.environ["LIVE_SCALING_RESULTS"])
        cls.policy = read_json(cls.output / "plan.json")
        cls.run_record = read_json(cls.output / "run.json")
        cls.validation = read_json(cls.output / "validation.json")

    def test_full_predeclared_matrix_finished_execution_and_all_artifact_hashes(self):
        self.assertEqual(self.policy, json.loads(json.dumps(benchmark.plan())))
        self.assertFalse(self.policy["quick"])
        self.assertEqual(self.policy["controls"]["repeats"], 3)
        self.assertEqual(self.run_record["configuration"], self.policy)
        self.assertEqual(self.run_record["status"], "success")
        self.assertIsNotNone(self.run_record["finished_utc"])
        self.assertTrue(self.run_record["validation"]["valid"])
        self.assertTrue(self.validation["valid"])
        expected = {(case["case_id"], repeat) for case in self.policy["workload"] for repeat in range(3)}
        actual = {(row["case_id"], row["repeat"]) for row in self.validation["cases"]}
        self.assertEqual(actual, expected)
        self.assertEqual(len(self.validation["cases"]), len(expected))
        self.assertEqual(self.run_record["validation"]["cases"], len(expected))
        artifacts = self.run_record["artifacts"]
        self.assertIn("plan.json", artifacts)
        self.assertIn("validation.json", artifacts)
        self.assertIn("summary.json", artifacts)
        for name, digest in artifacts.items():
            with self.subTest(artifact=name):
                self.assertTrue(benchmark.provenance.verify_digest(self.output / name, digest))
        plan_raw = (self.output / "plan.json").read_bytes()
        self.assertEqual(hashlib.sha256(plan_raw).hexdigest(), artifacts["plan.json"]["raw_sha256"])

    def test_raw_response_transcripts_counts_timings_and_native_phy_evidence(self):
        for case in self.policy["workload"]:
            for repeat in range(3):
                with self.subTest(case=case["case_id"], repeat=repeat):
                    path = self.output / case["case_id"] / f"repeat_{repeat:02d}"
                    server = path / "server"
                    worker_run = read_json(server / "run.json")
                    self.assertEqual(worker_run["status"], "success")
                    self.assertTrue(worker_run["validation"]["valid"])
                    for name, digest in worker_run["artifacts"].items():
                        self.assertTrue(benchmark.provenance.verify_digest(server / name, digest))
                    records = [json.loads(line) for line in (server / "records.jsonl").read_text().splitlines()]
                    self.assertEqual([row["request_index"] for row in records], list(range(len(records))))
                    self.assertEqual(len(list((server / "requests").glob("*.pb"))), len(records))
                    self.assertEqual(len(list((server / "responses").glob("*.pb"))), len(records))
                    self.assertEqual([row["kind"] for row in records].count("init"), 1)
                    self.assertEqual(records[0]["kind"], "init")
                    self.assertEqual(records[-1]["kind"], "close")
                    config = benchmark.message_pb2.SimInitMessage.FromString((server / "configuration.pb").read_bytes())
                    checked = []
                    for index, record in enumerate(records):
                        request_raw = (server / "requests" / f"{index:06d}.pb").read_bytes()
                        response_raw = (server / "responses" / f"{index:06d}.pb").read_bytes()
                        self.assertEqual(record["request_bytes"], len(request_raw))
                        self.assertEqual(record["response_bytes"], len(response_raw))
                        if record["kind"] == "channel":
                            value = benchmark.validate_response(
                                benchmark.message_pb2.Wrapper.FromString(request_raw),
                                benchmark.message_pb2.Wrapper.FromString(response_raw), config)
                            checked.append({"request_index": index, **value})
                    self.assertTrue(checked)
                    self.assertEqual(read_json(server / "validation.json")["responses"], checked)
                    self.assertEqual(read_json(server / "summary.json"), benchmark.summarize(records))
                    with (path / "native/sends.csv").open(newline="") as stream:
                        sends = list(csv.DictReader(stream))
                    with (path / "native/receptions.csv").open(newline="") as stream:
                        receptions = list(csv.DictReader(stream))
                    native = read_json(path / "native/summary.json")
                    self.assertEqual(len(sends), self.policy["controls"]["epochs"] * 2)
                    self.assertEqual(native["sends"], len(sends))
                    self.assertEqual(native["validation"], "PASS")
                    self.assertGreater(native["phy_rx_success_count"], 0)
                    self.assertTrue(receptions)
                    # Recompute the production replay validator but capture its output
                    # so existing measured evidence is never rewritten by this test.
                    stored = read_json(path / "validation.json")
                    with patch.object(benchmark.provenance, "atomic_json") as write:
                        calculated = benchmark.validate_case(
                            {**case, "epochs": self.policy["controls"]["epochs"]}, path)
                    self.assertEqual(calculated, stored)
                    self.assertEqual(write.call_count, 1)
                    self.assertEqual(write.call_args.args[1], stored)

    def test_paired_cache_controls_recomputed_from_actual_native_metrics(self):
        pairs = benchmark.validate_pairs(self.policy, self.output)
        self.assertEqual(pairs, self.validation["cache_pairs"])
        self.assertEqual(len(pairs), len(self.policy["workload"]) // 2 * 3)
        self.assertEqual(self.run_record["validation"]["cache_pairs"], len(pairs))
        for pair in pairs:
            self.assertTrue(pair["valid"])
            self.assertGreater(pair["metrics_compared"], 0)
            self.assertGreater(pair["off_requests"], pair["on_requests"])


if __name__ == "__main__":
    unittest.main()
