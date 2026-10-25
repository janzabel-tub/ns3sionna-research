#!/usr/bin/env python3
"""R6: explicit RT backends, paired native diagnostics and portable reproduction."""
from __future__ import annotations

import argparse
import csv
import importlib.metadata
import json
import os
import platform
from pathlib import Path
import subprocess
import sys
import tarfile
import time

import numpy as np

import benchmark_sumimo as base
import benchmark_rt_convergence as convergence
import check_cpu_environment as environment
import experiment_bundle as bundles
import provenance

VARIANTS = {"cpu": "llvm_ad_mono_polarized", "cuda": "cuda_ad_mono_polarized"}
CASES = (("case_00", 90.0, 90.0), ("case_01", 90.0, 0.0))
SEEDS = (42, 43, 44)
SCHEMA = "ns3sionna.r6-backends"
ENVIRONMENT_VARIABLE = "NS3SIONNA_R6_ENVIRONMENT_MODE"
CORE_VERSIONS = {"sionna": "2.2.0", "sionna-rt": "2.2.0",
                 "mitsuba": "3.9.1", "drjit": "1.5.0"}


def environment_mode():
    mode = os.environ.get(ENVIRONMENT_VARIABLE, "locked")
    if mode not in ("locked", "existing"):
        raise ValueError("Unknown R6 environment mode")
    return mode


def normalized_name(name):
    return name.lower().replace("_", "-").replace(".", "-")


def lock_deviations(versions):
    installed = {normalized_name(name): value for name, value in versions.items()}
    return {name: {"expected": expected, "observed": installed.get(normalized_name(name))}
            for name, expected in environment.locked_versions(environment.LOCK).items()
            if installed.get(normalized_name(name)) != expected}


def check_recorded_environment(versions, mode):
    """Check measured versions without requiring the audit host to match them."""
    if not isinstance(versions, dict) or not all(isinstance(key, str) and
            isinstance(value, str) and value for key, value in versions.items()):
        raise ValueError("Invalid recorded package versions")
    if mode == "locked":
        if versions != environment.locked_versions(environment.LOCK):
            raise ValueError("Wrong recorded package versions")
    elif mode == "existing":
        installed = {normalized_name(name): value for name, value in versions.items()}
        for name, expected in CORE_VERSIONS.items():
            if installed.get(name) != expected:
                raise ValueError(f"Existing environment requires {name}=={expected}")
        for name in ("numpy", "torch", "protobuf", "pyzmq", "scipy", "matplotlib"):
            if not installed.get(name):
                raise ValueError(f"Existing environment lacks {name}")
    else:
        raise ValueError("Unknown recorded environment mode")


def environment_snapshot(mode):
    if mode == "existing":
        names = {normalized_name(dist.metadata["Name"])
                 for dist in importlib.metadata.distributions() if dist.metadata.get("Name")}
        # Setuptools can expose vendored dist-info alongside the active package.
        # Resolve each name through the same API used by the exact-lock checks.
        versions = {name: importlib.metadata.version(name) for name in sorted(names)}
    else:
        versions = {name: importlib.metadata.version(name)
                    for name in environment.locked_versions(environment.LOCK)}
    check_recorded_environment(versions, mode)
    return {"environment_mode": mode, "python_version": platform.python_version(),
            "python_executable": sys.executable, "versions": versions,
            "lock_sha256": base.sha256(environment.LOCK),
            "lock_deviations": lock_deviations(versions)}


def plan(samples=100000, repetitions=2, mode="locked"):
    if type(samples) is not int or samples <= 0 or type(repetitions) is not int or repetitions < 2:
        raise ValueError("Positive samples and at least two repetitions are required")
    if mode not in ("locked", "existing"):
        raise ValueError("Unknown R6 environment mode")
    policy = {"schema": SCHEMA, "schema_version": 1, "subphase": "R6",
            "controls": {**base.CONTROLS, "samples_per_src": samples,
                         "seed_ns3": "paired sweep", "seed_sionna": "paired sweep"},
            "sweep": {"cases": [list(case) for case in CASES], "seeds": list(SEEDS),
                      "repetitions": repetitions}, "variants": VARIANTS,
            "tolerances": convergence.TOLERANCES,
            "fixed_acceptance": samples == 100000 and repetitions == 2,
            "scope": "static RT/open-loop MMSE/MI; no PER, speedup or convergence claim"}
    if mode == "existing":
        policy["environment_mode"] = mode
    return policy


