# R6: backend comparison and reproduction contract

R6 is one implementation increment. Hardware acceptance requires real LLVM and
CUDA traces; a CPU-only run leaves GPU validation pending and does not close R6.
The fixed matrix is `simple_room`, 5180 MHz, 20 MHz/256 bins, explicit 2x2
arrays at 0.5 lambda, positions (3,2,2)/(4,3,1) m, TX/RX azimuths 90/90 and
90/0 degrees, seeds 42/43/44, run 1, streams 0/1, 100000 samples, depth 3,
two repetitions, and one LLVM thread. Other budgets/repetitions are diagnostics.

Each backend runs in a fresh process. Mitsuba's exact
`llvm_ad_mono_polarized` or `cuda_ad_mono_polarized` variant is selected before
importing Sionna/server classes. A real Dr.Jit evaluation must return [2,3].
No automatic fallback is accepted. CUDA driver/device observations, actual
variant, package versions, configuration/channel/metric hashes, raw channels,
native commands, initialization and CFR wall times are retained. The shared
CPU lock is intentional: Sionna RT CUDA runs through Mitsuba/Dr.Jit; PyTorch
remains on CPU so both sides use the identical Python environment. No GPU
PyTorch PHY calibration or GPU ns-3 execution is claimed. The explicit
`--use-existing-environment` mode also supports an already installed Conda
environment (Python >=3.11, including CUDA-enabled PyTorch). It records all
installed package versions and deviations from the reference lock, keeps the
Sionna/Sionna RT 2.0.1, Mitsuba 3.8.0 and Dr.Jit 1.3.1 requirements, and requires
identical recorded Python/package environments for the paired CPU/CUDA runs.
This is a measured existing environment, not an exact-lock reproduction.

The existing native open-loop MMSE/MI analyzer evaluates every response.
Independent NumPy checks apply the established SINR tolerances 1e-8, singular
value tolerances 1e-10, and MI rtol 1e-9/atol 1e-8. Paired CPU/CUDA metrics use
the existing convergence policy: wideband loss 0.5 dB, zero changed-rank tones,
singular values rtol 0.05/atol 1e-7, per-tone SINR 1 dB, effective SINR 0.5 dB,
MI 0.2 bit/s/Hz, and SINR floor 1e-12 before logarithms. Every tone, seed and
repetition must pass. Within each backend the same metric policy checks repeats;
byte identities are reported separately, not required across GPU executions.
Complex CFR relative error is diagnostic, since path discovery and ordering can
differ. Timing is descriptive, without a speedup requirement or convergence claim.

The explicit self-hosted CUDA workflow runs sequential CPU regressions first,
then both backends, comparison, portable packaging, relocation and frozen native
replay. It is dispatch-only because a privileged self-hosted runner must not
execute untrusted pull requests. Runner labels are `self-hosted,linux,x64,gpu`;
the host provides NVIDIA driver, Python 3.12.12 and documented native libraries.
The workflow uses an isolated venv and the existing exact CPU lock. Missing
CUDA is a failed GPU job, never a successful skip. CI execution itself must be
reported separately from local workflow implementation.

From ns-3 in the existing CPU Conda environment:

    ./ns3 build -j 2 sionna-test ns3_wifi_sumimo_benchmark ns3_sionna_cache_ipc_benchmark ns3_sionna_live_benchmark
    python contrib/sionna/tools/benchmark_backends.py run --cpu-only --output=/tmp/r6-cpu
    python contrib/sionna/tools/benchmark_backends.py validate --output=/tmp/r6-cpu --native

On a CUDA host omit `--cpu-only`. A child failure preserves its logs and failed
run sidecar. `probe --backend=cuda` checks CUDA without measuring a study.
Use a new output directory for every run; `--timeout` bounds each worker.

The `package` command exports successful backend studies and optional historical
`--study` directories with the existing trusted bundle contract. It also includes
a complete local ns-3 source archive (including the current module, patches,
lock, tests and source data), source inventory/hashes and reproduction commands.
Build outputs, Git metadata, caches, plots, PDFs, upstream documentation figure
trees and local experiment payloads
are excluded. Historical study bytes/statuses are preserved. External package
repositories and the native libraries remain installation prerequisites.
The archive captures local dirty source bytes; it is not a claim of a pristine
upstream checkout. Verify the bundle against an index hash retained outside it
before extracting the archive into a new directory. Its source snapshot has
`ns-3.40/` at its root. The packaged source's own runner can replay all saved
native channels, or execute a fresh backend study; exact wall times are not
reproduction targets. CPU-only packaging retains `gpu_pending` explicitly.

## RTX 3060 remote run

For the existing `sionna2` Conda environment on `hpc3`, the corrected runner
installs no packages and creates no venv. Copy the small update separately
to `experiments/r6/r6-env-update/`; the original trusted bundle remains unchanged.
From the transfer directory, run:

    conda activate sionna2
    bash r6-env-update/run_r6_gpu.sh --use-existing-environment \
      --bundle "$PWD/bundle" \
      --index-sha256 d983c092468268785e544d842c461907adcc069e43af6e7a905e1144c73a487b \
      --output "$PWD/r6-results-3"

