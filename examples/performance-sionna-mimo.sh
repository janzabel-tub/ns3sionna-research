#!/usr/bin/env bash
# Single runs or sequential comparison sweeps, from any working directory.
set -euo pipefail
EXAMPLES_DIR=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
NS3_DIR=$(cd -- "${EXAMPLES_DIR}/../../.." && pwd)

if [[ ${1:-} == --help ]]; then
    cat <<'HELP'
Usage: [ENV_VAR=value ...] bash performance-sionna-mimo.sh [extra ns-3 options]
       bash performance-sionna-mimo.sh --dry-run [extra ns-3 options]

Sweep variables (space-separated values):
  ANTENNAS_LIST="1 2 4"  WIDTHS_MHZ="20 40"  PRECODING_LIST="open-loop svd"
  MCS_LIST="0"  SEEDS="1 2 3"  SPEEDS_MPS="0 1"  TX_POWERS_DBM="20"
Defaults: 2 antennas, 20 MHz, open-loop, MCS 11, seed 1, speed 1, power 20.
SVD with 1 antenna is skipped. STREAMS=0 selects all antennas for open-loop
and one for SVD; use STREAMS=2 or 4 for SVD spatial multiplexing.

Other variables:
  EESM_BETA_LINEAR        Required for multi-antenna runs; no implicit calibration
  NUM_PAIRS=2            Two APs, each sending TCP to its own STA on one channel
  SIMULATION_TIME_SECONDS=10  TRAFFIC_START_SECONDS=1  REPORT_INTERVAL_MS=100
  OFFERED_RATE=1Gbps     TCP_VARIANT=ns3::TcpCubic  ENABLE_SNR=true
  ENVIRONMENT=apartment/apartment.xml  SERVER_URL=tcp://localhost:5555
  AP_POSITIONS='-1,0.5,1.5;2,0.5,1.5'  STA_POSITIONS='-2,1.5,1.6;3,1.5,1.6'
  ARRAY_ROWS=1  ANTENNA_SPACING_LAMBDA=0.5  NOISE_FIGURE_DB=7  RNG_RUN=1
  CACHING=true  COHERENCE_TIME_MS=0  MODE=1  SUB_MODE=1
  OUTPUT_BASE=<ns-3>/results/mimo/<timestamp>  BUILD=true  RUN_TIMEOUT_SECONDS=1800
  CHANNEL_SOURCE=sionna  (fixed requires SPEEDS_MPS=0; no server needed)
  START_SERVER=false     Set true to start and stop a local Sionna server
  SERVER_PYTHON=python3  RT_FAST=true  (only used with START_SERVER=true)
  RESTART_SERVER_ON_CRASH=true  MAX_RETRIES=3  (requires START_SERVER=true)
  Crashes/timeouts retry the current run; failed attempts and server logs are kept.

Extra --name=value options are forwarded to every run; see the C++ --PrintHelp
for all options. Each configuration writes interval CSV, summary CSV, config,
and run log into its own directory. Existing result files are never overwritten.
HELP
    exit 0
fi
DRY_RUN=false
if [[ ${1:-} == --dry-run ]]; then
    DRY_RUN=true
    shift
fi

read -r -a antennas <<< "${ANTENNAS_LIST:-2}"
read -r -a widths <<< "${WIDTHS_MHZ:-20}"
read -r -a precoders <<< "${PRECODING_LIST:-open-loop}"
read -r -a mcs_values <<< "${MCS_LIST:-11}"
read -r -a seeds <<< "${SEEDS:-1}"
read -r -a speeds <<< "${SPEEDS_MPS:-1}"
read -r -a powers <<< "${TX_POWERS_DBM:-20}"
# Explicit single-run options override sweep dimensions and their directory tags.
for option in "$@"; do
    case "$option" in
        --antennaCount=*) antennas=("${option#*=}") ;;
        --channelWidthMhz=*) widths=("${option#*=}") ;;
        --precoding=*) precoders=("${option#*=}") ;;
        --mcs=*) mcs_values=("${option#*=}") ;;
        --seed=*) seeds=("${option#*=}") ;;
        --mobileSpeedMps=*) speeds=("${option#*=}") ;;
        --txPowerDbm=*) powers=("${option#*=}") ;;
        --channelSource=*) CHANNEL_SOURCE=${option#*=} ;;
        --serverUrl=*) SERVER_URL=${option#*=} ;;
        --eesmBetaLinear=*) EESM_BETA_LINEAR=${option#*=} ;;
        --outputDirectory=*) echo 'Use OUTPUT_BASE to choose the runner output directory' >&2; exit 2 ;;
    esac