def backend_runtime(backend):
    """Select before Sionna imports any variant-specific classes; never fall back."""
    if backend not in VARIANTS:
        raise ValueError("Unknown backend")
    if sys.version_info < (3, 11):
        raise RuntimeError("R6 requires Python >=3.11")
    if backend == "cpu":
        os.environ["CUDA_VISIBLE_DEVICES"] = ""
    import mitsuba as mi
    import drjit as dr
    expected = VARIANTS[backend]
    if mi.variant() not in (None, expected):
        raise RuntimeError("Backend requires a fresh process")
    mi.set_variant(expected)
    from ns3sionna_server import SionnaEnv
    if mi.variant() != expected:
        raise RuntimeError(f"Expected {expected}, got {mi.variant()}")
    dr.set_thread_count(1)
    value = mi.Float([1, 2]) + 1
    dr.eval(value)
    observed = list(value)
    if observed != [2.0, 3.0]:
        raise RuntimeError("Backend evaluation failed")
    provenance.backend_selected(mi.variant())
    snapshot = environment_snapshot(environment_mode())
    device = environment.command(["nvidia-smi", "--query-gpu=name,uuid,driver_version",
                                  "--format=csv,noheader"])
    return SionnaEnv, {"status": "success", "backend": backend, "variant": mi.variant(),
                       "evaluation": observed, "drjit_threads": dr.thread_count(),
                       **snapshot,
                       "cuda_visible_devices": os.environ.get("CUDA_VISIBLE_DEVICES"),
                       "nvidia_smi": device}


def checked_snapshot(response):
    if len(response.csi) != 1:
        raise ValueError("Expected one channel snapshot")
    snapshot = response.csi[0]
    if (snapshot.start_time, snapshot.end_time, snapshot.tx_node.id) != (0, 5000000, 0):
        raise ValueError("Wrong snapshot time or TX")
    if len(snapshot.rx_nodes) != 1:
        raise ValueError("Expected one receiver")
    rx = snapshot.rx_nodes[0]
    if (rx.id, rx.num_rx_antennas, rx.num_tx_antennas, len(rx.frequencies),
            len(rx.csi_real), len(rx.csi_imag)) != (1, 2, 2, 256, 1024, 1024):
        raise ValueError("Wrong receiver or tensor dimensions")
    if list(rx.frequencies) != [78125 * (k - 128) for k in range(256)]:
        raise ValueError("Wrong frequency grid")
    h = base.channel_array(response)
    if not np.all(np.isfinite(h)) or not np.isfinite(rx.wb_loss):
        raise ValueError("Non-finite channel")
    np.testing.assert_allclose(np.mean(abs(h) ** 2), 1, rtol=1e-6, atol=1e-7)
    return h


def identities(policy):
    return [(case, seed, repetition) for case, *_ in CASES for seed in SEEDS
            for repetition in range(policy["sweep"]["repetitions"])]


def case_directory(case, seed):
    return Path(case) / f"seed_{seed}"


def aggregate_rows(path):
    with Path(path).open(newline="") as stream:
        rows = [{key: value if key == "case_id" else float(value)
                 for key, value in row.items()} for row in csv.DictReader(stream)]
    if not rows or not all(np.isfinite(value) for row in rows
                           for key, value in row.items() if key != "case_id"):
        raise ValueError("Empty or non-finite aggregate export")
    return rows


