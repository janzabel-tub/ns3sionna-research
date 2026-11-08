#!/usr/bin/env python3
"""Phase 5.2.3: frozen-channel production-cache and local cross-process IPC scaling.

The REP peer returns preencoded channels, never ray traces. Larger tensors are
transport fixtures tiled from one frozen 2x2 reference, not physical predictions.
All acceptance criteria are written before the first native measurement.

--copy-profile restricts the sweep to 64/2x2/1 and 1024/8x8/16 and adds
loss (const overload), delay, frequency and CSI hit timers. cache_hit remains
the channel-matrix timer. Each case uses three independent native processes;
the optional mode writes the tabular exports and its predefined policy/run.json.

--workload-sweep checks cache on/off over evolving time and active link windows
for 1/16/64 working-set links and depths 1/4/16. Three fresh process pairs per
case export exact occupancy/RPC trajectories and whole-epoch timings. The
default 128 epochs can be extended with --workload-steps (multiples of 16,
up to 4096). These synthetic changes test cache correctness, not mobility/RT.
"""
from __future__ import annotations

import argparse
import csv
from datetime import datetime, timezone
import hashlib
import importlib.metadata
import itertools
import json
from pathlib import Path
import platform
import resource
import shutil
import subprocess
import sys
import tempfile
import time

import numpy as np
import zmq
import benchmark_sumimo as base

SUBCARRIERS = (64, 256, 1024)
ANTENNAS = ((1, 1), (2, 2), (4, 4), (2, 4), (8, 8))  # (TX, RX)
LINKS = (1, 16, 64)
COPY_PROFILE_CASES = ((64, (2, 2), 1), (1024, (8, 8), 16))
COPY_PROFILE_OPERATIONS = ("cache_hit_loss", "cache_hit_delay", "cache_hit_frequency", "cache_hit_csi")
WORKLOAD_DEPTHS = (1, 4, 16)
WORKLOAD_PERIOD_NS = 5000001
CONTROLS = {"seed_ns3": 42, "run_ns3": 1, "fixture_seed": 42,
    "repetitions": 3, "iterations": 16, "hit_iterations": 128, "miss_iterations": 5,
    "warmup": 3, "transport": "local ipc:// REQ/REP, separate Python process",
    "socket_timeout_ms": 10000, "ray_tracing": False, "validity_ns": [0, 5000000],
    "expiry_probe_ns": 5000001, "frequency_spacing_hz": 78125,
    "cache_optimization": False, "cache_hit_timer": "public getter, CFR copies and cleanup",
    "miss_timer": "fresh-cache getter including IPC, decoding and batch insertion",
    "ipc_timer": "send/recv only; preencoded reply; byte validation outside timer",
    "memory": "Linux RSS and glibc mallinfo2 uordblks+hblkhd; isolated native processes",
    "fixture_derivation": "repeat source sc/rx/tx modulo 256/2/2; per-link exact quarter-turn "
                          "phase (42+link-1)%4; loss += (link-1)/8; delay += link-1"}
ACCEPTANCE = {"coefficient_and_metadata_preservation": "bit-exact coefficients; exact dimensions, "
                "frequency order, wideband loss and delay for every cached link",
    "payload_and_logical_memory": "exact Protobuf byte count and 16*L*Nsc*Nrx*Ntx + 4*L*Nsc bytes",
    "native_hit_ratio_absolute_tolerance": 1e-12,
    "transport": "all raw RTT replies byte-identical to the frozen Wrapper; exactly 27 channel "
                 "requests (26 at time 0, one at 5000001), one acknowledged close",
    "expiry": "inclusive end-time hit at 5000000; miss at 5000001",
    "repeatability": "all deterministic fields and fixture hashes identical across three processes",
    "measurements": "complete finite positive latency samples and positive RSS/heap snapshots; "
                    "signed allocator deltas are observations, not hard performance thresholds"}
ANCHOR = base.MODULE / "data/phase5_2_sample_convergence/case_05/seed_42/samples_100000/channel_00.pb"


def executable():
    paths = sorted((base.NS3 / "build/contrib/sionna/examples").glob(
        "ns3.40-ns3_sionna_cache_ipc_benchmark-*"))
    if len(paths) != 1:
        raise FileNotFoundError("Build ns3_sionna_cache_ipc_benchmark first")
    return paths[0]


def plan(copy_profile=False):
    result = {"schema_version": 1, "subphase": "5.2.3", "controls": CONTROLS,
            "sweep": {"subcarriers": SUBCARRIERS, "antenna_pairs_tx_rx": ANTENNAS, "links": LINKS},
            "acceptance": ACCEPTANCE, "anchor": str(ANCHOR.relative_to(base.MODULE)),
            "anchor_sha256": base.sha256(ANCHOR)}
    if copy_profile:
        result.update(subphase="r3-cfr-copy", copy_profile=True,
            controls={**CONTROLS, "cache_hit_timer": "public getters with scoped output cleanup; "
                "cache_hit=channel matrix; const loss, delay, frequency and CSI measured separately"},
            sweep={"cases": [{"subcarriers": nsc, "antenna_pairs_tx_rx": antennas, "links": links}
                              for nsc, antennas, links in COPY_PROFILE_CASES]},
            acceptance={**ACCEPTANCE, "copy_profile": {
                "additional_operations": COPY_PROFILE_OPERATIONS,
                "samples_per_additional_operation": CONTROLS["hit_iterations"],
                "cache_hit_count": 5 * (CONTROLS["warmup"] + CONTROLS["hit_iterations"]) + 1,
                "cache_miss_count": 2, "channel_requests_per_process": 27,
                "memory_validation": "5L hits, one miss; CSI coefficients also bit-exact for every link"}})
    return result


