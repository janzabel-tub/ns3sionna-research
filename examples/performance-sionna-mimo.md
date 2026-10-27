# TCP/MIMO performance experiments

`performance-sionna-mimo.cc` runs HE-SU TCP downlinks on one shared 5 GHz
channel. By default there are two APs and two STAs: AP 0 sends to STA 0,
and AP 1 sends to STA 1. Each pair has its own SSID and IPv4 subnet; both
pairs contend and interfere on the same channel. All devices use the same
antenna geometry, total transmit power, fixed MCS and stream count. TCP uses
Cubic by default. APs stay fixed; STAs can perform Sionna random walks.

The shell runner works from any directory. It builds once, executes sweep
configurations sequentially, and stores each run separately. It assumes a
running Sionna server unless `START_SERVER=true` is supplied. With that option,
`SERVER_PYTHON` must point to Python in your Sionna environment; only the server
started by this runner is stopped on exit. `RUN_TIMEOUT_SECONDS` limits each
run (default 1800; zero disables the limit). With `START_SERVER=true`,
`RESTART_SERVER_ON_CRASH=true` (default) restarts a crashed server, stops the
blocked simulation, and retries the same configuration. Timeouts also restart
both processes. `MAX_RETRIES=3` allows three retries per configuration; zero
or `RESTART_SERVER_ON_CRASH=false` disables retries. Failed attempts are kept
in `.failed-attempt-N` directories, and server restarts get separate logs.
Successful configurations are kept and the sweep continues. Other errors or
exhausted retries stop the sweep. Existing outputs are rejected.

`watchdog-mimo.sh` now starts and supervises the **TCP-style MIMO monolith**,
`performance-sionna-mimo-monolith.sh`. It uses one AP with a default 1/2/4-STA
sweep, HE Minstrel and beta-free BER mapping. Edit the configuration block at
the top of the watchdog for all experiment, recording, server and timeout
settings; environment variables and monolith CLI options can override them.
Optional packet recordings are disabled by default.

```bash
bash contrib/sionna/examples/watchdog-mimo.sh
# Print the resolved monolith commands without starting anything:
bash contrib/sionna/examples/watchdog-mimo.sh --dry-run
# Sweep coherence times and seeds; each invocation sweeps station counts:
COHERENCE_TIMES_MS='0 10 100' START_SEED=1 END_SEED=3 \
bash contrib/sionna/examples/watchdog-mimo.sh --prtTput=true
```

The watchdog builds the monolith once, starts a local server using
`SERVER_PYTHON`, waits for readiness and checks liveness every `CHECK_INTERVAL`
seconds (default 1). `RUN_TIMEOUT_SECONDS=1800` covers each coherence/seed
invocation including its station-count sweep; zero disables the limit. Managed
server crashes and simulation timeouts archive partial outputs and retry up to
`MAX_RETRIES=3` times. Only its owned server and simulation processes are stopped.
`START_SERVER=false` uses an existing server without managing its process.
Results are written below `OUTPUT_BASE/tcN_seedM/stas-K/`; server and per-attempt
logs are kept at the root. The output root must be new.

The remaining examples below describe the separate AP/STA-pair runner,
`performance-sionna-mimo.sh`.

From the ns-3 directory, inspect options without starting simulations:

```bash
bash contrib/sionna/examples/performance-sionna-mimo.sh --help
./ns3 run ns3sionna-performance-sionna-mimo -- --PrintHelp
```

Compare 1x1, 2x2 and 4x4 at 20 and 40 MHz with open-loop spatial multiplexing:

```bash
ANTENNAS_LIST='1 2 4' WIDTHS_MHZ='20 40' PRECODING_LIST=open-loop \
SEEDS='1 2 3' SPEEDS_MPS=0 MCS_LIST=0 EESM_BETA_LINEAR=2 \
bash contrib/sionna/examples/performance-sionna-mimo.sh
```

**Beta 2 above is a smoke-test value, not a calibration.** Multi-antenna runs
require an explicit positive `EESM_BETA_LINEAR`. Use a value calibrated for the
selected MCS and PHY context for research comparisons. SISO needs no beta.
When different sweep contexts need different betas, run separate invocations.

Compare unprecoded spatial multiplexing with ideal SVD beamforming:

```bash
ANTENNAS_LIST='2 4' WIDTHS_MHZ='20 40' PRECODING_LIST='open-loop svd' \
SPEEDS_MPS=0 EESM_BETA_LINEAR=2 \
bash contrib/sionna/examples/performance-sionna-mimo.sh
```

`STREAMS=0` (the default) selects one stream per antenna for open-loop and one
stream for SVD. To compare open-loop and SVD with the **same** stream count,
run one antenna count at a time, e.g. `ANTENNAS_LIST=4 STREAMS=4`. SVD can also
send two streams on a four-antenna link. `ARRAY_ROWS=2 ANTENNAS_LIST=4` selects
a 2x2 planar array with four ports; the default geometry is one row. Antenna
spacing defaults to half a wavelength. SVD with one antenna is skipped.

Beamforming uses equal-power per-subcarrier SVD with ideal current CSI, including
reverse HE-SU traffic carrying TCP ACKs. The transmitted precoder is selected
for the addressed receiver once per PPDU, and the same precoder is used at all
other receivers hearing that PPDU. This is an ideal CSI baseline; sounding
traffic, feedback overhead, quantization, delayed CSI, MU-MIMO and automatic
MCS/rank adaptation are not modeled by this example. Requested SVD streams
must be supported by the numerical rank of every tone; rank-deficient requests
fail rather than silently selecting fewer streams. Total device transmit power
stays fixed when the number of antennas/streams changes.

