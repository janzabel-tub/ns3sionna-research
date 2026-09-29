#!/usr/bin/env python3
"""Phase 5.2.1: fixed-scene spacing/azimuth sweep, native ns-3 MMSE diagnostics.

The two native actions export the actual helper configuration and evaluate a
serialized server response. Python orchestrates Sionna and plots CSV results;
it does not implement the link abstraction. No calibrated PER is inferred.
"""

from __future__ import annotations

import argparse
import csv
import hashlib
import importlib.metadata
import itertools
import json
import os
from pathlib import Path
import platform
import subprocess
import sys
import tempfile
import time

import numpy as np

MODULE = Path(__file__).resolve().parents[1]
NS3 = MODULE.parents[1]
SERVER = MODULE / "model/ns3sionna"
sys.path.insert(0, str(SERVER))
from common import message_pb2
from google.protobuf.json_format import MessageToDict

SPACINGS = (0.25, 0.5, 1.0)
AZIMUTHS = (0.0, 90.0)
SCENE = "simple_room/simple_room.xml"
POSITIONS = ((3.0, 2.0, 2.0), (4.0, 3.0, 1.0))
CONTROLS = {
    "scene": SCENE, "seed_ns3": 42, "run_ns3": 1, "seed_sionna": 42,
    "frequency_mhz": 5180, "bandwidth_hz": 20e6,
    "fft_size": 256, "subcarrier_spacing_hz": 78125,
    "tx_power_dbm_total": 20.0, "temperature_k": 290.0,
    "noise_figure_db": 7.0, "nss": 2, "precoding": "open_loop",
    "normalization": "unit_average_pair_power", "rank_relative_tolerance": 1e-12,
    "effective_sinr": "MI: exp(mean(log1p(SINR)))-1",
    "tone_selection": "all_256_fft_bins_including_dc_and_guards",
    "tx_position_m": list(POSITIONS[0]), "rx_position_m": list(POSITIONS[1]),
    "tx_pattern": "tr38901", "rx_pattern": "dipole", "polarization": "V",
    "array_rows": 1, "array_columns": 2, "vertical_spacing_lambda": 0.5,
    "fixed_coherence_time_ms": 5, "csi_time_ns": 0, "server_mode": "P2P",
    "samples_per_src": 100000, "max_depth": 3, "synthetic_array": False,
    "los": True, "specular_reflection": True, "diffuse_reflection": False,
    "refraction": True, "diffraction": False, "edge_diffraction": False,
    "diffraction_lit_region": False, "drjit_threads": 1,
    "repeatability_rtol": 1e-6, "repeatability_atol": 1e-7,
}


def executable() -> Path:
    candidates = sorted((NS3 / "build/contrib/sionna/examples").glob(
        "ns3.40-ns3_wifi_sumimo_benchmark-*"))
    if len(candidates) != 1:
        raise FileNotFoundError("Build ns3_wifi_sumimo_benchmark first; expected one executable")
    return candidates[0]


def native(binary: Path, **arguments) -> None:
    subprocess.run([str(binary), *(f"--{key}={value}" for key, value in arguments.items())],
                   check=True, cwd=NS3, capture_output=True, text=True, timeout=60)


def export_configuration(binary: Path, output: Path, spacing: float,
                         tx_angles=(0.0, 0.0, 0.0), rx_angles=(0.0, 0.0, 0.0), seed=42,
                         rx_spacing=None):
    arguments = dict(action="config", output=output, spacingLambda=spacing, seed=seed)
    if rx_spacing is not None:
        arguments["rxSpacingLambda"] = rx_spacing
    for endpoint, angles in (("tx", tx_angles), ("rx", rx_angles)):
        arguments.update({endpoint + label: value for label, value in
                          zip(("Azimuth", "Elevation", "Roll"), angles)})
    native(binary, **arguments)
    config = message_pb2.SimInitMessage.FromString(output.read_bytes())
    validate_configuration(config, spacing, tx_angles, rx_angles, seed, rx_spacing)
    return config


