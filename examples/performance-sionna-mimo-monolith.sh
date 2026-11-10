#!/usr/bin/env bash
# TCP-style one-AP/many-STA runner. A Sionna server must already be running.
# Like performance-sionna-tcp.sh, accepts environment defaults and CLI overrides.
set -euo pipefail
EXAMPLES_DIR=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
NS3_DIR=$(cd -- "$EXAMPLES_DIR/../../.." && pwd)

if [[ ${1:-} == --help || ${1:-} == -h ]]; then
    cat <<'HELP'
Usage: [ENV=value ...] bash performance-sionna-mimo-monolith.sh [--name=value ...]
       bash performance-sionna-mimo-monolith.sh --dry-run [--name=value ...]

One AP sends one TCP (default) or UDP flow to each STA on one shared channel. Throughput is
recorded per station. By default compare 1, 2 and 4 STAs; no beta is needed.
NUM_STAS=N selects one station count. Otherwise SIM_MIN_STAS=1 SIM_MAX_STAS=4
select a doubling sweep (1,2,4,...). Extra CLI options override env defaults.

Old TCP environment variables:
  MODE=2 NUM_RES=16 MOBILE_SCENARIO=false MOBILE_SPEED=1 COHERENCE_TIME=0
  SEED=1 DATA_RATE=600 RATE_ADAPTATION=minstrel MCS=11 TX_POWER=20 CACHING=true
  ENVIRONMENT=apartment/apartment.xml OUTPUT_DIR=<ns-3>/results/mimo-monolith/<timestamp>
  ENABLE_SNR=false ENABLE_POSITIONS=false ENABLE_RATE_TRACE=false

MIMO and timing variables:
  ANTENNA_COUNT=2 STREAMS=0 PRECODING=open-loop ARRAY_ROWS=1
  ANTENNA_SPACING_LAMBDA=0.5 CHANNEL_WIDTH_MHZ=20 CHANNEL=0 NOISE_FIGURE_DB=7
  SIMULATION_TIME_SECONDS=10 TRAFFIC_START_SECONDS=1 REPORT_INTERVAL_MS=100
  TRANSPORT=tcp (or udp) TCP_VARIANT=ns3::TcpCubic RNG_RUN=1 LDPC=true
  AP_POSITION='x,y,z' STA_POSITIONS='x,y,z;x,y,z' (optional scene overrides)
  SERVER_URL=tcp://localhost:5555 BUILD=true VERBOSE=false PRINT_THROUGHPUT=false
  RUN_TIMEOUT_SECONDS=1800 (wall-clock limit; 0 disables it)
  ENABLE_PCAP=false ENABLE_MIMO_TRACE=false ENABLE_THROUGHPUT_10MS=false

Minstrel adapts HE MCS, NSS, bandwidth and GI using the beta-free BER mapping.
STREAMS is its NSS ceiling (fixed NSS with RATE_ADAPTATION=fixed). STREAMS=0
uses the antenna count for open-loop and one for SVD. Open-loop stream s uses
antenna port s; all RX ports remain active. LDPC=true enables all HE MCSs.
MCS is used only with RATE_ADAPTATION=fixed.
All packet diagnostics are disabled by default.
PCAP may produce large files. The default report interval is 100 ms.
Each count writes stas-N/{throughput_results.csv,summary.csv,config.txt,
computationtime.csv}; process output goes to run.log at OUTPUT_DIR.
CLI flags use old TCP spelling: --mobile_scenario, --mobile_speed, --sub_mode,
--txPower, --minstrelRateTrace. Use --prtTput=true for live station throughput.
For all C++ flags, run the ns-3 target with --PrintHelp.
The wall-clock limit covers the whole simulation invocation, including its
station-count sweep; build time is excluded. On expiry, GNU timeout sends TERM
to the simulation process group, then KILL after 5 seconds if needed. Partial
outputs are kept. The external Sionna server is left running.
This runner uses an existing server; start it using your normal Sionna setup.
It does not start, monitor, or restart the server.
HELP
    exit 0