def worker(output, binary, backend, samples, repetitions):
    policy = plan(samples, repetitions, environment_mode())
    output = bundles.output_available(output)
    output.mkdir(parents=True, exist_ok=True)
    provenance.atomic_json(output / "plan.json", policy)
    dependencies = bundles.scene_dependencies(bundles.MODELS, base.SCENE)
    with provenance.Run(output / "run.json", {**policy, "backend": backend},
                        dependencies.values(), binary) as run:
        run.data["validation"] = {"verdict": "not completed"}
        env_class, probe = backend_runtime(backend)
        provenance.atomic_json(output / "probe.json", probe)
        records, tones, summaries = [], [], []
        metrics = {}
        for case, tx_azimuth, rx_azimuth in CASES:
            for seed in SEEDS:
                directory = output / case_directory(case, seed)
                directory.mkdir(parents=True)
                config_path = directory / "configuration.pb"
                config = base.export_configuration(binary, config_path, 0.5,
                    (tx_azimuth, 0, 0), (rx_azimuth, 0, 0), seed=seed)
                for repetition in range(repetitions):
                    response, geometry, init_seconds, cfr_seconds = base.trace(config, env_class, samples)
                    checked_snapshot(response)
                    channel = directory / f"channel_{repetition:02d}.pb"
                    channel.write_bytes(response.SerializeToString(deterministic=True))
                    csv_path = directory / f"metrics_{repetition:02d}.csv"
                    start = time.perf_counter()
                    base.native(binary, action="analyze", input=channel, output=csv_path)
                    native_seconds = time.perf_counter() - start
                    rows = base.read_metrics(csv_path)
                    base.validate_reference_metrics(response, rows)
                    identity = {"case_id": case, "seed": seed, "repetition": repetition}
                    if repetition:
                        verdict = convergence.compare(rows, metrics[case, seed])
                        if not verdict["passed"]:
                            raise ValueError(f"Backend repetition failed: {identity}: {verdict}")
                    else:
                        metrics[case, seed] = rows
                    tones.extend({**identity, **row} for row in rows)
                    summaries.append({**identity, **rows[0], "init_seconds": init_seconds,
                                      "cfr_seconds": cfr_seconds, "native_seconds": native_seconds})
                    records.append({**identity, "configuration": str(config_path.relative_to(output)),
                                    "channel": str(channel.relative_to(output)),
                                    "metrics": str(csv_path.relative_to(output)),
                                    "geometry": geometry})
        base.write_csv(output / "tones.csv", tones)
        base.write_csv(output / "summary.csv", summaries)
        files = ["plan.json", "probe.json", "tones.csv", "summary.csv"]
        files += sorted({record[key] for record in records for key in ("configuration", "channel", "metrics")})
        validation = {"numpy_reference": "passed", "repeat_metrics": "passed",
                      "gpu_comparison": "not performed by backend worker"}
        manifest = {**policy, "backend": backend, "provenance": {"backend": probe["variant"],
            "source_sha256": {"model/ns3sionna/models/" + name: base.sha256(path)
                              for name, path in dependencies.items()}},
            "configurations": records, "files": {name: base.sha256(output / name) for name in files},
            "validation": validation}
        provenance.atomic_json(output / "manifest.json", manifest)
        run.data["validation"] = validation
    return {"status": "success", "backend": backend, "traces": len(records)}


