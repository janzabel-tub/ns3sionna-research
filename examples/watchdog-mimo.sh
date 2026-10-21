#!/usr/bin/env bash
# Start the MIMO runner with its managed Sionna server watchdog enabled.
# The runner monitors server liveness, archives failed attempts, restarts the
# server and retries crashed or timed-out simulations.
set -euo pipefail

EXAMPLES_DIR=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)

if [[ ${1:-} == --help || ${1:-} == -h ]]; then
    cat <<'HELP'
Usage: watchdog-mimo.sh [--output-dir DIR] [performance-sionna-mimo options]
       watchdog-mimo.sh --outputDirectory=DIR [performance-sionna-mimo options]

Set the results root with --output-dir DIR, --outputDirectory=DIR, or OUTPUT_BASE.
Other sweep variables and --name=value options are passed to the MIMO runner.
HELP
    "$EXAMPLES_DIR/performance-sionna-mimo.sh" --help
    exit 0
fi

OUTPUT_BASE_ARG=''
RUN_ARGS=()
while (($#)); do
    case "$1" in
        --output-dir|--outputDirectory)
            if (($# < 2)) || [[ -z $2 ]]; then
                echo "[watchdog-mimo] $1 requires a directory argument" >&2
                exit 2
            fi
            OUTPUT_BASE_ARG=$2
            shift 2
            ;;
        --output-dir=*|--outputDirectory=*)
            OUTPUT_BASE_ARG=${1#*=}
            if [[ -z $OUTPUT_BASE_ARG ]]; then
                echo "[watchdog-mimo] $1 requires a directory argument" >&2
                exit 2
            fi
            shift
            ;;
        *)
            RUN_ARGS+=("$1")
            shift
            ;;
    esac
done
if [[ -n $OUTPUT_BASE_ARG ]]; then
    export OUTPUT_BASE=$OUTPUT_BASE_ARG
fi

# CPU-only, full ray tracing defaults for a static two-link 2x2 experiment.
# The offered load is 150 Mbit/s per spatial stream (300 Mbit/s per AP).
# EESM_BETA_LINEAR must still be supplied with a value calibrated for MCS 11.
export RT_FAST="${RT_FAST:-false}"
export NUM_PAIRS="${NUM_PAIRS:-2}"
export ANTENNAS_LIST="${ANTENNAS_LIST:-2}"
export STREAMS="${STREAMS:-2}"
export WIDTHS_MHZ="${WIDTHS_MHZ:-20}"
export PRECODING_LIST="${PRECODING_LIST:-open-loop svd}"
export MCS_LIST="${MCS_LIST:-11}"
export SPEEDS_MPS="${SPEEDS_MPS:-0}"
export MODE="${MODE:-2}"
export SUB_MODE="${SUB_MODE:-16}"
export OFFERED_RATE="${OFFERED_RATE:-300Mbps}"
export SIMULATION_TIME_SECONDS="${SIMULATION_TIME_SECONDS:-11}" # 1 s warmup + 10 s traffic
export ENVIRONMENT="${ENVIRONMENT:-simple_room/simple_room.xml}"
export AP_POSITIONS="${AP_POSITIONS:-2,1,1.5;4,3,1.5}"
export STA_POSITIONS="${STA_POSITIONS:-3,2,1.5;5,2,1.5}"
# Full CPU ray tracing may exceed the runner's normal 30-minute timeout.
export RUN_TIMEOUT_SECONDS="${RUN_TIMEOUT_SECONDS:-0}"

START_SERVER=${START_SERVER:-true} \
RESTART_SERVER_ON_CRASH=${RESTART_SERVER_ON_CRASH:-true} \
exec "$EXAMPLES_DIR/performance-sionna-mimo.sh" "${RUN_ARGS[@]}"
