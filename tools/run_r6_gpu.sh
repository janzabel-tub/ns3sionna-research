#!/usr/bin/env bash
# Run on the CUDA host as a regular user. No sudo or system changes are automatic.
set -euo pipefail

usage() {
  cat <<'EOF'
Usage:
  bash run_r6_gpu.sh --bundle BUNDLE --index-sha256 HASH --output NEW_DIRECTORY
  bash run_r6_gpu.sh --source NS3_SOURCE --output NEW_DIRECTORY
  bash run_r6_gpu.sh --use-existing-environment --bundle BUNDLE --index-sha256 HASH --output NEW_DIRECTORY

Prerequisites: Linux x86_64, NVIDIA driver >=535 (nvidia-smi), Python 3.12,
Python venv support, g++, CMake, pkg-config, patch, LLVM, ZeroMQ and Protobuf.
On Ubuntu 24.04 install once, if needed:
  sudo apt-get update
  sudo apt-get install -y python3-venv g++ cmake ninja-build pkg-config patch llvm libzmq3-dev cppzmq-dev libprotobuf-dev protobuf-compiler
No CUDA toolkit or GPU PyTorch wheel is needed for Sionna RT.
Set R6_PYTHON to a Python executable (Python 3.12.12 is the reference).
The bundle must be unpacked; copy this script separately and retain the hash
from the local catalog. --source is for an already trusted source checkout.
Results, source, isolated venv and logs are retained under NEW_DIRECTORY.
With --use-existing-environment, activate your existing Conda environment first.
This mode requires Python >=3.11, reuses its interpreter and installs nothing.
It records all package versions and deviations from the reference lock. Keep
benchmark_backends.py and test_benchmark_backends.py beside this script when
using an older source bundle; their applied hashes are recorded separately.
EOF
}

r6_bundle=''
r6_hash=''
r6_source=''
r6_output=''
r6_existing=0
r6_support_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
while (( $# )); do
  case "$1" in
    --bundle) r6_bundle="${2:?missing bundle}"; shift 2 ;;
    --index-sha256) r6_hash="${2:?missing hash}"; shift 2 ;;
    --source) r6_source="${2:?missing source}"; shift 2 ;;
    --output) r6_output="${2:?missing output}"; shift 2 ;;
    --use-existing-environment) r6_existing=1; shift ;;
    -h|--help) usage; exit 0 ;;
    *) usage >&2; exit 2 ;;
  esac
done
if [[ -z "$r6_output" ]] || { [[ -z "$r6_source" ]] && [[ -z "$r6_bundle" ]]; } ||
   { [[ -n "$r6_source" ]] && [[ -n "$r6_bundle" ]]; }; then
  usage >&2
  exit 2
fi
if [[ -e "$r6_output" ]]; then
  echo 'Output already exists; use a new directory to preserve results.' >&2
  exit 2
fi
if [[ "$(id -u)" == 0 ]]; then
  echo 'Run as a regular user; ns-3 does not support running this workflow as root.' >&2
  exit 2
fi
mkdir -p "$r6_output"
r6_output="$(cd "$r6_output" && pwd)"
mkdir "$r6_output/logs"
exec > >(tee "$r6_output/logs/remote.log") 2>&1
trap 'r6_exit=$?; printf "%s\n" "$r6_exit" > "$r6_output/exit-code.txt"; if (( r6_exit )); then echo "R6 failed; retain $r6_output/logs for diagnosis."; fi' EXIT

r6_python="${R6_PYTHON:-python3}"
if (( r6_existing )); then
  "$r6_python" -c 'import sys; assert sys.version_info >= (3,11), "Python >=3.11 required"'
else
  "$r6_python" -c 'import sys; assert sys.version_info[:2] == (3,12), "Python 3.12 required (3.12.12 reference)"'
fi
nvidia-smi --query-gpu=name,uuid,driver_version,memory.total --format=csv
for r6_tool in g++ cmake pkg-config patch protoc; do
  command -v "$r6_tool" >/dev/null
done
pkg-config --modversion libzmq protobuf

if [[ -n "$r6_bundle" ]]; then
  "$r6_python" - "$r6_bundle" "$r6_hash" "$r6_output" <<'PY'
import hashlib, json, pathlib, re, sys, tarfile
bundle = pathlib.Path(sys.argv[1]).resolve()
expected = sys.argv[2]
output = pathlib.Path(sys.argv[3])
assert re.fullmatch('[0-9a-f]{64}', expected), 'External trusted index SHA-256 required'
assert not bundle.is_symlink()
index_raw = (bundle / 'index.json').read_bytes()
assert hashlib.sha256(index_raw).hexdigest() == expected, 'Trusted index hash differs'
index = json.loads(index_raw)
assert (index['schema'], index['schema_version']) == ('ns3sionna.experiment-bundle', 1)
files = set()
directories = set()
for path in bundle.rglob('*'):
    assert not path.is_symlink(), f'Symlink refused: {path}'
    if path.is_file():
        files.add(path.relative_to(bundle).as_posix())
    elif path.is_dir():
        directories.add(path.relative_to(bundle).as_posix())
assert files == set(index['files']) | {'index.json'}, 'Bundle inventory differs'
assert directories == set(index['directories']), 'Bundle directory inventory differs'
for name, identity in index['files'].items():
    relative = pathlib.PurePosixPath(name)
    assert not relative.is_absolute() and '..' not in relative.parts and '\\' not in name
    raw = (bundle / name).read_bytes()
    assert hashlib.sha256(raw).hexdigest() == identity['raw_sha256'], f'File hash differs: {name}'