def validate_backend(output, binary=None):
    """Audit saved bytes and recompute references; distrust stored success verdicts."""
    output = Path(output).absolute()
    manifest = bundles.json_file(output / "manifest.json")
    backend = manifest.get("backend")
    if backend not in VARIANTS:
        raise ValueError("Unknown recorded backend")
    mode = manifest.get("environment_mode", "locked")
    policy = plan(manifest["controls"]["samples_per_src"], manifest["sweep"]["repetitions"], mode)
    if bundles.json_file(output / "plan.json") != policy:
        raise ValueError("R6 plan differs from fixed policy")
    for key in policy:
        if manifest[key] != policy[key]:
            raise ValueError("R6 manifest differs from plan")
    records = manifest["configurations"]
    if [(r["case_id"], r["seed"], r["repetition"]) for r in records] != identities(policy):
        raise ValueError("Incomplete or duplicate backend matrix")
    expected_files = {"plan.json", "probe.json", "tones.csv", "summary.csv"}
    for case, seed, repetition in identities(policy):
        directory = case_directory(case, seed)
        expected_files.update(str(directory / name) for name in
            ("configuration.pb", f"channel_{repetition:02d}.pb", f"metrics_{repetition:02d}.csv"))
    if set(manifest["files"]) != expected_files:
        raise ValueError("Invalid R6 file inventory")
    for name, expected in manifest["files"].items():
        if base.sha256(bundles.checked_path(output, name)) != expected:
            raise ValueError(f"R6 hash mismatch: {name}")
    probe = bundles.json_file(output / "probe.json")
    if (probe["status"], probe["backend"], probe["variant"], probe["evaluation"],
            probe["lock_sha256"]) != ("success", backend, VARIANTS[backend], [2, 3],
                                       base.sha256(environment.LOCK)):
        raise ValueError("Wrong actual backend, evaluation or environment lock")
    if probe.get("environment_mode", "locked") != mode:
        raise ValueError("Probe environment mode differs from plan")
    check_recorded_environment(probe["versions"], mode)
    if mode == "existing":
        try:
            python = tuple(int(value) for value in probe["python_version"].split("."))
        except (KeyError, ValueError, AttributeError) as error:
            raise ValueError("Invalid recorded Python version") from error
        if len(python) != 3 or python < (3, 11, 0):
            raise ValueError("Existing environment requires Python >=3.11")
        if probe.get("lock_deviations") != lock_deviations(probe["versions"]):
            raise ValueError("Recorded lock deviations differ from installed versions")
    metrics, responses, repeats = {}, {}, []
    all_tones = aggregate_rows(output / "tones.csv")
    if len(all_tones) != len(records) * 256:
        raise ValueError("Incomplete aggregate tone export")
    summaries = aggregate_rows(output / "summary.csv")
    if len(summaries) != len(records):
        raise ValueError("Incomplete summary export")
    for index, record in enumerate(records):
        case, seed, repetition = (record[key] for key in ("case_id", "seed", "repetition"))
        directory = case_directory(case, seed)
        locators = {"configuration": str(directory / "configuration.pb"),
                    "channel": str(directory / f"channel_{repetition:02d}.pb"),
                    "metrics": str(directory / f"metrics_{repetition:02d}.csv")}
        if any(record[key] != value for key, value in locators.items()):
            raise ValueError("Unexpected R6 record locator")
        config = base.message_pb2.SimInitMessage.FromString((output / record["configuration"]).read_bytes())
        _, tx_azimuth, rx_azimuth = next(item for item in CASES if item[0] == case)
        base.validate_configuration(config, 0.5, (tx_azimuth, 0, 0), (rx_azimuth, 0, 0), seed)
        from random_streams import resolve_randomness
        randomness = resolve_randomness(config)
        if not config.HasField("randomness") or (randomness["rt_seed"], config.randomness.ns3_run,
                config.randomness.rt_stream, config.randomness.mobility_stream) != (seed, 1, 0, 1):
            raise ValueError("Wrong R6 seed/run/streams")
        response = base.message_pb2.ChannelStateResponse.FromString((output / record["channel"]).read_bytes())
        checked_snapshot(response)
        rows = base.read_metrics(output / record["metrics"])
        base.validate_reference_metrics(response, rows)
        selected = all_tones[index * 256:(index + 1) * 256]
        for tone, row in zip(selected, rows):
            for key in ("case_id", "seed", "repetition"):
                if tone[key] != record[key]:
                    raise ValueError("Wrong aggregate row identity")
            if any(tone[key] != value for key, value in row.items()):
                raise ValueError("Aggregate tones differ from native output")
        if any(summaries[index][key] != value for key, value in rows[0].items()):
            raise ValueError("Summary differs from native output")
        if any(summaries[index][key] != record[key] for key in ("case_id", "seed", "repetition")):
            raise ValueError("Wrong summary identity")
        # Native replay is optional; the saved response always gets independent NumPy checks.
        if binary:
            import tempfile
            with tempfile.TemporaryDirectory(prefix="r6-replay-") as temporary:
                replay = Path(temporary) / "metrics.csv"
                base.native(binary, action="analyze", input=output / record["channel"], output=replay)
                actual = base.read_metrics(replay)
                for key in rows[0]:
                    if key != "analysis_seconds":
                        np.testing.assert_allclose([r[key] for r in actual], [r[key] for r in rows],
                                                   rtol=1e-8, atol=1e-8)
        identity = (case, seed, repetition)
        metrics[identity], responses[identity] = rows, response
        if repetition:
            verdict = convergence.compare(rows, metrics[case, seed, 0])
            repeats.append({"case_id": case, "seed": seed, "repetition": repetition,
                            "byte_identical": response.SerializeToString(deterministic=True) ==
                            responses[case, seed, 0].SerializeToString(deterministic=True), **verdict})
            if not verdict["passed"]:
                raise ValueError("Recorded backend repetitions do not pass")
    return policy, metrics, responses, repeats


