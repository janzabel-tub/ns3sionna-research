# Portable provenance and native CPU reproduction

R1 completes one native CPU subphase: explicit seed/run/stream mapping,
relocatable experiment bundles and lightweight lock/system checks. Acceptance
requires a relocated bundle to verify and a small RT/native experiment to
reproduce in the existing Conda CPU environment, followed by sequential CPU
regressions. Containers, Docker, resume and GPU validation are outside this scope.
Historical measurement hashes and the closed R2 scope remain unchanged.
## Run manifest v1 and migration

New benchmark runs add `run.json` (`schema: ns3sionna.run`, `schema_version: 1`).
Calibration CLI runs add `<output>.run.json`. The original `manifest.json`,
calibration result schema, CSVs and Protobuf exports retain their formats.
Historical exports are not rewritten. Their source hashes remain identities of
the measurement-time sources, even when today's tests replay them with another
revision. A historical export without a sidecar has an **unknown execution
status**; existing scientific verdicts remain available and are not promoted to
new execution provenance. No source hash is replaced with today's source hash.

The sidecar records the configuration and its sorted-JSON SHA-256, original
command, native argument vectors, input digests, actual backend, Python/packages,
platform/CPU information, compiler/CMake/protoc versions, native executable and
available CMake build-file digests. Native runs also record the ns-3 version
and its Git revision/status/diff digest when available; an unpacked release tree
has an explicit null revision and is not claimed to be a pristine checkout. `measurement_sources` captures Git revision,
local status, tracked diff digest and source-file digests before execution.
Successful benchmark sidecars also copy existing case/configuration/seed records
and scientific verdicts, and identify the legacy manifest's version/subphase.
Relative source and output paths support relocation; absolute command/build/input
paths are descriptive host provenance, not portable identity keys.

## Seed/run/stream mapping v1

`SionnaHelper::AssignStreams(s)` reserves two consecutive nonnegative int64 IDs:
RT `s`, mobility `s+1`, returning 2. The default is 0/1; callers reserve a disjoint
range for other ns-3 random objects. `PopulateRandomness` snapshots the actual
`RngSeedManager` seed/run into `SimInitMessage.randomness`; the benchmark exposes
`--seed`, `--run`, `--stream`. Distinct IDs and positive seed/run are validated.
Python consumes that tuple before loading the scene. This maps RNG domains;
it does not emulate ns-3's MRG32k3a or assert statistical independence.

For domain `rt` or `mobility`, the derived uint32 is the first four SHA-256 bytes,
big endian, of ASCII `ns3sionna-rng-v1:<seed>:<run>:<stream>:<domain>` (decimal
integers, no newline). RT at Run 1/stream 0 uses the ns-3 seed directly, preserving
42/43/44. Both PathSolver paths receive the effective RT seed. Mobility uses its
own effective seed and per-node `SeedSequence([mobility_seed,node_id,0x51A0])`;
other-node draws and RT draws cannot advance this generator. Repeated traces of
the same tuple intentionally share randomness; they are not independent samples.
Absent fields mean legacy mapping v0, retaining old RT and mobility behavior.
Unknown versions/invalid tuples fail. New `run.json` records actual tuples and
effective seeds separately from historical configuration/measurement hashes.
RT records the actual Mitsuba variant, including backend rejection. GPU validation
belongs to R6; frozen replay and calibration never claim a fresh RT backend.

Statuses are `incomplete`, `failed`, `success`. Before setup, an atomic JSON
replacement creates `incomplete` with no finish time. Normal completion seals
artifact hashes and writes `success`; this describes execution, while acceptance
remains under `validation`. Exceptions write `failed` plus a local traceback;
subprocess failures include captured stdout/stderr. Interruption writes
`incomplete` plus a finish time/log; abrupt termination leaves the initial
`incomplete` record. Command lists are sealed at exit without adding file I/O to
native timing. A disk failure can prevent the final log/status write and leaves
an incomplete record. Atomic replacement does not promise power-loss durability
of the parent directory.

Nonempty benchmark directories and existing calibration outputs/sidecars are
rejected without modification. Resume is deliberately unsupported, so mismatched
inputs cannot be resumed accidentally. CLI parsing or executable discovery errors
before a run begins do not create a run record. Only successful runs receive a
complete artifact index; partial files and failure logs remain local for diagnosis.

## Hash contract

Every file has `raw_sha256` and its raw byte length. Known UTF-8 text extensions
also have `text_hash_version: 1` and `lf_sha256`: decode strict UTF-8, replace CRLF
with LF, encode UTF-8, then SHA-256. Lone CR, whitespace, BOM, contents and final
newline are preserved. Portable verification uses the versioned text digest and
reports whether raw bytes also match; unsupported versions and content changes
are rejected. Protobufs are always verified against raw bytes and never normalized.
`.gitattributes` fixes historical CSV/JSON checkouts to LF and disables text
conversion for Protobuf/PDF files. Historical raw hashes retain their original
meaning; they are not silently reinterpreted as canonical hashes.

