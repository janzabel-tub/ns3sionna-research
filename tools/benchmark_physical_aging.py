#!/usr/bin/env python3
"""R5: paired physical sounding/transmission CFRs, with native SVD/MMSE.

Position retracing and local constant-path Doppler are distinct models. The
production server is used for position evolution; Doppler uses installed
Sionna RT Paths.cfr explicitly, without enabling the unfinished server branch.
"""
from __future__ import annotations

import argparse
import csv
import itertools
import json
from pathlib import Path
import tempfile
import time

import numpy as np
import benchmark_sumimo as base
from mimo_data_contract import append_cfr_to_message, normalize_link_cfr

CASES = (("case_00", 90.0, 90.0), ("case_01", 90.0, 0.0))
SEEDS = (42, 43, 44)
MODES = ("position", "doppler")
SPEEDS = (0.0, 1.0)
AGES_MS = (0, 1, 5, 20, 50)
TRANSMISSION_MS = 50
TIMES_MS = tuple(sorted({TRANSMISSION_MS - age for age in AGES_MS}))


def plan(samples=100000, repetitions=2):
    if type(samples) is not int or samples <= 0:
        raise ValueError("Samples must be a positive integer")
    if type(repetitions) is not int or repetitions < 2:
        raise ValueError("At least two fresh repetitions are required")
    return {"schema_version": 1, "subphase": "R5",
        "controls": {**{k: v for k, v in base.CONTROLS.items()
                        if k not in ("seed_ns3", "seed_sionna", "csi_time_ns")},
                     "samples_per_src": samples, "seed_contract": "paired seeds from sweep; run=1, streams=0/1",
                     "csi_time_ns": "exact per-snapshot sounding/transmission times",
                     "precoding": "SVD_equal_power_unquantized", "perfect_rx_csi": True},
        "sweep": {"cases": CASES, "seeds": SEEDS, "modes": MODES, "speeds_m_s": SPEEDS,
                  "ages_ms": AGES_MS, "repetitions": repetitions},
        "transmission_ms": TRANSMISSION_MS,
        "velocity_m_s": "RX=(speed,0,0); TX and scene objects stationary",
        "doppler": "Paths.cfr at 1 kHz, constant time-zero paths, amplitudes and delays",
        "position": "production compute_cfr at actual linear trajectory positions",
        "reference_tolerances": {"rtol": 1e-8, "atol": 1e-8,
                                 "doppler_rtol": 2e-4, "doppler_atol": 2e-9},
        "fixed_acceptance": samples == 100000 and repetitions == 2,
        "scope": "physical local CPU CSI aging diagnostics; no sample convergence, PER, "
                 "throughput, automatic sounding, server Doppler dispatch or GPU claim"}


def moving_configuration(config, speed):
    result = base.message_pb2.SimInitMessage()
    result.CopyFrom(config)
    node = result.nodes[1]
    position = tuple(base.POSITIONS[1])
    node.ClearField("constant_position_model")
    walk = node.random_walk_model
    walk.position.x, walk.position.y, walk.position.z = position
    walk.time_value = 1000000000
    walk.speed.constant.value = speed
    walk.direction.constant.value = 0.0
    return result


def check_snapshot(response):
    if len(response.csi) != 1:
        raise ValueError("Expected one physical snapshot")
    state = response.csi[0]
    if (not state.HasField("tx_node") or state.tx_node.id != 0 or
            len(state.rx_nodes) != 1 or state.rx_nodes[0].id != 1 or
            state.start_time > state.end_time or state.end_time > 2**63 - 1):
        raise ValueError("Invalid directed physical snapshot/time")
    rx = state.rx_nodes[0]
    if (rx.num_tx_antennas, rx.num_rx_antennas, len(rx.frequencies),
            len(rx.csi_real), len(rx.csi_imag)) != (2, 2, 256, 1024, 1024):
        raise ValueError("Physical snapshot requires 256-tone 2x2 CFR")
    for node in (state.tx_node, rx):
        if not node.HasField("position") or not np.all(np.isfinite(
                [node.position.x, node.position.y, node.position.z])):
            raise ValueError("Invalid physical position")
    h = base.channel_array(response)
    if (not np.all(np.isfinite(h)) or not np.isfinite(rx.wb_loss) or
            not np.all(np.diff(rx.frequencies) > 0) or
            abs(np.mean(abs(h)**2) - 1) > 1e-5):
        raise ValueError("Invalid CFR, frequency grid, loss or normalization")
    return state, rx, h