def compare_backends(cpu, cuda):
    cpu_policy, cpu_metrics, cpu_responses, cpu_repeats = cpu
    cuda_policy, cuda_metrics, cuda_responses, cuda_repeats = cuda
    if cpu_policy != cuda_policy:
        raise ValueError("Backend plans differ")
    comparisons = []
    for identity in identities(cpu_policy):
        reference = checked_snapshot(cpu_responses[identity])
        candidate = checked_snapshot(cuda_responses[identity])
        verdict = convergence.compare(cuda_metrics[identity], cpu_metrics[identity])
        comparisons.append({"case_id": identity[0], "seed": identity[1], "repetition": identity[2],
            "cfr_relative_error": float(np.linalg.norm(candidate - reference) / np.linalg.norm(reference)),
            **verdict})
    passed = all(row["passed"] for row in comparisons)
    return {"schema": SCHEMA, "schema_version": 1, "status": "passed" if passed else "failed",
            "fixed_acceptance": cpu_policy["fixed_acceptance"], "comparisons": comparisons,
            "repeatability": {"cpu": cpu_repeats, "cuda": cuda_repeats}}


def validate(output, binary=None):
    output = Path(output)
    top = bundles.json_file(output / "plan.json")
    cpu = validate_backend(output / "cpu", binary)
    cpu_probe = bundles.json_file(output / "cpu/probe.json")
    observed_environment = {key: cpu_probe[key] for key in
                            ("environment_mode", "python_version", "versions", "lock_sha256",
                             "lock_deviations") if key in cpu_probe}
    if cpu[0] != top["policy"]:
        raise ValueError("Top-level and backend plans differ")
    if top["requested_backends"] == ["cpu"]:
        if (output / "cuda").exists():
            raise ValueError("Unexpected CUDA artifacts in CPU-only run")
        return {"schema": SCHEMA, "schema_version": 1, "status": "gpu_pending",
                "fixed_acceptance": cpu[0]["fixed_acceptance"], "cpu_traces": len(cpu[1]),
                "repeatability": {"cpu": cpu[3]}, "gpu_validation": "not executed",
                "environment": observed_environment}
    if top["requested_backends"] != ["cpu", "cuda"]:
        raise ValueError("Invalid requested backend set")
    cuda = validate_backend(output / "cuda", binary)
    cuda_probe = bundles.json_file(output / "cuda/probe.json")
    if any(cpu_probe.get(key) != cuda_probe.get(key) for key in
           ("environment_mode", "python_version", "versions", "lock_sha256", "lock_deviations")):
        raise ValueError("CPU/CUDA recorded environments differ")
    return {**compare_backends(cpu, cuda), "environment": observed_environment}


