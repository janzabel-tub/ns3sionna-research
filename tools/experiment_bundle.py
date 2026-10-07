#!/usr/bin/env python3
"""Export, verify and replay relocatable ns3sionna experiment directories.

Keep the printed index SHA-256 outside the bundle (for example in the artifact
catalog). Verification trusts that identity, never a checksum stored beside the
files it authenticates. Historical files are copied verbatim, including original
source hashes and host information; replay uses only relative indexed inputs.
Installed source, a freshly built native executable and the CPU environment are
required for replay. They are recorded as new execution provenance.
"""
from __future__ import annotations

import argparse
from contextlib import contextmanager
import csv
import hashlib
import json
import math
import os
from pathlib import Path, PurePosixPath
import re
import shlex
import shutil
import stat
import sys
import tempfile
import xml.etree.ElementTree as ET

import provenance

MODULE = Path(__file__).resolve().parents[1]
MODELS = MODULE / "model/ns3sionna/models"
SCHEMA = "ns3sionna.experiment-bundle"
INDEX = "index.json"
# A declared numerical acceptance policy, not a promise of byte-identical RT or
# runtime across CPUs. Runtime is excluded even for frozen native replay.
POLICY = {"rtol": 1e-4, "atol": 1e-7,
          "excluded_fields": ["analysis_seconds"],
          "claim": "native scientific metrics within declared tolerances"}


def relative_path(value):
    """Reject nonportable paths before any filesystem access."""
    if not isinstance(value, str) or not value or "\\" in value or ":" in value:
        raise ValueError(f"Invalid relative path: {value!r}")
    path = PurePosixPath(value)
    if path.is_absolute() or any(part in ("", ".", "..") for part in value.split("/")):
        raise ValueError(f"Path escape or noncanonical path: {value!r}")
    return path


def checked_path(root, name):
    path = Path(root).absolute()
    # Do not resolve before checking: that would hide a symlink in the root.
    for parent in (path, *path.parents):
        if parent.is_symlink():
            raise ValueError(f"Symlink is not a bundle input: {parent}")
    for part in relative_path(name).parts:
        path = path / part
        if path.is_symlink():
            raise ValueError(f"Symlink is not a bundle input: {path}")
    return path


def tree_files(root):
    root = Path(root).absolute()
    checked_path(root.parent, root.name)
    if not root.is_dir():
        raise ValueError(f"Not a directory: {root}")
    files, directories = {}, set()
    for current, dirs, names in os.walk(root, followlinks=False):
        for name in (*dirs, *names):
            path = Path(current) / name
            relative = path.relative_to(root).as_posix()
            checked_path(root, relative)
            mode = path.stat().st_mode
            if stat.S_ISDIR(mode):
                directories.add(relative)
            elif stat.S_ISREG(mode):
                files[relative] = path
            else:
                raise ValueError(f"Only regular files/directories may be bundled: {path}")
    return files, directories


def copy_file(source, root, name):
    source = Path(source).absolute()
    checked_path(source.parent, source.name)
    if not source.is_file():
        raise ValueError(f"Missing input: {source}")
    target = checked_path(root, name)
    target.parent.mkdir(parents=True, exist_ok=True)
    if target.exists():
        if source.read_bytes() != target.read_bytes():
            raise ValueError(f"Conflicting bundled inputs: {name}")
    else:
        shutil.copyfile(source, target)


