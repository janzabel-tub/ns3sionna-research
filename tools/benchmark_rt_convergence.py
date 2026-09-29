#!/usr/bin/env python3
"""Phase 5.2.2: paired-seed sample convergence of the existing 2x2 CPU benchmark.

All channel/link metrics come from the existing server and native ns-3 analyzer.
The policy is exported before importing the ray tracer or starting any trace.
The largest sampled budget is a numerical reference, not a truth channel.
"""

from __future__ import annotations

import argparse
from datetime import datetime, timezone
import importlib.metadata
import json
from pathlib import Path
import platform
import tempfile
import time

import numpy as np

import benchmark_sumimo as base

BUDGETS = (10_000, 100_000, 1_000_000)
SEEDS = (42, 43, 44)
# Selected using only the earlier 100k/seed-42 spacing/orientation benchmark.
CASES = (
    ("case_03", 0.25, 90.0, 90.0, "weakest median sigma1/sigma0: 0.030958"),
    ("case_05", 0.5, 0.0, 90.0, "mixed orientation; median sigma1/sigma0: 0.256484"),
    ("case_06", 0.5, 90.0, 0.0, "best median sigma1/sigma0: 0.515959"),
    ("case_10", 1.0, 90.0, 0.0, "highest earlier MI efficiency: 39.902643 bit/s/Hz"),
)
TOLERANCES = {
    "wideband_loss_db": 0.5,
    "rank_changed_tones": 0,
    "singular_value_rtol": 0.05,
    "singular_value_atol": 1e-7,
    "per_tone_sinr_db": 1.0,
    "effective_sinr_db": 0.5,
    "spectral_efficiency_bps_hz": 0.2,
    "sinr_linear_floor_for_db": 1e-12,
}
CONTROLS = {key: value for key, value in base.CONTROLS.items()
            if key not in ("seed_ns3", "seed_sionna", "samples_per_src")}
CONTROLS["seed_contract"] = "each seed sets ns-3, SimInitMessage, NumPy and PathSolver; run=1"
IDENTITY_FIELDS = ("case_id", "seed", "samples_per_src", "repetition", "spacing_lambda",
                   "tx_azimuth_deg", "rx_azimuth_deg")


def plan(repetitions=2) -> dict:
    return {"schema_version": 1, "subphase": "5.2.2", "controls": CONTROLS,
            "sweep": {"samples_per_src": BUDGETS, "seeds": SEEDS, "cases": CASES,
                      "repetitions": repetitions}, "tolerances": TOLERANCES,
            "decision": "all tones, streams, cases and paired seeds must pass; "
                        "100k vs 1M establishes stabilization within the tested range; "
                        "10k is supported only if all three budget comparisons pass"}


def db(values):
    return 10 * np.log10(np.maximum(values, TOLERANCES["sinr_linear_floor_for_db"]))


def compare(candidate: list[dict], reference: list[dict]) -> dict:
    """Worst-tone paired comparison; relative singular tolerance uses reference values."""
    if not candidate or len(candidate) != len(reference):
        raise ValueError("Comparisons require matching nonempty tone sets")
    for key in ("subcarrier", "frequency_offset_hz"):
        if [row[key] for row in candidate] != [row[key] for row in reference]:
            raise ValueError("Comparisons require identical tone order and frequencies")
    def values(rows, keys):
        result = np.array([[row[key] for key in keys] for row in rows])
        if not np.all(np.isfinite(result)):
            raise ValueError("Non-finite convergence metric")
        return result
    singular, ref_singular = (values(rows, ("sigma0", "sigma1"))
                             for rows in (candidate, reference))
    tone_sinr, ref_tone_sinr = (values(rows, ("sinr0_linear", "sinr1_linear"))
                               for rows in (candidate, reference))
    effective, ref_effective = (values(rows[:1], ("effective_sinr0_linear",
                                               "effective_sinr1_linear"))
                               for rows in (candidate, reference))
    scalar, ref_scalar = (values(rows[:1], ("wideband_loss_db", "spectral_efficiency_bps_hz"))
                         for rows in (candidate, reference))
    ranks, ref_ranks = (values(rows, ("rank",)) for rows in (candidate, reference))
    errors = {
        "wideband_loss_db": float(abs(scalar[0, 0] - ref_scalar[0, 0])),
        "rank_changed_tones": int(np.count_nonzero(ranks != ref_ranks)),
        "singular_value_scaled_error": float(np.max(abs(singular - ref_singular) /
            (TOLERANCES["singular_value_atol"] + TOLERANCES["singular_value_rtol"] *
             abs(ref_singular)))),
        "per_tone_sinr_db": float(np.max(abs(db(tone_sinr) - db(ref_tone_sinr)))),
        "effective_sinr_db": float(np.max(abs(db(effective) - db(ref_effective)))),
        "spectral_efficiency_bps_hz": float(abs(scalar[0, 1] - ref_scalar[0, 1])),
    }
    passed = {key + "_passed": value <= (1.0 if key == "singular_value_scaled_error"
                                         else TOLERANCES[key]) for key, value in errors.items()}
    return {**errors, **passed, "passed": all(passed.values())}