fi
DRY_RUN=false
if [[ ${1:-} == --dry-run ]]; then DRY_RUN=true; shift; fi
RUN_TIMEOUT_SECONDS=${RUN_TIMEOUT_SECONDS:-1800}
[[ $RUN_TIMEOUT_SECONDS =~ ^[0-9]+$ ]] || {
    echo '[runner] RUN_TIMEOUT_SECONDS must be a non-negative integer (0 disables it)' >&2
    exit 2
}
OUTPUT_DIR=${OUTPUT_DIR:-"$NS3_DIR/results/mimo-monolith/$(date +%Y%m%d-%H%M%S)-$$"}
# Resolve relative output paths against the caller, before changing directory.
if [[ $OUTPUT_DIR != /* ]]; then OUTPUT_DIR="$PWD/$OUTPUT_DIR"; fi
args=(
    "--sim_min_stas=${SIM_MIN_STAS:-${NUM_STAS:-1}}"
    "--sim_max_stas=${SIM_MAX_STAS:-${NUM_STAS:-4}}"
    "--environment=${ENVIRONMENT:-apartment/apartment.xml}"
    "--mobile_scenario=${MOBILE_SCENARIO:-false}" "--mobile_speed=${MOBILE_SPEED:-1}"
    "--caching=${CACHING:-true}" "--mode=${MODE:-2}" "--sub_mode=${NUM_RES:-16}"
    "--fixedCoherenceTimeMs=${COHERENCE_TIME:-0}" "--dataRate=${DATA_RATE:-600}"
    "--rateAdaptation=${RATE_ADAPTATION:-minstrel}" "--mcs=${MCS:-11}"
    "--txPower=${TX_POWER:-20}" "--seed=${SEED:-1}" "--run=${RNG_RUN:-1}"
    "--antennaCount=${ANTENNA_COUNT:-2}" "--streams=${STREAMS:-0}"
    "--precoding=${PRECODING:-open-loop}" "--arrayRows=${ARRAY_ROWS:-1}"
    "--antennaSpacingLambda=${ANTENNA_SPACING_LAMBDA:-0.5}"
    "--channelWidthMhz=${CHANNEL_WIDTH_MHZ:-20}" "--channel=${CHANNEL:-0}"
    "--noiseFigureDb=${NOISE_FIGURE_DB:-7}" "--ldpc=${LDPC:-true}"
    "--simulationTimeSeconds=${SIMULATION_TIME_SECONDS:-10}"
    "--trafficStartSeconds=${TRAFFIC_START_SECONDS:-1}" "--reportIntervalMs=${REPORT_INTERVAL_MS:-100}"
    "--transport=${TRANSPORT:-tcp}"
    "--tcpVariant=${TCP_VARIANT:-ns3::TcpCubic}" "--serverUrl=${SERVER_URL:-tcp://localhost:5555}"
    "--snr=${ENABLE_SNR:-false}" "--positions=${ENABLE_POSITIONS:-false}"
    "--minstrelRateTrace=${ENABLE_RATE_TRACE:-false}" "--pcap=${ENABLE_PCAP:-false}"
    "--mimoTrace=${ENABLE_MIMO_TRACE:-false}" "--throughput10ms=${ENABLE_THROUGHPUT_10MS:-false}"
    "--prtTput=${PRINT_THROUGHPUT:-false}" "--verbose=${VERBOSE:-false}"
)
if [[ -n ${AP_POSITION:-} ]]; then args+=("--apPosition=$AP_POSITION"); fi
if [[ -n ${STA_POSITIONS:-} ]]; then args+=("--staPositions=$STA_POSITIONS"); fi
# The runner owns the result root so run.log follows any output override.
for option in "$@"; do
    case "$option" in
        --outputDirectory=*) OUTPUT_DIR=${option#*=} ;;
    esac
done
if [[ -z $OUTPUT_DIR ]]; then echo '[runner] output directory cannot be empty' >&2; exit 2; fi
if [[ $OUTPUT_DIR != /* ]]; then OUTPUT_DIR="$PWD/$OUTPUT_DIR"; fi
args+=("$@" "--outputDirectory=$OUTPUT_DIR")
command=(./ns3 run --no-build ns3sionna-performance-sionna-mimo-monolith -- "${args[@]}")
if [[ ! $RUN_TIMEOUT_SECONDS =~ ^0+$ ]]; then
    command=(timeout --signal=TERM --kill-after=5 "$RUN_TIMEOUT_SECONDS" "${command[@]}")
fi
if [[ $DRY_RUN == true ]]; then
    printf 'cd %q\n' "$NS3_DIR"
    printf '%q ' "${command[@]}"
    printf '\n'
    exit 0
fi
# Help must not build, start a server or create output directories.
for option in "$@"; do
    case "$option" in
        --PrintHelp|--PrintHelp=*)
            cd -- "$NS3_DIR"
            exec "${command[@]}"
            ;;
    esac
done
if [[ -e $OUTPUT_DIR ]]; then echo "[runner] output directory already exists: $OUTPUT_DIR" >&2; exit 2; fi
cd -- "$NS3_DIR"
if [[ ! $RUN_TIMEOUT_SECONDS =~ ^0+$ ]]; then
    command -v timeout >/dev/null || { echo '[runner] GNU timeout is required for RUN_TIMEOUT_SECONDS' >&2; exit 2; }
fi
if [[ ${BUILD:-true} == true ]]; then ./ns3 build ns3sionna-performance-sionna-mimo-monolith -j 2; fi
mkdir -p -- "$OUTPUT_DIR"
printf '[runner] %s -> %s\n' "${RUN_LABEL:-manual}" "$OUTPUT_DIR"
status=0
"${command[@]}" 2>&1 | tee "$OUTPUT_DIR/run.log" || status=$?
if [[ ! $RUN_TIMEOUT_SECONDS =~ ^0+$ && ( $status == 124 || $status == 137 ) ]]; then
    printf '[runner] simulation stopped (exit %s; wall-clock limit %s s, kill grace 5 s); partial outputs: %s\n' \
        "$status" "$RUN_TIMEOUT_SECONDS" "$OUTPUT_DIR" | tee -a "$OUTPUT_DIR/run.log" >&2
fi
exit "$status"
