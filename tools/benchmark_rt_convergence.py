#!/usr/bin/env python3
"""Phase 5.2.2: paired-seed sample convergence of the existing 2x2 CPU benchmark.

All channel/link metrics come from the existing server and native ns-3 analyzer.
The policy is exported before importing the ray tracer or starting any trace.
The largest sampled budget is a numerical reference, not a truth channel.
--independent-spacing runs the small Phase 5.2.4 matrix with selected spot checks.
--csi-aging runs Phase 5.2.5 with explicit analytic channel evolution and SVD.
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
# (case, TX spacing, RX spacing, TX azimuth, RX azimuth, selection reason).
# Fixed before tracing; the control reuses Phase 5.2.2 case_06.
SPACING_CASES = (
    ("case_00", 0.5, 0.5, 90.0, 0.0, "existing shared-spacing control"),
    ("case_01", 1.0, 0.25, 90.0, 0.0, "wide TX, narrow RX"),
    ("case_02", 0.25, 1.0, 90.0, 0.0, "narrow TX, wide RX"),
    ("case_03", 1.0, 0.25, 45.0, -45.0, "wide TX with new azimuths"),
    ("case_04", 0.25, 1.0, 45.0, -45.0, "wide RX with new azimuths"),
)
SPACING_SPOT_CHECKS = ("case_03", "case_04")
AGING_CASES = (CASES[0], CASES[2])
AGES_MS = (0.0, 1.0, 2.5, 5.0)
TRANSMISSION_MS = 5.0
ROTATION_RATE = 9.0
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


def plan(repetitions=2, independent_spacing=False, csi_aging=False) -> dict:
    if csi_aging:
        return {"schema_version": 1, "subphase": "5.2.5",
            "controls": {**CONTROLS, "precoding": "SVD_equal_power_unquantized",
                         "perfect_rx_csi": True, "csi_time_ns": "exported per evaluation"},
            "sweep": {"samples_per_src": (100000, 1000000), "seeds": SEEDS,
                      "cases": AGING_CASES, "ages_ms": AGES_MS,
                      "csi_kinds": ("fresh", "delayed"), "repetitions": repetitions},
            "evolution": {"formula": "H(t)=H(0) R(omega*t); R=[[cos,-sin],[sin,cos]]",
                "rotation_rate_deg_per_ms": ROTATION_RATE, "transmission_ms": TRANSMISSION_MS,
                "initial_channel_time_ns": 0, "update_interval_ns": 0,
                "feedback_delay": "requested age for delayed CSI; zero for fresh CSI",
                "scope": "analytic TX-basis mixing, not motion or calibrated Doppler; "
                         "one frozen RT channel per case/seed/budget/repetition"},
            "tolerances": TOLERANCES,
            "validation": "byte-identical RT repetitions; independent NumPy SVD/MMSE/MI; "
                          "zero age equals fresh CSI; zero-rate static control; unitary power",
            "reference_tolerances": {"sinr_rtol": 1e-8, "sinr_atol": 1e-8,
                "singular_value_rtol": 1e-10, "singular_value_atol": 1e-10,
                "mi_rtol": 1e-9, "mi_atol": 1e-8},
            "decision": "paired 100k/1M checks for both representative cases, all seeds, "
                        "ages and CSI kinds; no claim for untested geometry or evolution rates"}
    if independent_spacing:
        return {"schema_version": 1, "subphase": "5.2.4", "controls": CONTROLS,
            "sweep": {"samples_per_src": 100000, "seeds": SEEDS, "cases": SPACING_CASES,
                      "repetitions": repetitions}, "tolerances": TOLERANCES,
            "spot_checks": {"case_ids": SPACING_SPOT_CHECKS, "reference_samples_per_src": 1000000},
            "decision": "paired 100k/1M checks for both selected new-angle cases and every seed; "
                        "the result does not certify all untested configurations",
            "validation": "byte-identical repetitions; actual endpoint geometry; "
                          "independent NumPy SVD/MMSE/MI and link budget on every trace"}
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


@base.provenance.benchmark_run(lambda a: plan(a["repetitions"], a["independent_spacing"], a["csi_aging"]))
def run(output: Path, binary: Path, repetitions=2, independent_spacing=False, csi_aging=False) -> dict:
    if independent_spacing and csi_aging:
        raise ValueError("Select one benchmark matrix")
    if repetitions < 2:
        raise ValueError("At least two fresh repetitions are required")
    policy = {**plan(repetitions, independent_spacing, csi_aging),
              "defined_before_tracing_utc": datetime.now(timezone.utc).isoformat()}
    (output / "plan.json").write_text(json.dumps(policy, indent=2) + "\n")
    env_class, mi = base.runtime()
    if csi_aging:
        return run_aging(output, binary, repetitions, policy, env_class, mi)
    started = time.perf_counter()
    summaries, tones, configurations, paired_metrics = [], [], [], {}
    cases = SPACING_CASES if independent_spacing else [
        (case, spacing, spacing, tx, rx, reason) for case, spacing, tx, rx, reason in CASES]
    for case_id, spacing, rx_spacing, tx_yaw, rx_yaw, reason in cases:
        for seed in SEEDS:
            directory = output / case_id / f"seed_{seed}"
            directory.mkdir(parents=True)
            config_path = directory / "configuration.pb"
            config = base.export_configuration(binary, config_path, spacing,
                        (tx_yaw, 0, 0), (rx_yaw, 0, 0), seed=seed, rx_spacing=rx_spacing)
            config_bytes = config_path.read_bytes()
            record = {"case_id": case_id, "seed": seed, "selection_reason": reason,
                      "configuration": base.MessageToDict(config, preserving_proto_field_name=True,
                          always_print_fields_with_no_presence=True),
                      "configuration_sha256": base.sha256(config_path), "traces": []}
            budgets = ((100000, 1000000) if case_id in SPACING_SPOT_CHECKS else (100000,))
            for budget in (budgets if independent_spacing else BUDGETS):
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
                    if independent_spacing:
                        base.validate_reference_metrics(response, metrics)
                    if first_response is None:
                        first_response, first_metrics = response, metrics
                        paired_metrics[case_id, seed, budget] = metrics
                    else:
                        base.compare_repetitions(first_response, response, first_metrics, metrics)
                    common = dict(zip(IDENTITY_FIELDS, (case_id, seed, budget, repetition,
                                                       spacing, tx_yaw, rx_yaw)))
                    if independent_spacing:
                        common["tx_spacing_lambda"] = common.pop("spacing_lambda")
                        common["rx_spacing_lambda"] = rx_spacing
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
    if independent_spacing:
        errors = [{"case_id": case, "seed": seed, "samples_per_src": 100000,
                   "reference_samples_per_src": 1000000,
                   **compare(paired_metrics[case, seed, 100000], paired_metrics[case, seed, 1000000])}
                  for case in SPACING_SPOT_CHECKS for seed in SEEDS]
        decision = {"spot_checks_passed": all(row["passed"] for row in errors),
                    "checked_case_seed_pairs": len(errors), "scope": policy["decision"]}
    else:
        errors = comparisons(paired_metrics)
        decision = verdict(errors)
    for filename, rows in (("summary.csv", summaries), ("tones.csv", tones), ("convergence.csv", errors)):
        base.write_csv(output / filename, rows)
    (output / "convergence.json").write_text(json.dumps(decision, indent=2) + "\n")
    if independent_spacing:
        base.plots(output, [row for row in summaries if row["samples_per_src"] == 100000],
                   independent_spacing=True)
    else:
        plots(output, summaries, errors)
    manifest = {**policy, "plan_sha256": base.sha256(output / "plan.json"),
        "provenance": provenance(binary, mi),
        "validation": {"repeatability": "passed", "geometry_orientation": "passed",
                       "channel_responses_byte_identical_within_seed_budget": True,
                       **({"independent_numpy_reference": "passed", "cases": len(cases)}
                          if independent_spacing else {}),
                       "fresh_traces": len(summaries)},
        "sweep_seconds": sweep_seconds, "convergence": decision, "configurations": configurations}
    (output / "manifest.json").write_text(json.dumps(manifest, indent=2) + "\n")
    return manifest


def provenance(binary, mi):
    source_paths = [Path(__file__), Path(base.__file__),
                    Path(__file__).with_name("test_benchmark_rt_convergence.py"),
                    base.MODULE / "examples/ns3_wifi_sumimo_benchmark.cc",
                    base.SERVER / "ns3sionna_server.py", base.SERVER / "mimo_data_contract.py",
                    base.MODULE / "model/message.proto", base.MODULE / "model/su-mimo-link-model.cc",
                    base.MODULE / "model/mimo-channel-matrix.cc", base.MODULE / "model/wifi-link-budget.cc",
                    base.MODULE / "model/mimo-csi-feedback.cc",
                    base.MODULE / "model/mimo-precoding-matrix.cc",
                    base.MODULE / "model/sionna-propagation-cache.cc",
                    base.MODULE / "helper/sionna-mimo-helper.cc", base.MODULE / "helper/sionna-helper.cc"]
    source_paths.extend(sorted(path for path in (base.SERVER / "models/simple_room").rglob("*")
                               if path.suffix in (".xml", ".obj", ".mtl")))
    return {"platform": platform.platform(), "python": platform.python_version(),
            "backend": mi.variant(), "versions": {name: importlib.metadata.version(name)
                for name in ("sionna", "sionna-rt", "mitsuba", "drjit", "numpy", "torch")},
            "native_executable_sha256": base.sha256(binary),
            "source_sha256": {str(path.relative_to(base.MODULE)): base.sha256(path) for path in source_paths}}


def aging_analysis(binary, channel_path, age_ms, rotation_rate=ROTATION_RATE):
    with tempfile.TemporaryDirectory(prefix="sumimo-aging-") as temporary:
        path = Path(temporary) / "metrics.csv"
        started = time.perf_counter()
        base.native(binary, action="aging", input=channel_path, output=path,
                    csiAgeMs=age_ms, transmissionMs=TRANSMISSION_MS,
                    rotationRateDegPerMs=rotation_rate)
        return base.read_metrics(path), time.perf_counter() - started


def aging_degradation(rows, fresh):
    """Signed loss against fresh SVD on the SAME transmission-time channel."""
    return {**{f"effective_sinr{s}_degradation_db": float(
        db(fresh[0][f"effective_sinr{s}_linear"]) - db(rows[0][f"effective_sinr{s}_linear"]))
        for s in range(2)}, "mi_degradation_bps_hz":
        fresh[0]["spectral_efficiency_bps_hz"] - rows[0]["spectral_efficiency_bps_hz"]}


def run_aging(output, binary, repetitions, policy, env_class, mi):
    started = time.perf_counter()
    summaries, tones, records, paired = [], [], [], {}
    for case_id, spacing, tx_yaw, rx_yaw, reason in AGING_CASES:
        for seed in SEEDS:
            directory = output / case_id / f"seed_{seed}"
            directory.mkdir(parents=True)
            config_path = directory / "configuration.pb"
            config = base.export_configuration(binary, config_path, spacing,
                (tx_yaw, 0, 0), (rx_yaw, 0, 0), seed=seed)
            record = {"case_id": case_id, "seed": seed, "selection_reason": reason,
                "configuration_sha256": base.sha256(config_path), "traces": []}
            for budget in (100000, 1000000):
                trace_dir = directory / f"samples_{budget}"
                trace_dir.mkdir()
                first_response, first_metrics = None, {}
                for repetition in range(repetitions):
                    trace_started = time.perf_counter()
                    response, observed, init_seconds, cfr_seconds = base.trace(config, env_class, budget)
                    path = trace_dir / f"channel_{repetition:02d}.pb"
                    path.write_bytes(response.SerializeToString(deterministic=True))
                    fresh, fresh_seconds = aging_analysis(binary, path, 0)
                    for age in AGES_MS:
                        for kind in ("fresh", "delayed"):
                            actual_age = 0 if kind == "fresh" else age
                            rows, native_seconds = ((fresh, fresh_seconds) if actual_age == 0
                                                    else aging_analysis(binary, path, actual_age))
                            base.validate_reference_metrics(response, rows, actual_age)
                            key = (case_id, seed, budget, age, kind)
                            if first_response is None:
                                first_metrics[age, kind] = rows
                                paired[key] = rows
                            else:
                                base.compare_repetitions(first_response, response,
                                                         first_metrics[age, kind], rows)
                            common = {"case_id": case_id, "seed": seed, "samples_per_src": budget,
                                "repetition": repetition, "spacing_lambda": spacing,
                                "tx_azimuth_deg": tx_yaw, "rx_azimuth_deg": rx_yaw,
                                "age_ms": age, "csi_kind": kind}
                            degradation = aging_degradation(rows, fresh)
                            summaries.append({**common, **{key: value for key, value in rows[0].items()
                                if key not in ("subcarrier", "frequency_offset_hz", "rank", "sigma0",
                                               "sigma1", "sinr0_linear", "sinr1_linear",
                                               "channel_correlation", "channel_relative_change")},
                                "minimum_rank": min(row["rank"] for row in rows),
                                **{f"median_sigma{s}": float(np.median([row[f"sigma{s}"] for row in rows]))
                                   for s in range(2)},
                                "median_channel_correlation": float(np.median([
                                    row["channel_correlation"] for row in rows])),
                                "median_channel_relative_change": float(np.median([
                                    row["channel_relative_change"] for row in rows])),
                                **{f"effective_sinr{s}_db": float(db(rows[0][f"effective_sinr{s}_linear"]))
                                   for s in range(2)}, **degradation,
                                "init_seconds": init_seconds, "cfr_seconds": cfr_seconds,
                                "native_process_seconds": native_seconds})
                            tones.extend({**common, **row, **degradation,
                                **{f"sinr{s}_degradation_db": float(
                                    db(ref[f"sinr{s}_linear"]) - db(row[f"sinr{s}_linear"]))
                                   for s in range(2)}} for row, ref in zip(rows, fresh))
                    if first_response is None:
                        # Positive timestamps alone must leave a static channel unchanged.
                        static, _ = aging_analysis(binary, path, AGES_MS[-1], rotation_rate=0)
                        static_fresh, _ = aging_analysis(binary, path, 0, rotation_rate=0)
                        base.validate_reference_metrics(response, static, AGES_MS[-1], rotation_rate=0)
                        for field in ("sinr0_linear", "sinr1_linear", "spectral_efficiency_bps_hz"):
                            np.testing.assert_array_equal([row[field] for row in static],
                                                          [row[field] for row in static_fresh])
                        first_response = response
                    record["traces"].append({"samples_per_src": budget, "repetition": repetition,
                        "channel_sha256": base.sha256(path), "observed_geometry": observed,
                        "trace_and_evaluation_seconds": time.perf_counter() - trace_started})
                print(f"{case_id} seed={seed} samples={budget} aging/reference/repeatability=passed",
                      flush=True)
            records.append(record)
    sweep_seconds = time.perf_counter() - started
    errors = [{"case_id": case, "seed": seed, "age_ms": age, "csi_kind": kind,
        "samples_per_src": 100000, "reference_samples_per_src": 1000000,
        **compare(paired[case, seed, 100000, age, kind], paired[case, seed, 1000000, age, kind])}
        for case, *_ in AGING_CASES for seed in SEEDS for age in AGES_MS
        for kind in ("fresh", "delayed")]
    decision = {"spot_checks_passed": all(row["passed"] for row in errors),
                "checked_case_seed_age_kind_pairs": len(errors), "scope": policy["decision"]}
    for name, rows in (("summary", summaries), ("tones", tones), ("convergence", errors)):
        base.write_csv(output / (name + ".csv"), rows)
    (output / "convergence.json").write_text(json.dumps(decision, indent=2) + "\n")
    aging_plots(output, summaries)
    manifest = {**policy, "plan_sha256": base.sha256(output / "plan.json"),
        "provenance": provenance(binary, mi), "configurations": records,
        "validation": {"fresh_traces": sum(len(record["traces"]) for record in records),
            "evaluations": len(summaries), "native_analysis_calls": 12 * repetitions * 4 + 24,
            "repeatability": "passed",
            "independent_numpy_reference": "passed", "static_control": "passed",
            "geometry_orientation": "passed"},
        "sweep_seconds": sweep_seconds, "convergence": decision}
    (output / "manifest.json").write_text(json.dumps(manifest, indent=2) + "\n")
    return manifest


def aging_plots(output, summaries):
    import matplotlib
    matplotlib.use("Agg")
    import matplotlib.pyplot as plt
    for filename, keys in (("aging_degradation.png", ("effective_sinr0_degradation_db",
            "effective_sinr1_degradation_db", "mi_degradation_bps_hz")),
            ("aging_channel_runtime.png", ("median_channel_correlation", "analysis_seconds",
                                          "native_process_seconds"))):
        fig, axes = plt.subplots(1, 3, figsize=(15, 4))
        for axis, key in zip(axes, keys):
            for case, *_ in AGING_CASES:
                groups = [[row[key] for row in summaries if row["case_id"] == case
                    and row["age_ms"] == age and row["samples_per_src"] == 100000
                    and row["csi_kind"] == "delayed"] for age in AGES_MS]
                line, = axis.plot(AGES_MS, [np.median(group) for group in groups], marker="o", label=case)
                axis.fill_between(AGES_MS, [min(group) for group in groups], [max(group) for group in groups],
                                  color=line.get_color(), alpha=0.15)
            axis.set(xlabel="CSI age (ms)", ylabel=key)
            axis.grid(alpha=0.25)
        axes[0].legend()
        fig.suptitle("Explicit TX-basis rotation: median and range over seeds/repetitions (100k)")
        fig.tight_layout()
        fig.savefig(output / filename, dpi=150)
        plt.close(fig)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--executable", type=Path, default=None)
    modes = parser.add_mutually_exclusive_group()
    modes.add_argument("--independent-spacing", action="store_true",
                        help="Phase 5.2.4 independent TX/RX spacing with selected 100k/1M checks")
    modes.add_argument("--csi-aging", action="store_true",
                       help="Phase 5.2.5 fresh/delayed SVD with explicit TX-basis rotation")
    args = parser.parse_args()
    result = run(args.output.resolve(), (args.executable or base.executable()).resolve(),
                 independent_spacing=args.independent_spacing, csi_aging=args.csi_aging)
    print(json.dumps(result["convergence"], indent=2))


if __name__ == "__main__":
    main()