def validate_reference(transmission, sounding, rows):
    """Independent SVD/MMSE/current-power, timestamp and diagnostic reference."""
    tx, rx, h = check_snapshot(transmission)
    csi, old_rx, estimate = check_snapshot(sounding)
    if csi.start_time > tx.start_time or list(rx.frequencies) != list(old_rx.frequencies):
        raise ValueError("Incompatible sounding/transmission pair")
    if len(rows) != 256:
        raise ValueError("Expected 256 native tone rows")
    def close(keys, expected, rtol=1e-8, atol=1e-8):
        actual = [[row[key] for key in keys] for row in rows]
        np.testing.assert_allclose(actual, np.broadcast_to(expected, np.shape(actual)),
                                   rtol=rtol, atol=atol)
    noise = 1.3803e-23 * 290 * 20e6 * 10**0.7
    power = 10**((20 - rx.wb_loss - 30)/10)
    rho = power / noise
    _, _, vh = np.linalg.svd(estimate)
    w = vh.conj().transpose(0, 2, 1)
    effective_h = h @ w
    error = np.linalg.inv(np.eye(2) + rho/2 * (
        effective_h.conj().transpose(0, 2, 1) @ effective_h))
    sinr = 1 / np.diagonal(error, axis1=1, axis2=2).real - 1
    singular = np.linalg.svd(h, compute_uv=False)
    close(("sinr0_linear", "sinr1_linear"), sinr)
    close(("sigma0", "sigma1"), singular, 1e-10, 1e-10)
    close(("effective_sinr0_linear", "effective_sinr1_linear"),
          np.expm1(np.mean(np.log1p(sinr), axis=0)))
    close(("spectral_efficiency_bps_hz",), [np.mean(np.sum(np.log2(1+sinr), axis=1))])
    _, logdet = np.linalg.slogdet(np.eye(2) + rho/2 * (h.conj().transpose(0, 2, 1) @ h))
    bound = float(np.mean(logdet)/np.log(2))
    if rows[0]["spectral_efficiency_bps_hz"] > bound+1e-7:
        raise ValueError("Linear-MMSE MI exceeds the equal-power logdet bound")
    if np.array_equal(h, estimate):
        np.testing.assert_allclose(rows[0]["spectral_efficiency_bps_hz"], bound, rtol=1e-9, atol=1e-8)
    close(("wideband_loss_db", "noise_power_w", "received_power_w", "reference_snr_linear"),
          [rx.wb_loss, noise, power, rho], 1e-12, 0)
    close(("initial_time_ns", "csi_time_ns", "transmission_time_ns", "csi_age_ns", "feedback_delay_ns",
           "update_interval_ns", "rotation_rate_deg_per_ms", "precoder_power"),
          [csi.start_time, csi.start_time, tx.start_time, tx.start_time-csi.start_time,
           tx.start_time-csi.start_time, 0, 0, 2], 0, 1e-10)
    norm = np.sum(abs(h)**2, axis=(1, 2))
    old_norm = np.sum(abs(estimate)**2, axis=(1, 2))
    correlation = np.divide(abs(np.sum(estimate.conj()*h, axis=(1, 2))),
        np.sqrt(norm*old_norm), out=(norm == old_norm).astype(float), where=(norm*old_norm > 0))
    change = np.sqrt(np.divide(np.sum(abs(h-estimate)**2, axis=(1, 2)), norm,
                              out=np.zeros_like(norm), where=(norm > 0)))
    close(("channel_correlation",), correlation[:, None], 1e-10, 1e-10)
    close(("channel_relative_change",), change[:, None], 1e-10, 1e-10)
    np.testing.assert_array_equal([r["subcarrier"] for r in rows], np.arange(256))
    np.testing.assert_array_equal([r["frequency_offset_hz"] for r in rows], rx.frequencies)
    np.testing.assert_array_equal([r["rank"] for r in rows],
                                 np.sum(singular > singular[:, :1]*1e-12, axis=1))