def validate_configuration(config, spacing, tx_angles, rx_angles, seed=42, rx_spacing=None) -> None:
    """Assert fixed controls on the bytes emitted by the real native helper."""
    assert (config.scene_fname, config.seed, config.frequency, config.channel_bw,
            config.fft_size, config.subcarrier_spacing, config.mode, config.sub_mode,
            config.fixed_coherence_time_ms, config.time_evo_model) == (
                SCENE, seed, 5180, 20, 256, 78125, 1, -1, 5, "position")
    assert [node.id for node in config.nodes] == [0, 1]
    for array, pattern, distance in ((config.tx_array, "tr38901", spacing),
            (config.rx_array, "dipole", spacing if rx_spacing is None else rx_spacing)):
        assert (array.num_rows, array.num_cols, array.vertical_spacing,
                array.horizontal_spacing, array.pattern, array.polarization) == (
                    1, 2, 0.5, distance, pattern, "V")
    for node, position, angles in zip(config.nodes, POSITIONS, (tx_angles, rx_angles)):
        assert node.WhichOneof("model") == "constant_position_model"
        point = node.constant_position_model.position
        assert (point.x, point.y, point.z) == position
        assert node.HasField("orientation")
        assert (node.orientation.azimuth, node.orientation.elevation,
                node.orientation.roll) == tuple(angles)


def runtime():
    # Set before importing Sionna RT; the benchmark has a fixed CPU contract.
    os.environ["CUDA_VISIBLE_DEVICES"] = ""
    from ns3sionna_server import SionnaEnv
    import mitsuba as mi
    import drjit as dr
    if mi.variant() != "llvm_ad_mono_polarized":
        raise RuntimeError(f"Expected CPU LLVM backend, got {mi.variant()}")
    dr.set_thread_count(CONTROLS["drjit_threads"])
    assert dr.thread_count() == CONTROLS["drjit_threads"]
    return SionnaEnv, mi


def observed_geometry(env, config) -> dict:
    """Check real PlanarArrays and placed radio objects, including degree conversion."""
    observed = {}
    for endpoint, node in zip(("tx", "rx"), config.nodes):
        array = getattr(env.scene, endpoint + "_array")
        positions = np.asarray(array.normalized_positions).T
        # Dr.Jit vector arrays have coordinate-major NumPy layout.
        spacing = getattr(config, endpoint + "_array").horizontal_spacing
        np.testing.assert_allclose(positions, [[0, -spacing / 2, 0], [0, spacing / 2, 0]],
                                   rtol=0, atol=1e-7)
        radio = env.scene.get("tx" if endpoint == "tx" else "rx1")
        angles = [node.orientation.azimuth, node.orientation.elevation, node.orientation.roll]
        actual_angles = np.asarray(radio.orientation).reshape(-1)
        actual_position = np.asarray(radio.position).reshape(-1)
        np.testing.assert_allclose(actual_angles, np.deg2rad(angles), rtol=0, atol=1e-7)
        np.testing.assert_allclose(actual_position, POSITIONS[node.id], rtol=0, atol=1e-7)
        observed[endpoint] = {"array_positions_lambda": positions.tolist(),
                              "orientation_rad": actual_angles.tolist(),
                              "position_m": actual_position.tolist()}
    return observed


