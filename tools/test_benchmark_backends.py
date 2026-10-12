"""R6 policy, explicit backend failure, source closure and real-export auditing."""
import copy
import hashlib
import io
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tarfile
import tempfile
import unittest
from types import SimpleNamespace
from unittest.mock import patch

import benchmark_backends as backend


class TestBackendPolicy(unittest.TestCase):
    def test_fixed_matrix_and_invalid_controls(self):
        policy = backend.plan()
        self.assertTrue(policy["fixed_acceptance"])
        self.assertEqual(len(backend.identities(policy)), 12)
        self.assertEqual(policy["tolerances"], backend.convergence.TOLERANCES)
        self.assertFalse(backend.plan(10000)["fixed_acceptance"])
        for samples, repetitions in ((0, 2), (-1, 2), (True, 2), (1, 1), (1, True)):
            with self.assertRaises(ValueError):
                backend.plan(samples, repetitions)

    def test_cuda_probe_on_cpu_host_fails_without_llvm_fallback(self):
        # No GPU import occurs in the suite process. A subprocess either performs
        # CUDA evaluation or fails explicitly, according to the actual hardware.
        result = subprocess.run([sys.executable, str(Path(backend.__file__)), "probe", "--backend=cuda"],
                                capture_output=True, text=True, timeout=60)
        report = json.loads(result.stdout.splitlines()[-1])
        if result.returncode == 0:
            self.assertEqual(report["variant"], backend.VARIANTS["cuda"])
            self.assertEqual(report["evaluation"], [2, 3])
        else:
            self.assertEqual(report["status"], "failed")
            self.assertEqual(report["backend"], "cuda")
            self.assertTrue(report["error"])

    def test_existing_variant_cannot_be_reused_as_other_backend(self):
        fake_mi = type("Mitsuba", (), {"variant": staticmethod(lambda: backend.VARIANTS["cpu"])})
        with patch.dict(sys.modules, {"mitsuba": fake_mi, "drjit": object()}):
            with self.assertRaisesRegex(RuntimeError, "fresh process"):
                backend.backend_runtime("cuda")

    def test_source_archive_retains_phy_edits_and_excludes_build_state(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary) / "source"
            files = {"src/wifi/model/local.cc": b'local PHY\r\n',
                     "contrib/sionna/tools/benchmark_backends.py": b'runner\n',
                     "build/generated.cc": b'build', "cmake-cache/cache.txt": b'cache',
                     "contrib/sionna/experiments/r6/result.json": b'local data',
                     "contrib/sionna/.git/config": b'metadata',
                     "src/wifi/__pycache__/old.pyc": b'bytecode'}
            for name, value in files.items():
                path = root / name
                path.parent.mkdir(parents=True, exist_ok=True)
                path.write_bytes(value)
            archive = Path(temporary) / "source.tar.gz"
            inventory = backend.source_archive(archive, root)
            self.assertEqual(set(inventory), {"src/wifi/model/local.cc",
                                             "contrib/sionna/tools/benchmark_backends.py"})
            with tarfile.open(archive) as source:
                self.assertEqual(source.extractfile("ns-3.40/src/wifi/model/local.cc").read(), b'local PHY\r\n')
            (root / "src/alias").symlink_to(root / "src/wifi/model/local.cc")
            with self.assertRaisesRegex(ValueError, "Symlink"):
                backend.source_archive(archive, root)

    def test_existing_output_is_never_overwritten(self):
        with tempfile.TemporaryDirectory() as temporary:
            output = Path(temporary)
            (output / "valuable.txt").write_text("keep")
            with self.assertRaises(ValueError):
                backend.run(output, Path("/nonexistent"), cpu_only=True)
            self.assertEqual((output / "valuable.txt").read_text(), "keep")

    def test_worker_failure_and_timeout_preserve_logs_and_failed_status(self):
        for failure in (subprocess.CalledProcessError(1, ["worker"]),
                        subprocess.TimeoutExpired(["worker"], 1)):
            with self.subTest(failure=type(failure).__name__), tempfile.TemporaryDirectory() as temporary:
                output = Path(temporary) / "run"
                with patch.object(backend.subprocess, "run", side_effect=failure), \
                     patch.object(backend.provenance, "source_identity", return_value={}), \
                     patch.object(backend.provenance, "ns3_identity", return_value={}), \
                     patch.object(backend.provenance, "tool_versions", return_value={}):
                    with self.assertRaises(type(failure)):
                        backend.run(output, None, cpu_only=True)
                self.assertEqual(json.loads((output / "run.json").read_text())["status"], "failed")
                self.assertTrue((output / "cpu.log").is_file())
                self.assertTrue((output / "run.error.log").is_file())

    def test_existing_environment_records_python311_cuda_torch_and_lock_deviations(self):
        versions = {**backend.CORE_VERSIONS, "numpy": "2.4.4", "torch": "2.9.1+cu128",
                    "protobuf": "6.33.6", "pyzmq": "27.1.0", "scipy": "1.17.1",
                    "matplotlib": "3.10.9", "tensorflow": "2.20.0", "packaging": "26.2"}
        distributions = [SimpleNamespace(metadata={"Name": name}, version=version)
                         for name, version in versions.items()]
        distributions.append(SimpleNamespace(metadata={"Name": "packaging"}, version="25.0"))
        with patch.object(backend.importlib.metadata, "distributions", return_value=distributions), \
             patch.object(backend.importlib.metadata, "version", side_effect=versions.__getitem__), \
             patch.object(backend.platform, "python_version", return_value="3.11.14"):
            report = backend.environment_snapshot("existing")
        self.assertEqual(report["python_version"], "3.11.14")
        self.assertEqual(report["versions"], versions)
        self.assertEqual(report["lock_deviations"]["numpy"]["observed"], "2.4.4")
        self.assertEqual(report["lock_deviations"]["torch"]["observed"], "2.9.1+cu128")
        backend.check_recorded_environment(versions, "existing")
        with self.assertRaises(ValueError):
            backend.check_recorded_environment(versions, "locked")
        for package in ("sionna", "mitsuba", "numpy"):
            broken = {**versions}
            del broken[package]
            with self.assertRaises(ValueError):
                backend.check_recorded_environment(broken, "existing")
        self.assertEqual(backend.plan(mode="existing")["environment_mode"], "existing")
        self.assertNotIn("environment_mode", backend.plan())

    def test_existing_mode_propagates_to_fresh_worker_without_installation(self):
        with tempfile.TemporaryDirectory() as temporary:
            output = Path(temporary) / "run"
            with patch.dict(os.environ, {backend.ENVIRONMENT_VARIABLE: "existing"}), \
                 patch.object(backend.subprocess, "run", side_effect=subprocess.CalledProcessError(1, ["worker"])) as call, \
                 patch.object(backend.provenance, "source_identity", return_value={}), \
                 patch.object(backend.provenance, "ns3_identity", return_value={}), \
                 patch.object(backend.provenance, "tool_versions", return_value={}):
                with self.assertRaises(subprocess.CalledProcessError):
                    backend.run(output, None, cpu_only=True)
                self.assertEqual(os.environ[backend.ENVIRONMENT_VARIABLE], "existing")
            self.assertIn("worker", call.call_args.args[0])
            self.assertNotIn("install", call.call_args.args[0])
            self.assertEqual(json.loads((output / "plan.json").read_text())["policy"]["environment_mode"], "existing")

    def test_cpu_cuda_comparison_rejects_different_recorded_environments(self):
        policy = backend.plan(mode="existing")
        data = (policy, {}, {}, [])
        cpu_probe = {"environment_mode": "existing", "python_version": "3.11.14",
                     "versions": {"numpy": "2.4.4"}}
        cuda_probe = {**cpu_probe, "versions": {"numpy": "2.5.2"}}
        def read(path):
            if Path(path).name == "plan.json":
                return {"policy": policy, "requested_backends": ["cpu", "cuda"]}
            return cpu_probe if Path(path).parent.name == "cpu" else cuda_probe
        with patch.object(backend, "validate_backend", return_value=data), \
             patch.object(backend.bundles, "json_file", side_effect=read):
            with self.assertRaisesRegex(ValueError, "environments differ"):
                backend.validate(Path("/study"))