def doppler_reference(a, tau, doppler, frequencies, timestamp_ms):
    """Independent path sum with Sionna's normalized-delay convention."""
    tau = np.where(tau < 0, 0, tau)
    phase = np.exp(2j*np.pi*doppler*(timestamp_ms/1000))
    fourier = np.exp(-2j*np.pi*tau[..., None]*frequencies)
    return np.sum((a*phase)[..., None]*fourier, axis=-2)[0, :, 0, :, :]


def doppler_response(env, raw, timestamp_ms, speed, tau):
    result = base.message_pb2.ChannelStateResponse()
    state = result.csi.add()
    state.start_time = timestamp_ms*1000000
    state.end_time = state.start_time + 5000000
    state.tx_node.id = 0
    state.tx_node.position.x, state.tx_node.position.y, state.tx_node.position.z = base.POSITIONS[0]
    rx = state.rx_nodes.add()
    rx.id = 1
    rx.position.x, rx.position.y, rx.position.z = (
        base.POSITIONS[1][0] + speed*timestamp_ms/1000, *base.POSITIONS[1][1:])
    rx.end_time2 = 5000000
    valid = tau[np.isfinite(tau) & (tau >= 0)]
    if not valid.size:
        raise ValueError("R5 Doppler study requires real valid paths")
    rx.delay = int(round(float(np.min(valid))*1e9))
    rx.wb_loss, normalized = normalize_link_cfr(raw, env.resource_limits)
    append_cfr_to_message(rx, env.frequencies, normalized, env.resource_limits)
    check_snapshot(result)
    return result