def scene_dependencies(models, scene):
    """Follow XML includes/filenames and OBJ material/MTL texture references.

    References must stay within the supplied models root. Copy exact bytes;
    never rewrite scene XML or protobuf configurations to hide host paths.
    """
    pending, found = [str(relative_path(scene))], {}
    while pending:
        name = pending.pop()
        if name in found:
            continue
        path = checked_path(models, name)
        if not path.is_file():
            raise ValueError(f"Missing scene dependency: {name}")
        found[name] = path
        references = []
        if path.suffix.lower() == ".xml":
            for element in ET.fromstring(path.read_bytes()).iter():
                if element.tag == "include":
                    references.append(element.attrib.get("filename", ""))
                elif element.tag == "string" and element.attrib.get("name") == "filename":
                    references.append(element.attrib.get("value", ""))
        elif path.suffix.lower() in (".obj", ".mtl"):
            for line in path.read_text(encoding="utf-8").splitlines():
                tokens = shlex.split(line, comments=True)
                if tokens and tokens[0] == "mtllib":
                    references.extend(tokens[1:])
                elif tokens and (tokens[0].startswith("map_") or tokens[0] in ("bump", "disp", "decal")):
                    # Optional MTL flags precede the final filename. Scene assets
                    # using ambiguous unquoted spaces must be made explicit.
                    references.append(tokens[-1])
        for reference in references:
            relative_path(reference)
            dependency = (PurePosixPath(name).parent / reference).as_posix()
            relative_path(dependency)
            pending.append(dependency)
    return found


def protobuf():
    sys.path.insert(0, str(MODULE / "model/ns3sionna"))
    from common import message_pb2
    return message_pb2


def json_file(path):
    # Strict duplicate-key rejection keeps the index interpretation unambiguous.
    def unique(pairs):
        result = {}
        for key, value in pairs:
            if key in result:
                raise ValueError(f"Duplicate JSON key: {key}")
            result[key] = value
        return result
    return json.loads(Path(path).read_text(encoding="utf-8"), object_pairs_hook=unique)


def output_available(output):
    output = Path(output).absolute()
    checked_path(output.parent, output.name)
    if output.exists() and (not output.is_dir() or any(output.iterdir())):
        raise ValueError("Use an empty output directory; overwrite/resume is unsupported")
    return output


def replay_records(study, prefix, manifest):
    """Index existing SU-MIMO diagnostic channels; do not generate old metrics."""
    records = []
    tones = study / "tones.csv"
    if not tones.is_file():
        return records
    for channel in sorted(study.rglob("channel_*.pb")):
        configuration = channel.parent / "configuration.pb"
        while not configuration.exists() and configuration.parent != study:
            configuration = configuration.parent.parent / "configuration.pb"
        if not configuration.is_file():
            continue
        config = protobuf().SimInitMessage.FromString(configuration.read_bytes())
        selector = {"repetition": int(channel.stem.split("_")[-1])}
        for component in channel.relative_to(study).parts[:-1]:
            if component.startswith("case_"):
                selector["case_id"] = component
            elif component.startswith("seed_"):
                selector["seed"] = int(component[5:])
            elif component.startswith("samples_"):
                selector["samples_per_src"] = int(component[8:])
        samples = selector.get("samples_per_src", manifest.get("controls", {}).get("samples_per_src"))
        aging = manifest.get("subphase") == "5.2.5"
        if aging:
            # One real delayed-CSI diagnostic per channel is sufficient for the
            # portable replay. The complete original age sweep remains bundled.
            selector.update(age_ms=5.0, csi_kind="delayed")
        records.append({"configuration": f"{prefix}/{configuration.relative_to(study).as_posix()}",
            "channel": f"{prefix}/{channel.relative_to(study).as_posix()}",
            "metrics": f"{prefix}/tones.csv", "selector": selector,
            "scene": config.scene_fname, "samples_per_src": samples,
            "native_arguments": ({"action": "aging", "csiAgeMs": 5.0,
                "transmissionMs": 5.0, "rotationRateDegPerMs": 9.0} if aging else {"action": "analyze"})})
    return records