class TestRemoteVerifier(unittest.TestCase):
    def test_existing_environment_shell_path_creates_no_venv_or_package_install(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            support, source, fakebin = (root / name for name in ("support", "source", "bin"))
            support.mkdir()
            fakebin.mkdir()
            tools = source / "contrib/sionna/tools"
            tools.mkdir(parents=True)
            for name in ("run_r6_gpu.sh", "benchmark_backends.py", "test_benchmark_backends.py"):
                shutil.copyfile(Path(backend.__file__).parent / name, support / name)
            for name in ("run_r6_gpu.sh", "benchmark_backends.py", "test_benchmark_backends.py"):
                (tools / name).write_text("old source\n")
            (tools / "check_cpu_environment.py").write_text("print('environment reused'); raise SystemExit(17)\n")
            native = source / "ns3"
            native.write_text("#!/usr/bin/env bash\nexit 0\n")
            native.chmod(0o755)
            for name in ("id", "nvidia-smi", "pkg-config", "g++", "cmake", "patch", "protoc"):
                path = fakebin / name
                path.write_text("#!/usr/bin/env bash\n" + ("echo 1000\n" if name == "id" else "exit 0\n"))
                path.chmod(0o755)
            output = root / "output"
            result = subprocess.run(["bash", str(support / "run_r6_gpu.sh"), "--use-existing-environment",
                                     "--source", str(source), "--output", str(output)],
                                    env={**os.environ, "R6_PYTHON": sys.executable,
                                         "PATH": str(fakebin) + os.pathsep + os.environ["PATH"]},
                                    capture_output=True, text=True, timeout=60)
            self.assertEqual(result.returncode, 17, result.stdout + result.stderr)
            self.assertIn("no packages will be installed", result.stdout)
            self.assertFalse((output / "venv").exists())
            overlay = json.loads((output / "logs/runner-overlay.json").read_text())
            self.assertEqual(overlay["python_executable"], sys.executable)
            for name in ("run_r6_gpu.sh", "benchmark_backends.py", "test_benchmark_backends.py"):
                with self.subTest(companion=name):
                    self.assertEqual((tools / name).read_bytes(), (support / name).read_bytes())
                    self.assertEqual(overlay["overlays"][name]["original_sha256"],
                                     hashlib.sha256(b"old source\n").hexdigest())
                    self.assertEqual(overlay["overlays"][name]["applied_sha256"],
                                     backend.base.sha256(support / name))

    def test_verifier_checks_external_trust_and_refuses_unsafe_archive(self):
        script = (Path(backend.__file__).parent / "run_r6_gpu.sh").read_text()
        verifier = script.split("<<'PY'\n", 1)[1].split("\nPY\n", 1)[0]
        for variant in ("valid", "untrusted", "tampered", "escape", "link"):
            with self.subTest(variant=variant), tempfile.TemporaryDirectory() as temporary:
                root = Path(temporary)
                bundle = root / "bundle"
                source = bundle / "studies/00"
                source.mkdir(parents=True)
                archive_path = source / "ns3-source.tar.gz"
                with tarfile.open(archive_path, "w:gz") as archive:
                    member = tarfile.TarInfo("../escape" if variant == "escape" else "ns-3.40/ns3")
                    if variant == "link":
                        member.type = tarfile.SYMTYPE
                        member.linkname = "/etc/passwd"
                        archive.addfile(member)
                    else:
                        member.size = 4
                        archive.addfile(member, io.BytesIO(b"code"))
                index = {"schema": backend.bundles.SCHEMA, "schema_version": 1,
                         "directories": ["studies", "studies/00"],
                         "files": {"studies/00/ns3-source.tar.gz": backend.provenance.digest(archive_path)}}
                backend.provenance.atomic_json(bundle / "index.json", index)
                identity = backend.base.sha256(bundle / "index.json")
                if variant == "untrusted":
                    identity = "0" * 64
                elif variant == "tampered":
                    archive_path.write_bytes(b"tampered")
                output = root / "output"
                output.mkdir()
                result = subprocess.run([sys.executable, "-c", verifier, str(bundle), identity, str(output)],
                                        capture_output=True, text=True, timeout=60)
                self.assertEqual(result.returncode == 0, variant == "valid", result.stderr)
                if variant == "valid":
                    self.assertEqual((output / "ns-3.40/ns3").read_bytes(), b"code")
                else:
                    self.assertFalse((output / "ns-3.40/ns3").exists())


@unittest.skipUnless(os.environ.get("BACKEND_COMPARISON_RESULTS"), "Optional fresh R6 backend export")
class TestBackendExport(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.output = Path(os.environ["BACKEND_COMPARISON_RESULTS"]).resolve()
        cls.cpu = backend.validate_backend(cls.output / "cpu")

    def corrupt_copy(self):
        temporary = tempfile.TemporaryDirectory()
        self.addCleanup(temporary.cleanup)
        directory = Path(temporary.name) / "cpu"
        shutil.copytree(self.output / "cpu", directory)
        return directory

    def save_manifest(self, directory, manifest):
        backend.provenance.atomic_json(directory / "manifest.json", manifest)

    def test_all_saved_channels_match_native_and_numpy(self):
        policy, metrics, _, _ = backend.validate_backend(self.output / "cpu", backend.base.executable())
        self.assertEqual(len(metrics), len(backend.identities(policy)))
        binary = backend.base.executable()
        previous = Path.cwd()
        try:
            os.chdir(self.output.parent)
            _, relative_metrics, _, _ = backend.validate_backend(Path(self.output.name) / "cpu", binary)
        finally:
            os.chdir(previous)
        self.assertEqual(len(relative_metrics), len(metrics))

    def test_existing_environment_export_audits_without_matching_audit_host_packages(self):
        directory = self.corrupt_copy()
        manifest = json.loads((directory / "manifest.json").read_text())
        manifest["environment_mode"] = "existing"
        policy = backend.plan(manifest["controls"]["samples_per_src"],
                              manifest["sweep"]["repetitions"], "existing")
        backend.provenance.atomic_json(directory / "plan.json", policy)
        probe = json.loads((directory / "probe.json").read_text())
        versions = {**backend.CORE_VERSIONS, "numpy": "2.4.4", "torch": "2.9.1+cu128",
                    "protobuf": "6.33.6", "pyzmq": "27.1.0", "scipy": "1.17.1",
                    "matplotlib": "3.10.9"}
        probe.update(environment_mode="existing", python_version="3.11.14", versions=versions,
                     lock_deviations=backend.lock_deviations(versions))
        backend.provenance.atomic_json(directory / "probe.json", probe)
        for name in ("plan.json", "probe.json"):
            manifest["files"][name] = backend.base.sha256(directory / name)
        self.save_manifest(directory, manifest)
        policy, metrics, _, _ = backend.validate_backend(directory)
        self.assertEqual(policy["environment_mode"], "existing")
        self.assertEqual(len(metrics), 12)
        probe["lock_deviations"] = {}
        backend.provenance.atomic_json(directory / "probe.json", probe)
        manifest["files"]["probe.json"] = backend.base.sha256(directory / "probe.json")
        self.save_manifest(directory, manifest)
        with self.assertRaisesRegex(ValueError, "lock deviations"):
            backend.validate_backend(directory)

    def test_cpu_only_is_gpu_pending_and_paired_results_are_recomputed(self):
        result = backend.validate(self.output)
        requested = json.loads((self.output / "plan.json").read_text())["requested_backends"]
        self.assertEqual(result["status"], "gpu_pending" if requested == ["cpu"] else "passed")
        if requested == ["cpu"]:
            self.assertEqual(result["gpu_validation"], "not executed")
        identical = backend.compare_backends(self.cpu, self.cpu)
        self.assertEqual(identical["status"], "passed")
        self.assertTrue(all(row["cfr_relative_error"] == 0 for row in identical["comparisons"]))

    def test_missing_duplicate_wrong_locator_and_policy_are_rejected(self):
        for corruption in ("missing", "duplicate", "locator", "policy"):
            with self.subTest(corruption=corruption):
                directory = self.corrupt_copy()
                manifest = json.loads((directory / "manifest.json").read_text())
                if corruption == "missing":
                    manifest["configurations"].pop()
                elif corruption == "duplicate":
                    manifest["configurations"][-1] = manifest["configurations"][0]
                elif corruption == "locator":
                    manifest["configurations"][0]["channel"] = "../escape.pb"
                else:
                    manifest["tolerances"]["wideband_loss_db"] = 100
                self.save_manifest(directory, manifest)
                with self.assertRaises(ValueError):
                    backend.validate_backend(directory)

    def test_tampered_channels_and_backend_probe_are_rejected(self):
        for corruption in ("bytes", "probe", "rehash"):
            with self.subTest(corruption=corruption):
                directory = self.corrupt_copy()
                manifest = json.loads((directory / "manifest.json").read_text())
                if corruption == "probe":
                    path = directory / "probe.json"
                    probe = json.loads(path.read_text())
                    probe["variant"] = backend.VARIANTS["cuda"]
                    backend.provenance.atomic_json(path, probe)
                    manifest["files"]["probe.json"] = backend.base.sha256(path)
                else:
                    name = manifest["configurations"][0]["channel"]
                    path = directory / name
                    if corruption == "bytes":
                        path.write_bytes(b'corrupt')
                    else:
                        response = backend.base.message_pb2.ChannelStateResponse.FromString(path.read_bytes())
                        response.csi[0].rx_nodes[0].id = 9
                        path.write_bytes(response.SerializeToString())
                        manifest["files"][name] = backend.base.sha256(path)
                self.save_manifest(directory, manifest)
                with self.assertRaises(ValueError):
                    backend.validate_backend(directory)

    def test_aggregate_metrics_cannot_hide_changed_rows(self):
        directory = self.corrupt_copy()
        manifest = json.loads((directory / "manifest.json").read_text())
        rows = backend.aggregate_rows(directory / "tones.csv")
        rows[0]["sinr0_linear"] *= 2
        backend.base.write_csv(directory / "tones.csv", rows)
        manifest["files"]["tones.csv"] = backend.base.sha256(directory / "tones.csv")
        self.save_manifest(directory, manifest)
        with self.assertRaisesRegex(ValueError, "Aggregate tones"):
            backend.validate_backend(directory)

    def test_paired_comparison_rejects_scientific_difference_and_plan_mismatch(self):
        policy, metrics, responses, repeats = self.cpu
        changed = copy.deepcopy(metrics)
        for row in changed[next(iter(changed))]:
            row["sinr0_linear"] *= 100
        report = backend.compare_backends(self.cpu, (policy, changed, responses, repeats))
        self.assertEqual(report["status"], "failed")
        changed_policy = copy.deepcopy(policy)
        changed_policy["controls"]["samples_per_src"] *= 2
        with self.assertRaisesRegex(ValueError, "plans differ"):
            backend.compare_backends(self.cpu, (changed_policy, metrics, responses, repeats))


if __name__ == "__main__":
    unittest.main()