20/40/80 MHz select channels 36/38/42 automatically. Pass `--channelNumber=...`
to choose another valid 5 GHz channel for the selected width. Sionna uses the
corresponding center frequency, FFT size and bandwidth. AP/STA positions must
fit the scene and be distinct. If ray tracing finds no paths, the server warns
and returns an unreachable link with 300 dB loss, matching the janzabel branch.
Its unit-power CFR is a placeholder for serialization/SVD, not measured CSI;
the link cannot contribute useful throughput. The default positions are apartment
coordinates: `AP_POSITIONS='-1,0.5,1.5;-1.5,5.5,1.5'` and
`STA_POSITIONS='-2,1.5,1.6;-2.5,6.5,1.6'` (scene coordinates in metres). The
first pair is in the lower room and the second in the upper-right room.
Provide matching coordinates when changing scenes or pair count:

```bash
NUM_PAIRS=1 AP_POSITIONS='-1,0.5,1.5' STA_POSITIONS='-2,1.5,1.6' \
EESM_BETA_LINEAR=2 bash contrib/sionna/examples/performance-sionna-mimo.sh
```

Options can also be passed as `--name=value` after the shell script
and override its environment defaults. Use `OUTPUT_BASE` for the shell runner's
output location; `--outputDirectory` is reserved for direct C++ invocations. A `--dry-run` prints every command
without building, starting a server, or creating outputs. A single-run override such as `--antennaCount=4` also updates the configuration
directory tag and replaces that sweep dimension with the selected value.

Throughput stays in the CSV files by default. Add `--prtTput` to print aggregate
interval throughput and the final mean goodput to the console:

```bash
EESM_BETA_LINEAR=2 bash contrib/sionna/examples/performance-sionna-mimo.sh --prtTput
```

## Output and measurement definitions

Each run directory contains:

- `throughput_results.csv`: interval summaries, one row per link plus an
  aggregate row (`link_id=-1`). The default interval is 100 ms; the final
  interval can be shorter and uses its actual duration.
- `summary.csv`: per-link and aggregate received TCP payload bytes, measurement
  duration and mean goodput.
- `config.txt`: resolved parameters and the SNR averaging definition.
- `run.log`: process output, with no per-packet trace files or PCAP recording.

`throughput_results.csv` is long-form: each interval has one row per STA, keyed
by `STA_Index` and `STA_NodeId`. Plot `Time_s` versus `Throughput_Mbps` and group
by `STA_Index` to get one line per station; aggregate rows are not emitted.

`throughput_mbps = 8 * received_bytes / interval_seconds / 1e6`. Measurement
starts at `TRAFFIC_START_SECONDS` (default 1 s, allowing association warmup)
and ends at `SIMULATION_TIME_SECONDS`. TCP setup, contention and retransmissions
within this interval affect the measured goodput. Offered load is per AP.

`avg_snr_db = 10 * log10(mean(linear PHY SNR))`. It is a reception-weighted
average over addressed AP-to-STA **data PSDUs**, including payload decoding
failures where the PHY supplies SNR. Each PSDU contributes once, regardless of
its MPDU count. Beacons, control frames, overheard traffic and reverse traffic
are excluded. The PHY value includes its interference/noise calculation and,
for MIMO, the model's equivalent signal after MMSE/EESM mapping; it is not a
raw per-antenna RF SNR. Frames lost before a payload SNR is available do not
contribute. Empty intervals contain `nan`, with `snr_samples=0`. Aggregate SNR
pools the two links' samples before conversion to dB. `ENABLE_SNR=false` disables
these callbacks while keeping throughput recording.

The example retains interval counters, sinks, and one beamforming selection per
transmitter. It does not retain or write a history of individual packets.

## One-link SISO-to-SU-MIMO antenna sweep

Run one AP and one STA at matching antenna counts from 1x1 through 8x8:

```bash
bash contrib/sionna/examples/performance-sionna-sumimo-antenna-sweep.sh
```

The sweep uses fixed apartment positions, 80 MHz, HE LDPC, Minstrel and a
saturating 10 Gb/s TCP offered rate. It writes one `NxN/` directory per case
under `OUTPUT_DIR`; mobility is optional with `MOBILE_SCENARIO=true`.

## Server-free verification

A deterministic fixed-channel fixture verifies TCP delivery, interval arithmetic,
SNR aggregation and the sweep interface without ray tracing:

```bash
CHANNEL_SOURCE=fixed SPEEDS_MPS=0 MCS_LIST=0 EESM_BETA_LINEAR=2 \
ANTENNAS_LIST='1 2 4' WIDTHS_MHZ='20 40' PRECODING_LIST='open-loop svd' \
SIMULATION_TIME_SECONDS=2.35 \
bash contrib/sionna/examples/performance-sionna-mimo.sh

python3 contrib/sionna/tools/test_performance_mimo.py
```

The fixture uses the same orthogonal full-rank CFR and uniform 50 dB path loss
for every direction, with real MAC contention and TCP. It is a diagnostic mode;
its results do not predict apartment coverage or beamforming interference gain.
Static nodes are required. The default `CHANNEL_SOURCE=sionna` uses actual
ray-traced directed channels and positions.