def recorded_identities(study, manifest, run):
    """Check existing recorded output identities without changing their meaning."""
    if run:
        for name, expected in run.get("artifacts", {}).items():
            path = checked_path(study, name)
            if not path.is_file():
                raise ValueError(f"Missing recorded run artifact: {name}")
            provenance.verify_digest(path, expected)
    for record in manifest.get("configurations", []):
        if "configuration_sha256" not in record:
            continue
        directory = Path(study)
        if record.get("case_id"):
            directory = checked_path(directory, record["case_id"])
        if "seed" in record:
            directory = checked_path(directory, f"seed_{record['seed']}")
        path = directory / "configuration.pb"
        if provenance.digest(path)["raw_sha256"] != record["configuration_sha256"]:
            raise ValueError("Historical configuration identity differs")
        traces = record.get("traces", record.get("repetitions", []))
        for repetition, trace in enumerate(traces):
            if "channel_sha256" not in trace:
                continue
            trace_directory = directory
            if "samples_per_src" in trace:
                trace_directory = checked_path(directory, f"samples_{trace['samples_per_src']}")
            path = trace_directory / f"channel_{trace.get('repetition', repetition):02d}.pb"
            if provenance.digest(path)["raw_sha256"] != trace["channel_sha256"]:
                raise ValueError("Historical channel identity differs")


def export(studies, output, models=MODELS, inputs=()):
    """Package existing exports byte-for-byte; return the external index identity."""
    output = output_available(output)
    studies = [Path(study).absolute() for study in studies]
    if not studies:
        raise ValueError("At least one study export is required")
    for study in studies:
        if output == study or study in output.parents or output in study.parents:
            raise ValueError("Bundle and study directories must not contain one another")
    output.parent.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix=".bundle-", dir=output.parent) as temporary:
        staged = Path(temporary) / "bundle"
        staged.mkdir()
        index = {"schema": SCHEMA, "schema_version": 1,
                 "hash_contract": "raw-sha256+utf8-crlf-to-lf-v1",
                 "trust": "retain index.json raw SHA-256 in an external trusted catalog",
                 "studies": [], "replays": [], "comparison_policy": POLICY,
                 "semantics": {"execution": "copied run.json status; unknown if absent",
                    "validation": "historical scientific verdict retained separately",
                    "packaging": "does not execute or validate the historical experiment",
                    "host_paths": "legacy records are descriptive; indexed replay paths are relative"}}
        available_inputs = list(map(Path, inputs))
        for number, study in enumerate(studies):
            files, _ = tree_files(study)
            if "manifest.json" not in files:
                raise ValueError(f"Study lacks manifest.json: {study}")
            manifest = json_file(study / "manifest.json")
            run = json_file(study / "run.json") if "run.json" in files else None
            recorded_identities(study, manifest, run)
            prefix = f"studies/{number:02d}"
            for name, source in files.items():
                copy_file(source, staged, f"{prefix}/{name}")
            entry = {"path": prefix, "manifest": f"{prefix}/manifest.json",
                "subphase": manifest.get("subphase"),
                "execution_status": run.get("status", "unknown") if run else "unknown",
                "validation": run.get("validation", manifest.get("validation")) if run else manifest.get("validation"),
                "recorded_output_identities": "verified where present; scientific verdict not rerun",
                "run": f"{prefix}/run.json" if run else None, "inputs": [], "scenes": []}
            scenes = {manifest.get("controls", {}).get("scene")}
            for name, source in files.items():
                if PurePosixPath(name).name == "configuration.pb":
                    config = protobuf().SimInitMessage.FromString(source.read_bytes())
                    scenes.add(config.scene_fname)
            source_hashes = manifest.get("provenance", {}).get("source_sha256", {})
            for scene in sorted(scene for scene in scenes if scene):
                entry["scenes"].append(str(relative_path(scene)))
                for name, source in scene_dependencies(models, scene).items():
                    recorded = source_hashes.get("model/ns3sionna/models/" + name)
                    # Historical raw identities are not reinterpreted as LF hashes.
                    if recorded and provenance.digest(source)["raw_sha256"] != recorded:
                        raise ValueError(f"Scene input differs from measurement-time identity: {name}")
                    portable = "inputs/models/" + name
                    copy_file(source, staged, portable)
                    if portable not in entry["inputs"]:
                        entry["inputs"].append(portable)
            if run:
                for original, expected in run.get("inputs", {}).items():
                    candidates = [Path(original), *available_inputs]
                    candidate = None
                    for source in candidates:
                        if source.is_file():
                            checked_path(source.parent, source.name)
                            try:
                                provenance.verify_digest(source, expected)
                                candidate = source
                                break
                            except ValueError:
                                continue
                    if candidate is None:
                        raise ValueError("Run input unavailable; supply its bytes with --input")
                    portable = "inputs/external/" + expected["raw_sha256"] + "/" + candidate.name
                    relative_path(portable)
                    copy_file(candidate, staged, portable)
                    entry["inputs"].append(portable)
            entry["inputs"].sort()
            index["studies"].append(entry)
            index["replays"].extend(replay_records(study, prefix, manifest))
        # Explicit inputs are useful for exports lacking sidecars. Every supplied
        # input is indexed even when it has no measurement-time provenance.
        for source in available_inputs:
            identity = provenance.digest(source)
            copy_file(source, staged, "inputs/external/" + identity["raw_sha256"] + "/" + source.name)
        files, directories = tree_files(staged)
        index["files"] = {name: provenance.digest(path) for name, path in sorted(files.items())}
        index["directories"] = sorted(directories)
        provenance.atomic_json(staged / INDEX, index)
        identity = provenance.digest(staged / INDEX)["raw_sha256"]
        # All failures above leave an existing empty destination untouched.
        if output.exists():
            output.rmdir()
        os.replace(staged, output)
    return {"bundle": str(output), "index_sha256": identity}


