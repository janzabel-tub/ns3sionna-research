#!/usr/bin/env python3
"""R4: fresh CPU ray tracing through the production ns-3 propagation path.

Each repetition uses an isolated native process and a real Sionna REP worker.
The predeclared representative matrix is descriptive, not a latency/RSS bound.
No channel fixtures, frozen replies, or Python channel cache are used.
"""
from __future__ import annotations

import argparse
import csv
import json
import math
import os
from pathlib import Path
import subprocess
import sys
import time

import numpy as np

import benchmark_sumimo as base
import provenance
from mimo_data_contract import ResourceLimits, array_ports, resolve_array_config
from mimo_data_contract import TX_ARRAY_DEFAULTS, RX_ARRAY_DEFAULTS
from random_streams import resolve_randomness

message_pb2 = base.message_pb2
STAGES = ("parse_ns", "compute_ns", "serialize_ns", "send_ns")
REPRESENTATIVE = ((1, 1, 20, 1), (4, 1, 20, 1), (1, 2, 40, 1),
                  (4, 2, 40, 1), (1, 1, 20, 4), (4, 2, 40, 4))


def ensure_empty_output(output):
    output = Path(output).resolve()
    if output.exists() and (not output.is_dir() or any(output.iterdir())):
        raise ValueError("Use an empty output directory to preserve results")
    output.mkdir(parents=True, exist_ok=True)
    return output


def plan(samples=10000, epochs=4, repeats=3, quick=False):
    for name, value in (("samples", samples), ("epochs", epochs), ("repeats", repeats)):
        if type(value) is not int or value <= 0:
            raise ValueError(f"{name} must be a positive integer")
    shapes = REPRESENTATIVE[:1] if quick else REPRESENTATIVE
    workloads = [(shape, samples) for shape in shapes]
    if not quick and samples != 100000:
        workloads.append(((4, 2, 40, 1), 100000))
    cases = []
    for shape, budget in workloads:
        for cache in (False, True):
            receivers, antennas, width, depth = shape
            cases.append(dict(case_id=f"case_{len(cases):02d}", receivers=receivers,
                antennas=antennas, width_mhz=width, lookahead_depth=depth,
                cache_enabled=cache, samples=budget))
    policy = {"schema_version": 1, "subphase": "R4", "quick": bool(quick),
        "controls": {"samples": samples, "epochs": epochs,
            "repeats": 1 if quick else repeats, "packets_per_epoch": 2,
            "epoch_spacing_ns": 6000000, "first_send_ns": 1000000,
            "packet_spacing_ns": 1000000, "coherence_ns": 5000000,
            "seed_ns3": 42, "run_ns3": 1, "rt_stream": 0,
            "mobility_stream": 1, "randomness_mapping_version": 1,
            "backend": "llvm_ad_mono_polarized", "drjit_threads": 1,
            "scene": base.SCENE,
            "frequency_mhz_by_width": {"20": 5180, "40": 5190},
            "helper_guard_multiplier": 3, "subcarrier_spacing_hz": 78125,
            "phy_fft_size_by_width": {"20": 256, "40": 512},
            "rt_fft_size_by_width": {"20": 768, "40": 1536},
            "tx_power_dbm_total": 20, "noise_figure_db": 7, "mcs": 0,
            "nss": "equal to antenna ports", "guard_interval_ns": 800,
            "payload_bytes": 256, "psdu_bytes": "native summary exports complete PSDU size",
            "precoding": "native HE SU open loop", "eesm_beta_linear": 2,
            "beta_calibrated": False,
            "per_scope": "Diagnostic beta=2; no new MIMO/40 MHz PER calibration claimed",
            "tx_position_m": [3, 2, 2],
            "rx_position_m": "receiver i=1..N: [4+0.1*(i-1),3,1]",
            "tx_pattern": "iso", "rx_pattern": "iso", "polarization": "V",
            "array_rows": 1, "array_columns": "equal to antenna ports",
            "vertical_spacing_lambda": 0.5, "horizontal_spacing_lambda": 0.5,
            "rt_controls": {key: base.CONTROLS[key] for key in ("max_depth", "synthetic_array",
                "los", "specular_reflection", "diffuse_reflection", "refraction", "diffraction",
                "edge_diffraction", "diffraction_lit_region")},
            "mobility_history_limit": 8, "geometry": "fixed; no channel aging",
            "transport": "TCP localhost REQ/REP; fresh sequential process pairs",
            "memory": "isolated process RSS/current and high water; peaks are not summed",
            "resource_limits": vars(ResourceLimits())},
        "workload": cases,
        "acceptance": {"responses": "Every actual response: exact identities, tensor dimensions, "
            "frequency order, finite loss/CSI, normalized power and production validity intervals",
            "requests": "Replay native public-getter cache semantics against each raw RPC transcript",
            "phy": "Positive actual receive callbacks and finite native channel metrics",
            "cache_disabled": "No retained native CSI links/snapshots/payload",
            "cache_pair": {"rtol": 1e-4, "atol": 1e-7,
                "scope": "same samples, geometry, width, depth and batch; native per-reception metrics"},
            "performance": "No universal latency/RSS guarantee; medians and p95 descriptive; "
                "first channel request cold, later requests warm; zero observations yield null"}}
    validate_plan(policy)
    return policy


def validate_plan(policy):
    try:
        return _validate_plan(policy)
    except (KeyError, TypeError, ZeroDivisionError) as error:
        raise ValueError(f"Malformed live plan: {error}") from error


