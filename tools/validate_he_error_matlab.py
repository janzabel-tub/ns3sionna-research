#!/usr/bin/env python3
"""Run independent MATLAB multi-stream BER/PER validation; retain raw evidence.

Exit 0: implementation and PER accuracy passed. Exit 2: completed measurements
found PER/model inaccuracies. Other exits indicate an execution/check failure.
"""
import argparse
import json
from pathlib import Path
import shutil
import subprocess
import sys

from validate_mumimo_matlab import matlab_string, sha256


def main():
    module = Path(__file__).resolve().parents[1]
    root = module.parents[1]
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--matlab", default="matlab")
    parser.add_argument("--trials", type=int, default=600)
    args = parser.parse_args()
    output = args.output.resolve()
    matlab = shutil.which(args.matlab)
    if output.exists() or args.trials < 1 or matlab is None:
        parser.error("Need a fresh output directory, positive trials and MATLAB")
    names = ("ns3_he_error_reference_export", "ns3_wifi_mimo_awgn_curve")
    binaries = [root / "build/contrib/sionna/examples" / f"ns3.40-{name}-default"
                for name in names]
    if not all(binary.is_file() for binary in binaries):
        parser.error("Build ns3_he_error_reference_export and ns3_wifi_mimo_awgn_curve first")
    sources = [Path(__file__).resolve(), module / "tools/validate_mumimo_matlab.py",
               module / "tools/validate_he_error_matlab.m",
               module / "tools/he_gray_ber_reference.m",
               module / "examples/ns3_he_error_reference_export.cc"]
    for name in ("he-effective-sinr-mapping", "he-su-mimo-link-abstraction",
                 "he-dl-mu-mimo-link-abstraction"):
        sources.extend(module / "model" / (name + ext) for ext in (".cc", ".h"))
    source_hashes = {str(p.relative_to(module)): sha256(p) for p in sources}
    artifacts = binaries + [root / "build/lib/libns3.40-sionna-default.so",
                           root / "build/lib/libns3.40-wifi-default.so"]
    binary_hashes = {str(p.relative_to(root)): sha256(p) for p in artifacts}
    expression = (f"addpath({matlab_string(module / 'tools')}); validate_he_error_matlab("
                  f"{matlab_string(binaries[0])},{matlab_string(binaries[1])},"
                  f"{matlab_string(output)},{args.trials});")
    command = [matlab, "-batch", expression]
    # Stream MATLAB's progress through a file as well as stdout.
    log_path = output.parent / (output.name + ".startup.log")
    output.parent.mkdir(parents=True, exist_ok=True)
    with log_path.open("x") as log:
        process = subprocess.Popen(command, text=True, stdout=subprocess.PIPE,
                                   stderr=subprocess.STDOUT, bufsize=1)
        for line in process.stdout:
            print(line, end="", flush=True)
            log.write(line)
            log.flush()
        code = process.wait()
    output.mkdir(exist_ok=True)
    log_path.rename(output / "matlab.log")
    source_unchanged = all(sha256(module / p) == h for p, h in source_hashes.items())
    binaries_unchanged = all(sha256(root / p) == h for p, h in binary_hashes.items())
    provenance = {"schema_version": 1, "command": command, "matlab_exit_code": code,
                  "source_sha256": source_hashes, "binary_sha256": binary_hashes,
                  "sources_unchanged_during_run": source_unchanged,
                  "binaries_unchanged_during_run": binaries_unchanged,
                  "artifacts_sha256": {p.name: sha256(p) for p in output.iterdir()
                                       if p.is_file()}}
    (output / "provenance.json").write_text(json.dumps(provenance, indent=2) + "\n")
    if code:
        return code
    if not source_unchanged or not binaries_unchanged:
        print("Sources/binaries changed during validation", file=sys.stderr)
        return 1
    report = json.loads((output / "report.json").read_text())
    return 0 if report["coded_per_accuracy_status"] == "passed" else 2


if __name__ == "__main__":
    sys.exit(main())
