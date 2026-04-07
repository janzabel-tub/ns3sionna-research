#!/usr/bin/env python3
"""
Pure Sionna RT benchmark for comparing ray-tracing settings such as
specular reflection, refraction, and diffraction without ns-3.

This script mirrors the ns3sionna server logic closely:
- import sionna.rt first
- use PathSolver with the same flags as the server
- compute wideband loss from the unnormalized CFR as
  -10*log10(mean(|h|^2))
- measure wall-clock runtime per configuration

Examples
--------
# Built-in scene
python pure_sionna_rt_settings_benchmark.py \
  --scene munich \
  --tx 8 21 27 \
  --rx 45 90 1.5 \
  --repeats 3

# XML scene from your model folder
python pure_sionna_rt_settings_benchmark.py \
  --scene /path/to/models/apartment/apartment.xml \
  --tx -1 0.5 1.5 \
  --rx -4.5 1.5 1.6 \
  --repeats 3
"""

import argparse
import gc
import inspect
import json
import math
import time
from pathlib import Path

# Keep this first, mirroring the server comment.
import sionna.rt  # noqa: F401
import drjit as dr
import numpy as np
import pandas as pd
import tensorflow as tf

from sionna.rt import (
    PathSolver,
    PlanarArray,
    Receiver,
    Transmitter,
    load_scene,
    subcarrier_frequencies,
)
from sionna.rt import scene as rt_scene


def resolve_scene(scene_arg: str):
    """Resolve either a filesystem path or a built-in Sionna scene name."""
    p = Path(scene_arg)
    if p.exists():
        return str(p)

    # Allow e.g. "munich" or "simple_street_canyon" if available as built-ins.
    if hasattr(rt_scene, scene_arg):
        return getattr(rt_scene, scene_arg)

    # Fall back to the raw input in case the caller already passed a scene object string.
    return scene_arg


def safe_remove(scene, name: str):
    try:
        scene.remove(name)
    except Exception:
        pass


def solver_kwargs_supported(raw_kwargs: dict) -> dict:
    """Filter kwargs to the ones supported by the installed PathSolver version."""
    sig = inspect.signature(PathSolver.__call__)
    supported = set(sig.parameters.keys())
    return {k: v for k, v in raw_kwargs.items() if k in supported}


def maybe_add_devices(scene, tx_pos, rx_pos, tx_name="tx", rx_name="rx"):
    safe_remove(scene, tx_name)
    safe_remove(scene, rx_name)
    scene.add(Transmitter(name=tx_name, position=tx_pos, orientation=[0, -180, 0]))
    scene.add(Receiver(name=rx_name, position=rx_pos, orientation=[0, -180, 0]))


def compute_metrics(paths, frequencies):
    """Compute simple scalar metrics from Paths."""
    a, tau = paths.cir(sampling_frequency=1e9, normalize_delays=False, out_type="numpy")
    dr.eval()

    h_raw = paths.cfr(
        frequencies=frequencies,
        sampling_frequency=1.0,
        num_time_steps=1,
        normalize_delays=True,
        normalize=False,
        out_type="numpy",
    )
    dr.eval()

    tau_s = np.asarray(tau).squeeze()
    h_s = np.asarray(h_raw).squeeze()

    valid_tau = tau_s[np.isfinite(tau_s) & (tau_s >= 0)] if np.size(tau_s) else np.array([])
    num_paths = int(valid_tau.size)
    first_delay_ns = float(np.min(valid_tau) * 1e9) if num_paths > 0 else math.nan

    if np.size(h_s) == 0 or not np.isfinite(h_s).any():
        mean_power = 0.0
    else:
        mean_power = float(np.mean(np.abs(h_s) ** 2))

    wb_loss_db = float(-10.0 * np.log10(mean_power)) if mean_power > 0 else math.inf

    return {
        "num_paths": num_paths,
        "first_delay_ns": first_delay_ns,
        "mean_cfr_power": mean_power,
        "wb_loss_db": wb_loss_db,
    }