def comparisons(metrics: dict) -> list[dict]:
    rows = []
    for case_id, *_ in CASES:
        for seed in SEEDS:
            for budget, reference in ((BUDGETS[0], BUDGETS[1]),
                                      (BUDGETS[1], BUDGETS[2]), (BUDGETS[0], BUDGETS[2])):
                rows.append({"case_id": case_id, "seed": seed, "samples_per_src": budget,
                             "reference_samples_per_src": reference,
                             **compare(metrics[case_id, seed, budget],
                                       metrics[case_id, seed, reference])})
    return rows


def verdict(rows: list[dict]) -> dict:
    expected = {(case[0], seed, low, high) for case in CASES for seed in SEEDS
                for low, high in ((BUDGETS[0], BUDGETS[1]), (BUDGETS[1], BUDGETS[2]),
                                  (BUDGETS[0], BUDGETS[2]))}
    actual = {(row["case_id"], row["seed"], row["samples_per_src"],
               row["reference_samples_per_src"]) for row in rows}
    if actual != expected or len(rows) != len(expected):
        raise ValueError("Incomplete or duplicated comparison matrix")
    high_pass = all(row["passed"] for row in rows if row["samples_per_src"] == BUDGETS[1])
    all_pass = all(row["passed"] for row in rows)
    return {"convergence_achieved_in_tested_range": high_pass,
            "minimum_supported_samples_per_src": (BUDGETS[0] if all_pass else
                                                  BUDGETS[1] if high_pass else None),
            "passed_comparisons": sum(row["passed"] for row in rows),
            "total_comparisons": len(rows),
            "failed_comparisons": [{key: row[key] for key in (
                "case_id", "seed", "samples_per_src", "reference_samples_per_src")}
                | {"failed_metrics": [key.removesuffix("_passed") for key, value in row.items()
                                       if key.endswith("_passed") and not value]}
                for row in rows if not row["passed"]],
            "scope": "four static simple_room 2x2 links, three seeds, stated solver options; "
                     "1M is a sampled reference, no asymptotic or PER claim"}