def template(subcarriers, tx, rx):
    source = base.message_pb2.ChannelStateResponse.FromString(ANCHOR.read_bytes())
    h = base.channel_array(source)
    derived = h[np.arange(subcarriers)[:, None, None] % 256,
                np.arange(rx)[None, :, None] % 2, np.arange(tx)[None, None, :] % 2]
    wrapper = base.message_pb2.Wrapper()
    wrapper.channel_state_response.CopyFrom(source)
    state = wrapper.channel_state_response.csi[0]
    state.start_time, state.end_time = 0, 5000000
    radio = state.rx_nodes[0]
    radio.num_tx_antennas, radio.num_rx_antennas = tx, rx
    radio.ClearField("frequencies")
    radio.ClearField("csi_real")
    radio.ClearField("csi_imag")
    radio.frequencies.extend((np.arange(subcarriers) - subcarriers // 2) * 78125)
    radio.csi_real.extend(derived.real.reshape(-1))
    radio.csi_imag.extend(derived.imag.reshape(-1))
    radio.end_time2 = state.end_time
    return wrapper


def batch(fixture, links):
    if not 1 <= links <= 64:
        raise ValueError("Expected 1..64 links")
    result = base.message_pb2.Wrapper()
    result.CopyFrom(fixture)
    state = result.channel_state_response.csi[0]
    first = base.message_pb2.ChannelStateResponse.ChannelState.RxNodeInfo()
    first.CopyFrom(state.rx_nodes[0])
    state.ClearField("rx_nodes")
    real, imag = np.array(first.csi_real), np.array(first.csi_imag)
    phases = ((real, imag), (-imag, real), (-real, -imag), (imag, -real))
    for link in range(1, links + 1):
        radio = state.rx_nodes.add()
        radio.CopyFrom(first)
        radio.id = link
        radio.wb_loss = first.wb_loss + (link - 1) / 8
        radio.delay = first.delay + link - 1
        radio.ClearField("csi_real")
        radio.ClearField("csi_imag")
        values = phases[(CONTROLS["fixture_seed"] + link - 1) % 4]
        radio.csi_real.extend(values[0])
        radio.csi_imag.extend(values[1])
    return result


def coefficient_digest(fixture):
    """Noncryptographic ordered IEEE-bit checksum; native also checks every coefficient."""
    digest, offset = 0, 0
    for radio in fixture.channel_state_response.csi[0].rx_nodes:
        real, imag = (np.asarray(values, dtype=np.float64).view(np.uint64)
                      for values in (radio.csi_real, radio.csi_imag))
        index = np.arange(offset, offset + len(real), dtype=np.uint64)
        digest ^= int(np.bitwise_xor.reduce(real * (2 * index + 1) ^ imag * (2 * index + 3)))
        offset += len(real)
    return f"{digest:016x}"


def workload_state(fixture, step, depth):
    """One epoch, with a rotating bounded active window and time-dependent CFR."""
    source = fixture.channel_state_response.csi[0]
    links = len(source.rx_nodes)
    active = max(1, links // 8)
    first = (step // depth * active) % links
    state = base.message_pb2.ChannelStateResponse.ChannelState()
    state.CopyFrom(source)
    state.ClearField("rx_nodes")
    state.start_time = step * WORKLOAD_PERIOD_NS
    state.end_time = state.start_time + WORKLOAD_PERIOD_NS - 1
    for original in source.rx_nodes[first:first + active]:
        radio = state.rx_nodes.add()
        radio.CopyFrom(original)
        radio.end_time2 = state.end_time
        radio.wb_loss += step / 8
        radio.delay += step
        real, imag = np.asarray(original.csi_real), np.asarray(original.csi_imag)
        phases = ((real, imag), (-imag, real), (-real, -imag), (imag, -real))
        radio.ClearField("csi_real")
        radio.ClearField("csi_imag")
        radio.csi_real.extend(phases[step % 4][0])
        radio.csi_imag.extend(phases[step % 4][1])
    return state


def workload_batch(fixture, step, depth):
    result = base.message_pb2.Wrapper()
    start = step // depth * depth
    for epoch in range(start, start + depth):
        result.channel_state_response.csi.add().CopyFrom(workload_state(fixture, epoch, depth))
    return result


def serve(input_path, endpoint, ready_path, stats_path, workload_depth=0):
    wire = input_path.read_bytes()
    response = base.message_pb2.Wrapper.FromString(wire)
    context = zmq.Context()
    socket = context.socket(zmq.REP)
    socket.setsockopt(zmq.LINGER, 0)
    socket.bind(endpoint)
    ready_path.write_text("ready\n")
    requests = []
    last_block, workload_wire = None, None
    try:
        while True:
            if not socket.poll(60000):
                raise TimeoutError("Frozen REP peer received no request for 60 seconds")
            request = base.message_pb2.Wrapper.FromString(socket.recv())
            if request.WhichOneof("msg") == "sim_close_request":
                ack = base.message_pb2.Wrapper()
                ack.sim_ack.no_error = True
                socket.send(ack.SerializeToString(deterministic=True))
                break
            if request.WhichOneof("msg") != "channel_state_request":
                raise ValueError("Unexpected frozen-peer request type")
            query = request.channel_state_request
            if workload_depth:
                if query.tx_node != 0 or query.time % WORKLOAD_PERIOD_NS:
                    raise ValueError("Unexpected workload transmitter or timestamp")
                step = query.time // WORKLOAD_PERIOD_NS
                block = step // workload_depth
                active = max(1, len(response.channel_state_response.csi[0].rx_nodes) // 8)
                first = block * active % len(response.channel_state_response.csi[0].rx_nodes) + 1
                if not first <= query.rx_node < first + active:
                    raise ValueError("Unexpected workload active receiver")
                if block != last_block:
                    workload_wire = workload_batch(response, step, workload_depth).SerializeToString(deterministic=True)
                    last_block = block
                requests.append({"tx_node": query.tx_node, "rx_node": query.rx_node, "time_ns": query.time})
                socket.send(workload_wire)
                continue
            if (query.tx_node, query.rx_node) != (0, 1) or (query.time < 0 or query.time % 5000001 != 0):
                raise ValueError("Unexpected frozen-peer endpoints or timestamp")
            requests.append({"tx_node": query.tx_node, "rx_node": query.rx_node, "time_ns": query.time})
            if query.time:
                state = response.channel_state_response.csi[0]
                state.start_time, state.end_time = query.time, query.time + 5000000
                for radio in state.rx_nodes:
                    radio.end_time2 = state.end_time
                socket.send(response.SerializeToString(deterministic=True))
            else:
                socket.send(wire)
        hwm = next(line for line in Path("/proc/self/status").read_text().splitlines()
                   if line.startswith("VmHWM:"))
        stats_path.write_text(json.dumps({"channel_requests": len(requests), "close_requests": 1,
            "requests": requests, "peak_rss_bytes": int(hwm.split()[1]) * 1024,
            "lifetime_peak_rss_bytes": resource.getrusage(resource.RUSAGE_SELF).ru_maxrss * 1024},
            indent=2) + "\n")
    finally:
        socket.close()
        context.term()


def deterministic_fields(record, *, include_environment=True):
    """Compare behavior; historical replay may use different native libraries.

    Keep library versions in same-environment repeat checks and raw records.
    Cross-environment replay excludes only these two version fields.
    """
    return {key: value for key, value in record.items()
            if (include_environment or key not in ("cpp_protobuf_version", "cpp_zmq_version"))
            and not any(token in key for token in ("rss", "heap", "_ns", "seconds"))}


def workload_plan(steps=128):
    if not 16 <= steps <= 4096 or steps % 16:
        raise ValueError("workload steps must be a multiple of 16 in [16, 4096]")
    return {"schema_version": 1, "subphase": "r3-cache-workload",
        "anchor": str(ANCHOR.relative_to(base.MODULE)), "anchor_sha256": base.sha256(ANCHOR),
        "controls": {"steps": steps, "epoch_period_ns": WORKLOAD_PERIOD_NS,
            "repetitions": 3, "seed_ns3": 42, "run_ns3": 1, "ray_tracing": False,
            "subcarriers": 64, "tx_antennas": 2, "rx_antennas": 2,
            "active_links": "max(1, working_set_links // 8); disjoint cyclic windows each depth epochs",
            "sub_mode_equivalent": "receiver_count * look_ahead; depth=floor(sub_mode/receiver_count)",
            "channel_evolution": "per epoch exact quarter-turn; loss += epoch/8 dB; delay += epoch ns",
            "timing": "whole epoch: four public getters per active link plus full coefficient/metadata validation",
            "memory": "logical payload/capacity per epoch, glibc arena-used+mmap proxy, Linux RSS/HWM; "
                      "no allocator or RSS acceptance threshold; no RT or evolving mobility history"},
        "sweep": {"links": LINKS, "look_ahead": WORKLOAD_DEPTHS, "cache_enabled": (True, False)},
        "acceptance": {"values": "bit-exact CFR/frequencies/loss/delay for every active link each epoch",
            "budget": "exact active_links * depth snapshots and their CFR/frequency payload; "
                      "no valid/future eviction; zero retained payload when disabled",
            "cleanup": "snapshots decrease every epoch; inactive links and capacities empty after final expiry",
            "rpc": "enabled: one first-link RPC per depth epochs; disabled: four RPCs per active link per epoch",
            "repeatability": "identical deterministic summaries and trajectory fields in three independent processes",
            "timings": "exactly steps positive finite epoch samples; median/p95 and process range reported"}}


def validate_workload(record, fixture, server, samples, trace, steps, depth, enabled):
    """Validate exported observations, including exact RPC identities and future occupancy."""
    links = len(fixture.channel_state_response.csi[0].rx_nodes)
    active = max(1, links // 8)
    radio = fixture.channel_state_response.csi[0].rx_nodes[0]
    payload = len(radio.csi_real) * 16 + len(radio.frequencies) * 4
    calls = 4 * active * steps
    misses = steps // depth if enabled else calls
    expected = {"steps": steps, "look_ahead": depth, "cache_enabled": int(enabled),
        "links": links, "active_links": active, "getter_calls": calls,
        "cache_miss_count": misses, "cache_hit_count": calls - misses,
        "snapshot_budget": active * depth, "payload_budget_bytes": payload * active * depth,
        "peak_snapshots": active * depth if enabled else 0,
        "peak_payload_bytes": payload * active * depth if enabled else 0,
        "final_snapshots": 0, "final_payload_bytes": 0, "final_capacity_bytes": 0}
    for key, value in expected.items():
        assert record[key] == value, (key, record[key], value)
    assert abs(record["cache_hit_ratio"] - (calls - misses) / calls) <= 1e-12
    assert len(trace) == len(samples) == steps
    requests = []
    for step, row in enumerate(trace):
        first = step // depth * active % links + 1
        snapshots = active * (depth - step % depth) if enabled else 0
        wrapped = base.message_pb2.Wrapper()
        wrapped.channel_state_response.csi.add().CopyFrom(workload_state(fixture, step, depth))
        required = {"step": step, "time_ns": step * WORKLOAD_PERIOD_NS, "active_first": first,
            "active_links": active, "links": active if enabled else 0, "snapshots": snapshots,
            "payload_bytes": snapshots * payload, "peak_snapshots": expected["peak_snapshots"],
            "peak_payload_bytes": expected["peak_payload_bytes"],
            "coefficient_digest": coefficient_digest(wrapped)}
        for key, value in required.items():
            assert row[key] == value, (step, key, row[key], value)
        assert row["capacity_bytes"] >= row["payload_bytes"]
        if not enabled:
            assert row["capacity_bytes"] == 0
        assert row["heap_bytes"] > 0 and row["rss_bytes"] > 0
        sample = samples[step]
        assert sample["operation"] == "workload_epoch" and sample["iteration"] == step
        assert np.isfinite(sample["nanoseconds"]) and sample["nanoseconds"] > 0
        if enabled:
            if step % depth == 0:
                requests.append({"tx_node": 0, "rx_node": first, "time_ns": step * WORKLOAD_PERIOD_NS})
        else:
            for receiver in range(first, first + active):
                requests.extend({"tx_node": 0, "rx_node": receiver, "time_ns": step * WORKLOAD_PERIOD_NS}
                                for _ in range(4))
    assert server["channel_requests"] == misses and server["close_requests"] == 1
    assert server["requests"] == requests
    assert server["peak_rss_bytes"] > 0
    for key, value in record.items():
        if isinstance(value, (int, float)):
            assert np.isfinite(value), key
            if "heap" in key or "rss" in key:
                assert value > 0, key


def deterministic_trajectory(trace):
    return [{key: value for key, value in row.items() if key not in ("heap_bytes", "rss_bytes")}
            for row in trace]


def validate_record(record, fixture, server, samples, storage_steps=0, copy_profile=False):
    state = fixture.channel_state_response.csi[0]
    radio = state.rx_nodes[0]
    nsc, rx, tx, links = len(radio.frequencies), radio.num_rx_antennas, radio.num_tx_antennas, len(state.rx_nodes)
    expected = {"subcarriers": nsc, "num_tx_antennas": tx, "num_rx_antennas": rx, "links": links,
        "complex_coefficients": links * nsc * rx * tx, "cache_cfr_bytes": 16 * links * nsc * rx * tx,
        "cache_frequency_bytes": 4 * links * nsc,
        "response_bytes": len(fixture.SerializeToString(deterministic=True)),
        "request_bytes": 4, "coefficient_digest": coefficient_digest(fixture),
        "cache_hit_count": (5 if copy_profile else 1) * (
            CONTROLS["warmup"] + CONTROLS["hit_iterations"]) + 1,
        "cache_miss_count": 2 + storage_steps, "expected_channel_requests": 27 + storage_steps, "verified_links": links,
        "boundary_hit": 1, "expired_miss": 1}
    if copy_profile:
        expected.update(copy_profile=1, verified_csi_links=links)
    for key, value in expected.items():
        if record[key] != value:
            raise AssertionError(f"Incorrect native {key}: {record[key]} != {value}")
    hits = expected["cache_hit_count"]
    np.testing.assert_allclose(record["cache_hit_ratio"], hits / (hits + 2 + storage_steps), rtol=0, atol=1e-12)
    getters = 5 if copy_profile else 4
    np.testing.assert_allclose(record["memory_validation_hit_ratio"], getters * links / (getters * links + 1),
                               rtol=0, atol=1e-12)
    expected_requests = [{"tx_node": 0, "rx_node": 1, "time_ns": 0}] * 26 + [
                         {"tx_node": 0, "rx_node": 1, "time_ns": 5000001}]
    expected_requests.extend({"tx_node": 0, "rx_node": 1, "time_ns": 5000001 * (step + 2)}
                             for step in range(storage_steps))
    if storage_steps:
        assert record["storage_steps"] == storage_steps
        assert record["storage_snapshot_budget"] == record["storage_peak_snapshots"] == links
        assert record["storage_payload_budget_bytes"] == record["storage_peak_payload_bytes"] == (
            expected["cache_cfr_bytes"] + expected["cache_frequency_bytes"])
    assert server["channel_requests"] == 27 + storage_steps and server["close_requests"] == 1
    assert server["requests"] == expected_requests
    operations = {"ipc_roundtrip": 16, "cpp_serialize": 16, "cpp_parse": 16, "matrix_decode": 16,
                  "cache_hit": 128, "cache_miss": 5, "python_serialize": 16, "python_parse": 16}
    if copy_profile:
        operations.update({operation: CONTROLS["hit_iterations"] for operation in COPY_PROFILE_OPERATIONS})
    assert set(row["operation"] for row in samples) == set(operations)
    for operation, count in operations.items():
        rows = [row for row in samples if row["operation"] == operation]
        assert [row["iteration"] for row in rows] == list(range(count))
        assert all(np.isfinite(row["nanoseconds"]) and row["nanoseconds"] > 0 for row in rows)
    for key, value in record.items():
        if isinstance(value, (float, int)):
            assert np.isfinite(value), key
            if "rss" in key or ("heap" in key and "delta" not in key):
                assert value > 0, key
    assert server["peak_rss_bytes"] > 0
    assert record["peak_rss_bytes"] >= record["rss_with_cache_bytes"]


def python_timings(fixture, wire):
    samples = []
    for operation in ("python_serialize", "python_parse"):
        for index in range(CONTROLS["warmup"] + CONTROLS["iterations"]):
            start = time.perf_counter_ns()
            value = (fixture.SerializeToString(deterministic=True) if operation == "python_serialize"
                     else base.message_pb2.Wrapper.FromString(wire))
            elapsed = time.perf_counter_ns() - start
            del value  # Cleanup outside the Python timer; unlike native scoped cleanup.
            if index >= CONTROLS["warmup"]:
                samples.append({"operation": operation, "iteration": index - CONTROLS["warmup"],
                                "nanoseconds": elapsed})
    return samples


def measure(binary, fixture, storage_steps=0, copy_profile=False, workload=None):
    """One fresh native process and one fresh Python REP peer, both bounded/cleaned."""
    wire = fixture.SerializeToString(deterministic=True)
    with tempfile.TemporaryDirectory(prefix="cache-ipc-") as temporary:
        path = Path(temporary)
        input_path = path / "response.pb"
        input_path.write_bytes(wire)
        endpoint = "ipc://" + str(path / "peer.sock")
        ready, stats = path / "ready", path / "server.json"
        with (path / "peer.log").open("w+") as log:
            peer_options = [f"--workload-depth={workload['depth']}"] if workload else []
            process = subprocess.Popen([sys.executable, str(Path(__file__)), "--serve", *peer_options,
                "--input=" + str(input_path), "--endpoint=" + endpoint,
                "--ready=" + str(ready), "--stats=" + str(stats)], stdout=log, stderr=log)
            try:
                deadline = time.monotonic() + 20
                while not ready.exists():
                    if process.poll() is not None or time.monotonic() > deadline:
                        log.seek(0)
                        raise RuntimeError("Frozen REP peer failed to become ready: " + log.read())
                    time.sleep(0.01)
                started = time.perf_counter()
                optional = {"copyProfile": 1} if copy_profile else {}
                if workload:
                    optional.update(workloadSteps=workload["steps"], lookAhead=workload["depth"],
                                    cacheEnabled=int(workload["enabled"]), trajectory=path / "trajectory.csv")
                base.native(binary, input=input_path, output=path / "native.json",
                    timings=path / "timings.csv", endpoint=endpoint,
                    links=len(fixture.channel_state_response.csi[0].rx_nodes),
                    iterations=CONTROLS["iterations"], hitIterations=CONTROLS["hit_iterations"],
                    missIterations=CONTROLS["miss_iterations"], warmup=CONTROLS["warmup"], storageSteps=storage_steps,
                    subprocess_timeout=60 + workload["steps"] if workload else 60,
                    **optional)
                native_seconds = time.perf_counter() - started
                process.wait(timeout=10)
                if process.returncode:
                    log.seek(0)
                    raise RuntimeError("Frozen REP peer failed: " + log.read())
                record = json.loads((path / "native.json").read_text())
                server = json.loads(stats.read_text())
                with (path / "timings.csv").open(newline="") as stream:
                    samples = [{"operation": row["operation"], "iteration": int(row["iteration"]),
                                "nanoseconds": int(row["nanoseconds"])} for row in csv.DictReader(stream)]
                if workload:
                    with (path / "trajectory.csv").open(newline="") as stream:
                        trace = [{key: value if key == "coefficient_digest" else int(value)
                                  for key, value in row.items()} for row in csv.DictReader(stream)]
                    validate_workload(record, fixture, server, samples, trace, **workload)
            except Exception as error:
                active_run = base.provenance.ACTIVE_RUN.get()
                if active_run:
                    failure = active_run.path.parent / "failed_process"
                    failure.mkdir(exist_ok=True)
                    log.flush()
                    for artifact in path.iterdir():
                        if artifact.is_file() and artifact.name != "ready":
                            shutil.copyfile(artifact, failure / artifact.name)
                log.seek(0)
                error.add_note("Frozen REP peer log:\n" + log.read())
                raise
            finally:
                if process.poll() is None:
                    process.terminate()
                    try:
                        process.wait(timeout=5)
                    except subprocess.TimeoutExpired:
                        process.kill()
                        process.wait()
    if not workload:
        samples.extend(python_timings(fixture, wire))
        validate_record(record, fixture, server, samples, storage_steps, copy_profile)
    for operation in sorted({row["operation"] for row in samples}):
        values = [row["nanoseconds"] for row in samples if row["operation"] == operation]
        for statistic, value in (("median", np.median(values)), ("p95", np.percentile(values, 95)),
                                 ("min", min(values)), ("max", max(values))):
            record[f"{operation}_{statistic}_ns"] = float(value)
    record.update(native_process_seconds=native_seconds, server_channel_requests=server["channel_requests"],
                  server_close_requests=server["close_requests"], rep_server_peak_rss_bytes=server["peak_rss_bytes"],
                  rep_server_lifetime_peak_rss_bytes=server["lifetime_peak_rss_bytes"])
    return (record, samples, server, trace) if workload else (record, samples, server)


def plots(output, summaries):
    import matplotlib
    matplotlib.use("Agg")
    import matplotlib.pyplot as plt
    figures = {
        "memory_comparison.png": ("response_bytes", "cache_cfr_bytes", "cache_frequency_bytes",
                                   "cache_live_heap_delta_bytes", "rss_with_cache_bytes"),
        "cache_comparison.png": ("cache_hit_median_ns", "cache_miss_median_ns"),
        "ipc_comparison.png": ("ipc_roundtrip_median_ns", "cpp_serialize_median_ns", "cpp_parse_median_ns",
                                "python_serialize_median_ns", "python_parse_median_ns"),
    }
    for filename, keys in figures.items():
        fig, axes = plt.subplots(len(SUBCARRIERS), len(keys), figsize=(4 * len(keys), 10), squeeze=False)
        for row_axes, nsc in zip(axes, SUBCARRIERS):
            for axis, key in zip(row_axes, keys):
                for tx, rx in ANTENNAS:
                    groups = [[row[key] for row in summaries if row["subcarriers"] == nsc
                               and row["num_tx_antennas"] == tx and row["num_rx_antennas"] == rx
                               and row["links"] == links] for links in LINKS]
                    scale = 1e-6 if key.endswith("_ns") else 1 / 2 ** 20
                    line, = axis.plot(LINKS, [np.median(group) * scale for group in groups], marker="o",
                                      label=f"TX {tx}, RX {rx}")
                    axis.fill_between(LINKS, [min(group) * scale for group in groups],
                                      [max(group) * scale for group in groups], color=line.get_color(), alpha=0.12)
                axis.set(xscale="log", xlabel=f"Cached links; Nsc={nsc}",
                         ylabel=key + (" (ms)" if key.endswith("_ns") else " (MiB)"))
                axis.grid(alpha=0.25)
        axes[0, 0].legend(fontsize=8)
        fig.suptitle("Frozen channels, local cross-process IPC; median and range over three native processes")
        fig.tight_layout()
        fig.savefig(output / filename, dpi=130)
        plt.close(fig)


@base.provenance.benchmark_run(lambda a: plan(a["copy_profile"]), inputs=(ANCHOR,))
def run(output, binary, copy_profile=False):
    base.provenance.backend_selected("frozen-protobuf-local-zmq")
    policy = {**plan(copy_profile), "defined_before_measurements_utc": datetime.now(timezone.utc).isoformat()}
    (output / "plan.json").write_text(json.dumps(policy, indent=2) + "\n")
    started = time.perf_counter()
    summaries, timings, requests, configurations = [], [], [], []
    (output / "fixtures").mkdir()
    cases = COPY_PROFILE_CASES if copy_profile else itertools.product(SUBCARRIERS, ANTENNAS, LINKS)
    for index, (nsc, antennas, links) in enumerate(cases):
        tx, rx = antennas
        case_id = f"case_{index:02d}"
        fixture_path = output / "fixtures" / f"nsc{nsc}_tx{tx}_rx{rx}.pb"
        if not fixture_path.exists():
            fixture_path.write_bytes(template(nsc, tx, rx).SerializeToString(deterministic=True))
        single = base.message_pb2.Wrapper.FromString(fixture_path.read_bytes())
        frozen = batch(single, links)
        wire = frozen.SerializeToString(deterministic=True)
        record = {"case_id": case_id, "subcarriers": nsc, "num_tx_antennas": tx,
                  "num_rx_antennas": rx, "links": links,
                  "template": str(fixture_path.relative_to(output)), "template_sha256": base.sha256(fixture_path),
                  "response_sha256": hashlib.sha256(wire).hexdigest(),
                  "coefficient_digest": coefficient_digest(frozen)}
        first = None
        for repetition in range(CONTROLS["repetitions"]):
            result, samples, server = measure(binary, frozen, copy_profile=copy_profile)
            identity = {"case_id": case_id, "repetition": repetition}
            if first is None:
                first = deterministic_fields(result)
            else:
                assert deterministic_fields(result) == first, "Deterministic native results changed between processes"
            summaries.append({**identity, **result})
            timings.extend({**identity, **row} for row in samples)
            requests.extend({**identity, "request_index": i, **request}
                            for i, request in enumerate(server["requests"]))
        configurations.append(record)
        print(f"{case_id} Nsc={nsc} TX={tx} RX={rx} links={links} bytes={len(wire)} "
              f"hit_us={result['cache_hit_median_ns']/1000:.3f} "
              f"miss_ms={result['cache_miss_median_ns']/1e6:.3f} validation=passed", flush=True)
    elapsed = time.perf_counter() - started
    for filename, rows in (("summary.csv", summaries), ("timings.csv", timings), ("requests.csv", requests)):
        base.write_csv(output / filename, rows)
    if not copy_profile:
        plots(output, summaries)
    paths = [Path(__file__), Path(__file__).with_name("test_benchmark_cache_ipc.py"), Path(base.__file__),
             base.MODULE / "examples/ns3_sionna_cache_ipc_benchmark.cc", base.MODULE / "examples/CMakeLists.txt",
             base.MODULE / "model/message.proto", base.MODULE / "model/mimo-channel-matrix.cc",
             base.MODULE / "model/mimo-channel-matrix.h", base.MODULE / "model/sionna-propagation-cache.cc",
             base.MODULE / "model/sionna-propagation-cache.h", base.MODULE / "helper/sionna-helper.cc",
             base.MODULE / "helper/sionna-helper.h", base.MODULE / "model/sionna-mobility-model.cc"]
    manifest = {**policy, "plan_sha256": base.sha256(output / "plan.json"), "configurations": configurations,
        "provenance": {"platform": platform.platform(), "python": platform.python_version(),
            "libc": platform.libc_ver(), "pyzmq_libzmq": zmq.zmq_version(),
            "versions": {name: importlib.metadata.version(name) for name in ("numpy", "protobuf", "pyzmq")},
            "native_executable_sha256": base.sha256(binary),
            "source_sha256": {str(path.relative_to(base.MODULE)): base.sha256(path) for path in paths}},
        "validation": {"status": "passed", "repeatability": "passed", "bit_exact_cache_and_raw_ipc": True,
                       "expiry": "passed", "cases": len(configurations), "native_processes": len(summaries),
                       "timing_samples": len(timings), "channel_requests": len(requests)},
        "sweep_seconds": elapsed}
    (output / "manifest.json").write_text(json.dumps(manifest, indent=2) + "\n")
    return manifest


@base.provenance.benchmark_run(lambda a: {"storage_steps": a["steps"], "subcarriers": 64,
    "tx_antennas": 2, "rx_antennas": 4, "links": 4, "rt": False}, inputs=(ANCHOR,))
def run_storage(output, binary, steps):
    """Small bounded advancing-time validation; reuse the frozen IPC peer."""
    if not 1 <= steps <= 128:
        raise ValueError("storage steps must be in [1, 128]")
    base.provenance.backend_selected("frozen-protobuf-local-zmq")
    (output / "plan.json").write_text(json.dumps({"storage_steps": steps, "links": 4,
        "subcarriers": 64, "tx_antennas": 2, "rx_antennas": 4,
        "budget": "exact one-batch logical payload and snapshots", "rt": False}, indent=2) + "\n")
    record, samples, server = measure(binary, batch(template(64, 2, 4), 4), steps)
    result = {"schema_version": 1, "subphase": "r3-storage-budget",
              "configurations": [{"storage_steps": steps, "links": 4, "subcarriers": 64,
                                  "tx_antennas": 2, "rx_antennas": 4}],
              "validation": {"status": "passed"}, "record": record, "server": server}
    (output / "manifest.json").write_text(json.dumps(result, indent=2) + "\n")
    base.write_csv(output / "timings.csv", samples)
    return result


@base.provenance.benchmark_run(lambda a: workload_plan(a["steps"]), inputs=(ANCHOR,))
def run_workload(output, binary, steps=128):
    """Extend the existing IPC runner with bounded evolving cache working sets."""
    base.provenance.backend_selected("frozen-protobuf-local-zmq")
    policy = {**workload_plan(steps), "defined_before_measurements_utc": datetime.now(timezone.utc).isoformat()}
    (output / "plan.json").write_text(json.dumps(policy, indent=2) + "\n")
    (output / "fixtures").mkdir()
    fixture_path = output / "fixtures/nsc64_tx2_rx2.pb"
    fixture_path.write_bytes(template(64, 2, 2).SerializeToString(deterministic=True))
    single = base.message_pb2.Wrapper.FromString(fixture_path.read_bytes())
    summaries, timings, requests, trajectories, configurations = [], [], [], [], []
    for index, (links, depth, enabled) in enumerate(itertools.product(LINKS, WORKLOAD_DEPTHS, (True, False))):
        case_id = f"case_{index:02d}"
        fixture = batch(single, links)
        configuration = {"case_id": case_id, "links": links, "depth": depth, "enabled": enabled,
            "steps": steps, "template": str(fixture_path.relative_to(output)),
            "template_sha256": base.sha256(fixture_path),
            "response_sha256": hashlib.sha256(fixture.SerializeToString(deterministic=True)).hexdigest()}
        first = None
        for repetition in range(3):
            result, samples, server, trace = measure(binary, fixture,
                workload={"steps": steps, "depth": depth, "enabled": enabled})
            deterministic = (deterministic_fields(result), deterministic_trajectory(trace))
            if first is None:
                first = deterministic
            else:
                assert deterministic == first, "Workload deterministic values changed between processes"
            identity = {"case_id": case_id, "repetition": repetition}
            summaries.append({**identity, **result})
            timings.extend({**identity, **row} for row in samples)
            trajectories.extend({**identity, **row} for row in trace)
            requests.extend({**identity, "request_index": i, **request}
                            for i, request in enumerate(server["requests"]))
        configurations.append(configuration)
        print(f"{case_id} links={links} depth={depth} cache={enabled} steps={steps} "
              f"requests={result['cache_miss_count']} peak_payload={result['peak_payload_bytes']} "
              "validation=passed", flush=True)
    for filename, rows in (("summary.csv", summaries), ("timings.csv", timings),
                           ("requests.csv", requests), ("trajectory.csv", trajectories)):
        base.write_csv(output / filename, rows)
    manifest = {**policy, "plan_sha256": base.sha256(output / "plan.json"),
        "configurations": configurations, "validation": {"status": "passed", "cases": len(configurations),
            "native_processes": len(summaries), "timing_samples": len(timings),
            "channel_requests": len(requests), "trajectory_rows": len(trajectories),
            "repeatability": "passed", "bounded_storage": "passed", "expiry": "passed"}}
    (output / "manifest.json").write_text(json.dumps(manifest, indent=2) + "\n")
    return manifest


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--output", type=Path)
    parser.add_argument("--executable", type=Path)
    parser.add_argument("--storage-steps", type=int, default=0)
    parser.add_argument("--copy-profile", action="store_true",
        help="Profile five public hit getters for two fixed shapes, three fresh processes each")
    parser.add_argument("--workload-sweep", action="store_true",
        help="Sweep cache on/off, evolving active links and look-ahead 1/4/16")
    parser.add_argument("--workload-steps", type=int, default=128,
        help="Workload epochs: multiple of 16 in [16, 4096] (default: 128)")
    parser.add_argument("--workload-depth", type=int, default=0, choices=(0, *WORKLOAD_DEPTHS),
        help=argparse.SUPPRESS)
    parser.add_argument("--serve", action="store_true")
    parser.add_argument("--input", type=Path)
    parser.add_argument("--endpoint")
    parser.add_argument("--ready", type=Path)
    parser.add_argument("--stats", type=Path)
    args = parser.parse_args()
    if args.copy_profile and (args.storage_steps or args.serve):
        parser.error("--copy-profile cannot be combined with --storage-steps or --serve")
    if args.workload_sweep and (args.copy_profile or args.storage_steps or args.serve):
        parser.error("--workload-sweep cannot be combined with --copy-profile, --storage-steps or --serve")
    if args.workload_depth and not args.serve:
        parser.error("--workload-depth is only for the internal REP peer")
    if args.workload_sweep:
        try:
            workload_plan(args.workload_steps)
        except ValueError as error:
            parser.error(str(error))
    if args.serve:
        serve(args.input, args.endpoint, args.ready, args.stats, args.workload_depth)
    elif args.output:
        binary = (args.executable or executable()).resolve()
        manifest = (run_workload(args.output.resolve(), binary, args.workload_steps) if args.workload_sweep else
                    run_storage(args.output.resolve(), binary, args.storage_steps) if args.storage_steps
                    else run(args.output.resolve(), binary, copy_profile=args.copy_profile))
        print(json.dumps(manifest["validation"]))
    else:
        parser.error("Provide --output=<empty directory>")


if __name__ == "__main__":
    main()