DEFAULT_CASES = [
    {
        "label": "los_only",
        "los": True,
        "specular_reflection": False,
        "diffuse_reflection": False,
        "refraction": False,
        "diffraction": False,
        "edge_diffraction": False,
        "diffraction_lit_region": False,
    },
    {
        "label": "specular_only",
        "los": True,
        "specular_reflection": True,
        "diffuse_reflection": False,
        "refraction": False,
        "diffraction": False,
        "edge_diffraction": False,
        "diffraction_lit_region": False,
    },
    {
        "label": "specular_plus_refraction",
        "los": True,
        "specular_reflection": True,
        "diffuse_reflection": False,
        "refraction": True,
        "diffraction": False,
        "edge_diffraction": False,
        "diffraction_lit_region": False,
    },
    {
        "label": "server_fast_like",
        "los": True,
        "specular_reflection": True,
        "diffuse_reflection": False,
        "refraction": True,
        "diffraction": False,
        "edge_diffraction": False,
        "diffraction_lit_region": False,
    },
    {
        "label": "server_realistic_like_no_diffraction",
        "los": True,
        "specular_reflection": True,
        "diffuse_reflection": True,
        "refraction": True,
        "diffraction": False,
        "edge_diffraction": False,
        "diffraction_lit_region": False,
    },
    {
        "label": "server_realistic_like_with_diffraction",
        "los": True,
        "specular_reflection": True,
        "diffuse_reflection": True,
        "refraction": True,
        "diffraction": True,
        "edge_diffraction": True,
        "diffraction_lit_region": True,
    },
]


def run_benchmark(
    scene_arg,
    tx,
    rx,
    fc_hz,
    bandwidth_hz,
    fft_size,
    subcarrier_spacing_hz,
    max_depth,
    samples_per_src,
    synthetic_array,
    repeats,
    output_csv,
):
    # Configure TensorFlow similarly to the docs: use visible GPU, grow memory on demand.
    gpus = tf.config.list_physical_devices("GPU")
    if gpus:
        try:
            for gpu in gpus:
                tf.config.experimental.set_memory_growth(gpu, True)
        except RuntimeError:
            pass

    print("TensorFlow GPU devices:", gpus if gpus else "none")

    scene_ref = resolve_scene(scene_arg)
    scene = load_scene(scene_ref)
    solver = PathSolver()

    scene.frequency = fc_hz
    scene.bandwidth = bandwidth_hz
    scene.tx_array = PlanarArray(
        num_rows=1,
        num_cols=1,
        vertical_spacing=0.5,
        horizontal_spacing=0.5,
        pattern="tr38901",
        polarization="V",
    )
    scene.rx_array = PlanarArray(
        num_rows=1,
        num_cols=1,
        vertical_spacing=0.5,
        horizontal_spacing=0.5,
        pattern="dipole",
        polarization="V",
    )

    freqs = subcarrier_frequencies(
        num_subcarriers=fft_size,
        subcarrier_spacing=subcarrier_spacing_hz,
    )

    rows = []

    for case in DEFAULT_CASES:
        label = case["label"]
        print(f"\n=== {label} ===")

        case_metrics = []
        for repeat_idx in range(repeats):
            maybe_add_devices(scene, tx, rx)

            raw_kwargs = {
                "scene": scene,
                "max_depth": max_depth,
                "samples_per_src": int(samples_per_src),
                "synthetic_array": synthetic_array,
                **{k: v for k, v in case.items() if k != "label"},
            }
            kwargs = solver_kwargs_supported(raw_kwargs)

            t0 = time.perf_counter()
            paths = solver(**kwargs)
            dr.eval()
            metrics = compute_metrics(paths, freqs)
            elapsed_s = time.perf_counter() - t0

            row = {
                "label": label,
                "repeat": repeat_idx,
                "runtime_s": elapsed_s,
                "fc_hz": fc_hz,
                "bandwidth_hz": bandwidth_hz,
                "fft_size": fft_size,
                "subcarrier_spacing_hz": subcarrier_spacing_hz,
                "max_depth": max_depth,
                "samples_per_src": int(samples_per_src),
                "synthetic_array": bool(synthetic_array),
                **{k: kwargs.get(k, None) for k in [
                    "los",
                    "specular_reflection",
                    "diffuse_reflection",
                    "refraction",
                    "diffraction",
                    "edge_diffraction",
                    "diffraction_lit_region",
                ]},
                **metrics,
            }
            rows.append(row)
            case_metrics.append(row)

            print(
                f"repeat={repeat_idx:02d} runtime={elapsed_s:7.3f}s "
                f"paths={metrics['num_paths']:5d} "
                f"first_delay={metrics['first_delay_ns']:.1f}ns "
                f"wb_loss={metrics['wb_loss_db']:.2f}dB"
            )

            # Explicit cleanup, similar in spirit to the server.
            del paths
            gc.collect()
            dr.sync_thread()

        case_df = pd.DataFrame(case_metrics)
        print(case_df[["runtime_s", "num_paths", "first_delay_ns", "wb_loss_db"]].mean(numeric_only=True))

    df = pd.DataFrame(rows)
    df.to_csv(output_csv, index=False)
    print(f"\nSaved detailed results to: {output_csv}")

    summary = (
        df.groupby("label", dropna=False)
          .agg(
              runtime_mean_s=("runtime_s", "mean"),
              runtime_std_s=("runtime_s", "std"),
              num_paths_mean=("num_paths", "mean"),
              first_delay_ns_mean=("first_delay_ns", "mean"),
              wb_loss_db_mean=("wb_loss_db", "mean"),
          )
          .sort_values("runtime_mean_s")
          .reset_index()
    )

    print("\nSummary:")
    print(summary.to_string(index=False))

    summary_path = Path(output_csv).with_name(Path(output_csv).stem + "_summary.csv")
    summary.to_csv(summary_path, index=False)
    print(f"Saved summary to: {summary_path}")

    print("\nJSON summary:")
    print(summary.to_json(orient="records", indent=2))