def verify(bundle, index_sha256):
    """Authenticate an index and check exact inventory plus portable file hashes."""
    bundle = Path(bundle).absolute()
    path = checked_path(bundle, INDEX)
    if not re.fullmatch(r"[0-9a-f]{64}", index_sha256 or ""):
        raise ValueError("A trusted external index SHA-256 is required")
    if provenance.digest(path)["raw_sha256"] != index_sha256:
        raise ValueError("Bundle index identity differs from trusted external SHA-256")
    index = json_file(path)
    if index.get("schema") != SCHEMA or index.get("schema_version") != 1:
        raise ValueError("Unsupported experiment bundle schema")
    expected_files = index.get("files")
    if not isinstance(expected_files, dict) or INDEX in expected_files:
        raise ValueError("Invalid file inventory")
    files, directories = tree_files(bundle)
    for name in expected_files:
        relative_path(name)
    for name in index.get("directories", []):
        relative_path(name)
    if set(files) != {*expected_files, INDEX}:
        raise ValueError("Bundle inventory has missing or extra files")
    if directories != set(index.get("directories", [])):
        raise ValueError("Bundle inventory has missing or extra directories")
    raw_differences = []
    for name, expected in expected_files.items():
        if not isinstance(expected.get("bytes"), int) or expected["bytes"] < 0:
            raise ValueError(f"Invalid file size: {name}")
        if not re.fullmatch(r"[0-9a-f]{64}", expected.get("raw_sha256", "")):
            raise ValueError(f"Invalid raw digest: {name}")
        actual = provenance.digest(files[name])
        if not provenance.verify_digest(files[name], expected):
            raw_differences.append(name)
        elif actual["bytes"] != expected["bytes"]:
            raise ValueError(f"File size differs: {name}")
    # Validate every executable input locator even if the files are all present.
    for study in index.get("studies", []):
        for name in (study["manifest"], *study.get("inputs", []), *([study["run"]] if study.get("run") else [])):
            if str(relative_path(name)) not in expected_files:
                raise ValueError(f"Unindexed study input: {name}")
        relative_path(study["path"])
        for scene in study.get("scenes", []):
            for name in scene_dependencies(bundle / "inputs/models", scene):
                if "inputs/models/" + name not in expected_files:
                    raise ValueError(f"Unindexed scene dependency: {name}")
    for record in index.get("replays", []):
        for key in ("configuration", "channel", "metrics"):
            if str(relative_path(record[key])) not in expected_files:
                raise ValueError(f"Unindexed replay input: {record[key]}")
        scene = "inputs/models/" + str(relative_path(record["scene"]))
        if scene not in expected_files:
            raise ValueError(f"Unindexed replay scene: {scene}")
        # Rewalk dependency references: an indexed XML may not point outside the
        # bundle or omit a referenced mesh, material or texture.
        for name in scene_dependencies(bundle / "inputs/models", record["scene"]):
            if "inputs/models/" + name not in expected_files:
                raise ValueError(f"Unindexed scene dependency: {name}")
    return {"index_sha256": index_sha256, "files_verified": len(expected_files),
            "raw_differences": raw_differences, "portable_identity": "verified",
            "historical_execution": [entry["execution_status"] for entry in index["studies"]]}