def trace(config, env_class, samples_per_src=None):
    """Fresh CPU trace; an explicit sample budget supports the Phase 5.2.2 sweep."""
    controls = {**CONTROLS, "samples_per_src": (
        CONTROLS["samples_per_src"] if samples_per_src is None else samples_per_src)}
    if not isinstance(controls["samples_per_src"], int) or controls["samples_per_src"] <= 0:
        raise ValueError("Samples per source must be a positive integer")
    env = env_class(model_folder=str(SERVER / "models"), rt_fast=True,
                    rt_max_parallel_links=1, VERBOSE=False)
    env.rt_samples_per_src = controls["samples_per_src"]
    init_start = time.perf_counter()
    successful, error = env.init_simulation_env(config)
    if not successful:
        raise RuntimeError(error)
    init_seconds = time.perf_counter() - init_start
    try:
        for option in ("max_depth", "synthetic_array", "los", "specular_reflection",
                       "diffuse_reflection", "refraction", "diffraction",
                       "edge_diffraction", "diffraction_lit_region", "samples_per_src"):
            assert getattr(env, "rt_" + option) == controls[option]
        assert env.my_seed == config.seed
        request = message_pb2.ChannelStateRequest(tx_node=0, rx_node=1, time=0)
        wrapper = message_pb2.Wrapper()
        cfr_start = time.perf_counter()
        env.compute_cfr(request, wrapper)
        cfr_seconds = time.perf_counter() - cfr_start
        observed = observed_geometry(env, config)
        response = wrapper.channel_state_response
        assert len(response.csi) == 1
        snapshot = response.csi[0]
        assert (snapshot.start_time, snapshot.end_time, snapshot.tx_node.id) == (0, 5000000, 0)
        assert len(snapshot.rx_nodes) == 1 and snapshot.rx_nodes[0].id == 1
        rx = snapshot.rx_nodes[0]
        assert (rx.num_rx_antennas, rx.num_tx_antennas, len(rx.frequencies)) == (2, 2, 256)
        np.testing.assert_allclose(np.mean(np.abs(channel_array(response)) ** 2), 1.0,
                                   rtol=1e-6, atol=1e-7)
        return response, observed, init_seconds, cfr_seconds
    finally:
        env.release()


def channel_array(response) -> np.ndarray:
    rx = response.csi[0].rx_nodes[0]
    return (np.asarray(rx.csi_real) + 1j * np.asarray(rx.csi_imag)).reshape(-1, 2, 2)


def read_metrics(path: Path) -> list[dict]:
    with path.open(newline="") as stream:
        rows = [{key: float(value) for key, value in row.items()}
                for row in csv.DictReader(stream)]
    if not rows or not all(np.isfinite(value) for row in rows for value in row.values()):
        raise ValueError("Empty or non-finite native diagnostic output")
    return rows


def validate_reference_metrics(response, rows) -> None:
    """Independent NumPy SVD/MMSE/MI and link-budget check, shared with tests."""
    h = channel_array(response)
    rx = response.csi[0].rx_nodes[0]
    assert len(rows) == len(h)
    np.testing.assert_array_equal([row["subcarrier"] for row in rows], np.arange(len(h)))
    np.testing.assert_array_equal([row["frequency_offset_hz"] for row in rows], rx.frequencies)
    noise = 1.3803e-23 * 290 * 20e6 * 10 ** 0.7
    power = 10 ** ((20 - rx.wb_loss - 30) / 10)
    rho = power / noise
    error = np.linalg.inv(np.eye(2) + rho / 2 * (h.conj().transpose(0, 2, 1) @ h))
    sinr = 1 / np.diagonal(error, axis1=1, axis2=2).real - 1
    singular = np.linalg.svd(h, compute_uv=False)
    np.testing.assert_allclose([[row["sinr0_linear"], row["sinr1_linear"]] for row in rows],
                               sinr, rtol=1e-8, atol=1e-8)
    np.testing.assert_allclose([[row["sigma0"], row["sigma1"]] for row in rows],
                               singular, rtol=1e-10, atol=1e-10)
    np.testing.assert_array_equal([row["rank"] for row in rows],
                                 np.sum(singular > 1e-12 * singular[:, :1], axis=1))
    effective = np.expm1(np.mean(np.log1p(sinr), axis=0))
    for row in rows:
        np.testing.assert_allclose([row[f"effective_sinr{s}_linear"] for s in range(2)],
                                   effective, rtol=1e-8, atol=1e-8)
        np.testing.assert_allclose([row["wideband_loss_db"], row["noise_power_w"],
            row["received_power_w"], row["reference_snr_linear"]],
            [rx.wb_loss, noise, power, rho], rtol=1e-12)
        np.testing.assert_allclose(row["spectral_efficiency_bps_hz"],
            np.mean(np.sum(np.log2(1 + sinr), axis=1)), rtol=1e-9, atol=1e-8)