archives = [name for name in index['files'] if name.endswith('/ns3-source.tar.gz')]
assert len(archives) == 1, 'Exactly one R6 source archive required'
with tarfile.open(bundle / archives[0], 'r:gz') as archive:
    for member in archive.getmembers():
        name = pathlib.PurePosixPath(member.name)
        assert name.parts[0] == 'ns-3.40' and '..' not in name.parts and not name.is_absolute()
        assert member.isfile(), 'Only ordinary source files accepted'
    archive.extractall(output, filter='data')
(output / 'input-index-sha256.txt').write_text(expected + '\n')
print('Verified bundle and extracted source snapshot:', len(index['files']), 'files')
PY
  r6_source="$r6_output/ns-3.40"
else
  r6_source="$(cd "$r6_source" && pwd)"
fi

if (( r6_existing )); then
  r6_python="$("$r6_python" -c 'import sys; print(sys.executable)')"
  export NS3SIONNA_R6_ENVIRONMENT_MODE=existing
  export PATH="$(dirname "$r6_python"):$PATH"
  # Verify the original bundle first; apply this explicit runner update to the
  # extracted source, preserving the trusted input bundle and its identities.
  "$r6_python" - "$r6_support_dir" "$r6_source" "$r6_output/logs/runner-overlay.json" <<'UPDATE'
import hashlib, json, pathlib, shutil, sys
support, source, report = map(pathlib.Path, sys.argv[1:])
updates = {}
for name in ('run_r6_gpu.sh', 'benchmark_backends.py', 'test_benchmark_backends.py'):
    origin = support / name
    target = source / 'contrib/sionna/tools' / name
    if not origin.is_file():
        raise SystemExit(f'Missing companion {origin}; unpack the entire runner update beside the script')
    original_hash = hashlib.sha256(target.read_bytes()).hexdigest()
    updated_hash = hashlib.sha256(origin.read_bytes()).hexdigest()
    if origin.resolve() != target.resolve():
        shutil.copyfile(origin, target)
    updates[name] = {'original_sha256': original_hash, 'applied_sha256': updated_hash}
report.write_text(json.dumps({'mode': 'existing', 'python_executable': sys.executable,
                              'python_version': sys.version, 'overlays': updates}, indent=2) + '\n')
print('Reusing existing interpreter:', sys.executable, '; no packages will be installed')
UPDATE
else
  "$r6_python" -m venv "$r6_output/venv"
  r6_python="$r6_output/venv/bin/python"
  "$r6_python" -m pip install pip==26.0.1
  "$r6_python" -m pip install --no-deps -r "$r6_source/contrib/sionna/model/ns3sionna/requirements_cpu_ci.lock"
  "$r6_python" -m pip check
  export PATH="$r6_output/venv/bin:$PATH"
fi
cd "$r6_source"
rm -f contrib/sionna/model/message.pb.cc contrib/sionna/model/message.pb.h contrib/sionna/model/ns3sionna/common/message_pb2.py
./ns3 configure --enable-examples --enable-tests --enable-modules=sionna,wifi,internet,applications,buildings 2>&1 | tee "$r6_output/logs/configure.log"
./ns3 build -j 2 test-runner sionna-test ns3_wifi_sumimo_benchmark ns3_sionna_cache_ipc_benchmark ns3_sionna_live_benchmark 2>&1 | tee "$r6_output/logs/build.log"
"$r6_python" contrib/sionna/tools/check_cpu_environment.py > "$r6_output/logs/environment.json"
"$r6_python" contrib/sionna/tools/run_cpu_regressions.py 2>&1 | tee "$r6_output/logs/regressions.log"
"$r6_python" contrib/sionna/tools/benchmark_backends.py probe --backend=cuda | tee "$r6_output/logs/cuda-probe.json"
"$r6_python" contrib/sionna/tools/benchmark_backends.py run --output="$r6_output/study" --timeout=3600 | tee "$r6_output/logs/comparison.json"
"$r6_python" contrib/sionna/tools/benchmark_backends.py validate --output="$r6_output/study" --native > "$r6_output/logs/native-audit.json"
(cd contrib/sionna/tools && BACKEND_COMPARISON_RESULTS="$r6_output/study" "$r6_python" -m unittest -v test_benchmark_backends) 2>&1 | tee "$r6_output/logs/backend-tests.log"
"$r6_python" contrib/sionna/tools/benchmark_backends.py package --output="$r6_output/study" --bundle="$r6_output/bundle" > "$r6_output/logs/bundle-export.json"
mv "$r6_output/bundle" "$r6_output/bundle-relocated"
r6_result_hash="$("$r6_python" -c 'import json,sys; print(json.load(open(sys.argv[1]))["index_sha256"])' "$r6_output/logs/bundle-export.json")"
"$r6_python" contrib/sionna/tools/experiment_bundle.py verify --bundle="$r6_output/bundle-relocated" --index-sha256="$r6_result_hash" > "$r6_output/logs/bundle-verify.json"
for r6_backend in 00 01; do
  "$r6_python" contrib/sionna/tools/benchmark_backends.py audit --study="$r6_output/bundle-relocated/studies/$r6_backend" --native > "$r6_output/logs/relocated-$r6_backend.json"
done
printf '%s\n' "$r6_result_hash" > "$r6_output/result-index-sha256.txt"
r6_return_files=(logs study result-index-sha256.txt)
if [[ -f "$r6_output/input-index-sha256.txt" ]]; then
  r6_return_files+=(input-index-sha256.txt)
fi
tar -czf "$r6_output/r6-results.tar.gz" -C "$r6_output" "${r6_return_files[@]}"
echo "R6 passed. Send back $r6_output/r6-results.tar.gz and retain bundle-relocated with its external index hash."