def run(output, binary, cpu_only=False, samples=100000, repetitions=2, timeout=1800):
    policy = plan(samples, repetitions, environment_mode())
    output = bundles.output_available(output)
    output.mkdir(parents=True, exist_ok=True)
    backends = ["cpu"] if cpu_only else ["cpu", "cuda"]
    configuration = {"policy": policy, "requested_backends": backends}
    provenance.atomic_json(output / "plan.json", configuration)
    with provenance.Run(output / "run.json", configuration, binary=binary) as sidecar:
        sidecar.data["validation"] = {"verdict": "not completed"}
        for backend in backends:
            command = [sys.executable, str(Path(__file__).resolve()), "worker", "--backend=" + backend,
                       "--output=" + str(output / backend), "--binary=" + str(binary),
                       "--samples=" + str(samples), "--repetitions=" + str(repetitions)]
            with (output / f"{backend}.log").open("w") as log:
                try:
                    subprocess.run(command, stdout=log, stderr=subprocess.STDOUT, check=True, timeout=timeout)
                except subprocess.SubprocessError:
                    child_sidecar = output / backend / "run.json"
                    if child_sidecar.is_file():
                        child = bundles.json_file(child_sidecar)
                        if child["status"] == "running":
                            child.update(status="incomplete", finished_utc=provenance.utc(),
                                         termination="parent observed worker failure/timeout")
                            provenance.atomic_json(child_sidecar, child)
                    raise
        report = validate(output)
        provenance.atomic_json(output / "comparison.json", report)
        sidecar.data["backend"] = "+".join(VARIANTS[b] for b in backends)
        sidecar.data["validation"] = report
        if report["status"] == "failed":
            raise ValueError("CPU/CUDA comparison failed; see comparison.json")
    return report


def source_archive(destination, root=base.NS3):
    """Snapshot buildable source, including local PHY edits, without build/state files."""
    excluded = {".git", ".idea", ".vscode", ".aws", ".codex", ".agents", ".cache",
                "__pycache__", "build", "cmake-cache",
                "experiments", "plots", "testpy-output", "sionna-venv", ".venv"}
    root = Path(root)
    inventory = {}
    with tarfile.open(destination, "w:gz") as archive:
        for directory, dirs, files in os.walk(root, followlinks=False):
            # Upstream documentation has symlinked figure trees and is not a
            # native build input. Module contracts remain in contrib/sionna/doc.
            if Path(directory) == root:
                dirs[:] = [name for name in dirs if name != "doc"]
            if any((Path(directory) / name).is_symlink() for name in dirs if name not in excluded):
                raise ValueError(f"Symlink source directory is not portable: {directory}")
            dirs[:] = sorted(name for name in dirs if name not in excluded)
            for name in sorted(files):
                path = Path(directory) / name
                relative = path.relative_to(root)
                if path.is_symlink():
                    raise ValueError(f"Symlink source is not portable: {relative}")
                if path.suffix in (".pdf", ".png", ".pcap", ".pyc", ".orig") or name.startswith(".lock"):
                    continue
                if len(relative.parts) == 1 and path.suffix == ".csv":
                    continue
                inventory[relative.as_posix()] = provenance.digest(path)
                archive.add(path, arcname="ns-3.40/" + relative.as_posix(), recursive=False)
    return inventory