def compare_repetitions(first_response, response, first_metrics, metrics) -> None:
    # Runtime is deliberately excluded. Compare CFR plus every deterministic native field.
    assert response.SerializeToString(deterministic=True) == first_response.SerializeToString(
        deterministic=True), "Single-thread CPU repetitions must have byte-identical channel responses"
    rtol, atol = CONTROLS["repeatability_rtol"], CONTROLS["repeatability_atol"]
    np.testing.assert_allclose(channel_array(response), channel_array(first_response),
                               rtol=rtol, atol=atol)
    rx, first_rx = response.csi[0].rx_nodes[0], first_response.csi[0].rx_nodes[0]
    np.testing.assert_allclose(rx.wb_loss, first_rx.wb_loss, rtol=0, atol=atol)
    assert list(rx.frequencies) == list(first_rx.frequencies)
    assert len(metrics) == len(first_metrics)
    for key in first_metrics[0]:
        if key != "analysis_seconds":
            np.testing.assert_allclose([row[key] for row in metrics],
                                       [row[key] for row in first_metrics], rtol=rtol, atol=atol)


def sha256(path: Path) -> str:
    return hashlib.sha256(path.read_bytes()).hexdigest()


def write_csv(path: Path, rows: list[dict]) -> None:
    with path.open("w", newline="") as stream:
        writer = csv.DictWriter(stream, fieldnames=list(rows[0]), lineterminator="\n")
        writer.writeheader()
        writer.writerows(rows)


def plots(output: Path, summaries: list[dict], independent_spacing=False) -> None:
    import matplotlib
    matplotlib.use("Agg")
    import matplotlib.pyplot as plt
    groups = sorted({(row["tx_azimuth_deg"], row["rx_azimuth_deg"]) for row in summaries})
    figures = {
        "channel_comparison.png": ("median_sigma0", "median_sigma1", "median_sigma_ratio",
                                   "minimum_rank", "wideband_loss_db"),
        "sinr_comparison.png": ("effective_sinr0_db", "effective_sinr1_db",
                                "spectral_efficiency_bps_hz"),
        "runtime_comparison.png": ("init_seconds", "cfr_seconds", "analysis_seconds"),
    }
    labels = {"median_sigma0": "Median normalized singular value σ₀",
              "median_sigma1": "Median normalized singular value σ₁",
              "median_sigma_ratio": "Median σ₁ / σ₀", "minimum_rank": "Minimum rank",
              "wideband_loss_db": "Wideband loss (dB)",
              "effective_sinr0_db": "Stream 0 MI effective SINR (dB)",
              "effective_sinr1_db": "Stream 1 MI effective SINR (dB)",
              "spectral_efficiency_bps_hz": "Gaussian spectral efficiency (bit/s/Hz)",
              "init_seconds": "Scene initialization (s)",
              "cfr_seconds": "Ray tracing and CFR response (s)",
              "analysis_seconds": "Native deserialization/MMSE/SVD (s)"}
    for filename, keys in figures.items():
        fig, axes = plt.subplots(1, len(keys), figsize=(4 * len(keys), 3.9), squeeze=False)
        for axis, key in zip(axes[0], keys):
            if independent_spacing:
                cases = sorted({row["case_id"] for row in summaries})
                samples = [[row[key] for row in summaries if row["case_id"] == case] for case in cases]
                axis.plot(range(len(cases)), [np.median(values) for values in samples], marker="o")
                axis.fill_between(range(len(cases)), [min(values) for values in samples],
                                  [max(values) for values in samples], alpha=0.15)
                captions = []
                for case in cases:
                    row = next(row for row in summaries if row["case_id"] == case)
                    captions.append(f"{case[-2:]}: {row['tx_spacing_lambda']:g}/{row['rx_spacing_lambda']:g}λ\n"
                                    f"{row['tx_azimuth_deg']:g}/{row['rx_azimuth_deg']:g}°")
                axis.set(xlabel="Case: TX/RX spacing, azimuth", xticks=range(len(cases)),
                         xticklabels=captions, ylabel=labels[key])
                axis.tick_params(axis="x", labelsize=7)
            else:
                for tx, rx in groups:
                    values = [np.median([row[key] for row in summaries
                                         if row["spacing_lambda"] == spacing
                                         and row["tx_azimuth_deg"] == tx
                                         and row["rx_azimuth_deg"] == rx]) for spacing in SPACINGS]
                    axis.plot(SPACINGS, values, marker="o", label=f"TX {tx:g}°, RX {rx:g}°")
                axis.set(xlabel="TX and RX spacing (wavelengths)", ylabel=labels[key], xticks=SPACINGS)
            axis.grid(alpha=0.25)
        if not independent_spacing:
            axes[0][0].legend(fontsize=8)
        fig.suptitle("Fixed 2×2 open-loop SU-MIMO, CPU; " + (
            "100k samples: median and seed/repetition range" if independent_spacing else
            "medians over fresh repetitions"))
        fig.tight_layout()
        fig.savefig(output / filename, dpi=150)
        plt.close(fig)