def selected_metrics(path, selector):
    with Path(path).open(encoding="utf-8", newline="") as stream:
        rows = list(csv.DictReader(stream))
    def matches(row):
        for key, value in selector.items():
            if key not in row:
                return False
            if isinstance(value, (int, float)):
                if float(row[key]) != value:
                    return False
            elif row[key] != value:
                return False
        return True
    result = [row for row in rows if matches(row)]
    if not result:
        raise ValueError("No scientific reference rows match the indexed replay")
    return result


def compare_metrics(expected, actual, policy=POLICY):
    if not expected or len(expected) != len(actual):
        raise ValueError("Scientific metric row count differs")
    fields = set(actual[0]) - set(policy["excluded_fields"])
    required = {"rank", "sigma0", "sigma1", "sinr0_linear", "sinr1_linear",
                "effective_sinr0_linear", "effective_sinr1_linear", "spectral_efficiency_bps_hz"}
    if not required.issubset(fields):
        raise ValueError("Missing native scientific metric fields")
    for first, second in zip(expected, actual):
        if not fields.issubset(first):
            raise ValueError("Reference is missing native scientific fields")
        for key in fields:
            a, b = float(first[key]), float(second[key])
            if not math.isfinite(a) or not math.isfinite(b):
                raise ValueError(f"Non-finite scientific metric: {key}")
            if key in ("subcarrier", "frequency_offset_hz", "rank"):
                accepted = a == b
            else:
                accepted = math.isclose(a, b, rel_tol=policy["rtol"], abs_tol=policy["atol"])
            if not accepted:
                raise ValueError(f"Scientific metric differs: {key}: {a} versus {b}")
    return {"verdict": "passed", "rows": len(actual), "fields": sorted(fields), "policy": policy}


@contextmanager
def representative_workspace(output):
    """Retain partial execution records if small-experiment creation fails."""
    with tempfile.TemporaryDirectory(prefix="sumimo-representative-") as temporary:
        study = Path(temporary)
        try:
            yield study
        except BaseException as error:
            if any(study.iterdir()):
                output = output_available(output)
                output.mkdir(parents=True, exist_ok=True)
                shutil.copytree(study, output / "failed-study")
                provenance.atomic_json(output / "packaging.json", {
                    "schema": "ns3sionna.bundle-creation", "schema_version": 1,
                    "status": "incomplete" if isinstance(error, (KeyboardInterrupt, SystemExit)) else "failed",
                    "complete_bundle": False, "partial_study": "failed-study",
                    "error": f"{type(error).__name__}: {error}"})
            raise


