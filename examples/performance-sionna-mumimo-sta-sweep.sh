#!/usr/bin/env bash
# One eight-antenna AP, one to eight single-antenna STAs.
set -euo pipefail

EXAMPLES_DIR=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
NS3_DIR=$(cd -- "$EXAMPLES_DIR/../../.." && pwd)
RUNNER="$EXAMPLES_DIR/performance-sionna-mumimo-monolith.sh"

if [[ ${1:-} == --help || ${1:-} == -h ]]; then
    cat <<'HELP'
Usage: [ENV=value ...] bash performance-sionna-mumimo-sta-sweep.sh [--name=value ...]
       bash performance-sionna-mumimo-sta-sweep.sh --dry-run

Runs one eight-antenna AP with 1, 2, ..., 8 single-antenna STAs in separate
simulations. The one-STA case is a current-CSI MRT TCP baseline; cases with
2--8 STAs use eligible full-band MU-MIMO groups, one stream per selected STA,
ZF precoding and ideal-CSI MU rates; SU fallback uses Minstrel.
MU_GROUPING=fixed MU_RATE_CONTROL=station-manager restores the strict legacy case. Apartment STAs use the existing randomized same-room positions.
The native MU scheduler is limited to 20 MHz and HE MCS 0--9.
REVERSE_PATH=reciprocal-he (default) enables single-stream HE uplink with
ideal reciprocal CFRs and all AP receive antennas; legacy24 restores the baseline.

Defaults: ENVIRONMENT=apartment/apartment.xml MOBILE_SCENARIO=false
MOBILE_SPEED=1 DATA_RATE=600 (Mb/s per STA) RATE_ADAPTATION=minstrel
ANTENNA_COUNT=8 ARRAY_ROWS=2 CHANNEL_WIDTH_MHZ=20 CHANNEL=36 PRECODING=zf TX_POWER=20
OUTPUT_DIR=<ns-3>/results/mumimo-sta-sweep/<timestamp>
BUILD=true SIMULATION_TIME_SECONDS=10 TRAFFIC_START_SECONDS=1
ENABLE_SNR=false ENABLE_POSITIONS=false ENABLE_RATE_TRACE=false

Pass runner options such as --mobile_scenario=true or --simulationTimeSeconds=20
to override individual settings. The results root must not already exist.
Each case is written to OUTPUT_DIR/N-sta/run/stas-N/.
throughput_results.csv is long-form: one row per STA per interval, with
STA_Index and STA_NodeId columns. Plot Time_s against Throughput_Mbps and group
by STA_Index (or STA_NodeId); each N-STA case therefore yields N series.
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

OUTPUT_DIR=${OUTPUT_DIR:-"$NS3_DIR/results/mumimo-sta-sweep/$(date +%Y%m%d-%H%M%S)-$$"}
if [[ $OUTPUT_DIR != /* ]]; then OUTPUT_DIR="$PWD/$OUTPUT_DIR"; fi
BUILD=${BUILD:-true}
[[ $BUILD == true || $BUILD == false ]] || { echo '[sweep] BUILD must be true or false' >&2; exit 2; }

if [[ $DRY_RUN == false ]]; then
    [[ ! -e $OUTPUT_DIR ]] || { echo "[sweep] results root already exists: $OUTPUT_DIR" >&2; exit 2; }
    mkdir -p -- "$OUTPUT_DIR"
    if [[ $BUILD == true ]]; then
        (cd -- "$NS3_DIR" && ./ns3 build ns3sionna-performance-sionna-mumimo-monolith -j 2)
        BUILD=false
    fi
fi

for stas in {1..8}; do
    case_dir="$OUTPUT_DIR/${stas}-sta"
    printf '[sweep] %s STA(s) -> %s\n' "$stas" "$case_dir"
    runner_args=("${forwarded[@]}"
        "--sim_min_stas=$stas" "--sim_max_stas=$stas"
        "--antennaCount=8" "--streams=1" "--channelWidthMhz=20"
        "--outputDirectory=$case_dir")
    runner_prefix=()
    [[ $DRY_RUN == false ]] || runner_prefix+=(--dry-run)
    ENVIRONMENT=${ENVIRONMENT:-apartment/apartment.xml} \
    MOBILE_SCENARIO=${MOBILE_SCENARIO:-false} \
    MOBILE_SPEED=${MOBILE_SPEED:-1} \
    DATA_RATE=${DATA_RATE:-600} \
    RATE_ADAPTATION=${RATE_ADAPTATION:-minstrel} \
    REVERSE_PATH=${REVERSE_PATH:-reciprocal-he} \
    PRECODING=${PRECODING:-zf} \
    ARRAY_ROWS=${ARRAY_ROWS:-2} \
    ANTENNA_SPACING_LAMBDA=${ANTENNA_SPACING_LAMBDA:-0.5} \
    CHANNEL_WIDTH_MHZ=${CHANNEL_WIDTH_MHZ:-20} \
    CHANNEL=${CHANNEL:-36} \
    TX_POWER=${TX_POWER:-20} \
    LDPC=${LDPC:-false} \
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