def run(output: Path, binary: Path, repetitions: int) -> dict:
    if repetitions < 2:
        raise ValueError("At least two fresh repetitions are required")
    if output.exists() and any(output.iterdir()):
        raise ValueError("Use an empty output directory to preserve existing results")
    output.mkdir(parents=True, exist_ok=True)
    env_class, mi = runtime()
    started = time.perf_counter()
    summaries, tones, configurations = [], [], []
    for index, (spacing, tx_yaw, rx_yaw) in enumerate(itertools.product(SPACINGS, AZIMUTHS, AZIMUTHS)):
        case_id = f"case_{index:02d}"
        case_dir = output / case_id
        case_dir.mkdir()
        config_path = case_dir / "configuration.pb"
        config = export_configuration(binary, config_path, spacing,
                                      (tx_yaw, 0, 0), (rx_yaw, 0, 0))
        record = {"case_id": case_id, "configuration": MessageToDict(config,
                  preserving_proto_field_name=True, always_print_fields_with_no_presence=True),
                  "configuration_sha256": sha256(config_path), "repetitions": []}
        first_response, first_metrics = None, None
        for repetition in range(repetitions):
            case_started = time.perf_counter()
            response, observed, init_seconds, cfr_seconds = trace(config, env_class)
            response_path = case_dir / f"channel_{repetition:02d}.pb"
            response_path.write_bytes(response.SerializeToString(deterministic=True))
            with tempfile.TemporaryDirectory(prefix="sumimo-metrics-") as temporary:
                metrics_path = Path(temporary) / "metrics.csv"
                native_started = time.perf_counter()
                native(binary, action="analyze", input=response_path, output=metrics_path)
                native_process_seconds = time.perf_counter() - native_started
                metrics = read_metrics(metrics_path)
            if first_response is None:
                first_response, first_metrics = response, metrics
            else:
                compare_repetitions(first_response, response, first_metrics, metrics)
            common = {"case_id": case_id, "repetition": repetition,
                      "spacing_lambda": spacing, "tx_azimuth_deg": tx_yaw, "rx_azimuth_deg": rx_yaw}
            summary = {**common, "minimum_rank": min(row["rank"] for row in metrics),
                       "median_sigma0": float(np.median([row["sigma0"] for row in metrics])),
                       "median_sigma1": float(np.median([row["sigma1"] for row in metrics])),
                       "median_sigma_ratio": float(np.median([row["sigma1"] / row["sigma0"]
                                                                 for row in metrics])),
                       **{key: metrics[0][key] for key in (
                           "effective_sinr0_linear", "effective_sinr1_linear",
                           "spectral_efficiency_bps_hz", "wideband_loss_db",
                           "reference_snr_linear", "received_power_w", "noise_power_w",
                           "analysis_seconds")},
                       "effective_sinr0_db": float(10 * np.log10(metrics[0]["effective_sinr0_linear"])),
                       "effective_sinr1_db": float(10 * np.log10(metrics[0]["effective_sinr1_linear"])),
                       "init_seconds": init_seconds, "cfr_seconds": cfr_seconds,
                       "native_process_seconds": native_process_seconds,
                       "case_seconds": time.perf_counter() - case_started}
            summaries.append(summary)
            tones.extend({**common, **row} for row in metrics)
            record["repetitions"].append({"channel_sha256": sha256(response_path),
                                          "observed_geometry": observed})
            print(f"{case_id} spacing={spacing:g} TX={tx_yaw:g} RX={rx_yaw:g} "
                  f"repeat={repetition} rank={summary['minimum_rank']:g} "
                  f"MI_SINR_dB=({summary['effective_sinr0_db']:.3f},"
                  f"{summary['effective_sinr1_db']:.3f}) CFR_s={cfr_seconds:.3f}", flush=True)
        configurations.append(record)
    source_paths = [MODULE / "examples/ns3_wifi_sumimo_benchmark.cc", Path(__file__),
                    SERVER / "ns3sionna_server.py", SERVER / "mimo_data_contract.py",
                    MODULE / "model/message.proto", MODULE / "model/su-mimo-link-model.cc",
                    MODULE / "model/mimo-channel-matrix.cc", MODULE / "model/wifi-link-budget.cc",
                    MODULE / "model/sionna-propagation-cache.cc",
                    MODULE / "helper/sionna-mimo-helper.cc", MODULE / "helper/sionna-helper.cc"]
    source_paths.extend(sorted(path for path in (SERVER / "models/simple_room").rglob("*")
                               if path.suffix in (".xml", ".obj", ".mtl")))
    manifest = {"schema_version": 1, "subphase": "5.2.1", "controls": CONTROLS,
                "sweep": {"spacing_lambda": SPACINGS, "tx_azimuth_deg": AZIMUTHS,
                          "rx_azimuth_deg": AZIMUTHS, "repetitions": repetitions},
                "provenance": {"platform": platform.platform(), "python": platform.python_version(),
                    "backend": mi.variant(), "versions": {name: importlib.metadata.version(name)
                        for name in ("sionna", "sionna-rt", "mitsuba", "drjit", "numpy", "torch")},
                    "native_executable_sha256": sha256(binary),
                    "source_sha256": {str(path.relative_to(MODULE)): sha256(path) for path in source_paths}},
                "validation": {"repeatability": "passed", "geometry_orientation": "passed",
                               "channel_responses_byte_identical": True,
                               "cases": len(configurations), "fresh_traces": len(summaries)},
                "sweep_seconds": time.perf_counter() - started, "configurations": configurations}
    write_csv(output / "summary.csv", summaries)
    write_csv(output / "tones.csv", tones)
    plots(output, summaries)
    (output / "manifest.json").write_text(json.dumps(manifest, indent=2) + "\n")
    return manifest


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--executable", type=Path, default=None)
    parser.add_argument("--repetitions", type=int, default=2)
    args = parser.parse_args()
    manifest = run(args.output.resolve(), (args.executable or executable()).resolve(), args.repetitions)
    print(json.dumps(manifest["validation"]))


if __name__ == "__main__":
    main()