def representative(output, binary=None, samples=10000):
    """One fresh fixed 2x2 CPU trace and native diagnostics, then a portable bundle."""
    import benchmark_sumimo as base
    output = output_available(output)
    binary = Path(binary).absolute() if binary else base.executable()
    if not isinstance(samples, int) or samples <= 0:
        raise ValueError("Samples must be a positive integer")
    dependencies = scene_dependencies(MODELS, base.SCENE)
    with representative_workspace(output) as study:
        configuration = {"controls": {**base.CONTROLS, "samples_per_src": samples},
            "randomness": {"mapping_version": 1, "ns3_seed": 42, "ns3_run": 1,
                           "rt_stream": 0, "mobility_stream": 1},
            "case": {"spacing_lambda": 0.5, "tx_angles": [0, 0, 0], "rx_angles": [90, 0, 0]},
            "scope": "one small RT/native numerical reproduction; no convergence acceptance"}
        with provenance.Run(study / "run.json", configuration, dependencies.values(), binary) as run:
            run.data["validation"] = {"verdict": "not completed"}
            env_class, mi = base.runtime()
            config = base.export_configuration(binary, study / "configuration.pb", 0.5, rx_angles=(90, 0, 0))
            if not config.HasField("randomness"):
                raise ValueError("Rebuild the native benchmark for explicit seed/run/stream provenance")
            response, geometry, init_seconds, cfr_seconds = base.trace(config, env_class, samples)
            channel = study / "channel_00.pb"
            channel.write_bytes(response.SerializeToString(deterministic=True))
            metrics_path = study / "metrics.csv"
            base.native(binary, action="analyze", input=channel, output=metrics_path)
            rows = base.read_metrics(metrics_path)
            base.validate_reference_metrics(response, rows)
            base.write_csv(study / "tones.csv", [{"repetition": 0, **row} for row in rows])
            validation = {"independent_numpy_reference": "passed", "fresh_traces": 1,
                          "sample_convergence": "not assessed"}
            manifest = {"schema_version": 1, "subphase": "R1.representative",
                "controls": configuration["controls"], "validation": validation,
                "provenance": {"backend": mi.variant(), "source_sha256": {
                    "model/ns3sionna/models/" + name: provenance.digest(path)["raw_sha256"]
                    for name, path in dependencies.items()}},
                "configurations": [{"configuration_sha256": base.sha256(study / "configuration.pb"),
                    "randomness": base.MessageToDict(config.randomness,
                        preserving_proto_field_name=True, always_print_fields_with_no_presence=True),
                    "observed_geometry": geometry}], "runtime": {"init_seconds": init_seconds, "cfr_seconds": cfr_seconds}}
            provenance.atomic_json(study / "manifest.json", manifest)
            run.data["validation"] = validation
        return export([study], output)


