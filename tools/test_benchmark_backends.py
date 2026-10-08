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


class TestRemoteVerifier(unittest.TestCase):
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