def package(output, destination, extra_studies=()):
    """Preserve backend/history exports and add a complete, buildable source snapshot."""
    output, destination = Path(output).absolute(), bundles.output_available(destination)
    report = validate(output)
    if report["status"] == "failed":
        raise ValueError("Cannot package a failed backend comparison as a reproduction result")
    import tempfile
    with tempfile.TemporaryDirectory(prefix="r6-sources-") as temporary:
        source = Path(temporary)
        archive = source / "ns3-source.tar.gz"
        provenance.atomic_json(source / "source-inventory.json", source_archive(archive))
        provenance.atomic_json(source / "comparison.json", report)
        provenance.atomic_json(source / "manifest.json", {"schema_version": 1, "subphase": "R6.sources",
            "controls": {"scene": base.SCENE}, "validation": report,
            "source_archive": archive.name, "semantics": "local source snapshot; dependencies require installation"})
        existing = report.get("environment", {}).get("environment_mode") == "existing"
        environment_instructions = (
            "Activate an existing Python >=3.11 environment matching the recorded versions in comparison.json.\n"
            "The measurement used existing packages; no package installation is required.\n"
            if existing else
            "Use Python 3.12.12, pip 26.0.1 and the native libraries in doc/r6-backend-completion.md.\n"
            "python -m pip install --no-deps -r contrib/sionna/model/ns3sionna/requirements_cpu_ci.lock\n"
            "python -m pip check\n")
        (source / "REPRODUCE.md").write_text(
            "Verify the portable bundle using its externally retained index SHA-256 first.\n"
            "Extract ns3-source.tar.gz into an empty directory, then cd ns-3.40.\n"
            + environment_instructions +
            "./ns3 configure --enable-examples --enable-tests\n"
            "./ns3 build -j 2 test-runner sionna-test ns3_wifi_sumimo_benchmark ns3_sionna_cache_ipc_benchmark ns3_sionna_live_benchmark\n"
            "python contrib/sionna/tools/run_cpu_regressions.py\n"
            "For each packaged backend study, replay every saved response:\n"
            "python contrib/sionna/tools/benchmark_backends.py audit --study=<backend-study> --native\n"
            "Fresh measurement (use --cpu-only without CUDA; preserve samples/repetitions in plan.json):\n"
            "python contrib/sionna/tools/benchmark_backends.py run " +
            ("--use-existing-environment " if existing else "") + "--output=<new-output>\n",
            encoding="utf-8")
        studies = [output / "cpu"]
        if report["status"] != "gpu_pending":
            studies.append(output / "cuda")
        studies.extend(map(Path, extra_studies))
        return {**bundles.export([*studies, source], destination), "backend_validation": report["status"]}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    commands = parser.add_subparsers(dest="command", required=True)
    probe = commands.add_parser("probe")
    probe.add_argument("--backend", choices=VARIANTS, required=True)
    probe.add_argument("--use-existing-environment", action="store_true")
    for name in ("run", "worker", "validate", "audit", "package"):
        command = commands.add_parser(name)
        command.add_argument("--output" if name != "audit" else "--study", type=Path, required=True)
        if name in ("run", "worker"):
            command.add_argument("--use-existing-environment", action="store_true")
            command.add_argument("--binary", type=Path)
            command.add_argument("--samples", type=int, default=100000)
            command.add_argument("--repetitions", type=int, default=2)
        if name == "run":
            command.add_argument("--cpu-only", action="store_true")
            command.add_argument("--timeout", type=float, default=1800)
        elif name == "worker":
            command.add_argument("--backend", choices=VARIANTS, required=True)
        elif name in ("validate", "audit"):
            command.add_argument("--native", action="store_true")
        elif name == "package":
            command.add_argument("--bundle", type=Path, required=True)
            command.add_argument("--study", type=Path, action="append", default=[])
    args = parser.parse_args()
    if getattr(args, "use_existing_environment", False):
        os.environ[ENVIRONMENT_VARIABLE] = "existing"
    if args.command == "probe":
        try:
            result = backend_runtime(args.backend)[1]
        except Exception as error:
            print(json.dumps({"status": "failed", "backend": args.backend, "error": str(error)}))
            return 1
    elif args.command in ("run", "worker"):
        binary = args.binary.absolute() if args.binary else base.executable()
        if args.command == "run":
            if not np.isfinite(args.timeout) or args.timeout <= 0:
                parser.error("timeout must be positive and finite")
            result = run(args.output.absolute(), binary, args.cpu_only, args.samples,
                         args.repetitions, args.timeout)
        else:
            result = worker(args.output.absolute(), binary, args.backend, args.samples, args.repetitions)
    elif args.command == "validate":
        result = validate(args.output, base.executable() if args.native else None)
    elif args.command == "audit":
        policy, metrics, _, repeats = validate_backend(args.study, base.executable() if args.native else None)
        result = {"status": "passed", "fixed_acceptance": policy["fixed_acceptance"],
                  "traces": len(metrics), "repeatability": repeats}
    else:
        result = package(args.output, args.bundle, args.study)
    print(json.dumps(result, indent=2, sort_keys=True))
    return int(result.get("status") == "failed")


if __name__ == "__main__":
    raise SystemExit(main())
