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