## Relocatable bundles

`tools/experiment_bundle.py export --study <export> --output <empty-directory>`
copies original configurations, channels, results, manifests and logs verbatim.
It includes scene XML/includes, meshes/materials/textures and all recorded run
inputs; missing or incompatible inputs fail (supply recovered bytes with `--input`).
Recorded configuration/channel and raw measurement identities are checked without
replacing historical source hashes. `index.json` contains only relative payload
locators and the complete hash inventory. Keep its printed raw SHA-256 outside
the bundle: [the trusted catalog](../experiments/r1/catalog.json) records local
artifact access. It claims no hosted download. Copy the entire bundle directory
and retain the catalog separately; original host paths remain descriptive only.

`verify --bundle <relocated> --index-sha256 <trusted-hash>` authenticates the index,
checks dependency closure and rejects path escapes, symlinks, missing/extra files
and changed content. LF-text matches may differ in raw bytes and report those
paths; binary hashes must match exactly. Index JSON itself requires its trusted
raw hash. Verification does not promote unknown/failed historical execution.

`representative --output <bundle> --samples 10000` reuses the established native
configuration, CPU trace and NumPy/native diagnostics for one fixed 2x2 link.
`reproduce --bundle <relocated> --index-sha256 <hash> --mode rt --output <empty>`
loads only bundled inputs and compares one indexed experiment's scientific
metrics at rtol 1e-4/atol 1e-7; rank, tone index and frequency are exact. Runtime
and cross-machine byte equality are not acceptance criteria. `--mode frozen`
replays the saved channel; `--record` selects another channel, including aging.
New execution/validation sidecars belong to replay output; the bundle is unchanged.
Creation failures retain a partial failed study, never a complete bundle marker.
The representative budget demonstrates reproduction, not sample convergence.

## Native CPU environment

Use the existing `sionna-venv` Conda environment. System dependencies are GCC/G++,
CMake, pkg-config, patch, libzmq3-dev, cppzmq-dev, libprotobuf-dev,
protobuf-compiler and a loadable LLVM library (Ubuntu: `llvm`). Python 3.12.12,
pip 26.0.1 and the existing exact lock define the reference environment; install
with `python -m pip install --no-deps -r requirements_cpu_ci.lock`, then `pip check`.
`tools/check_cpu_environment.py --strict` reports JSON and fails on pin deviations,
missing tools/libraries/bindings or an actual non-LLVM backend. Without `--strict`,
version deviations are reported while dependency/backend failures still fail.
Binding freshness is timestamp evidence; the targeted native build must regenerate
bindings and rebuild benchmarks after C++/Protobuf changes. No packages are installed
by the checker. System package versions are recorded, not hermetically locked.

## CPU CI and local reproduction

`.github/workflows/cpu.yml` checks out the current module and immutable ns-3.40
revision `4fdc985b5d5b800992e0ce87df7a727e32033794`, uses Ubuntu 24.04 and Python
3.12.12, installs 55 exact Python package versions from
`model/ns3sionna/requirements_cpu_ci.lock` with `--no-deps`, and runs `pip check`.
The lock is the installed dependency closure for this Linux/Python combination;
it excludes unrelated notebook/TensorFlow/CUDA packages. Actions are pinned to
commits. System apt packages follow Ubuntu updates and wheel hashes are not yet
locked. This is initial CPU CI, not a fully hermetic environment.

Each CI job removes generated bindings, configures/builds from scratch, then runs
`tools/run_cpu_regressions.py`: seven native SU/DL/UL/CSI/RU/adapter suites,
package/backend checks, provenance/calibration tests, server data-contract,
coherence and mobility tests, policies, native analytic controls and the three
existing frozen export regressions, including local frozen RPC. Three expensive
fresh RT tests are named as separate validation; optional 5.2.4/5.2.5 tests still
require their local exports. An always-run CI artifact step retains available install/configure/build/test logs;
unexpected native subprocess errors include captured stdout/stderr.
No complete RT sweep, timing acceptance or new dataset is part of this job.

From an ns-3 directory after installing the lock:

```sh
./ns3 configure --enable-examples --enable-tests --enable-modules=sionna,wifi,internet,applications,buildings
./ns3 build -j 2 test-runner sionna-test ns3_wifi_sumimo_benchmark ns3_sionna_cache_ipc_benchmark
python contrib/sionna/tools/run_cpu_regressions.py
```