def trace(config, env_class, mode, speed, samples, directory):
    """Fresh environment per trajectory/repetition, with saved physical inputs."""
    env = env_class(model_folder=str(base.SERVER / "models"), rt_fast=True,
                    rt_max_parallel_links=1, VERBOSE=False)
    env.rt_samples_per_src = samples
    started = time.perf_counter()
    try:
        success, error = env.init_simulation_env(config)
        if not success:
            raise RuntimeError(error)
        env._place_tx_rx_node(0, [1])
        base.observed_geometry(env, config)
        np.testing.assert_allclose(env.node_info[1].velocity, [speed, 0, 0], atol=1e-12, rtol=0)
        result = {}
        if mode == "position":
            for timestamp in TIMES_MS:
                request = base.message_pb2.ChannelStateRequest(
                    tx_node=0, rx_node=1, time=timestamp*1000000)
                wrapper = base.message_pb2.Wrapper()
                env.compute_cfr(request, wrapper)
                response = base.message_pb2.ChannelStateResponse()
                response.CopyFrom(wrapper.channel_state_response)
                if response.csi[0].rx_nodes[0].wb_loss >= 300:
                    raise ValueError("No-path fallback cannot count as physical acceptance")
                check_snapshot(response)
                result[timestamp] = response
        elif mode == "doppler":
            from sionna.rt import PathSolver
            env._place_tx_rx_node(0, [1])
            env.scene.get("rx1").velocity = [speed, 0, 0]
            options = {name: getattr(env, "rt_"+name) for name in (
                "max_depth", "samples_per_src", "los", "specular_reflection",
                "diffuse_reflection", "refraction", "synthetic_array", "diffraction",
                "edge_diffraction", "diffraction_lit_region")}
            paths = PathSolver(deterministic=True)(scene=env.scene, seed=env.my_seed, **options)
            raw = paths.cfr(frequencies=env.frequencies, sampling_frequency=1000,
                num_time_steps=TRANSMISSION_MS+1, normalize_delays=True,
                normalize=False, out_type="numpy")
            a, tau = paths.cir(normalize_delays=True, out_type="numpy")
            a = a[..., 0]
            doppler = np.asarray(paths.doppler)
            if speed == 0:
                np.testing.assert_array_equal(doppler, np.zeros_like(doppler))
            elif not np.any(abs(doppler) > 1e-5):
                raise ValueError("Moving receiver must produce physical path Doppler")
            if not np.all(np.isfinite(doppler)) or np.max(abs(doppler)) > speed*env.fc/299792458.0 + 1e-3:
                raise ValueError("Path Doppler exceeds stationary-scene velocity bound")
            physical_tau = np.asarray(paths.tau)
            np.savez(directory / "paths.npz", a=a, tau=tau, doppler=doppler,
                     physical_tau=physical_tau, frequencies=np.asarray(env.frequencies))
            for timestamp in TIMES_MS:
                channel = raw[0, :, 0, :, timestamp, :]
                reference = doppler_reference(a, tau, doppler, np.asarray(env.frequencies), timestamp)
                np.testing.assert_allclose(channel, reference, rtol=2e-4, atol=2e-9)
                result[timestamp] = doppler_response(env, channel, timestamp, speed, physical_tau)
        else:
            raise ValueError("Unknown physical evolution mode")
        for timestamp, response in result.items():
            state = response.csi[0]
            assert state.start_time == timestamp*1000000
            np.testing.assert_allclose([state.rx_nodes[0].position.x,
                state.rx_nodes[0].position.y, state.rx_nodes[0].position.z],
                np.array(base.POSITIONS[1])+[speed*timestamp/1000, 0, 0], rtol=0, atol=1e-7)
            (directory / f"snapshot_{timestamp:02d}.pb").write_bytes(
                response.SerializeToString(deterministic=True))
        return result, time.perf_counter()-started
    finally:
        env.release()


def analyze(binary, transmission, sounding, output):
    base.native(binary, action="physical-aging", input=transmission,
                csiInput=sounding, output=output)
    return base.read_metrics(output)


def deterministic_rows(rows):
    return [{k: v for k, v in row.items() if k != "analysis_seconds"} for row in rows]