def reproduce(bundle, output, index_sha256, mode="frozen", binary=None, record=0):
    """Replay one indexed channel; create new execution and scientific verdicts."""
    verified = verify(bundle, index_sha256)
    import benchmark_sumimo as base
    bundle = Path(bundle).absolute()
    output = output_available(output)
    if output == bundle or bundle in output.parents or output in bundle.parents:
        raise ValueError("Replay output must be outside the immutable bundle")
    if mode not in ("frozen", "rt"):
        raise ValueError("Mode must be frozen or rt")
    index = json_file(bundle / INDEX)
    if not isinstance(record, int) or record < 0 or record >= len(index["replays"]):
        raise ValueError("No such indexed replay record")
    replay = index["replays"][record]
    binary = Path(binary).absolute() if binary else base.executable()
    output.mkdir(parents=True, exist_ok=True)
    inputs = [bundle / INDEX, *(checked_path(bundle, replay[key]) for key in ("configuration", "channel", "metrics"))]
    inputs += [path for path in scene_dependencies(bundle / "inputs/models", replay["scene"]).values()]
    config = base.message_pb2.SimInitMessage.FromString(inputs[1].read_bytes())
    if config.scene_fname != replay["scene"]:
        raise ValueError("Replay configuration does not match indexed scene")
    configuration = {"bundle_index_sha256": index_sha256, "mode": mode, "record": record,
                     "replay": replay, "comparison_policy": index["comparison_policy"]}
    with provenance.Run(output / "run.json", configuration, inputs, binary) as run:
        provenance.randomness_selected(config)
        run.data["validation"] = {"verdict": "not completed", "mode": mode}
        channel = output / "channel.pb"
        if mode == "frozen":
            provenance.backend_selected("frozen-protobuf-native-analysis")
            shutil.copyfile(inputs[2], channel)
            response = base.message_pb2.ChannelStateResponse.FromString(channel.read_bytes())
        else:
            env_class, _ = base.runtime()
            if not isinstance(replay.get("samples_per_src"), int) or replay["samples_per_src"] <= 0:
                raise ValueError("Indexed RT sample budget is unavailable")
            # Reuse the established trace orchestration with the relocated assets.
            def relocated_environment(**kwargs):
                kwargs["model_folder"] = str(bundle / "inputs/models")
                return env_class(**kwargs)
            response, _, _, _ = base.trace(config, relocated_environment, replay["samples_per_src"])
            channel.write_bytes(response.SerializeToString(deterministic=True))
        metrics_path = output / "metrics.csv"
        arguments = replay["native_arguments"]
        allowed = {"action", "csiAgeMs", "transmissionMs", "rotationRateDegPerMs"}
        if set(arguments) - allowed or arguments.get("action") not in ("analyze", "aging"):
            raise ValueError("Unsupported indexed native analysis arguments")
        base.native(binary, input=channel, output=metrics_path, **arguments)
        actual = base.read_metrics(metrics_path)
        try:
            base.validate_reference_metrics(response, actual,
                csi_age_ms=arguments.get("csiAgeMs"), transmission_ms=arguments.get("transmissionMs", 5.0),
                rotation_rate=arguments.get("rotationRateDegPerMs", 9.0))
            expected = selected_metrics(inputs[3], replay["selector"])
            validation = compare_metrics(expected, actual, index["comparison_policy"])
        except (AssertionError, ValueError) as error:
            run.data["validation"] = {"verdict": "failed", "mode": mode, "error": str(error)}
            raise
        validation.update(mode=mode, index_sha256=index_sha256, independent_numpy_reference="passed",
            rt_execution="fresh" if mode == "rt" else "not executed", runtime_comparison="excluded",
            cross_machine_byte_equality="not claimed", bundle_verification=verified)
        provenance.atomic_json(output / "validation.json", validation)
        run.data["validation"] = validation
    return {"output": str(output), "execution_status": "success", "validation": "passed", "mode": mode}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    commands = parser.add_subparsers(dest="command", required=True)
    pack = commands.add_parser("export", help="package existing local study exports")
    pack.add_argument("--study", type=Path, action="append", required=True)
    pack.add_argument("--output", type=Path, required=True)
    pack.add_argument("--models", type=Path, default=MODELS)
    pack.add_argument("--input", type=Path, action="append", default=[])
    check = commands.add_parser("verify", help="check a relocated bundle against a trusted index identity")
    check.add_argument("--bundle", type=Path, required=True)
    check.add_argument("--index-sha256", required=True)
    small = commands.add_parser("representative", help="create one small fresh RT/native bundle")
    small.add_argument("--output", type=Path, required=True)
    small.add_argument("--binary", type=Path)
    small.add_argument("--samples", type=int, default=10000)
    replay = commands.add_parser("reproduce", help="replay one indexed numerical diagnostic")
    replay.add_argument("--bundle", type=Path, required=True)
    replay.add_argument("--output", type=Path, required=True)
    replay.add_argument("--index-sha256", required=True)
    replay.add_argument("--mode", choices=("frozen", "rt"), default="frozen")
    replay.add_argument("--record", type=int, default=0)
    replay.add_argument("--binary", type=Path)
    args = parser.parse_args()
    if args.command == "export":
        result = export(args.study, args.output, args.models, args.input)
    elif args.command == "verify":
        result = verify(args.bundle, args.index_sha256)
    elif args.command == "representative":
        result = representative(args.output, args.binary, args.samples)
    else:
        result = reproduce(args.bundle, args.output, args.index_sha256, args.mode, args.binary, args.record)
    print(json.dumps(result, sort_keys=True))


if __name__ == "__main__":
    main()