Locally prepared archive: `experiments/r6/r6-existing-environment-update.tar.gz`.
Extract it in the transfer directory to obtain `r6-env-update/`. The three
companion files (`run_r6_gpu.sh`, `benchmark_backends.py`,
`test_benchmark_backends.py`) must stay together. The original bundle is verified
before the updated runner/tests are copied into the extracted source. Separate
`logs/runner-overlay.json` records original/applied hashes and the actual
interpreter. Regenerated bindings, native build, CPU regression, CUDA execution,
scientific checks and result packaging still run. No changes to the installed
Conda packages are made. Use a fresh output directory after each failure.

The first existing-environment remote regression exposed an incomplete overlay:
the two Python companions were updated but the extracted shell runner still
rejected `--use-existing-environment`. The corrected overlay copies all three
companions, including `run_r6_gpu.sh`, and records their original/applied hashes.
The shell-path regression now starts with stale versions of all three files and
checks every copied file and hash. This failure occurred before the fresh
CPU/CUDA study; it does not constitute a GPU measurement.

The follow-up archive is `experiments/r6/r6-runner-overlay-update.tar.gz`.
After transferring and extracting it, reuse the already compiled source while
retaining the failed logs and starting a new results directory:

    bash r6-env-update/run_r6_gpu.sh --use-existing-environment \
      --source "$PWD/r6-results-3/ns-3.40" --output "$PWD/r6-results-4"

This uses the source already verified against the original trusted bundle.
Configure/build still run, regenerate bindings and reuse the existing build.
The original input bundle and its index hash remain unchanged.

The initial Python-3.12 guard was too restrictive for an existing environment;
simply deleting it still left venv creation and the incompatible NumPy 2.5.2
pin active. The explicit existing-environment mode corrects both paths rather
than relaxing channel, numerical or backend acceptance. The original isolated
exact-lock workflow below remains available for provisioned CI.

Copy the prepared `experiments/r6/r6-gpu-input.tar.gz` to the GPU machine. Keep
the trusted index SHA-256 from `experiments/r6/catalog.json` separately. The
archive contains `bundle/`, `run_r6_gpu.sh`, and `INDEX_SHA256.txt` for convenient
comparison with that catalog. It captures this implementation and local source
changes; cloning the upstream ns-3 tree alone is insufficient.

On Linux x86_64 with NVIDIA driver >=535 and Python 3.12 (3.12.12 is the reference),
install missing native prerequisites once:

    sudo apt-get update
    sudo apt-get install -y python3-venv g++ cmake ninja-build pkg-config patch llvm libzmq3-dev cppzmq-dev libprotobuf-dev protobuf-compiler

Then, as a regular user, in a new transfer directory:

    tar -xzf r6-gpu-input.tar.gz
    bash run_r6_gpu.sh --bundle "$PWD/bundle" \
      --index-sha256 "<trusted hash from catalog>" --output "$PWD/r6-results"

Set `R6_PYTHON=/path/to/python3.12` if the default `python3` is another version.
The script checks the GPU, verifies every input hash, extracts the source safely,
creates a venv, installs the shared exact package lock, regenerates bindings,
builds native targets with two jobs, runs CPU regressions, probes actual CUDA,
measures both backends, audits all saved responses, runs R6 corruption tests,
exports a portable result bundle, moves it, verifies integrity and repeats all
native diagnostics at the relocated paths. The output directory must be new.

Return `r6-results/r6-results.tar.gz`, containing logs, raw CPU/CUDA studies and
the externally retained result index hash. Retain `bundle-relocated/` on the
GPU machine as the complete source/result reproduction package. If the script
fails, return `logs/`, `exit-code.txt` and any partial `study/`; the logs identify
the failed step. A failed scientific comparison remains failed even if every
trace executed. The local catalog must be updated with independently audited
remote results before claiming R6 hardware acceptance.

Sionna RT delegates CUDA to Mitsuba/Dr.Jit; an NVIDIA driver is sufficient without
installing the CUDA SDK ([Dr.Jit core documentation](https://github.com/mitsuba-renderer/drjit-core)).
The driver prerequisite follows [Mitsuba installation](https://mitsuba.readthedocs.io/en/stable/).
API selection was verified against the installed Sionna RT 2.0.1 source; newer
online Sionna documentation does not change this experiment's pins.

## Local implementation validation

The fixed CPU matrix produced 12 fresh traces and 3072 native tone evaluations.
All NumPy and within-backend metric comparisons passed. The actual CUDA probe
failed on the local host, as expected; no GPU result or CI execution is claimed.
The strict CPU lock/system check passed. Seven native suites and 165 Python
tests ran sequentially (160 passed, five unrelated optional export tests skipped).
The final R6 suite separately passed all 13 tests, including the added remote
hash/extraction checks and native replay of all 12 CPU responses. Timings are
descriptive: initialization 2.43–7.51 ms, CFR 658.62–767.89 ms, native subprocess
31.25–36.55 ms. Relocation, source inventory and final PDF evidence are recorded
in the local R6 catalog. Hardware acceptance and CUDA CI execution remain pending.
