#!/usr/bin/env bash
# SISO-to-SU-MIMO TCP/UDP sweep: one AP and one STA, from 1x1 through 8x8.
set -euo pipefail

EXAMPLES_DIR=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
NS3_DIR=$(cd -- "$EXAMPLES_DIR/../../.." && pwd)
RUNNER="$EXAMPLES_DIR/performance-sionna-mimo-monolith.sh"

if [[ ${1:-} == --help || ${1:-} == -h ]]; then
    cat <<'HELP'
Usage: [ENV=value ...] bash performance-sionna-sumimo-antenna-sweep.sh [--name=value ...]
       bash performance-sionna-sumimo-antenna-sweep.sh --dry-run

Runs one AP and one STA at matching antenna counts 1x1 through 8x8. All eight
cases use the same apartment positions and one flow (TRANSPORT=tcp by default; udp for saturated downlink). Counts 2--8
use ideal-CSI SVD; Minstrel can use up to the antenna count
of streams. Defaults use 20 MHz, HE LDPC and a high offered application rate.

Defaults: ENVIRONMENT=apartment/apartment.xml MOBILE_SCENARIO=false
MOBILE_SPEED=1 DATA_RATE=10000 RATE_ADAPTATION=minstrel TRANSPORT=tcp PRECODING=SVD-for-2--8
CHANNEL_WIDTH_MHZ=20 CHANNEL=40 ARRAY_ROWS=1 TX_POWER=20 LDPC=true
AP_POSITION='-1,0.5,1.5' STA_POSITIONS='-2,1.5,1.6'
OUTPUT_DIR=<ns-3>/results/sumimo-antenna-sweep/<timestamp>
BUILD=true SIMULATION_TIME_SECONDS=10 TRAFFIC_START_SECONDS=1
ENABLE_SNR=false ENABLE_POSITIONS=false ENABLE_RATE_TRACE=false

Pass runner options such as --mobile_scenario=true or --simulationTimeSeconds=20
to override individual settings. The results root must not already exist.
Each case is written to OUTPUT_DIR/NxN/stas-1/.
throughput_results.csv is long-form: one row per STA per interval, with
STA_Index and STA_NodeId columns. Plot Time_s against Throughput_Mbps and group
by STA_Index (or STA_NodeId); no aggregate-throughput rows are written.
HELP
    exit 0
fi

DRY_RUN=false
forwarded=()
for option in "$@"; do
    case "$option" in
        --dry-run) DRY_RUN=true ;;
        --outputDirectory=*)
            echo '[sweep] OUTPUT_DIR is owned by this sweep; set it with the OUTPUT_DIR environment variable' >&2
            exit 2
            ;;
        *) forwarded+=("$option") ;;
    esac
done

OUTPUT_DIR=${OUTPUT_DIR:-"$NS3_DIR/results/sumimo-antenna-sweep/$(date +%Y%m%d-%H%M%S)-$$"}
if [[ $OUTPUT_DIR != /* ]]; then OUTPUT_DIR="$PWD/$OUTPUT_DIR"; fi
BUILD=${BUILD:-true}
[[ $BUILD == true || $BUILD == false ]] || { echo '[sweep] BUILD must be true or false' >&2; exit 2; }

if [[ $DRY_RUN == false ]]; then
    [[ ! -e $OUTPUT_DIR ]] || { echo "[sweep] results root already exists: $OUTPUT_DIR" >&2; exit 2; }
    mkdir -p -- "$OUTPUT_DIR"
    if [[ $BUILD == true ]]; then
        (cd -- "$NS3_DIR" && ./ns3 build ns3sionna-performance-sionna-mimo-monolith -j 2)
        BUILD=false
    fi
fi

for antennas in {1..8}; do
    case_dir="$OUTPUT_DIR/${antennas}x${antennas}"
    printf '[sweep] %sx%s -> %s\n' "$antennas" "$antennas" "$case_dir"
    precoding=open-loop
    if (( antennas > 1 )); then
        precoding=svd
    fi
    runner_args=("${forwarded[@]}"
        "--sim_min_stas=1" "--sim_max_stas=1"
        "--antennaCount=$antennas" "--streams=$antennas"
        "--outputDirectory=$case_dir")
    runner_prefix=()
    [[ $DRY_RUN == false ]] || runner_prefix+=(--dry-run)
    ENVIRONMENT=${ENVIRONMENT:-apartment/apartment.xml} \
    AP_POSITION=${AP_POSITION:--1,0.5,1.5} \
    STA_POSITIONS=${STA_POSITIONS:--2,1.5,1.6} \
    MOBILE_SCENARIO=${MOBILE_SCENARIO:-false} \
    MOBILE_SPEED=${MOBILE_SPEED:-1} \
    DATA_RATE=${DATA_RATE:-10000} \
    RATE_ADAPTATION=${RATE_ADAPTATION:-minstrel} \
    PRECODING="$precoding" \
    ARRAY_ROWS=${ARRAY_ROWS:-1} \
    ANTENNA_SPACING_LAMBDA=${ANTENNA_SPACING_LAMBDA:-0.5} \
    CHANNEL_WIDTH_MHZ=${CHANNEL_WIDTH_MHZ:-20} \
    CHANNEL=${CHANNEL:-40} \
    TX_POWER=${TX_POWER:-20} \
    LDPC=${LDPC:-true} \
    SIMULATION_TIME_SECONDS=${SIMULATION_TIME_SECONDS:-10} \
    TRAFFIC_START_SECONDS=${TRAFFIC_START_SECONDS:-1} \
    REPORT_INTERVAL_MS=${REPORT_INTERVAL_MS:-100} \
    OUTPUT_DIR="$case_dir" BUILD="$BUILD" \
    ENABLE_SNR=${ENABLE_SNR:-false} \
    ENABLE_POSITIONS=${ENABLE_POSITIONS:-false} \
    ENABLE_RATE_TRACE=${ENABLE_RATE_TRACE:-false} \
    bash "$RUNNER" "${runner_prefix[@]}" "${runner_args[@]}"
done

printf '[sweep] completed; results are in %s\n' "$OUTPUT_DIR"