done
OUTPUT_BASE=${OUTPUT_BASE:-"${NS3_DIR}/results/mimo/$(date +%Y%m%d-%H%M%S)-$$"}
if [[ $OUTPUT_BASE != /* ]]; then OUTPUT_BASE="$PWD/$OUTPUT_BASE"; fi
CHANNEL_SOURCE=${CHANNEL_SOURCE:-sionna}
START_SERVER=${START_SERVER:-false}
RUN_TIMEOUT_SECONDS=${RUN_TIMEOUT_SECONDS:-1800}
RESTART_SERVER_ON_CRASH=${RESTART_SERVER_ON_CRASH:-true}
MAX_RETRIES=${MAX_RETRIES:-3}
[[ $MAX_RETRIES =~ ^[0-9]+$ ]] || { echo 'MAX_RETRIES must be a non-negative integer' >&2; exit 2; }
[[ $RUN_TIMEOUT_SECONDS =~ ^[0-9]+$ ]] || { echo 'RUN_TIMEOUT_SECONDS must be a non-negative integer' >&2; exit 2; }

if [[ $DRY_RUN == false ]]; then
    cd -- "$NS3_DIR"
    if [[ ${BUILD:-true} == true ]]; then ./ns3 build ns3sionna-performance-sionna-mimo -j 2; fi
fi
server_pid=''
run_pid=''
server_starts=0
stop_run() {
    if [[ -n $run_pid ]]; then
        # setsid isolates this run and its ns-3 child in an owned process group.
        kill -TERM -- "-$run_pid" 2>/dev/null || true
        kill -KILL -- "-$run_pid" 2>/dev/null || true
        kill -KILL "$run_pid" 2>/dev/null || true
        wait "$run_pid" 2>/dev/null || true
        run_pid=''
    fi
}
stop_server() {
    if [[ -n $server_pid ]]; then
        kill "$server_pid" 2>/dev/null || true
        wait "$server_pid" 2>/dev/null || true
        server_pid=''
    fi
}
cleanup() { stop_run; stop_server; }
trap cleanup EXIT
trap 'exit 130' INT
trap 'exit 143' TERM
start_server() {
    # Fail before launching if another process already owns the local endpoint.
    "${SERVER_PYTHON:-python3}" - <<'PORT_CHECK' || return 2
import socket
with socket.socket() as probe:
    probe.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    try:
        probe.bind(('127.0.0.1', 5555))
    except OSError as error:
        raise SystemExit('Port 5555 is already in use; use START_SERVER=false: ' + str(error))
PORT_CHECK
    mkdir -p -- "$OUTPUT_BASE" || return 2
    server_starts=$((server_starts + 1))
    server_log="$OUTPUT_BASE/server.log"
    if [[ $server_starts -gt 1 ]]; then server_log="$OUTPUT_BASE/server-$server_starts.log"; fi
    server_args=(--model_folder="$EXAMPLES_DIR/../model/ns3sionna/models")
    if [[ ${RT_FAST:-true} == true ]]; then server_args+=(--rt_fast); fi
    # exec preserves the PID so cleanup stops only the server owned by this run.
    (cd -- "$EXAMPLES_DIR/../model/ns3sionna"; exec "${SERVER_PYTHON:-python3}" -u ns3sionna_server.py "${server_args[@]}") \
        > "$server_log" 2>&1 &
    server_pid=$!
    local ready=false startup_check
    for ((startup_check=0; startup_check<120; startup_check++)); do
        if ! kill -0 "$server_pid" 2>/dev/null; then
            echo "[runner] server failed; see $server_log" >&2
            tail -n 30 -- "$server_log" >&2
            return 2
        fi
        if grep -q 'Sionna server socket ready' "$server_log"; then
            ready=true
            break
        fi
        sleep 0.25
    done
    if [[ $ready != true ]]; then
        echo "[runner] server was not ready within 30 seconds; see $server_log" >&2
        return 2
    fi
}
managed_server=false
if [[ $START_SERVER == true && $CHANNEL_SOURCE == sionna && $DRY_RUN == false ]]; then
    [[ ${SERVER_URL:-tcp://localhost:5555} == tcp://localhost:5555 ]] || {
        echo 'START_SERVER supports the local tcp://localhost:5555 endpoint' >&2; exit 2;
    }
    managed_server=true
    start_server || exit 2
fi

for ant in "${antennas[@]}"; do
for width in "${widths[@]}"; do
for pre in "${precoders[@]}"; do
    if [[ $ant == 1 && $pre == svd ]]; then
        echo '[runner] skipping 1x1 SVD (no transmit array to beamform)'
        continue
    fi
    if [[ $ant != 1 && -z ${EESM_BETA_LINEAR:-} ]]; then
        echo '[runner] set EESM_BETA_LINEAR explicitly for multi-antenna experiments' >&2
        exit 2
    fi
for mcs in "${mcs_values[@]}"; do
for seed in "${seeds[@]}"; do
for speed in "${speeds[@]}"; do
for power in "${powers[@]}"; do
    tag="${ant}x${ant}_${width}MHz_${pre}_mcs${mcs}_seed${seed}_speed${speed}_power${power}"
    run_dir="$OUTPUT_BASE/$tag"
    args=(
        "--antennaCount=$ant" "--channelWidthMhz=$width" "--precoding=$pre"
        "--mcs=$mcs" "--seed=$seed" "--mobileSpeedMps=$speed" "--txPowerDbm=$power"
        "--numPairs=${NUM_PAIRS:-2}" "--streams=${STREAMS:-0}"
        "--arrayRows=${ARRAY_ROWS:-1}" "--antennaSpacingLambda=${ANTENNA_SPACING_LAMBDA:-0.5}"
        "--noiseFigureDb=${NOISE_FIGURE_DB:-7}" "--run=${RNG_RUN:-1}"
        "--environment=${ENVIRONMENT:-apartment/apartment.xml}"
        "--serverUrl=${SERVER_URL:-tcp://localhost:5555}" "--channelSource=$CHANNEL_SOURCE"
        "--simulationTimeSeconds=${SIMULATION_TIME_SECONDS:-10}"
        "--trafficStartSeconds=${TRAFFIC_START_SECONDS:-1}" "--reportIntervalMs=${REPORT_INTERVAL_MS:-100}"
        "--offeredRate=${OFFERED_RATE:-1Gbps}" "--tcpVariant=${TCP_VARIANT:-ns3::TcpCubic}"
        "--snr=${ENABLE_SNR:-true}" "--caching=${CACHING:-true}"
        "--fixedCoherenceTimeMs=${COHERENCE_TIME_MS:-0}" "--mode=${MODE:-1}" "--subMode=${SUB_MODE:-1}"
        "--apPositions=${AP_POSITIONS:--1,0.5,1.5;2,0.5,1.5}"
        "--staPositions=${STA_POSITIONS:--2,1.5,1.6;3,1.5,1.6}"
        "--outputDirectory=$run_dir"
    )
    if [[ -n ${EESM_BETA_LINEAR:-} ]]; then args+=("--eesmBetaLinear=$EESM_BETA_LINEAR"); fi
    args+=("$@")
    command=(./ns3 run --no-build ns3sionna-performance-sionna-mimo -- "${args[@]}")
    if [[ $RUN_TIMEOUT_SECONDS != 0 ]]; then command=(timeout --kill-after=5 "$RUN_TIMEOUT_SECONDS" "${command[@]}"); fi
    if [[ $DRY_RUN == true ]]; then
        printf '%q ' "${command[@]}"
        printf '\n'
        continue
    fi
    if [[ -e $run_dir ]]; then echo "[runner] output directory already exists: $run_dir" >&2; exit 2; fi
    attempt=0
    while true; do
        attempt=$((attempt + 1))
        mkdir -p -- "$run_dir"
        crashed=false
        status=0
        if [[ $managed_server == true ]] && ! kill -0 "$server_pid" 2>/dev/null; then
            if [[ $RESTART_SERVER_ON_CRASH == true ]]; then
                stop_server
                start_server || status=125
            else
                status=125
            fi
        fi
        printf '[runner] %s (attempt %s)\n' "$tag" "$attempt"
        if [[ $status == 0 ]]; then
            setsid "${command[@]}" > "$run_dir/run.log" 2>&1 &
            run_pid=$!
            while kill -0 "$run_pid" 2>/dev/null; do
                if [[ $managed_server == true ]] && ! kill -0 "$server_pid" 2>/dev/null; then
                    crashed=true
                    stop_run
                    status=125
                    break
                fi
                sleep 1
            done
            if [[ $crashed == false ]]; then
                wait "$run_pid" || status=$?
                # Reap any descendants left by a failed wrapper/timeout.
                if [[ $status != 0 ]]; then stop_run; else run_pid=''; fi
            fi
        else
            crashed=true
            echo '[runner] managed server is unavailable' > "$run_dir/run.log"
        fi
        if [[ $status == 0 ]]; then
            cat -- "$run_dir/summary.csv"
            break
        fi
        if [[ $managed_server == true ]] && ! kill -0 "$server_pid" 2>/dev/null; then crashed=true; fi
        if [[ $managed_server == true && $RESTART_SERVER_ON_CRASH == true &&
              ( $crashed == true || $status == 124 || $status == 137 ) && $attempt -le $MAX_RETRIES ]]; then
            failed_dir="$run_dir.failed-attempt-$attempt"
            [[ ! -e $failed_dir ]] || { echo "[runner] archive already exists: $failed_dir" >&2; exit 2; }
            mv -- "$run_dir" "$failed_dir"
            echo "[runner] server crash/timeout; restarting and retrying $tag ($attempt/$MAX_RETRIES)" >&2
            stop_server
            # The next attempt starts a fresh server before launching ns-3.
            continue
        fi
        echo "[runner] failed (exit $status): $run_dir/run.log" >&2
        tail -n 30 -- "$run_dir/run.log" >&2
        exit "$status"
    done
 done
 done
 done
 done
 done
 done
 done
if [[ $DRY_RUN == false ]]; then printf '[runner] outputs: %s\n' "$OUTPUT_BASE"; fi
