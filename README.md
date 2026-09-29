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
apt-get install pkg-config

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
   (Python 3.11 or newer is required by Sionna 2.0.1):

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

Note: ns3sionna is pinned and tested with Sionna==2.0.1 and
Sionna-RT==2.0.1. Sionna 2 uses PyTorch instead of TensorFlow; Sionna RT
selects its CUDA or LLVM/Mitsuba backend independently during import.

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
* HE SU-MIMO and the documented DL-/UL-MU-MIMO abstractions require explicit CSI and EESM calibration.
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
