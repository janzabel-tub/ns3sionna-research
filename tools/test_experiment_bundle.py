"""Bundle relocation, historical identities, dependency closure and tamper refusal."""
import csv
import json
from pathlib import Path
import shutil
import tempfile
import unittest
from unittest.mock import patch

import experiment_bundle as bundle
import provenance


class TestExperimentBundle(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory(prefix="test-experiment-bundle-")
        self.addCleanup(self.temporary.cleanup)
        self.root = Path(self.temporary.name)
        self.models = self.root / "original-models"
        scene = self.models / "room"
        (scene / "meshes").mkdir(parents=True)
        (scene / "scene.xml").write_bytes(
            b'<scene version="2.1.0"><include filename="part.xml"/></scene>\r\n')
        (scene / "part.xml").write_text(
            '<scene version="2.1.0"><shape type="obj"><string name="filename" '
            'value="meshes/room.obj"/></shape></scene>\n')
        (scene / "meshes/room.obj").write_text('mtllib room.mtl\nv 0 0 0\n')
        (scene / "meshes/room.mtl").write_text('newmtl room\nmap_Kd texture.png\n')
        (scene / "meshes/texture.png").write_bytes(b'\x89PNG\xff\r\n\x00')
        self.study = self.root / "original-study"
        self.study.mkdir()
        self.scene = "room/scene.xml"
        config = bundle.protobuf().SimInitMessage(scene_fname=self.scene, seed=42)
        (self.study / "configuration.pb").write_bytes(config.SerializeToString(deterministic=True))
        (self.study / "channel_00.pb").write_bytes(b'\x00\xff\n')
        self.manifest = {"schema_version": 1, "subphase": "5.2.4",
            "controls": {"scene": self.scene, "samples_per_src": 10000},
            "validation": {"repeatability": "passed"},
            "provenance": {"source_sha256": {"historical.cc": "a" * 64,
                "model/ns3sionna/models/room/scene.xml": provenance.digest(scene / "scene.xml")["raw_sha256"]}}}
        provenance.atomic_json(self.study / "manifest.json", self.manifest)
        (self.study / "summary.csv").write_bytes(b'value\n1\n')
        (self.study / "tones.csv").write_text('repetition,subcarrier,rank,sigma0,sigma1,'
            'sinr0_linear,sinr1_linear,effective_sinr0_linear,effective_sinr1_linear,'
            'spectral_efficiency_bps_hz,analysis_seconds\n0,0,2,1,0.5,2,1,2,1,3,9\n')
        self.destination = self.root / "bundle"

    def pack(self):
        result = bundle.export([self.study], self.destination, self.models)
        self.identity = result["index_sha256"]
        return bundle.json_file(self.destination / "index.json")

    def trust_modified_index(self, index):
        provenance.atomic_json(self.destination / "index.json", index)
        self.identity = provenance.digest(self.destination / "index.json")["raw_sha256"]

    def test_relocation_preserves_historical_bytes_and_unknown_execution(self):
        originals = {name: path.read_bytes() for name, path in bundle.tree_files(self.study)[0].items()}
        index = self.pack()
        self.assertEqual(index["studies"][0]["execution_status"], "unknown")
        self.assertEqual(index["studies"][0]["validation"], self.manifest["validation"])
        self.assertIsNone(index["studies"][0]["run"])
        for name, raw in originals.items():
            self.assertEqual((self.destination / "studies/00" / name).read_bytes(), raw)
        self.assertEqual(len(index["studies"][0]["inputs"]), 5)
        self.assertIn("inputs/models/room/meshes/texture.png", index["files"])
        self.assertEqual(index["replays"][0]["scene"], self.scene)
        relocated = self.root / "different-host" / "artifact"
        relocated.parent.mkdir()
        shutil.move(self.destination, relocated)
        shutil.rmtree(self.study)
        shutil.rmtree(self.models)
        result = bundle.verify(relocated, self.identity)
        self.assertEqual(result["raw_differences"], [])
        self.assertEqual(result["historical_execution"], ["unknown"])
        self.assertNotIn(str(self.root), (relocated / "index.json").read_text())

    def test_multiple_studies_and_failed_status_are_not_promoted(self):
        second = self.root / "other-study"
        shutil.copytree(self.study, second)
        manifest = {**self.manifest, "subphase": "5.2.5"}
        provenance.atomic_json(second / "manifest.json", manifest)
        provenance.atomic_json(second / "run.json", {"status": "failed", "validation": {"accepted": False}, "inputs": {}})
        result = bundle.export([self.study, second], self.destination, self.models)
        index = bundle.json_file(self.destination / "index.json")
        self.assertEqual([entry["execution_status"] for entry in index["studies"]], ["unknown", "failed"])
        self.assertEqual(index["studies"][1]["validation"], {"accepted": False})
        self.assertEqual(index["replays"][1]["native_arguments"]["action"], "aging")
        self.assertEqual(index["replays"][1]["native_arguments"]["transmissionMs"], 5)
        bundle.verify(self.destination, result["index_sha256"])

    def test_trusted_index_prevents_rehashing_tampered_payload(self):
        index = self.pack()
        path = self.destination / "studies/00/channel_00.pb"
        path.write_bytes(b'tampered\xff')
        with self.assertRaisesRegex(ValueError, "Hash mismatch"):
            bundle.verify(self.destination, self.identity)
        index["files"]["studies/00/channel_00.pb"] = provenance.digest(path)
        provenance.atomic_json(self.destination / "index.json", index)
        with self.assertRaisesRegex(ValueError, "trusted external"):
            bundle.verify(self.destination, self.identity)
        with self.assertRaises(ValueError):
            bundle.verify(self.destination, None)

    def test_inventory_rejects_missing_extra_files_and_empty_directories(self):
        self.pack()
        path = self.destination / "extra.pb"
        path.write_bytes(b'new')
        with self.assertRaisesRegex(ValueError, "extra files"):
            bundle.verify(self.destination, self.identity)
        path.unlink()
        path.mkdir()
        with self.assertRaisesRegex(ValueError, "extra directories"):
            bundle.verify(self.destination, self.identity)
        path.rmdir()
        (self.destination / "studies/00/channel_00.pb").unlink()
        with self.assertRaisesRegex(ValueError, "missing or extra files"):
            bundle.verify(self.destination, self.identity)

    def test_portable_lf_text_retains_original_raw_identity_and_binary_does_not(self):
        index = self.pack()
        name = "studies/00/summary.csv"
        original_identity = dict(index["files"][name])
        (self.destination / name).write_bytes(b'value\r\n1\r\n')
        result = bundle.verify(self.destination, self.identity)
        self.assertEqual(result["raw_differences"], [name])
        self.assertEqual(bundle.json_file(self.destination / "index.json")["files"][name], original_identity)
        binary = self.destination / "studies/00/channel_00.pb"
        binary.write_bytes(binary.read_bytes().replace(b'\n', b'\r\n'))
        with self.assertRaisesRegex(ValueError, "Hash mismatch"):
            bundle.verify(self.destination, self.identity)

    def test_symlinks_rejected_in_source_bundle_and_parent(self):
        symlink = self.study / "redirect"
        symlink.symlink_to(self.models, target_is_directory=True)
        with self.assertRaisesRegex(ValueError, "Symlink"):
            self.pack()
        self.assertFalse(self.destination.exists())
        symlink.unlink()
        self.pack()
        channel = self.destination / "studies/00/channel_00.pb"
        channel.unlink()
        channel.symlink_to(self.study / "channel_00.pb")
        with self.assertRaisesRegex(ValueError, "Symlink"):
            bundle.verify(self.destination, self.identity)
        parent = self.root / "alias"
        parent.symlink_to(self.destination, target_is_directory=True)
        with self.assertRaisesRegex(ValueError, "Symlink"):
            bundle.verify(parent, self.identity)

    def test_index_path_escape_and_unindexed_replay_rejected(self):
        index = self.pack()
        for escape in ("../outside.pb", "/tmp/outside.pb", "a/../../b", "a\\b", "C:/b", "a//b", "a/./b"):
            bad = json.loads(json.dumps(index))
            bad["files"][escape] = next(iter(index["files"].values()))
            self.trust_modified_index(bad)
            with self.assertRaises(ValueError):
                bundle.verify(self.destination, self.identity)
        index["replays"][0]["channel"] = "not-indexed.pb"
        self.trust_modified_index(index)
        with self.assertRaisesRegex(ValueError, "Unindexed replay"):
            bundle.verify(self.destination, self.identity)

    def test_mesh_closure_refuses_missing_and_escape_references(self):
        texture = self.models / "room/meshes/texture.png"
        raw = texture.read_bytes()
        texture.unlink()
        with self.assertRaisesRegex(ValueError, "Missing scene dependency"):
            self.pack()
        texture.write_bytes(raw)
        (self.models / "room/part.xml").write_text(
            '<scene><shape><string name="filename" value="../../outside.obj"/></shape></scene>')
        with self.assertRaisesRegex(ValueError, "Path escape"):
            self.pack()

    def test_scene_input_must_match_historical_source_identity(self):
        (self.models / self.scene).write_text('<scene version="2.1.0"/>\n')
        with self.assertRaisesRegex(ValueError, "measurement-time identity"):
            self.pack()
        self.assertFalse(self.destination.exists())

    def test_historical_channel_and_configuration_hashes_checked_before_packaging(self):
        configuration = provenance.digest(self.study / "configuration.pb")["raw_sha256"]
        channel = provenance.digest(self.study / "channel_00.pb")["raw_sha256"]
        self.manifest["configurations"] = [{"configuration_sha256": configuration,
            "repetitions": [{"channel_sha256": channel}]}]
        provenance.atomic_json(self.study / "manifest.json", self.manifest)
        (self.study / "channel_00.pb").write_bytes(b'changed-channel')
        with self.assertRaisesRegex(ValueError, "Historical channel identity"):
            self.pack()
        (self.study / "channel_00.pb").write_bytes(b'\x00\xff\n')
        self.manifest["configurations"][0]["configuration_sha256"] = "b" * 64
        provenance.atomic_json(self.study / "manifest.json", self.manifest)
        with self.assertRaisesRegex(ValueError, "Historical configuration identity"):
            self.pack()

    def test_external_run_input_can_be_supplied_after_original_host_disappears(self):
        supplied = self.root / "capture.pb"
        supplied.write_bytes(b'\xffmeasurement')
        expected = provenance.digest(supplied)
        provenance.atomic_json(self.study / "run.json", {"status": "success",
            "inputs": {"/unavailable/original-host/capture.pb": expected}})
        with self.assertRaisesRegex(ValueError, "Run input unavailable"):
            self.pack()
        result = bundle.export([self.study], self.destination, self.models, [supplied])
        shutil.rmtree(self.study)
        supplied.unlink()
        self.assertEqual(bundle.verify(self.destination, result["index_sha256"])["historical_execution"], ["success"])

    def test_export_refuses_overwrite_and_recursive_destination(self):
        self.pack()
        raw = (self.destination / "index.json").read_bytes()
        with self.assertRaisesRegex(ValueError, "empty output"):
            self.pack()
        self.assertEqual((self.destination / "index.json").read_bytes(), raw)
        with self.assertRaisesRegex(ValueError, "contain one another"):
            bundle.export([self.study], self.study / "bundle", self.models)

    def test_metric_comparison_excludes_runtime_but_checks_science_and_order(self):
        reference = bundle.selected_metrics(self.study / "tones.csv", {"repetition": 0})
        actual = {key: float(value) for key, value in reference[0].items() if key != "repetition"}
        actual["analysis_seconds"] = 999999
        self.assertEqual(bundle.compare_metrics(reference, [actual])["verdict"], "passed")
        for field, value in (("sinr0_linear", 20), ("rank", 1), ("subcarrier", 1), ("sigma1", float("nan"))):
            with self.assertRaises(ValueError):
                bundle.compare_metrics(reference, [{**actual, field: value}])
        with self.assertRaisesRegex(ValueError, "row count"):
            bundle.compare_metrics(reference, [])

    def test_failed_representative_retains_execution_record_and_refuses_resume(self):
        import benchmark_sumimo as base
        with patch.object(base, "runtime", side_effect=RuntimeError("injected RT startup failure")):
            with self.assertRaisesRegex(RuntimeError, "RT startup"):
                bundle.representative(self.destination, binary=self.study / "channel_00.pb")
        run = bundle.json_file(self.destination / "failed-study/run.json")
        self.assertEqual(run["status"], "failed")
        self.assertEqual(run["validation"]["verdict"], "not completed")
        self.assertFalse(bundle.json_file(self.destination / "packaging.json")["complete_bundle"])
        self.assertFalse((self.destination / "index.json").exists())
        with self.assertRaisesRegex(ValueError, "empty output"):
            bundle.representative(self.destination, binary=self.study / "channel_00.pb")


if __name__ == "__main__":
    unittest.main()
