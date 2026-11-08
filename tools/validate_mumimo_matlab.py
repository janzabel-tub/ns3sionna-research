#!/usr/bin/env python3
"""Run the independent base-MATLAB MU-MIMO comparison and retain provenance.

Build first: ./ns3 build -j 2 ns3_mumimo_reference_export
This runner never installs software or changes MATLAB/toolbox settings.
"""

import argparse
import hashlib
import json
from pathlib import Path
import shutil
import subprocess
import sys


def sha256(path):
    with path.open("rb") as stream:
        return hashlib.file_digest(stream, "sha256").hexdigest()


def matlab_string(value):
    return "'" + str(value).replace("'", "''") + "'"


def main():
    module = Path(__file__).resolve().parents[1]
    root = module.parents[1]
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--output", required=True, type=Path)
    parser.add_argument("--matlab", default="matlab")
    parser.add_argument("--binary", type=Path, default=root / "build/contrib/sionna/examples/"
                        "ns3.40-ns3_mumimo_reference_export-default")
    args = parser.parse_args()
    output = args.output.resolve()
    binary = args.binary.resolve()
    if output.exists():
        parser.error("Choose a fresh output directory")
    if not binary.is_file():
        parser.error("Build ns3_mumimo_reference_export first, or specify --binary")
    matlab = shutil.which(args.matlab)
    if not matlab:
        parser.error("MATLAB executable not found")
    output.parent.mkdir(parents=True, exist_ok=True)
    sources = [Path(__file__).resolve(), module / "tools/validate_mumimo_matlab.m",
               module / "examples/ns3_mumimo_reference_export.cc"]
    for name in ("dl-mu-mimo-link-model", "ul-mu-mimo-link-model",
                 "mimo-channel-matrix", "mimo-precoding-matrix"):
        sources.extend(module / "model" / (name + extension) for extension in (".cc", ".h"))
    hashes = {str(path.relative_to(module)): sha256(path) for path in sources}
    binary_hash = sha256(binary)
    library = root / "build/lib/libns3.40-sionna-default.so"
    library_hash = sha256(library) if library.is_file() else None
    expression = (f"addpath({matlab_string(module / 'tools')}); "
                  f"validate_mumimo_matlab({matlab_string(binary)},{matlab_string(output)});")
    command = [matlab, "-batch", expression]
    completed = subprocess.run(command, text=True, stdout=subprocess.PIPE,
                               stderr=subprocess.STDOUT, check=False)
    print(completed.stdout, end="")
    # Keep startup/license errors even when MATLAB could not create its output.
    output.mkdir(exist_ok=True)
    (output / "matlab.log").write_text(completed.stdout)
    unchanged = all(sha256(module / path) == digest for path, digest in hashes.items())
    provenance = {
        "schema_version": 1, "command": command, "matlab_exit_code": completed.returncode,
        "source_sha256": hashes, "sources_unchanged_during_run": unchanged,
        "binary_sha256": binary_hash,
        "binary_unchanged_during_run": sha256(binary) == binary_hash,
        "artifacts_sha256": {name: sha256(output / name) for name in
                            ("fixtures.txt", "native.json", "matlab_reference.json",
                             "report.json", "matlab.log")
                            if (output / name).is_file()},
    }
    if library_hash:
        provenance["sionna_library_sha256"] = library_hash
        provenance["library_unchanged_during_run"] = sha256(library) == library_hash
    (output / "provenance.json").write_text(json.dumps(provenance, indent=2) + "\n")
    if completed.returncode:
        return completed.returncode
    if (not unchanged or not provenance["binary_unchanged_during_run"] or
            not provenance.get("library_unchanged_during_run", True)):
        print("Sources or binaries changed during validation", file=sys.stderr)
        return 1
    report = json.loads((output / "report.json").read_text())
    return 0 if report["status"] == "passed" else 1


if __name__ == "__main__":
    sys.exit(main())