def _validate_plan(policy):
    _require(policy.get("schema_version") == 1 and policy.get("subphase") == "R4",
             "Unsupported live plan schema")
    controls = policy["controls"]
    for key in ("samples", "epochs", "repeats", "packets_per_epoch"):
        if type(controls[key]) is not int or controls[key] <= 0:
            raise ValueError(f"Invalid plan control {key}")
    limits = ResourceLimits(**controls["resource_limits"])
    identifiers = set()
    if not policy["workload"]:
        raise ValueError("Empty live workload")
    for case in policy["workload"]:
        if case["case_id"] in identifiers:
            raise ValueError("Duplicate case identity")
        identifiers.add(case["case_id"])
        if (case["receivers"] not in (1, 4) or case["antennas"] not in (1, 2)
                or case["width_mhz"] not in (20, 40) or case["lookahead_depth"] not in (1, 4)
                or type(case["cache_enabled"]) is not bool
                or type(case["samples"]) is not int or case["samples"] <= 0):
            raise ValueError("Unsupported live workload dimensions")
        limits.plan(3 * case["width_mhz"] * 256 // 20, case["antennas"], case["antennas"],
                    case["receivers"], case["lookahead_depth"])
    return policy


def _require(condition, message):
    if not condition:
        raise ValueError(message)


