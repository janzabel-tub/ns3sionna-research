ns3sionna
============

<img src="./doc/img/ns3sionna_logo_small_sw.jpg" width="15%" alt="Logo" style="text-align: left; vertical-align: middle;">

ns3sionna is a software module that brings realistic channel simulation using ray tracing from
Sionna (https://nvlabs.github.io/sionna/) to the widely used ns-3 network simulator (https://github.com/nsnam).

<table style="width:70%">
<tr>
<td><img src="./doc/img/munich2.png" width="75%" alt="Outdoor scenario: area around Frauenkirche in Munich" style="text-align: center; vertical-align: middle;"></td>
<td><img src="./doc/img/ex2_munich_paper.jpg" width="90%" alt="Results for outdoor scenario: trajectory of STA, CSI, Prx over time, distance vs. Prx." style="text-align: center; vertical-align: middle;"></td>
</tr>
<tr>
<td>Outdoor scenario (Munich)</td>
<td>Results from simulation</td>
</tr>
</table>

The implemented MIMO model and its validation are documented in [ns3sionna_mimo_model.md](./ns3sionna_mimo_model.md).


More information can be found in our paper: [Ns3Sionna paper](https://arxiv.org/abs/2412.20524)

Installation
============

We recommend using Linux (e.g. Ubuntu 22 or higher) and a multi-core/GPU compute node.

0. Install all dependencies required by ns-3 and those needed by our framework: ZMQ, Protocol Buffers:

```
apt-get update
apt-get install gcc g++ python3 python3-pip cmake
apt-get install libzmq5 libzmq5-dev
apt-get install libprotobuf-dev
apt-get install protobuf-compiler
apt-get install pkg-config patch

```

1. Download and install ns3

```
wget https://www.nsnam.org/releases/ns-allinone-3.40.tar.bz2
tar xf ns-allinone-3.40.tar.bz2
cd ns-allinone-3.40
```

2. Clone ns3sionna repository into contrib directory of ns3:

```
cd ./ns-3.40/contrib
git clone https://github.com/tkn-tub/ns3sionna.git ./sionna
```

Note: it is important to use the sionna as the name of the ns3sionna app directory.

3. Configure and build ns-3 project:

```
cd ../
./ns3 configure --enable-examples
./ns3 build
```

Note: ns3sionna Protocol Buffer messages (C++ and Python) are build during configure.

4. Install the server component of ns3sionna located in model/ns3sionna
   (Python 3.11 or newer is required by Sionna 2.2.0):

```
cd ./contrib/sionna/model/ns3sionna/
python3 -m venv sionna-venv
source sionna-venv/bin/activate
python3 -m pip install --upgrade -r requirements.txt
python3 tests/test_imports.py # all packages should be correctly installed
```

For a CPU-only installation, use `requirements_cpuonly.txt` instead. It selects
the official CPU-only PyTorch 2.9.1 wheel while keeping the same Sionna and
Sionna RT versions.

or using conda:

```
cd ./contrib/sionna/model/ns3sionna/
conda create -n sionna-venv python=3.12
conda activate sionna-venv
python3 -m pip install --upgrade -r requirements.txt
python3 tests/test_imports.py # all packages should be correctly installed
```

Note: ns3sionna is pinned and tested with Sionna==2.2.0 and
Sionna-RT==2.2.0. Sionna 2 uses PyTorch instead of TensorFlow; Sionna RT
selects its CUDA or LLVM/Mitsuba backend independently during import.
The matching ray-tracing dependencies are Mitsuba 3.9.1 and Dr.Jit 1.5.0.

5. Start server component of ns3sionna

```
cd ./contrib/sionna/model/ns3sionna
./run.sh
```

Note: if you experience problems with Protocol Buffers use:

```
run_python_proto.sh
```


5. Start a ns-3 example script (in separate terminal)
```
cd ns-3.40/contrib/sionna/examples
./example-sionna-sensing-mobile.sh
```

Note: you need to have matplotlib installed to see the results.

Examples
========

All examples can be found [here](./examples/).

The Phase 5.2.1 CPU benchmark sweeps a fixed 2×2 open-loop SU-MIMO link over
antenna spacing and independent TX/RX azimuths. From the ns-3 directory:

```shell
./ns3 configure --enable-examples --enable-tests
./ns3 build ns3_wifi_sumimo_benchmark sionna-test
conda run --no-capture-output -n sionna-venv python contrib/sionna/tools/benchmark_sumimo.py \
  --output=/tmp/sumimo-spacing-orientation
cd contrib/sionna/tools
conda run --no-capture-output -n sionna-venv python -m unittest -v test_benchmark_sumimo
```

Use an empty output directory. The runner calls the existing server environment
directly, so no ZMQ server process is needed. It checks two fresh ray traces per
case and exports helper configurations, replayable channels, per-tone and
summary CSV metrics, runtime, and three PNG comparisons. Generated plots stay
local and must not be committed; `.gitignore` covers `plots/` and plot files
under `data/`. The tests also replay
the checked-in [CPU reference export](./data/phase5_2_spacing_orientation/manifest.json);
set `SUMIMO_BENCHMARK_RESULTS` to validate a different complete export.
Only channel/MMSE/MI metrics are reported: the documented PER calibration covers
HE MCS 0/BCC/32 bytes/1×1. See [the model document](./ns3sionna_mimo_model.md)
for fixed controls, formulas, results, and scope.

Subphase 5.2.2 checks sample convergence for representative cases 03, 05, 06
and 10 from that benchmark. It pairs seeds 42/43/44 across 10,000, 100,000 and
1,000,000 samples per source, with two fresh traces for every case/seed/budget.
The runner exports its convergence policy before tracing, complete configurations,
replayable channels, per-tone and summary metrics, runtime, and the verdict.
From the ns-3 directory, using an empty output directory:

```shell
./ns3 build ns3_wifi_sumimo_benchmark sionna-test
conda run --no-capture-output -n sionna-venv python contrib/sionna/tools/benchmark_rt_convergence.py \
  --output=/tmp/sumimo-sample-convergence
cd contrib/sionna/tools
conda run --no-capture-output -n sionna-venv python -m unittest -v test_benchmark_rt_convergence
```

Tests default to the checked-in `data/phase5_2_sample_convergence/` CPU reference;
set `SUMIMO_CONVERGENCE_RESULTS` to validate a different complete export.
The four generated PNG comparison plots stay local under the existing ignore
rules. The [model document](./ns3sionna_mimo_model.md) gives the tolerances,
paired-seed formulas, measured results and finite-reference limits. The scope
remains channel/MMSE/MI diagnostics; the 1×1 PER calibration is not applied to 2×2.
In the checked-in reference, all 12 case/seed pairs pass at 100,000 against
1,000,000 samples. The 10,000 budget fails for case 03/seed 43 on singular values
and per-tone SINR, making 100,000 the smallest supported budget for this matrix.

Subphase 5.2.3 benchmarks the existing native cache and actual local ZMQ IPC
using a separate frozen-response Python process. It sweeps 64/256/1024
subcarriers, five TX/RX tensor shapes (including 2/4), and 1/16/64 cached links,
with three fresh native processes per case. Ray tracing is excluded.
From the ns-3 directory, with an empty output directory:

```shell
./ns3 configure --enable-examples --enable-tests
./ns3 build ns3_sionna_cache_ipc_benchmark sionna-test
conda run --no-capture-output -n sionna-venv python contrib/sionna/tools/benchmark_cache_ipc.py \
  --output=/tmp/sionna-cache-ipc
cd contrib/sionna/tools
conda run --no-capture-output -n sionna-venv python -m unittest -v test_benchmark_cache_ipc
```

The runner exports its acceptance policy before measurement, replayable tensor
templates, configurations/hashes, raw latency samples, request transcripts and
memory summaries. It validates every cached coefficient and metadata field,
actual transport bytes, hit/miss counts and the inclusive expiry boundary.
Tests default to `data/phase5_2_cache_ipc/`; set `CACHE_IPC_BENCHMARK_RESULTS`
to validate another complete export. Three PNG comparison plots remain local.
Logical payload bytes, allocator bookkeeping and resident memory are separate
metrics; the [model document](./ns3sionna_mimo_model.md) explains their limits.
The larger tensors are transport fixtures derived from a frozen 2×2 channel.
All 45 reference cases pass across three processes each. At 256 subcarriers
and 2×2, median cache hits stay near 2 µs for 1–64 links; the largest 1024/8×8/64
fixture has a 64.44 MiB wire response and a 127.5 ms median local IPC roundtrip.
These are descriptive measurements with the ranges and timer limits in the report.

Subphase 5.2.4 reuses the convergence runner for five independent TX/RX spacing
and azimuth configurations, seeds 42/43/44 and two repetitions. All cases use
100,000 samples; two selected new-angle cases also use 1,000,000 samples.
From the ns-3 directory, with an empty output directory:

```shell
./ns3 build ns3_wifi_sumimo_benchmark sionna-test
conda run --no-capture-output -n sionna-venv python contrib/sionna/tools/benchmark_rt_convergence.py \
  --independent-spacing --output=/tmp/sumimo-independent-spacing
```

The existing tests cover independent array geometry and repeatability. Every new
trace is checked against independent NumPy SVD/MMSE/MI and link-budget formulas.
All six paired-seed convergence spot checks passed. Measurements, fixtures and
plots remain local; `data/phase5_2_independent_spacing/` is ignored by Git.
The native example accepts `txSpacingLambda` and `rxSpacingLambda` overrides;
the existing shared `spacingLambda` option remains the default for both arrays.

Subphase 5.2.5 compares fresh and delayed equal-power, unquantized SVD CSI for
representative cases 03/06 at ages 0/1/2.5/5 ms, with perfect RX CSI. It uses
explicit analytic evolution `H(t)=H(0) R(9 deg/ms * t)` at fixed geometry;
timestamps alone do not change the channel. Transmission time stays at 5 ms.
This controlled TX-basis rotation measures separate-stream MMSE sensitivity;
it is not a calibrated mobility/Doppler model. From the ns-3 directory:

```shell
./ns3 build ns3_wifi_sumimo_benchmark sionna-test
conda run --no-capture-output -n sionna-venv python contrib/sionna/tools/benchmark_rt_convergence.py \
  --csi-aging --output=/tmp/sumimo-csi-aging
cd contrib/sionna/tools
SUMIMO_AGING_RESULTS=/tmp/sumimo-csi-aging conda run --no-capture-output -n sionna-venv \
  python -m unittest -v test_benchmark_sumimo test_benchmark_rt_convergence
```

Use an empty output directory. The plan exports the matrix and established
convergence tolerances before tracing. Both configurations use seeds 42/43/44,
100,000 samples and paired 1,000,000-sample checks, with two fresh repetitions.
CSV exports include CSI/transmission times, channel metrics, per-tone/effective
MMSE SINR, MI efficiency, signed degradation and runtime. Two local plots show
degradation and channel/runtime diagnostics against age. Raw measurements and
fixtures stay local; `data/phase5_2_csi_aging/` is ignored. The optional export
test uses `SUMIMO_AGING_RESULTS`; no aging dataset is required by default tests.
The native `--action=aging --csiAgeMs=5` replays an initial channel from `--input`;
`--rotationRateDegPerMs=0` provides the static-channel control. See the model
document for formulas, measured results and limits. PER remains outside this
2×2 benchmark because calibration covers only HE MCS 0/BCC/32 bytes/1×1.

Usage in HPC cluster
============

Using Docker [here](./docker/docker_howto.md)

Using singularity [here](./singularity/singularity_howto.md)

Current limitations
========
* HE SU/MU data reception defaults to parameter-free mean uncoded BER after the detector. Explicit EESM still requires caller-supplied betas; the existing calibrated 1x1 pilot selects EESM.
* Automatic sounding, MAC feedback, trigger-scheduler installation, and synchronization impairments are not modeled.
  See [the MIMO model documentation](./ns3sionna_mimo_model.md) for the exact contracts.

Contact
============
* Anatolij Zubow, TU-Berlin, zubow@tkn
* Sascha Roesler, TU-Berlin, zubow@tkn
* tkn = tkn.tu-berlin.de

How to reference ns3sionna?
============

Please use the following bibtex:

```
@techreport{zubow2024ns3-preprint,
    author = {Zubow, Anatolij and Pilz, Yannik and R{\"{o}}sler, Sascha and Dressler, Falko},
    doi = {10.48550/arXiv.2412.20524},
    title = {{Ns3 meets Sionna: Using Realistic Channels in Network Simulation}},
    institution = {arXiv},
    month = {12},
    number = {2412.20524},
    type = {cs.NI},
    year = {2024},
   }
```

CPU regression CI and provenance
===============================

The initial CPU workflow builds the current module against a fixed ns-3.40
revision with fresh Protobuf bindings, Python 3.12.12 and exact direct/transitive
versions in `model/ns3sionna/requirements_cpu_ci.lock`. After building the native
benchmark examples, `test-runner` and `sionna-test`, run `python tools/run_cpu_regressions.py`
from this module (or pass its path from ns-3). It reuses seven native suites,
server contracts, calibration/policy checks and the three existing frozen exports.
Full RT checks remain separate; build/test logs are retained by CI.

New tool runs add a versioned `run.json` sidecar (calibration: `<output>.run.json`)
with portable text/raw binary hashes and explicit execution status. Existing
exports and historical measurement source hashes are preserved. Disabling the
propagation cache now returns each new validated reply and releases retained
snapshots. See [the increment contract and limitations](doc/provenance-cpu-increment.md)
and [the model validation](ns3sionna_mimo_model.md#719-r1r3-inkrement-portable-provenienz-und-erste-cpu-ci).
This increment does not complete all of R1/R3.

The following [R3 cache increment](ns3sionna_mimo_model.md#720-zweites-r3-inkrement-cachekorrektheit-und-belegungsstatistik)
adds validity/direction/atomic-batch regressions, finite zero-lookup statistics,
current/lifetime-peak storage counters and lazy expiry cleanup across inactive
links. Future snapshots are retained. Existing IPC exports remain unchanged;
long-run budgets, copy optimization and hosted CPU validation remain open.

The next [R3 increment](ns3sionna_mimo_model.md#722-viertes-r3-inkrement-cfr-kopierreduktion-und-gettereigentum)
removes internal CFR copies from scalar/cache lookups, moves decoded storage, and
reuses each validated MU CFR by explicit user ID. Public matrix/vector results
remain independent values. To measure matrix, const loss, delay, frequency and
CSI getters separately for two fixed shapes and three processes per shape, run
from ns-3 with an empty output directory:

```shell
python contrib/sionna/tools/benchmark_cache_ipc.py --copy-profile --output=/tmp/r3-copy-profile
cd contrib/sionna/tools
CACHE_IPC_BENCHMARK_RESULTS=/tmp/r3-copy-profile python -m unittest -v test_benchmark_cache_ipc
```

The existing default sweep and frozen exports remain compatible. Profile exports
check all five getters, exact payloads, 656 hits/2 misses per process and no hidden
hit RPCs. Performance observations use frozen channels and local process IPC;
long working-set/look-ahead sweeps, transport timeouts and Live/GPU evidence remain
open Phase-5 work.

## HE pilot calibration at reception

For the validated HE MCS 0/BCC/32-byte/1×1/20 MHz/800 ns pilot:

```cpp
helper.LoadHeCalibrationProfile("contrib/sionna/data/he_mcs0_bcc_32_calibration.json");
helper.Apply(sionna, txPhyHelper, rxPhyHelper, spectrumModel);
helper.InstallHeCalibration(rxDevice); // after device creation, before Simulator::Run()
```

The 32 bytes are the complete nonaggregated PSDU, including MAC header and FCS.
The payload-only wrapper requires the default `TableBasedErrorRateModel`;
unmatched receptions use the existing error calculation. Configure applies a
small, idempotent ns-3.40 payload-context patch. Details and formulas are in
[the model contract](ns3sionna_mimo_model.md).

## TCP/MIMO experiment runner

Use [performance-sionna-mimo](examples/performance-sionna-mimo.md) for configurable
TCP downlinks with two AP/STA pairs on one channel, antenna/channel-width sweeps,
ideal SVD beamforming, and 100 ms throughput and average PHY SNR summaries.

## Native CPU reproduction (Phase 5 R1)

Use the existing Conda `sionna-venv` environment and the
[lock/system checks and bundle contract](doc/provenance-cpu-increment.md).
From ns-3, rebuild configuration consumers before running tests:

```sh
./ns3 build -j 2 sionna-test ns3_wifi_sumimo_benchmark ns3_sionna_cache_ipc_benchmark
conda run --no-capture-output -n sionna-venv python contrib/sionna/tools/check_cpu_environment.py --strict
conda run --no-capture-output -n sionna-venv python contrib/sionna/tools/experiment_bundle.py representative --output=/tmp/r1-bundle
mv /tmp/r1-bundle /tmp/r1-relocated
# Set R1_INDEX_SHA256 to the printed index_sha256, retained outside the bundle.
conda run --no-capture-output -n sionna-venv python contrib/sionna/tools/experiment_bundle.py verify --bundle=/tmp/r1-relocated --index-sha256="$R1_INDEX_SHA256"
conda run --no-capture-output -n sionna-venv python contrib/sionna/tools/experiment_bundle.py reproduce --bundle=/tmp/r1-relocated --index-sha256="$R1_INDEX_SHA256" --mode=rt --output=/tmp/r1-replay
conda run --no-capture-output -n sionna-venv python contrib/sionna/tools/run_cpu_regressions.py
```

The [artifact catalog](experiments/r1/catalog.json) identifies packaged local
5.2.4/5.2.5 studies. Export additional studies with `export --study <directory>`;
copy the entire bundle to relocate it. Historical hashes/statuses are preserved.
`SionnaHelper::AssignStreams(s)` reserves RT/mobility IDs `s`/`s+1`; native
configuration accepts `--seed`, `--run`, `--stream`. Run 1/RT stream 0 preserves
RT seeds 42/43/44. This subphase requires no containers or Docker.

## Bounded mobility storage (Phase 5 R3)

Mobility histories retain at most `--mobility_history_limit` position/velocity
pairs per node (default 1024; zero disables retention). Filled histories reuse
arrays; public getters/exported histories return independent values. Look-ahead
uses history-free motion/RNG clones and explicit epoch poses, then restores
committed state on success or forecast failure. Existing actual-time motion runs
before forecasting. `mobility_storage_statistics()` reports retained/transient
NumPy data separately from Python/RT overhead.

`SionnaPropagationCache::SetBatchPoolLimit(bytes)` bounds one empty staging vector
(default 1 MiB; zero disables). `GetPoolStatistics()` reports its actual reserve
allocations and reuse independently of CSI storage. No CFR/Protobuf pooling or
RSS/latency guarantee is claimed. R1/R2 remain closed; live scaling belongs to R4.

From the ns-3 root, using a new output directory in the existing CPU environment:

```sh
./ns3 build -j 2 sionna-test ns3_wifi_sumimo_benchmark ns3_sionna_cache_ipc_benchmark
conda run --no-capture-output -n sionna-venv python contrib/sionna/tools/benchmark_mobility_storage.py --output=/tmp/r3-storage --native-check
conda run --no-capture-output -n sionna-venv python contrib/sionna/tools/experiment_bundle.py export --study=/tmp/r3-storage --output=/tmp/r3-bundle
# Set R3_INDEX_SHA256 to the printed index_sha256, retained outside the bundle.
mv /tmp/r3-bundle /tmp/r3-relocated
conda run --no-capture-output -n sionna-venv python contrib/sionna/tools/experiment_bundle.py verify --bundle=/tmp/r3-relocated --index-sha256="$R3_INDEX_SHA256"
conda run --no-capture-output -n sionna-venv python contrib/sionna/tools/benchmark_mobility_storage.py --output=/tmp/r3-reproduced --native-check
conda run --no-capture-output -n sionna-venv python contrib/sionna/tools/run_cpu_regressions.py
```

The [R3 catalog](experiments/r3/catalog.json) identifies local complete payloads.
The fixed 15-case storage plan compares 20,000 updates per case with an identically
seeded history-disabled twin; elapsed time is descriptive. The native fixture
checks 512 uncached RPCs, allocation reuse, byte bounds and independent getters.

## R4 live CPU scaling

The live benchmark sends real HE-SU PPDUs through SpectrumWifiPhy, the production
Sionna cache and a fresh local Sionna RT server process. It records raw protocol
bytes, real PHY callbacks, cache/RPC counts, cold and subsequent latency samples,
and separate server/native RSS and high-water observations. Its explicit beta 2
is a diagnostic setting; it does not extend the HE calibration profile.

From ns-3, in the documented Conda CPU environment and with a new output path:

```shell
./ns3 configure --enable-examples --enable-tests
./ns3 build -j 2 ns3_sionna_live_benchmark sionna-test
python contrib/sionna/tools/benchmark_live_scaling.py --output=/tmp/r4-live
LIVE_SCALING_RESULTS=/tmp/r4-live python contrib/sionna/tools/run_cpu_regressions.py
```

The default acceptance matrix has 14 settings and three fresh process pairs per
setting; `--quick --epochs=2` checks a small cache pair. The runner protects
existing output, preserves failure logs and reaps subprocesses. `--samples`,
`--epochs`, `--repeats` and `--timeout` support explicit custom measurements;
custom/quick runs are separate from the default acceptance. Production PHY guard
bands expand 20/40 MHz to 768/1536 CFR bins. Latency and RSS are descriptive
measurements, with no universal performance limit. See the
[R4 measurement contract](doc/r4-live-scaling.md) and
[local artifact catalog](experiments/r4/catalog.json). R5 physical CPU channel aging is complete. R6 implementation and CPU checks are
complete; actual CUDA hardware acceptance remains pending.

## R6 CPU/CUDA comparison and RTX 3060 remote script

Use the active Conda environment and the existing ns-3 build. Copy the local
`experiments/r6/r6-existing-ns3-update.tar.gz` to the GPU machine, then run:

```sh
tar -xzf r6-existing-ns3-update.tar.gz
conda activate sionna2
bash r6-env-update/run_r6_gpu.sh --use-existing-build \
  --source "$HOME/ns-allinone-3.40/ns-3.40" \
  --output "$PWD/r6-results-existing"
```

If the preflight lists missing R6 benchmark executables, compile those targets
once in the existing configured checkout:

```sh
cd "$HOME/ns-allinone-3.40/ns-3.40"
./ns3 build -j 2 ns3_wifi_sumimo_benchmark ns3_sionna_cache_ipc_benchmark ns3_sionna_live_benchmark
```

This is an incremental target build using the existing build tree and its
current dependencies. The general program can already work while these separate
benchmark programs have never been built. The launcher does not build them
automatically. Afterward, rerun with a fresh output directory; keep the failed
run's logs. If a target is unknown, the selected checkout lacks that benchmark's
source or CMake registration; report the error instead of starting a clean build.

Keep the three files in `r6-env-update/` together. `--use-existing-build` also
reuses the active Python environment (Python >=3.11). It does not configure or
compile ns-3, regenerate bindings, extract another source tree, create a venv
or install packages. Existing R6 benchmark/test binaries and bindings must be
present; missing files cause an explicit failure without an automatic build.
Only the three companion runner files are updated in the selected checkout.
Use a new output directory for each run.

The runner records installed package versions, deviations from the reference
lock, and existing binary/binding hashes. It checks that CPU/CUDA environments
match, runs CPU regressions and actual paired RT traces, audits every native
result, and packages the relocated reproduction payload. Return
`r6-results-existing/r6-results.tar.gz` for independent audit.

The fixed matrix, scientific tolerances, supported Sionna RT stack, optional
isolated CI setup and provenance details are in
[the R6 contract](doc/r6-backend-completion.md). The
[artifact catalog](experiments/r6/catalog.json) retains the trusted identities.
Local CPU acceptance passed; actual GPU acceptance and CUDA CI execution remain
pending. CPU-only runs and packages explicitly record `gpu_pending`.

From ns-3, CPU diagnostics can also reuse the active environment directly:

```sh
python contrib/sionna/tools/benchmark_backends.py run --cpu-only --use-existing-environment --output=/tmp/r6-cpu
python contrib/sionna/tools/benchmark_backends.py validate --output=/tmp/r6-cpu --native
```

HE bit-error averaging
----------------------

`MEAN_UNCODED_BER` is the default for supported HE SISO, SU and MU reception.
The common mapper supports BPSK, QPSK and exact Gray square QAM through 1024-QAM
using symbol SINR; coded PER comes from the existing ns-3 error model.
`LEGACY_SISO` preserves the original 1x1 scalar payload path. Supplying a beta
does not select EESM. See model section 5.20 for equations, limits and the pilot
comparison. The historical EESM examples explicitly select EESM.

```shell
./ns3 run ns3_wifi_ber_mapping
./ns3 run "ns3_wifi_ber_mapping --reference=contrib/sionna/data/he_mcs0_bcc_32_matlab_r2026a.json"
```