@base.provenance.benchmark_run(lambda a: plan(a["samples"], a["repetitions"]))
def run(output, binary, samples=100000, repetitions=2):
    output, binary = Path(output).resolve(), Path(binary).resolve()
    policy = plan(samples, repetitions)
    base.provenance.atomic_json(output / "plan.json", policy)
    env_class, _ = base.runtime()
    configurations, evaluations, summaries = [], [], []
    for case_id, tx_angle, rx_angle in CASES:
        for seed in SEEDS:
            case_dir = output / case_id / f"seed_{seed}"
            case_dir.mkdir(parents=True)
            config_path = case_dir / "configuration.pb"
            config = base.export_configuration(binary, config_path, 0.5,
                (tx_angle, 0, 0), (rx_angle, 0, 0), seed=seed)
            configurations.append({"case_id": case_id, "seed": seed,
                "configuration_sha256": base.sha256(config_path)})
            for mode, speed in itertools.product(MODES, SPEEDS):
                previous = None
                for repetition in range(repetitions):
                    directory = case_dir / f"{mode}_speed_{speed:g}" / f"repeat_{repetition}"
                    directory.mkdir(parents=True)
                    moving = moving_configuration(config, speed)
                    (directory / "configuration.pb").write_bytes(moving.SerializeToString(deterministic=True))
                    channels, seconds = trace(moving, env_class, mode, speed, samples, directory)
                    tx_path = directory / f"snapshot_{TRANSMISSION_MS:02d}.pb"
                    metrics = {}
                    for age in AGES_MS:
                        sounding_path = directory / f"snapshot_{TRANSMISSION_MS-age:02d}.pb"
                        path = directory / f"age_{age:02d}.csv"
                        rows = analyze(binary, tx_path, sounding_path, path)
                        validate_reference(channels[TRANSMISSION_MS], channels[TRANSMISSION_MS-age], rows)
                        metrics[age] = rows
                        identity = {"case_id": case_id, "seed": seed, "mode": mode,
                            "speed_m_s": speed, "repetition": repetition, "age_ms": age}
                        record = {**identity, "transmission": str(tx_path.relative_to(output)),
                            "sounding": str(sounding_path.relative_to(output)),
                            "metrics": str(path.relative_to(output)),
                            "transmission_sha256": base.sha256(tx_path),
                            "sounding_sha256": base.sha256(sounding_path),
                            "metrics_sha256": base.sha256(path)}
                        evaluations.append(record)
                        first, fresh = rows[0], metrics[0][0]
                        loss = fresh["spectral_efficiency_bps_hz"]-first["spectral_efficiency_bps_hz"]
                        assert loss >= -1e-7, "Delayed MMSE MI exceeds fresh SVD bound"
                        if speed == 0 or age == 0:
                            np.testing.assert_allclose(
                                [r["sinr0_linear"] for r in rows],
                                [r["sinr0_linear"] for r in metrics[0]], rtol=1e-8, atol=1e-8)
                            np.testing.assert_allclose(
                                [r["sinr1_linear"] for r in rows],
                                [r["sinr1_linear"] for r in metrics[0]], rtol=1e-8, atol=1e-8)
                        summaries.append({**identity, "wideband_loss_db": first["wideband_loss_db"],
                            "sounding_loss_db": channels[TRANSMISSION_MS-age].csi[0].rx_nodes[0].wb_loss,
                            "median_channel_correlation": float(np.median([r["channel_correlation"] for r in rows])),
                            "median_relative_change": float(np.median([r["channel_relative_change"] for r in rows])),
                            "effective_sinr0_db": float(10*np.log10(first["effective_sinr0_linear"])),
                            "effective_sinr1_db": float(10*np.log10(first["effective_sinr1_linear"])),
                            "spectral_efficiency_bps_hz": first["spectral_efficiency_bps_hz"],
                            "mi_loss_bps_hz": loss, "trace_seconds": seconds,
                            "analysis_seconds": first["analysis_seconds"]})
                    if speed > 0:
                        assert max(r["channel_relative_change"] for r in metrics[50]) > 1e-5
                    if previous:
                        old_channels, old_metrics = previous
                        for timestamp in TIMES_MS:
                            assert channels[timestamp] == old_channels[timestamp], "RT repetition differs"
                        for age in AGES_MS:
                            assert deterministic_rows(metrics[age]) == deterministic_rows(old_metrics[age])
                    previous = channels, metrics
                print(f"{case_id} seed={seed} mode={mode} speed={speed:g}: passed", flush=True)
    base.write_csv(output / "summary.csv", summaries)
    manifest = {**policy, "configurations": configurations, "evaluations": evaluations,
        "plan_sha256": base.sha256(output / "plan.json"),
        "validation": {"status": "passed", "evaluations": len(evaluations),
                       "snapshots": len(evaluations), "repetitions": "byte-identical",
                       "controls": "stationary/zero-age/native-reference/path-sum passed"}}
    base.provenance.atomic_json(output / "manifest.json", manifest)
    validate_export(output)
    plots(output, summaries)
    return manifest