def plots(output: Path, summaries: list[dict], errors: list[dict]) -> None:
    import matplotlib
    matplotlib.use("Agg")
    import matplotlib.pyplot as plt
    figures = {
        "channel_comparison.png": ("wideband_loss_db", "minimum_rank", "median_sigma0",
                                   "median_sigma1", "median_sigma_ratio"),
        "sinr_comparison.png": ("effective_sinr0_db", "effective_sinr1_db",
                                "spectral_efficiency_bps_hz"),
        "runtime_comparison.png": ("init_seconds", "cfr_seconds", "analysis_seconds"),
    }
    for filename, keys in figures.items():
        fig, axes = plt.subplots(1, len(keys), figsize=(4 * len(keys), 4), squeeze=False)
        for axis, key in zip(axes[0], keys):
            for case_id, *_ in CASES:
                groups = [[row[key] for row in summaries if row["case_id"] == case_id
                           and row["samples_per_src"] == budget] for budget in BUDGETS]
                line, = axis.plot(BUDGETS, [np.median(group) for group in groups],
                                  marker="o", label=case_id)
                axis.fill_between(BUDGETS, [min(group) for group in groups],
                                  [max(group) for group in groups], color=line.get_color(), alpha=0.15)
            axis.set(xscale="log", xlabel="Samples per source", ylabel=key)
            axis.grid(alpha=0.25)
        axes[0][0].legend(fontsize=8)
        fig.suptitle("Fixed 2×2 CPU links: median and range over seeds and repetitions")
        fig.tight_layout()
        fig.savefig(output / filename, dpi=150)
        plt.close(fig)
    keys = ("wideband_loss_db", "singular_value_scaled_error", "per_tone_sinr_db",
            "effective_sinr_db", "spectral_efficiency_bps_hz")
    fig, axes = plt.subplots(1, len(keys), figsize=(20, 4), squeeze=False)
    for axis, key in zip(axes[0], keys):
        tolerance = 1.0 if key == "singular_value_scaled_error" else TOLERANCES[key]
        for case_id, *_ in CASES:
            values = [max(row[key] / tolerance for row in errors if row["case_id"] == case_id
                          and row["samples_per_src"] == budget
                          and row["reference_samples_per_src"] == BUDGETS[-1])
                      for budget in BUDGETS[:-1]]
            axis.plot(BUDGETS[:-1], values, marker="o", label=case_id)
        axis.axhline(1, color="black", linestyle="--")
        axis.set(xscale="log", xlabel="Samples per source", ylabel=key + " / tolerance")
        axis.grid(alpha=0.25)
    axes[0][0].legend(fontsize=8)
    fig.suptitle("Worst paired-seed error against 1M samples; pass threshold = 1")
    fig.tight_layout()
    fig.savefig(output / "convergence_comparison.png", dpi=150)
    plt.close(fig)