if __name__ == "__main__":
    parser = argparse.ArgumentParser()
    parser.add_argument("--scene", type=str, default="munich",
                        help="Filesystem path to an XML scene or a built-in Sionna scene name such as 'munich'.")
    parser.add_argument("--tx", type=float, nargs=3, default=[8.0, 21.0, 27.0],
                        metavar=("X", "Y", "Z"), help="TX position in meters.")
    parser.add_argument("--rx", type=float, nargs=3, default=[45.0, 90.0, 1.5],
                        metavar=("X", "Y", "Z"), help="RX position in meters.")
    parser.add_argument("--fc-hz", type=float, default=5.2e9)
    parser.add_argument("--bandwidth-hz", type=float, default=80e6)
    parser.add_argument("--fft-size", type=int, default=256)
    parser.add_argument("--subcarrier-spacing-hz", type=float, default=312500.0)
    parser.add_argument("--max-depth", type=int, default=5)
    parser.add_argument("--samples-per-src", type=int, default=10 ** 6)
    parser.add_argument("--synthetic-array", action="store_true",
                        help="Enable synthetic_array=True for faster but less exact array handling.")
    parser.add_argument("--repeats", type=int, default=3)
    parser.add_argument("--output-csv", type=str, default="sionna_rt_settings_benchmark.csv")
    args = parser.parse_args()

    run_benchmark(
        scene_arg=args.scene,
        tx=np.asarray(args.tx, dtype=float),
        rx=np.asarray(args.rx, dtype=float),
        fc_hz=float(args.fc_hz),
        bandwidth_hz=float(args.bandwidth_hz),
        fft_size=int(args.fft_size),
        subcarrier_spacing_hz=float(args.subcarrier_spacing_hz),
        max_depth=int(args.max_depth),
        samples_per_src=int(args.samples_per_src),
        synthetic_array=bool(args.synthetic_array),
        repeats=int(args.repeats),
        output_csv=args.output_csv,
    )