## Disabled-cache behavior

Previously, disabling caching skipped the initial lookup, but responses still
accumulated in the cache and the final lookup selected an older valid snapshot.
The focused native REQ/REP regression constructs five distinct overlapping valid
replies in memory. Before the fix it observed loss 41 instead of 42, delay 1 ns
instead of 3 ns, and CFR `(1,-1)` instead of `(4,-4)`; re-enabling also reused 41.
Only four RPCs reached the peer instead of five. The failure log stays in `/tmp`.

Disabling now clears previously retained entries. Every disabled request validates
and decodes its entire reply batch before returning a matching new entry, with
inclusive validity and the existing direction/reciprocity rule. It stores no
snapshots, including other receivers and look-ahead entries. A reply without a
valid requested link raises an explicit error. Re-enabling caching starts cold
and the next enabled request populates the cache normally. The focused test checks
fresh loss/delay/CFR, absence of retained snapshots, and subsequent enabled hits.
Enabled-cache selection and its historical benchmark policy remain unchanged.

## Results and remaining work

Local validation passed against freshly fetched ns-3.40 and freshly generated
C++/Python Protobuf bindings: seven native suites (50 cases), 57 selected Python
tests (56 passed; one optional local aging-export test skipped), plus package/
actual LLVM backend checks. Three expensive fresh RT tests were excluded explicitly.
All 55 installed pins and pip 26.0.1 matched the lock/bootstrap; `pip check` passed.
The one-case/three-process frozen IPC smoke and real calibration CLI replay also
produced successful sidecars with verified artifact hashes. The historical MAE
remains 0.054886 against 0.10. The workflow YAML and action pins were checked;
the hosted GitHub job has not been executed here. After including binary plots
in the complete artifact index, all nine focused provenance checks passed again.
PNG hashes remain raw-byte hashes. The model records the details.
Builds, logs and generated measurements remain under `/tmp`; no new datasets or
recorded fixtures are committed. Existing historical exports are reused.

R1 now provides native seed/run/streams, complete local study bundles and their
trusted catalog, representative relocation/reproduction and lock/system checks.
Resume remains unsupported. Container recipes and GPU validation are outside this
native CPU scope. The initial CI results above remain historical.
R3 now closes the remaining mobility-history and native staging-pool work.
History arrays and one empty staging allocation have explicit retention bounds
and actual reuse counters; history-free look-ahead clones restore committed
mobility and RNGs after forecast failures. The existing actual-time motion update
precedes forecasting and is not transactional. Cache correctness/budget/copy
measurements below remain historical; live scaling, Doppler and GPU evidence
remain R4–R6 work. R2 stays closed.

The subsequent [R3 cache increment](../ns3sionna_mimo_model.md#713-phase-5-ergebnisse-und-verbleibende-grenzen)
implements those correctness regressions, counters and lazy inactive-link cleanup.
It records new validation separately; the first increment's results above remain historical.

## R1 native acceptance, 7 October 2026

The strict check matched all 55 pins, Python/pip and actual LLVM execution.
Seven fresh seed/run/stream traces retained default 42/43/44 and repeated tuple
channel bytes. The relocated representative verified 20 files and passed fresh
RT/native and frozen replay with the original scene root unavailable. The local
5.2.4/5.2.5 bundle verified 129 files and passed one frozen replay per study.
Seven native suites (58 cases) and 98 Python tests passed sequentially (96 passed,
two optional exports skipped). Historical measurements and R2 remain unchanged.

## R3 storage acceptance, 7 October 2026

The fixed 15-case plan compared 300000 updates at caps 0/1/8/64/1024 and
WALL/TIME/DISTANCE (Run 1, seeds 42/43/44, streams 0/1). All trajectories,
control values and RNG states matched; slot IDs were reused and retained array
bytes stayed at 48 per position/velocity pair (49152 bytes at cap 1024).
The existing native fixture served 512 uncached RPCs with one staging allocation,
511 hits and 336 bytes retained within 1 MiB. Forecast tests and six small real
LLVM traces preserved serialized responses to the pre-R3 implementation.
Seven native suites (60 cases) and 122 Python tests passed sequentially
(120 passed, two optional local exports skipped); the strict lock/backend check
passed. The relocated 39-file bundle verified from /tmp using system Python;
a fresh native CPU storage run reproduced all deterministic result fields.

The [R3 catalog](../experiments/r3/catalog.json) retains the trusted index hash
and local relative paths. Existing Run status/validation separation and raw/LF
hash conventions are unchanged. New measurements stay local; historical hashes
and R1/R2 are preserved. Commands and byte-accounting exclusions are in the
[module README](../README.md#bounded-mobility-storage-phase-5-r3).