def run(output: Path, binary: Path, repetitions=2) -> dict:
    if repetitions < 2:
        raise ValueError("At least two fresh repetitions are required")
    if output.exists() and any(output.iterdir()):
        raise ValueError("Use an empty output directory to preserve results")
    output.mkdir(parents=True, exist_ok=True)
    policy = {**plan(repetitions), "defined_before_tracing_utc": datetime.now(timezone.utc).isoformat()}
    (output / "plan.json").write_text(json.dumps(policy, indent=2) + "\n")
    env_class, mi = base.runtime()
    started = time.perf_counter()
    summaries, tones, configurations, paired_metrics = [], [], [], {}
    for case_id, spacing, tx_yaw, rx_yaw, reason in CASES:
        for seed in SEEDS:
            directory = output / case_id / f"seed_{seed}"
            directory.mkdir(parents=True)
            config_path = directory / "configuration.pb"
            config = base.export_configuration(binary, config_path, spacing,
                        (tx_yaw, 0, 0), (rx_yaw, 0, 0), seed=seed)
            config_bytes = config_path.read_bytes()
            record = {"case_id": case_id, "seed": seed, "selection_reason": reason,
                      "configuration": base.MessageToDict(config, preserving_proto_field_name=True,
                          always_print_fields_with_no_presence=True),
                      "configuration_sha256": base.sha256(config_path), "traces": []}
            for budget in BUDGETS:
                trace_dir = directory / f"samples_{budget}"
                trace_dir.mkdir()
                first_response, first_metrics = None, None
                for repetition in range(repetitions):
                    case_started = time.perf_counter()
                    assert config.SerializeToString(deterministic=True) == config_bytes
                    response, observed, init_seconds, cfr_seconds = base.trace(config, env_class, budget)
                    response_path = trace_dir / f"channel_{repetition:02d}.pb"
                    response_path.write_bytes(response.SerializeToString(deterministic=True))
                    with tempfile.TemporaryDirectory(prefix="sumimo-convergence-") as temporary:
                        metrics_path = Path(temporary) / "metrics.csv"
                        native_started = time.perf_counter()
                        base.native(binary, action="analyze", input=response_path, output=metrics_path)
                        native_seconds = time.perf_counter() - native_started
                        metrics = base.read_metrics(metrics_path)
                    if first_response is None:
                        first_response, first_metrics = response, metrics
                        paired_metrics[case_id, seed, budget] = metrics
                    else:
                        base.compare_repetitions(first_response, response, first_metrics, metrics)
                    common = dict(zip(IDENTITY_FIELDS, (case_id, seed, budget, repetition,
                                                       spacing, tx_yaw, rx_yaw)))
                    summary = {**common, "minimum_rank": min(row["rank"] for row in metrics),
                        **{f"median_sigma{s}": float(np.median([row[f"sigma{s}"] for row in metrics]))
                           for s in range(2)},
                        "median_sigma_ratio": float(np.median([row["sigma1"] / row["sigma0"]
                                                               for row in metrics])),
                        **{key: metrics[0][key] for key in (
                            "effective_sinr0_linear", "effective_sinr1_linear", "wideband_loss_db",
                            "spectral_efficiency_bps_hz", "reference_snr_linear", "received_power_w",
                            "noise_power_w", "analysis_seconds")},
                        **{f"effective_sinr{s}_db": float(db(metrics[0][f"effective_sinr{s}_linear"]))
                           for s in range(2)},
                        "init_seconds": init_seconds, "cfr_seconds": cfr_seconds,
                        "native_process_seconds": native_seconds,
                        "case_seconds": time.perf_counter() - case_started}
                    summaries.append(summary)
                    tones.extend({**common, **row} for row in metrics)
                    record["traces"].append({"samples_per_src": budget, "repetition": repetition,
                        "channel_sha256": base.sha256(response_path), "observed_geometry": observed})
                print(f"{case_id} seed={seed} samples={budget} repeatability=passed "
                      f"loss={summary['wideband_loss_db']:.3f} "
                      f"MI_dB=({summary['effective_sinr0_db']:.3f},"
                      f"{summary['effective_sinr1_db']:.3f})", flush=True)
            configurations.append(record)
    sweep_seconds = time.perf_counter() - started
    errors = comparisons(paired_metrics)
    decision = verdict(errors)
    for filename, rows in (("summary.csv", summaries), ("tones.csv", tones), ("convergence.csv", errors)):
        base.write_csv(output / filename, rows)
    (output / "convergence.json").write_text(json.dumps(decision, indent=2) + "\n")
    plots(output, summaries, errors)
    source_paths = [Path(__file__), Path(base.__file__),
                    Path(__file__).with_name("test_benchmark_rt_convergence.py"),
                    base.MODULE / "examples/ns3_wifi_sumimo_benchmark.cc",
                    base.SERVER / "ns3sionna_server.py", base.SERVER / "mimo_data_contract.py",
                    base.MODULE / "model/message.proto", base.MODULE / "model/su-mimo-link-model.cc",
                    base.MODULE / "model/mimo-channel-matrix.cc", base.MODULE / "model/wifi-link-budget.cc",
                    base.MODULE / "model/sionna-propagation-cache.cc",
                    base.MODULE / "helper/sionna-mimo-helper.cc", base.MODULE / "helper/sionna-helper.cc"]
    source_paths.extend(sorted(path for path in (base.SERVER / "models/simple_room").rglob("*")
                               if path.suffix in (".xml", ".obj", ".mtl")))
    manifest = {**policy, "plan_sha256": base.sha256(output / "plan.json"),
        "provenance": {"platform": platform.platform(), "python": platform.python_version(),
            "backend": mi.variant(), "versions": {name: importlib.metadata.version(name)
                for name in ("sionna", "sionna-rt", "mitsuba", "drjit", "numpy", "torch")},
            "native_executable_sha256": base.sha256(binary),
            "source_sha256": {str(path.relative_to(base.MODULE)): base.sha256(path) for path in source_paths}},
        "validation": {"repeatability": "passed", "geometry_orientation": "passed",
                       "channel_responses_byte_identical_within_seed_budget": True,
                       "fresh_traces": len(summaries)},
        "sweep_seconds": sweep_seconds, "convergence": decision, "configurations": configurations}
    (output / "manifest.json").write_text(json.dumps(manifest, indent=2) + "\n")
    return manifest


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--executable", type=Path, default=None)
    args = parser.parse_args()
    result = run(args.output.resolve(), (args.executable or base.executable()).resolve())
    print(json.dumps(result["convergence"], indent=2))


if __name__ == "__main__":
    main()