def validate_response(request, response, config, limits=None):
    """Validate serialized production tensors without importing the RT framework."""
    limits = limits or ResourceLimits()
    if isinstance(request, message_pb2.Wrapper):
        _require(request.HasField("channel_state_request"), "Wrong request wrapper type")
        request = request.channel_state_request
    if isinstance(response, message_pb2.Wrapper):
        _require(response.HasField("channel_state_response"), "Wrong response wrapper type")
        limits.check(response.ByteSize(), "reply_bytes")
        response = response.channel_state_response
    limits.check(response.ByteSize(), "reply_bytes")
    node_ids = [node.id for node in config.nodes]
    _require(len(set(node_ids)) == len(node_ids), "Duplicate configuration node")
    _require(request.tx_node in node_ids and request.rx_node in node_ids
             and request.tx_node != request.rx_node, "Unknown request identity")
    receivers = sorted(i for i in node_ids if i != request.tx_node)
    if config.mode == 1:
        receivers = [request.rx_node]
    depth = config.sub_mode // len(receivers) if config.mode == 3 else 1
    _require(depth > 0, "Invalid lookahead depth")
    _require(len(response.csi) == depth, "Response epoch/depth mismatch")
    rx_array = resolve_array_config(config.rx_array if config.HasField("rx_array") else None,
                                    RX_ARRAY_DEFAULTS)
    tx_array = resolve_array_config(config.tx_array if config.HasField("tx_array") else None,
                                    TX_ARRAY_DEFAULTS)
    nrx, ntx = array_ports(rx_array), array_ports(tx_array)
    limits.plan(config.fft_size, nrx, ntx, len(receivers), depth)
    expected_frequencies = (np.arange(config.fft_size) - config.fft_size // 2) * config.subcarrier_spacing
    coherence = config.fixed_coherence_time_ms * 1000000
    _require(coherence > 0, "Live validation requires fixed coherence")
    nodes = {node.id: node for node in config.nodes}
    for epoch, state in enumerate(response.csi):
        start = request.time + epoch * coherence
        end = start + coherence - (1 if config.mode == 3 else 0)
        _require((state.start_time, state.end_time, state.tx_node.id) ==
                 (start, end, request.tx_node), "Response TX/time identity mismatch")
        _require(sorted(rx.id for rx in state.rx_nodes) == receivers,
                 "Response receiver identity mismatch or duplicate")
        for radio in [state.tx_node, *state.rx_nodes]:
            position = np.array([radio.position.x, radio.position.y, radio.position.z])
            _require(np.isfinite(position).all(), "Non-finite radio position")
            node = nodes[radio.id]
            if node.WhichOneof("model") == "constant_position_model":
                point = node.constant_position_model.position
                _require(np.allclose(position, [point.x, point.y, point.z], rtol=0, atol=1e-6),
                         "Static radio position mismatch")
        for radio in state.rx_nodes:
            _require((radio.num_rx_antennas or 1, radio.num_tx_antennas or 1) == (nrx, ntx),
                     "Response antenna dimension mismatch")
            count = limits.channel(config.fft_size, nrx, ntx)
            _require(len(radio.csi_real) == count and len(radio.csi_imag) == count,
                     "Response tensor shape mismatch")
            _require(np.array_equal(radio.frequencies, expected_frequencies),
                     "Response frequency order/dimension mismatch")
            real, imag = np.asarray(radio.csi_real), np.asarray(radio.csi_imag)
            _require(np.isfinite(real).all() and np.isfinite(imag).all() and math.isfinite(radio.wb_loss),
                     "Non-finite CSI or wideband loss")
            h = real + 1j * imag
            _require(np.isclose(np.mean(abs(h) ** 2), 1.0, rtol=1e-3, atol=1e-7),
                     "Response normalized channel power mismatch")
            expected_end2 = start + coherence if config.mode == 3 else coherence
            _require(radio.end_time2 == expected_end2, "Response link validity mismatch")
            if len(receivers) > 1:
                _require(radio.HasField("user_identity"), "Missing multi-user identity")
            if radio.HasField("user_identity"):
                identity = radio.user_identity
                _require((identity.user_id, identity.tx_node_id, identity.rx_node_id,
                    identity.direction, identity.csi_timestamp) ==
                    (radio.id, request.tx_node, radio.id, 1, start), "Wrong user channel identity")
                _require(resolve_array_config(identity.rx_array, RX_ARRAY_DEFAULTS) == rx_array,
                         "Wrong receiver array identity")
    return {"valid": True, "epochs": depth, "links": len(receivers) * depth,
            "tx_node": request.tx_node, "requested_rx_node": request.rx_node,
            "request_time_ns": request.time, "subcarriers": config.fft_size,
            "rx_antennas": nrx, "tx_antennas": ntx}


def _statistics(values):
    values = np.asarray(values, dtype=float)
    _require(np.isfinite(values).all() and (values >= 0).all(), "Invalid latency sample")
    return {"count": len(values), "median_ns": float(np.median(values)) if len(values) else None,
            "p95_ns": float(np.percentile(values, 95)) if len(values) else None}


def summarize(records):
    channels = [r for r in records if r.get("kind") == "channel"]
    return {"cold": {stage: _statistics([r[stage] for r in channels[:1]]) for stage in STAGES},
            "warm": {stage: _statistics([r[stage] for r in channels[1:]]) for stage in STAGES},
            "init": {stage: _statistics([r[stage] for r in records if r.get("kind") == "init"])
                     for stage in STAGES},
            "rss": {"current_peak_kib": max((r.get("rss_kib", 0) for r in records), default=0),
                    "hwm_peak_kib": max((r.get("hwm_kib", 0) for r in records), default=0)}}


def rss():
    values = {}
    for line in Path("/proc/self/status").read_text().splitlines():
        if line.startswith(("VmRSS:", "VmHWM:")):
            key, value, *_ = line.split()
            values[key.rstrip(":")] = int(value)
    return {"rss_kib": values.get("VmRSS", 0), "hwm_kib": values.get("VmHWM", 0)}


def worker(output, samples=10000, timeout=300):
    """Instrument the same init/compute/close dispatch used by SionnaEnv.run."""
    import zmq
    output = ensure_empty_output(output)
    limits = ResourceLimits()
    configuration = {"subphase": "R4-worker", "samples": samples,
        "controls": {"run_ns3": 1, "resource_limits": vars(limits), "history_limit": 8}}
    with provenance.Run(output / "run.json", configuration,
                        inputs=[base.SERVER / "models" / base.SCENE]) as run:
        env_class, mi = base.runtime()
        env = env_class(model_folder=str(base.SERVER / "models"), rt_fast=True,
            rt_max_parallel_links=256, resource_limits=limits, mobility_history_limit=8, VERBOSE=False)
        env.rt_samples_per_src = samples
        context = zmq.Context()
        socket = context.socket(zmq.REP)
        socket.setsockopt(zmq.LINGER, 0)
        socket.setsockopt(zmq.RCVTIMEO, int(timeout * 1000))
        socket.setsockopt(zmq.SNDTIMEO, int(timeout * 1000))
        port = socket.bind_to_random_port("tcp://127.0.0.1")
        provenance.atomic_json(output / "ready.json", {"endpoint": f"tcp://127.0.0.1:{port}",
            "pid": os.getpid(), "backend": mi.variant(), "samples": samples})
        (output / "requests").mkdir()
        (output / "responses").mkdir()
        records, validations, config = [], [], None
        try:
            with (output / "records.jsonl").open("w") as stream:
                while True:
                    raw = socket.recv()
                    index = len(records)
                    (output / "requests" / f"{index:06d}.pb").write_bytes(raw)
                    start = time.perf_counter_ns()
                    request = message_pb2.Wrapper.FromString(raw)
                    parsed = time.perf_counter_ns()
                    response = message_pb2.Wrapper()
                    failure = None
                    kind = request.WhichOneof("msg")
                    record = {"request_index": index, "request_bytes": len(raw)}
                    try:
                        if kind == "sim_init_msg":
                            _require(config is None, "Repeated initialization")
                            config = message_pb2.SimInitMessage()
                            config.CopyFrom(request.sim_init_msg)
                            success, error = env.init_simulation_env(config)
                            response.sim_ack.no_error = success
                            response.sim_ack.error_msg = error
                            response.sim_ack.SetInParent()
                            _require(success, f"Sionna initialization failed: {error}")
                            record["kind"] = "init"
                        elif kind == "channel_state_request":
                            _require(config is not None, "Channel request before initialization")
                            env.compute_cfr(request.channel_state_request, response)
                            # Production CFR conversion uses out_type="numpy" and appends
                            # scalar doubles: RT and CFR materialize before this returns.
                            limits.check(response.ByteSize(), "reply_bytes")
                            record.update(kind="channel", request_time_ns=request.channel_state_request.time,
                                tx_node=request.channel_state_request.tx_node,
                                rx_node=request.channel_state_request.rx_node)
                        elif kind == "sim_close_request":
                            response.sim_ack.SetInParent()
                            record["kind"] = "close"
                        else:
                            raise ValueError(f"Unexpected request type {kind}")
                    except Exception as error:
                        failure = error
                        response.Clear()
                        response.sim_ack.no_error = False
                        response.sim_ack.error_msg = str(error)
                    computed = time.perf_counter_ns()
                    reply = response.SerializeToString()
                    serialized = time.perf_counter_ns()
                    socket.send(reply)
                    sent = time.perf_counter_ns()
                    (output / "responses" / f"{index:06d}.pb").write_bytes(reply)
                    if record.get("kind") == "init" and failure is None:
                        (output / "configuration.pb").write_bytes(config.SerializeToString())
                        provenance.randomness_selected(config)
                        run.data["configuration_protobuf"] = provenance.digest(output / "configuration.pb")
                        run.data["actual_rt_controls"] = {key: value for key, value in vars(env).items()
                            if key.startswith("rt_") and isinstance(value, (bool, int, str, float))}
                    record.update(parse_ns=parsed-start, compute_ns=computed-parsed,
                        serialize_ns=serialized-computed, send_ns=sent-serialized,
                        response_bytes=len(reply), **rss())
                    if record.get("kind") == "channel" and failure is None:
                        validation = validate_response(request, response, config, limits)
                        validations.append({"request_index": index, **validation})
                        record.update(response_epochs=validation["epochs"], response_links=validation["links"])
                    record["mobility"] = env.mobility_storage_statistics()
                    records.append(record)
                    stream.write(json.dumps(record, sort_keys=True) + "\n")
                    stream.flush()
                    if failure:
                        raise failure
                    if kind == "sim_close_request":
                        break
            provenance.atomic_json(output / "validation.json", {"valid": True, "responses": validations})
            provenance.atomic_json(output / "summary.json", summarize(records))
            fields = sorted({key for record in records for key in record} - {"mobility"})
            with (output / "records.csv").open("w", newline="") as stream:
                writer = csv.DictWriter(stream, fieldnames=fields, extrasaction="ignore")
                writer.writeheader()
                writer.writerows(records)
            run.data["validation"] = {"valid": True, "responses": len(validations)}
        except BaseException as error:
            provenance.atomic_json(output / "validation.json", {"valid": False,
                "error": str(error), "responses": validations})
            raise
        finally:
            socket.close()
            context.term()
            if hasattr(env, "scene"):
                env.release()


def executable():
    paths = sorted((base.NS3 / "build/contrib/sionna/examples").glob(
        "ns3.40-ns3_sionna_live_benchmark-*"))
    if len(paths) != 1:
        raise FileNotFoundError("Build ns3_sionna_live_benchmark first; expected one executable")
    return paths[0]


def _stop(process):
    if process and process.poll() is None:
        process.terminate()
        try:
            process.wait(timeout=5)
        except subprocess.TimeoutExpired:
            process.kill()
            process.wait(timeout=5)


def run_pair(case, repeat, case_output, binary, timeout=300):
    case_output = ensure_empty_output(case_output)
    server = case_output / "server"
    native = case_output / "native"
    command = [sys.executable, str(Path(__file__).resolve()), "--worker",
               f"--output={server}", f"--samples={case['samples']}", f"--timeout={timeout}"]
    provenance.atomic_json(case_output / "processes.json", {"worker_command": command})
    peer = process = None
    with (case_output / "server.stdout.log").open("w") as out, \
            (case_output / "server.stderr.log").open("w") as err, \
            (case_output / "native.stdout.log").open("w") as native_out, \
            (case_output / "native.stderr.log").open("w") as native_err:
        try:
            startup_start = time.perf_counter_ns()
            peer = subprocess.Popen(command, cwd=base.NS3, stdout=out, stderr=err)
            deadline = time.monotonic() + timeout
            ready = server / "ready.json"
            while not ready.exists():
                _require(peer.poll() is None, "Live server exited before ready; see server.stderr.log")
                if time.monotonic() > deadline:
                    raise TimeoutError("Live server readiness timed out")
                time.sleep(0.05)
            endpoint = json.loads(ready.read_text())["endpoint"]
            startup_ns = time.perf_counter_ns() - startup_start
            arguments = dict(output=native, endpoint=endpoint, receivers=case["receivers"],
                antennas=case["antennas"], widthMhz=case["width_mhz"],
                cacheEnabled=str(case["cache_enabled"]).lower(), lookaheadDepth=case["lookahead_depth"],
                epochs=case["epochs"], packetsPerEpoch=2, seed=42, run=1)
            native_command = [str(binary), *(f"--{key}={value}" for key, value in arguments.items())]
            provenance.native_command(native_command)
            provenance.atomic_json(case_output / "processes.json", {"worker_command": command,
                "native_command": native_command, "repeat": repeat, "endpoint": endpoint,
                "worker_pid": peer.pid, "server_startup_to_ready_ns": startup_ns})
            process = subprocess.Popen(native_command, cwd=base.NS3, stdout=native_out, stderr=native_err)
            process.wait(timeout=timeout)
            _require(process.returncode == 0, f"Native live process failed ({process.returncode}); see native.stderr.log")
            peer.wait(timeout=timeout)
            _require(peer.returncode == 0, f"Live server failed ({peer.returncode}); see server.stderr.log")
        finally:
            _stop(process)
            _stop(peer)
    return case_output


def _read_csv(path):
    with Path(path).open(newline="") as stream:
        return list(csv.DictReader(stream))


def validate_transcript(case, records, server, native_summary):
    """Exact counts/times for four production getters per receiver per send.

    Scalar loss/delay run at transmission time. Adapter loss/CFR run at
    transmission time plus propagation delay. One cached batch fills every
    receiver and forecast interval; no instrumentation invokes a getter.
    """
    from collections import Counter
    expected_getters = case["epochs"] * 2 * case["receivers"] * 4
    channels = [row for row in records if row["kind"] == "channel"]
    sends = [(epoch, packet, 1000000 + epoch * 6000000 + packet * 1000000)
             for epoch in range(case["epochs"]) for packet in range(2)]
    if case["cache_enabled"]:
        expected = []
        coverage_end = -1
        for epoch, packet, timestamp in sends:
            if timestamp > coverage_end:
                expected.append(timestamp)
                coverage_end = timestamp + 5000000 * case["lookahead_depth"]
                if case["lookahead_depth"] > 1:
                    coverage_end -= 1
        _require([row["request_time_ns"] for row in channels] == expected,
                 "Cached RPC times/count disagree with validity replay")
        _require(all(row["tx_node"] == 0 and row["rx_node"] == 1 for row in channels),
                 "Cached batch misses unexpected link identity")
    else:
        _require(len(channels) == expected_getters, "Disabled-cache production getter/RPC count mismatch")
        observed = Counter()
        for row in channels:
            response = message_pb2.Wrapper.FromString((Path(server) / "responses" /
                f"{row['request_index']:06d}.pb").read_bytes()).channel_state_response
            radio = next(rx for rx in response.csi[0].rx_nodes if rx.id == row["rx_node"])
            match = [(epoch, packet, timestamp) for epoch, packet, timestamp in sends
                     if row["request_time_ns"] in (timestamp, timestamp + radio.delay)]
            _require(len(match) == 1 and row["tx_node"] == 0, "Unscheduled uncached RPC time/identity")
            epoch, packet, timestamp = match[0]
            phase = "send" if row["request_time_ns"] == timestamp else "receive"
            observed[(epoch, packet, row["rx_node"], phase)] += 1
        expected = Counter({(epoch, packet, receiver, phase): 2
            for epoch, packet, _ in sends for receiver in range(1, case["receivers"] + 1)
            for phase in ("send", "receive")})
        _require(observed == expected, "Uncached getter time/link multiplicities mismatch")
    hit_ratio = (expected_getters - len(channels)) / expected_getters
    _require(np.isclose(float(native_summary["cache_hit_ratio"]), hit_ratio, rtol=0, atol=1e-12),
             "Native cache hit ratio differs from observed RPC/getter totals")
    return {"getter_calls": expected_getters, "rpc_requests": len(channels),
            "expected_hit_ratio": hit_ratio, "valid": True}


def summarize_native(sends, propagation, summary, case):
    """Separate the first packet from subsequent native wall timers.

    Subsequent wall timers are descriptive and separate: Send excludes the
    later scheduled adapter work, whereas Simulator::Run includes both.
    """
    result = {}
    for name, rows, column in (("send_ns", sends, "send_ns"),
                               ("adapter_ns", propagation, "adapter_ns")):
        rows = sorted(rows, key=lambda row: (int(row["epoch"]), int(row["packet"]),
                                             int(row.get("receiver", 0))))
        first_key = (0, 0)
        cold = [float(row[column]) for row in rows
                if (int(row["epoch"]), int(row["packet"])) == first_key]
        warm = [float(row[column]) for row in rows
                if (int(row["epoch"]), int(row["packet"])) != first_key]
        result[name] = {"cold": _statistics(cold), "warm": _statistics(warm)}
    simulated_ns = 1000000 + (case["epochs"] - 1) * 6000000 + 2000000
    result.update(startup_ns=summary["startup_ns"], helper_start_ns=summary["helper_start_ns"],
        simulator_run_ns=summary["simulator_run_ns"], simulated_duration_ns=simulated_ns,
        simulated_seconds_per_wall_second=simulated_ns / summary["simulator_run_ns"],
        rss={"current_peak_bytes": summary["peak_rss_bytes"], "hwm_peak_bytes": summary["hwm_bytes"]},
        interpretation="First packet of first epoch separated from later observations; "
            "later observations include subsequent cache misses; no latency threshold")
    return result


def validate_case(case, path, write=True):
    """Recheck every raw reply, transcript, native counters, and retention snapshots."""
    path = Path(path)
    server = path / "server"
    native = path / "native"
    records = [json.loads(line) for line in (server / "records.jsonl").read_text().splitlines()]
    config = message_pb2.SimInitMessage.FromString((server / "configuration.pb").read_bytes())
    _require(config.fft_size == 3 * case["width_mhz"] * 256 // 20
             and config.channel_bw == 3 * case["width_mhz"],
             "Native width/config mismatch")
    _require(config.scene_fname == base.SCENE and config.fixed_coherence_time_ms == 5
             and config.time_evo_model == "position" and config.subcarrier_spacing == 78125,
             "Native scene/coherence/FFT configuration mismatch")
    _require(config.frequency == (5180 if case["width_mhz"] == 20 else 5190),
             "Native center frequency mismatch")
    randomness = resolve_randomness(config)
    _require(all(randomness[key] == value for key, value in dict(mapping_version=1, ns3_seed=42,
        ns3_run=1, rt_stream=0, mobility_stream=1, rt_seed=42).items()),
        "Native randomness mapping/stream identity mismatch")
    for array in (config.tx_array, config.rx_array):
        _require(array_ports(resolve_array_config(array, RX_ARRAY_DEFAULTS)) == case["antennas"],
                 "Native helper antenna configuration mismatch")
        _require((array.num_rows, array.num_cols, array.vertical_spacing, array.horizontal_spacing,
            array.pattern, array.polarization) == (1, case["antennas"], 0.5, 0.5, "iso", "V"),
            "Native array geometry/pattern mismatch")
    _require(len(config.nodes) == case["receivers"] + 1 and config.seed == 42,
             "Native nodes/random seed mismatch")
    _require(config.mode == (3 if case["lookahead_depth"] > 1 else 2), "Native dispatch mode mismatch")
    if config.mode == 3:
        _require(config.sub_mode == case["receivers"] * case["lookahead_depth"], "Native lookahead control mismatch")
    _require([r["kind"] for r in records].count("init") == 1 and records[0]["kind"] == "init"
             and records[-1]["kind"] == "close", "Init/close transcript mismatch")
    for index, record in enumerate(records):
        _require(record["rss_kib"] > 0 and record["hwm_kib"] >= record["rss_kib"],
                 "Missing isolated server RSS/high-water sample")
        mobility = record["mobility"]
        _require(mobility["history_entries"] <= 8 * len(config.nodes)
                 and mobility["lookahead_states"] == 0 and mobility["prediction_nodes"] == 0,
                 "Server mobility/history storage is not bounded or forecast state retained")
        request_raw = (server / "requests" / f"{index:06d}.pb").read_bytes()
        response_raw = (server / "responses" / f"{index:06d}.pb").read_bytes()
        _require(len(request_raw) == record["request_bytes"] and len(response_raw) == record["response_bytes"],
                 "Transcript byte count mismatch")
        request = message_pb2.Wrapper.FromString(request_raw)
        response = message_pb2.Wrapper.FromString(response_raw)
        if record["kind"] == "channel":
            validation = validate_response(request, response, config)
            _require(record["request_index"] == index and record["response_epochs"] == validation["epochs"]
                     and record["response_links"] == validation["links"]
                     and record["request_time_ns"] == request.channel_state_request.time
                     and record["tx_node"] == request.channel_state_request.tx_node
                     and record["rx_node"] == request.channel_state_request.rx_node,
                     "Raw transcript and JSONL identity mismatch")
        elif record["kind"] == "init":
            _require(request.HasField("sim_init_msg") and response.HasField("sim_ack")
                     and response.sim_ack.no_error and response.sim_ack.error_msg in ("", "OK"),
                     "Initialization acknowledgement mismatch")
            _require(request.sim_init_msg == config, "Native init bytes/configuration artifact mismatch")
        else:
            _require(request.HasField("sim_close_request") and response.HasField("sim_ack")
                     and not response.sim_ack.error_msg, "Close acknowledgement mismatch")
    summary = json.loads((native / "summary.json").read_text())
    worker_manifest = json.loads((server / "run.json").read_text())
    _require(worker_manifest["status"] == "success"
             and worker_manifest["backend"] == "llvm_ad_mono_polarized", "Failed or wrong-backend live worker")
    _require(worker_manifest["configuration"]["samples"] == case["samples"]
             and worker_manifest["actual_rt_controls"]["rt_samples_per_src"] == case["samples"],
             "Actual RT sample budget differs from predeclared plan")
    _require(all(worker_manifest["actual_rt_controls"]["rt_" + key] == base.CONTROLS[key]
        for key in ("max_depth", "synthetic_array", "los", "specular_reflection", "diffuse_reflection",
                    "refraction", "diffraction", "edge_diffraction", "diffraction_lit_region")),
        "Actual live RT solver flags differ from predeclared controls")
    _require(summary["validation"] == "PASS", "Native live validation did not pass")
    _require(summary["tx_power_dbm_total"] == 20 and summary["phy_rx_success_count"] > 0,
             "Fixed power or successful complete PHY reception missing")
    _require(summary["seed"] == 42 and summary["run"] == 1 and summary["packets_per_epoch"] == 2
             and summary["coherence_ms"] == 5 and summary["epoch_spacing_ms"] == 6,
             "Native workload control mismatch")
    _require(summary["mcs"] == 0 and summary["nss"] == case["antennas"]
             and summary["eesm_beta_linear"] == 2 and not summary["beta_calibrated"],
             "Native PHY abstraction controls mismatch")
    _require(summary["phy_rx_success_count"] + summary["phy_rx_failure_count"]
             == case["epochs"] * 2 * case["receivers"], "Missing distinct native reception outcomes")
    _require(summary["simulator_run_ns"] > 0 and summary["peak_rss_bytes"] > 0 and summary["hwm_bytes"] > 0,
             "Missing isolated native runtime/memory samples")
    for key in ("receivers", "antennas", "width_mhz", "lookahead_depth", "cache_enabled", "epochs"):
        _require(summary[key] == case[key], f"Native summary {key} mismatch")
    sends = _read_csv(native / "sends.csv")
    channels = _read_csv(native / "channels.csv")
    receptions = _read_csv(native / "receptions.csv")
    propagation = _read_csv(native / "propagation.csv")
    _require(len(sends) == case["epochs"] * 2, "Missing native PHY send samples")
    _require(sorted((int(row["epoch"]), int(row["packet"]), int(row["time_ns"])) for row in sends)
             == [(epoch, packet, 1000000 + epoch * 6000000 + packet * 1000000)
                 for epoch in range(case["epochs"]) for packet in range(2)],
             "Native send schedule differs from the predeclared workload")
    _require(bool(channels) and bool(receptions), "No actual PHY receive/channel callbacks")
    _require(len(propagation) == case["epochs"] * 2 * case["receivers"]
             and summary["adapter_calls"] == len(propagation), "Missing production adapter samples")
    _require(summary["rx_ok_callbacks"] + summary["rx_error_callbacks"] + summary["rx_drop_callbacks"]
             == len(receptions), "Native PHY callback totals mismatch")
    if case["antennas"] == 2:
        _require(len(channels) == len(propagation) and summary["mimo_trace_count"] == len(channels),
                 "Missing actual MIMO trace callbacks")
    for row in channels:
        _require(math.isfinite(float(row["sinr_linear"])) and float(row["sinr_linear"]) > 0,
                 "Non-finite native PHY SINR")
        if row.get("per"):
            _require(0 <= float(row["per"]) <= 1, "Invalid native PHY PER")
    if not case["cache_enabled"]:
        for row in [*sends, *propagation, summary]:
            _require(all(int(row[key]) == 0 for key in ("cache_links", "cache_snapshots",
                         "cache_payload_bytes", "cache_capacity_bytes")),
                     "Disabled cache retains native CSI")
    for row in [*sends, *propagation, summary]:
        expected_payload = int(row["cache_snapshots"]) * config.fft_size * (16 * case["antennas"] ** 2 + 4)
        _require(int(row["cache_payload_bytes"]) == expected_payload
                 and int(row["cache_capacity_bytes"]) >= expected_payload,
                 "Native logical CSI payload/dimension accounting mismatch")
        _require(int(row["pool_capacity_bytes"]) <= 1048576
                 and int(row["pool_peak_capacity_bytes"]) <= 1048576,
                 "Native bounded staging pool exceeds its production default")
    channels_records = [record for record in records if record["kind"] == "channel"]
    # Native propagation asks delay/loss and (for MIMO) channel for each link.
    # Exact getter replay is validated from the exported counters/trajectory below.
    _require(bool(channels_records), "No real RT requests")
    transcript = validate_transcript(case, records, server, summary)
    stage_summary = summarize_native(sends, propagation, summary, case)
    result = {"valid": True, "requests": len(channels_records), "responses": len(channels_records),
              "phy_channel_callbacks": len(channels), "phy_receptions": len(receptions),
              "server": summarize(records), "native": summary, "native_timing": stage_summary,
              "transcript": transcript, "decoded_cfr_subcarriers": config.fft_size,
              "spectrum_width_mhz": case["width_mhz"], "rt_bandwidth_mhz": config.channel_bw}
    if write:
        provenance.atomic_json(path / "validation.json", result)
    return result


def validate_pairs(policy, output):
    results = []
    cases = policy["workload"]
    for off in [case for case in cases if not case["cache_enabled"]]:
        key = lambda c: tuple(c[k] for k in ("receivers", "antennas", "width_mhz", "lookahead_depth", "samples"))
        on = next(case for case in cases if case["cache_enabled"] and key(case) == key(off))
        for repeat in range(policy["controls"]["repeats"]):
            off_path = Path(output) / off["case_id"] / f"repeat_{repeat:02d}"
            on_path = Path(output) / on["case_id"] / f"repeat_{repeat:02d}"
            a = _read_csv(off_path / "native/channels.csv")
            b = _read_csv(on_path / "native/channels.csv")
            identity = lambda r: (int(r["epoch"]), int(r["packet"]), int(r["receiver"]), r["source"])
            a, b = sorted(a, key=identity), sorted(b, key=identity)
            _require([identity(r) for r in a] == [identity(r) for r in b], "Cache pair PHY identity mismatch")
            for ra, rb in zip(a, b):
                for metric in ("sinr_linear", "per", "rank"):
                    if ra.get(metric) or rb.get(metric):
                        _require(bool(ra.get(metric)) and bool(rb.get(metric)), "Cache pair metric missing")
                        _require(np.isclose(float(ra[metric]), float(rb[metric]), rtol=1e-4, atol=1e-7),
                                 f"Cache pair {metric} mismatch for {identity(ra)}")
            va = json.loads((off_path / "validation.json").read_text())
            vb = json.loads((on_path / "validation.json").read_text())
            _require(va["requests"] > vb["requests"], "Cache on does not reduce actual RPC requests")
            # First requests have identical physical geometry, batching and seed;
            # compare raw channels as well as the actual native PHY diagnostics.
            def first_response(path):
                rows = [json.loads(line) for line in (path / "server/records.jsonl").read_text().splitlines()]
                index = next(row["request_index"] for row in rows if row["kind"] == "channel")
                return message_pb2.Wrapper.FromString((path / "server/responses" /
                    f"{index:06d}.pb").read_bytes()).channel_state_response
            raw_a, raw_b = first_response(off_path), first_response(on_path)
            _require(len(raw_a.csi) == len(raw_b.csi), "Cache pair raw forecast depth mismatch")
            for state_a, state_b in zip(raw_a.csi, raw_b.csi):
                for radio_a, radio_b in zip(state_a.rx_nodes, state_b.rx_nodes):
                    _require(radio_a.id == radio_b.id, "Cache pair raw receiver order mismatch")
                    _require(np.allclose([radio_a.wb_loss, *radio_a.csi_real, *radio_a.csi_imag],
                        [radio_b.wb_loss, *radio_b.csi_real, *radio_b.csi_imag], rtol=1e-4, atol=1e-7),
                        "Cache pair initial raw coefficients/wideband loss mismatch")
            results.append({"off": off["case_id"], "on": on["case_id"], "repeat": repeat,
                "valid": True, "off_requests": va["requests"], "on_requests": vb["requests"],
                "metrics_compared": len(a)})
    return results


def validate_export(output):
    """Read-only audit of a complete export, including every provenance hash."""
    output = Path(output)
    policy = validate_plan(json.loads((output / "plan.json").read_text()))
    manifest = json.loads((output / "run.json").read_text())
    _require(manifest["status"] == "success" and manifest["backend"] == "llvm_ad_mono_polarized",
             "Export is not a successful CPU live run")
    verdict = json.loads((output / "validation.json").read_text())
    _require(verdict["valid"], "Live acceptance failed")
    for root in [output, *(output / case["case_id"] / f"repeat_{repeat:02d}" / "server"
        for case in policy["workload"] for repeat in range(policy["controls"]["repeats"]))]:
        record = json.loads((root / "run.json").read_text())
        _require(record["status"] == "success", "Failed process manifest in export")
        for name, digest in record["artifacts"].items():
            provenance.verify_digest(root / name, digest)
    cases = []
    for case in policy["workload"]:
        for repeat in range(policy["controls"]["repeats"]):
            path = output / case["case_id"] / f"repeat_{repeat:02d}"
            actual = {**case, "epochs": policy["controls"]["epochs"]}
            observed = validate_case(actual, path, write=False)
            _require(observed == json.loads((path / "validation.json").read_text()),
                     "Stored case verdict differs from read-only revalidation")
            cases.append({"case_id": case["case_id"], "repeat": repeat, **observed})
    pairs = validate_pairs(policy, output)
    _require(verdict == {"valid": True, "cases": cases, "cache_pairs": pairs},
             "Stored acceptance verdict differs from revalidation")
    return verdict


def run(output, samples=10000, epochs=4, repeats=3, quick=False, binary=None, timeout=300):
    output = ensure_empty_output(output)
    policy = plan(samples, epochs, repeats, quick)
    provenance.atomic_json(output / "plan.json", policy)
    # Resolve the binary inside the Run so missing executables retain failure diagnostics.
    with provenance.Run(output / "run.json", policy,
            inputs=[base.SERVER / "models" / base.SCENE], binary=binary) as manifest:
        validations = []
        try:
            binary = Path(binary).resolve() if binary else executable()
            manifest.data["build_live_executable"] = {"path": str(binary), "digest": provenance.digest(binary)}
            if not manifest.binary:
                manifest.data["build"] = {"executable": str(binary), "digest": provenance.digest(binary),
                                          "ns3_source": provenance.ns3_identity()}
                for name in ("cmake-cache/CMakeCache.txt", "cmake-cache/compile_commands.json"):
                    build_input = base.NS3 / name
                    if build_input.exists():
                        manifest.data["build"][name] = provenance.digest(build_input)
            for case in policy["workload"]:
                for repeat in range(policy["controls"]["repeats"]):
                    case_output = output / case["case_id"] / f"repeat_{repeat:02d}"
                    actual = {**case, "epochs": policy["controls"]["epochs"]}
                    print(f"R4 {case['case_id']} repeat {repeat}: {actual}", flush=True)
                    run_pair(actual, repeat, case_output, binary, timeout)
                    validation = validate_case(actual, case_output)
                    validations.append({"case_id": case["case_id"], "repeat": repeat, **validation})
                    manifest.data["backend"] = json.loads((case_output / "server/run.json").read_text())["backend"]
            pairs = validate_pairs(policy, output)
            verdict = {"valid": True, "cases": validations, "cache_pairs": pairs}
            provenance.atomic_json(output / "validation.json", verdict)
            provenance.atomic_json(output / "manifest.json", {"schema_version": 1, "subphase": "R4",
                "controls": policy["controls"], "plan": policy,
                "configurations": [{**case, "epochs": policy["controls"]["epochs"]}
                                   for case in policy["workload"]],
                "validation": {"valid": True, "cases": len(validations), "cache_pairs": len(pairs)}})
            provenance.atomic_json(output / "summary.json", {"schema_version": 1,
                "subphase": "R4", "cases": validations,
                "limits": "Descriptive representative measurements; few warm observations; "
                    "process high-water RSS is isolated and peaks are not added; fixed geometry, no aging"})
            manifest.data["validation"] = {"valid": True, "cases": len(validations), "cache_pairs": len(pairs)}
            return verdict
        except BaseException as error:
            provenance.atomic_json(output / "validation.json", {"valid": False, "cases": validations,
                "error": str(error), "diagnostics": "Retained process stdout/stderr, raw requests/replies and run.error.log"})
            raise


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--output", required=True, type=Path)
    parser.add_argument("--samples", type=int, default=10000)
    parser.add_argument("--epochs", type=int, default=4)
    parser.add_argument("--repeats", type=int, default=3)
    parser.add_argument("--quick", action="store_true")
    parser.add_argument("--worker", action="store_true", help=argparse.SUPPRESS)
    parser.add_argument("--binary", type=Path)
    parser.add_argument("--timeout", type=float, default=300)
    args = parser.parse_args(argv)
    if args.samples <= 0 or args.epochs <= 0 or args.repeats <= 0 or args.timeout <= 0:
        parser.error("samples, epochs, repeats and timeout must be positive")
    if args.worker:
        worker(args.output, args.samples, args.timeout)
    else:
        run(args.output, args.samples, args.epochs, args.repeats, args.quick, args.binary, args.timeout)


if __name__ == "__main__":
    main()