def validate_export(output, binary=None):
    """Audit a complete export without trusting its claimed scientific verdict."""
    output = Path(output)
    manifest = json.loads((output / "manifest.json").read_text())
    policy = plan(manifest["controls"]["samples_per_src"], manifest["sweep"]["repetitions"])
    recorded = json.loads((output / "plan.json").read_text())
    if recorded != json.loads(json.dumps(policy)) or base.sha256(output / "plan.json") != manifest["plan_sha256"]:
        raise ValueError("R5 plan changed")
    for field, value in recorded.items():
        if manifest.get(field) != value:
            raise ValueError("R5 manifest policy differs from plan")
    configurations = {}
    for record in manifest["configurations"]:
        key = record["case_id"], record["seed"]
        if key in configurations or key not in set(itertools.product([c[0] for c in CASES], SEEDS)):
            raise ValueError("Invalid/duplicate configuration identity")
        path = output / key[0] / f"seed_{key[1]}" / "configuration.pb"
        if base.sha256(path) != record["configuration_sha256"]:
            raise ValueError("Configuration integrity mismatch")
        config = base.message_pb2.SimInitMessage.FromString(path.read_bytes())
        _, tx_angle, rx_angle = next(c for c in CASES if c[0] == key[0])
        base.validate_configuration(config, 0.5, (tx_angle, 0, 0), (rx_angle, 0, 0), key[1])
        configurations[key] = config
    if set(configurations) != set(itertools.product([c[0] for c in CASES], SEEDS)):
        raise ValueError("Incomplete configuration matrix")
    expected = set(itertools.product([c[0] for c in CASES], SEEDS, MODES, SPEEDS,
                                     range(policy["sweep"]["repetitions"]), AGES_MS))
    actual, groups, metric_index = set(), {}, {}
    for record in manifest["evaluations"]:
        key = tuple(record[k] for k in ("case_id", "seed", "mode", "speed_m_s", "repetition", "age_ms"))
        if key in actual or key not in expected:
            raise ValueError("Duplicate/unexpected physical evaluation")
        actual.add(key)
        paths = {}
        for name in ("transmission", "sounding", "metrics"):
            path = (output / record[name]).resolve()
            if not path.is_relative_to(output.resolve()) or base.sha256(path) != record[name+"_sha256"]:
                raise ValueError("Physical artifact integrity mismatch")
            paths[name] = path
        tx = base.message_pb2.ChannelStateResponse.FromString(paths["transmission"].read_bytes())
        csi = base.message_pb2.ChannelStateResponse.FromString(paths["sounding"].read_bytes())
        rows = base.read_metrics(paths["metrics"])
        validate_reference(tx, csi, rows)
        if (tx.csi[0].start_time, csi.csi[0].start_time) != (
                TRANSMISSION_MS*1000000, (TRANSMISSION_MS-key[-1])*1000000):
            raise ValueError("Export timestamp differs from evaluation identity")
        for response in (tx, csi):
            state = response.csi[0]
            position = state.rx_nodes[0].position
            np.testing.assert_allclose([state.tx_node.position.x, state.tx_node.position.y,
                state.tx_node.position.z], base.POSITIONS[0], atol=1e-7, rtol=0)
            np.testing.assert_array_equal(state.rx_nodes[0].frequencies, (np.arange(256)-128)*78125)
            np.testing.assert_allclose([position.x, position.y, position.z],
                np.array(base.POSITIONS[1])+[key[3]*state.start_time/1e9, 0, 0], atol=1e-7, rtol=0)
        group = groups.setdefault(key[:-1], {})
        group[key[-1]] = (tx, csi, rows)
        metric_index[key] = rows
        if binary:
            with tempfile.TemporaryDirectory(prefix="r5-replay-") as temporary:
                replay = analyze(binary, paths["transmission"], paths["sounding"], Path(temporary)/"metrics.csv")
                np.testing.assert_allclose(
                    list(map(list, [r.values() for r in deterministic_rows(replay)])),
                    list(map(list, [r.values() for r in deterministic_rows(rows)])), rtol=1e-8, atol=1e-8)
    if actual != expected:
        raise ValueError("Incomplete physical evaluation matrix")
    with (output / "summary.csv").open(newline="") as stream:
        summaries = list(csv.DictReader(stream))
    seen = set()
    for summary in summaries:
        key = (summary["case_id"], int(summary["seed"]), summary["mode"],
               float(summary["speed_m_s"]), int(summary["repetition"]), int(summary["age_ms"]))
        if key in seen or key not in expected:
            raise ValueError("Unexpected/duplicate summary identity")
        seen.add(key)
        rows = metric_index[key]
        first = rows[0]
        fresh = metric_index[(*key[:-1], 0)][0]
        csi = groups[key[:-1]][key[-1]][1]
        reference = {"wideband_loss_db": first["wideband_loss_db"],
            "sounding_loss_db": csi.csi[0].rx_nodes[0].wb_loss,
            "median_channel_correlation": np.median([r["channel_correlation"] for r in rows]),
            "median_relative_change": np.median([r["channel_relative_change"] for r in rows]),
            "effective_sinr0_db": 10*np.log10(first["effective_sinr0_linear"]),
            "effective_sinr1_db": 10*np.log10(first["effective_sinr1_linear"]),
            "spectral_efficiency_bps_hz": first["spectral_efficiency_bps_hz"],
            "mi_loss_bps_hz": fresh["spectral_efficiency_bps_hz"]-first["spectral_efficiency_bps_hz"],
            "analysis_seconds": first["analysis_seconds"]}
        for field, value in reference.items():
            np.testing.assert_allclose(float(summary[field]), value, rtol=1e-10, atol=1e-10)
        if not np.isfinite(float(summary["trace_seconds"])) or float(summary["trace_seconds"]) < 0:
            raise ValueError("Invalid trace runtime")
    if seen != expected:
        raise ValueError("Incomplete summary matrix")
    for key, ages in groups.items():
        directory = output / key[0] / f"seed_{key[1]}" / f"{key[2]}_speed_{key[3]:g}" / f"repeat_{key[4]}"
        config = base.message_pb2.SimInitMessage.FromString((directory / "configuration.pb").read_bytes())
        if config != moving_configuration(configurations[key[:2]], key[3]):
            raise ValueError("Physical trajectory configuration differs")
        if key[2] == "doppler" and not (directory / "paths.npz").is_file():
            raise ValueError("Missing physical Doppler path inputs")
        fresh = ages[0][2]
        for age, (tx, csi, rows) in ages.items():
            assert tx == ages[0][0], "Ages must share one transmission channel"
            assert rows[0]["spectral_efficiency_bps_hz"] <= fresh[0]["spectral_efficiency_bps_hz"]+1e-7
            if key[3] == 0 or age == 0:
                np.testing.assert_allclose(base.channel_array(tx), base.channel_array(csi), rtol=1e-6, atol=1e-7)
                np.testing.assert_allclose(tx.csi[0].rx_nodes[0].wb_loss, csi.csi[0].rx_nodes[0].wb_loss, atol=1e-7, rtol=0)
                np.testing.assert_allclose([[r["sinr0_linear"], r["sinr1_linear"]] for r in rows],
                    [[r["sinr0_linear"], r["sinr1_linear"]] for r in fresh], rtol=1e-8, atol=1e-8)
            if key[4] > 0:
                initial = groups[(*key[:4], 0)][age]
                assert tx == initial[0] and csi == initial[1]
                assert deterministic_rows(rows) == deterministic_rows(initial[2])
        if key[3] > 0:
            assert max(r["channel_relative_change"] for r in ages[50][2]) > 1e-5
    # Every retained path input is independently re-evaluated after export.
    for path in output.rglob("paths.npz"):
        with np.load(path, allow_pickle=False) as data:
            np.testing.assert_array_equal(data["frequencies"], (np.arange(256)-128)*78125)
            valid_delays = data["physical_tau"][(data["physical_tau"] >= 0) & np.isfinite(data["physical_tau"])]
            if not valid_delays.size:
                raise ValueError("Doppler path inputs require valid physical delays")
            expected_tau = np.where(data["physical_tau"] >= 0,
                                    data["physical_tau"]-np.min(valid_delays), -1)
            np.testing.assert_allclose(data["tau"], expected_tau, atol=1e-12, rtol=1e-6)
            speed = float(path.parent.parent.name.split("_speed_")[1])
            shifts = data["doppler"]
            if (not np.all(np.isfinite(shifts)) or np.max(abs(shifts)) > speed*5180e6/299792458.0+1e-3
                    or (speed == 0 and np.any(shifts)) or (speed > 0 and not np.any(abs(shifts) > 1e-5))):
                raise ValueError("Invalid physical Doppler velocity bound")
            for timestamp in TIMES_MS:
                raw = doppler_reference(data["a"], data["tau"], data["doppler"], data["frequencies"], timestamp)
                loss, normalized = normalize_link_cfr(raw)
                response = base.message_pb2.ChannelStateResponse.FromString((path.parent/f"snapshot_{timestamp:02d}.pb").read_bytes())
                if response.csi[0].rx_nodes[0].delay != int(round(float(np.min(valid_delays))*1e9)):
                    raise ValueError("Doppler snapshot delay differs from physical path input")
                np.testing.assert_allclose(base.channel_array(response), normalized.transpose(2, 0, 1), rtol=2e-4, atol=2e-5)
                np.testing.assert_allclose(response.csi[0].rx_nodes[0].wb_loss, loss, atol=2e-4, rtol=0)
    for key, ages in groups.items():
        if key[2] == "position":
            other = groups[(*key[:2], "doppler", *key[3:])][50][1]
            anchor = ages[50][1]
            np.testing.assert_allclose(base.channel_array(anchor), base.channel_array(other), rtol=1e-5, atol=1e-6)
            np.testing.assert_allclose(anchor.csi[0].rx_nodes[0].wb_loss, other.csi[0].rx_nodes[0].wb_loss, atol=1e-5, rtol=0)
    return {"status": "passed", "evaluations": len(actual), "trajectories": len(groups)}


def plots(output, rows):
    import matplotlib
    matplotlib.use("Agg")
    import matplotlib.pyplot as plt
    fig, axes = plt.subplots(1, 2, figsize=(10, 4))
    for case, mode in itertools.product([c[0] for c in CASES], MODES):
        selected = [r for r in rows if r["case_id"] == case and r["mode"] == mode and r["speed_m_s"] == 1]
        for axis, field in zip(axes, ("median_channel_correlation", "mi_loss_bps_hz")):
            values = [[r[field] for r in selected if r["age_ms"] == age] for age in AGES_MS]
            axis.plot(AGES_MS, [np.median(v) for v in values], marker="o", label=f"{case} {mode}")
            axis.fill_between(AGES_MS, [min(v) for v in values], [max(v) for v in values], alpha=0.1)
            axis.set(xlabel="CSI age (ms)", ylabel=field.replace("_", " "))
            axis.grid(alpha=0.3)
    axes[0].legend(fontsize=7)
    fig.suptitle("Physical 2x2 CSI aging, RX 1 m/s; medians and seed range")
    fig.tight_layout()
    fig.savefig(output / "physical_aging.png", dpi=150)
    plt.close(fig)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--binary", type=Path, default=None)
    parser.add_argument("--samples", type=int, default=100000)
    parser.add_argument("--repetitions", type=int, default=2)
    parser.add_argument("--validate-export", action="store_true", help="Audit saved pairs and optionally replay native analysis")
    args = parser.parse_args()
    if args.validate_export:
        print(json.dumps(validate_export(args.output.resolve(), args.binary), indent=2))
        return
    plan(args.samples, args.repetitions)
    result = run(args.output.resolve(), (args.binary or base.executable()).resolve(),
                 args.samples, args.repetitions)
    print(json.dumps(result["validation"], indent=2))


if __name__ == "__main__":
    main()
