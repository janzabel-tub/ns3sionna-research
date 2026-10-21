#!/usr/bin/env bash
# Own the Sionna server and supervise the TCP-style MIMO monolith runner.
set -euo pipefail
EXAMPLES_DIR=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
NS3_DIR=$(cd -- "$EXAMPLES_DIR/../../.." && pwd)
SERVER_DIR="$EXAMPLES_DIR/../model/ns3sionna"
MONOLITH_RUNNER="$EXAMPLES_DIR/performance-sionna-mimo-monolith.sh"

# ===========================================================================
# EXPERIMENT CONFIGURATION — edit here, or override via environment variables.
# ===========================================================================

# Outer sweep: coherence times in ms and an inclusive seed range.
# Example: COHERENCE_TIMES_MS='0 1 10 100 1000' START_SEED=1 END_SEED=3
COHERENCE_TIMES=(0)
if [[ -n ${COHERENCE_TIMES_MS:-${COHERENCE_TIME:-}} ]]; then
    read -r -a COHERENCE_TIMES <<< "${COHERENCE_TIMES_MS:-${COHERENCE_TIME:-0}}"
fi
START_SEED=${START_SEED:-${SEED:-1}}
END_SEED=${END_SEED:-$START_SEED}

# Station-count sweep inside each monolith invocation: 1, 2, 4, ... by default.
# NUM_STAS=4 selects only four stations; SIM_MIN_STAS/MAX_STAS can override it.
export NUM_STAS=${NUM_STAS:-}
export SIM_MIN_STAS=${SIM_MIN_STAS:-${NUM_STAS:-1}}
export SIM_MAX_STAS=${SIM_MAX_STAS:-${NUM_STAS:-4}}

# Scene, mobility and propagation cache. Default apartment STAs share the AP room.
export ENVIRONMENT=${ENVIRONMENT:-apartment/apartment.xml}
export AP_POSITION=${AP_POSITION:-}       # Optional 'x,y,z'; otherwise scene default.
export STA_POSITIONS=${STA_POSITIONS:-}   # Optional 'x,y,z;x,y,z'; one station count only.
export MOBILE_SCENARIO=${MOBILE_SCENARIO:-false}
export MOBILE_SPEED=${MOBILE_SPEED:-1}
export CACHING=${CACHING:-true}
export MODE=${MODE:-2}
export NUM_RES=${NUM_RES:-16}
export RNG_RUN=${RNG_RUN:-1}

# MIMO/PHY. STREAMS is Minstrel's NSS ceiling; 0 uses antenna count for open-loop,
# one for SVD. Total device transmit power remains fixed across stream counts.
export ANTENNA_COUNT=${ANTENNA_COUNT:-2}
export ARRAY_ROWS=${ARRAY_ROWS:-1}
export ANTENNA_SPACING_LAMBDA=${ANTENNA_SPACING_LAMBDA:-0.5}
export STREAMS=${STREAMS:-0}
export PRECODING=${PRECODING:-open-loop}  # open-loop or svd
export CHANNEL_WIDTH_MHZ=${CHANNEL_WIDTH_MHZ:-20}
export CHANNEL=${CHANNEL:-0}             # 0 selects 36/38/42 for 20/40/80 MHz.
export TX_POWER=${TX_POWER:-20}
export NOISE_FIGURE_DB=${NOISE_FIGURE_DB:-7}
export RATE_ADAPTATION=${RATE_ADAPTATION:-minstrel}
export MCS=${MCS:-11}                    # Used only with RATE_ADAPTATION=fixed.
export LDPC=${LDPC:-true}

# TCP and measurement. DATA_RATE is offered Mb/s PER STATION; BER needs no beta.
export DATA_RATE=${DATA_RATE:-600}
export TCP_VARIANT=${TCP_VARIANT:-ns3::TcpCubic}
export SIMULATION_TIME_SECONDS=${SIMULATION_TIME_SECONDS:-10}
export TRAFFIC_START_SECONDS=${TRAFFIC_START_SECONDS:-1}
export REPORT_INTERVAL_MS=${REPORT_INTERVAL_MS:-100}

# Optional recordings are disabled by default.
export ENABLE_SNR=${ENABLE_SNR:-false}
export ENABLE_POSITIONS=${ENABLE_POSITIONS:-false}
export ENABLE_RATE_TRACE=${ENABLE_RATE_TRACE:-false}
export ENABLE_PCAP=${ENABLE_PCAP:-false}
export ENABLE_MIMO_TRACE=${ENABLE_MIMO_TRACE:-false}
export ENABLE_THROUGHPUT_10MS=${ENABLE_THROUGHPUT_10MS:-false}
export PRINT_THROUGHPUT=${PRINT_THROUGHPUT:-false}
export VERBOSE=${VERBOSE:-false}

# Server. SERVER_PYTHON must belong to your installed Sionna environment.
START_SERVER=${START_SERVER:-true}
export SERVER_URL=${SERVER_URL:-tcp://localhost:5555}
SERVER_PYTHON=${SERVER_PYTHON:-python3}
RT_FAST=${RT_FAST:-false}
EQUIDISTANT_WALK_STEPS=${EQUIDISTANT_WALK_STEPS:-100000} # Server mobility tick, ns.
SERVER_STARTUP_TIMEOUT_SECONDS=${SERVER_STARTUP_TIMEOUT_SECONDS:-120}

# Supervision. A timeout covers one coherence/seed invocation and its STA sweep.
# MAX_RETRIES counts retries after the initial attempt. Zero disables retries.
CHECK_INTERVAL=${CHECK_INTERVAL:-1}       # Seconds between server-liveness polls.
RUN_TIMEOUT_SECONDS=${RUN_TIMEOUT_SECONDS:-1800} # Wall clock; 0 disables the limit.
RESTART_SERVER_ON_CRASH=${RESTART_SERVER_ON_CRASH:-true}
MAX_RETRIES=${MAX_RETRIES:-3}
BUILD=${BUILD:-true}                     # Build the monolith once, before the server.

# Outputs: one tcN_seedM directory per invocation, plus server/attempt logs.
export RUN_LABEL=${RUN_LABEL:-$(date +%Y%m%d_%H%M%S)-$$}
OUTPUT_BASE=${OUTPUT_BASE:-"$NS3_DIR/results/mimo-monolith/$RUN_LABEL"}

# ===========================================================================
# Configuration ends here. Extra --name=value CLI options override the above.
# ===========================================================================

if [[ ${1:-} == --help || ${1:-} == -h ]]; then
    cat <<'HELP'
Usage: watchdog-mimo.sh [--output-dir DIR] [monolith --name=value options]
       watchdog-mimo.sh --dry-run [monolith --name=value options]

Edit the configuration block at the top, or override its environment variables.
Runs performance-sionna-mimo-monolith.sh with HE Minstrel and beta-free mapping.
By default starts a local Sionna server, waits for readiness, checks it every
second, and stops the owned server on exit. START_SERVER=false uses an existing
server; its process is not managed or monitored by this watchdog.

COHERENCE_TIMES_MS='0 1 10' START_SEED=1 END_SEED=3 selects the outer sweep.
NUM_STAS=4 selects one station count; otherwise SIM_MIN_STAS=1 SIM_MAX_STAS=4
selects the monolith's doubling sweep. Other monolith settings are listed below.
RUN_TIMEOUT_SECONDS=1800 limits each invocation, excluding the build; 0 disables.
With RESTART_SERVER_ON_CRASH=true, managed-server crashes and simulation timeouts
archive partial outputs and retry up to MAX_RETRIES=3 times. Other failures stop
the sweep. Existing outputs are rejected. Completed invocations remain intact.

--output-dir DIR, --outputDirectory=DIR, or OUTPUT_BASE selects the results root.
--seed=N and --fixedCoherenceTimeMs=N replace their respective sweep dimensions.
--dry-run prints resolved commands without building, starting a server or writing
outputs. Server logs and separate per-attempt console logs are kept at the root.
HELP
    bash "$MONOLITH_RUNNER" --help
    exit 0
fi

DRY_RUN=false
RUN_ARGS=()
while (($#)); do
    case "$1" in
        --dry-run) DRY_RUN=true; shift ;;
        --output-dir|--outputDirectory)
            if (($# < 2)) || [[ -z $2 ]]; then
                echo "[watchdog-mimo] $1 requires a directory" >&2; exit 2
            fi
            OUTPUT_BASE=$2; shift 2 ;;
        --output-dir=*|--outputDirectory=*)
            OUTPUT_BASE=${1#*=}
            [[ -n $OUTPUT_BASE ]] || { echo '[watchdog-mimo] output directory cannot be empty' >&2; exit 2; }
            shift ;;
        --seed=*) START_SEED=${1#*=}; END_SEED=$START_SEED; shift ;;
        --fixedCoherenceTimeMs=*) COHERENCE_TIMES=("${1#*=}"); shift ;;
        --serverUrl=*) SERVER_URL=${1#*=}; RUN_ARGS+=("$1"); shift ;;
        --prtTput|--prtTput=true|--prtTput=1)
            PRINT_THROUGHPUT=true; RUN_ARGS+=(--prtTput=true); shift ;;
        --prtTput=false|--prtTput=0)
            PRINT_THROUGHPUT=false; RUN_ARGS+=(--prtTput=false); shift ;;
        --PrintHelp|--PrintHelp=*) exec bash "$MONOLITH_RUNNER" "${RUN_ARGS[@]}" "$1" ;;
        *) RUN_ARGS+=("$1"); shift ;;
    esac
done
if [[ $OUTPUT_BASE != /* ]]; then OUTPUT_BASE="$PWD/$OUTPUT_BASE"; fi
for value in "$START_SEED" "$END_SEED" "$MAX_RETRIES" "$RUN_TIMEOUT_SECONDS" \
             "$CHECK_INTERVAL" "$SERVER_STARTUP_TIMEOUT_SECONDS" "$EQUIDISTANT_WALK_STEPS" "${COHERENCE_TIMES[@]}"; do
    [[ $value =~ ^[0-9]+$ ]] || { echo "[watchdog-mimo] expected a non-negative integer, got: $value" >&2; exit 2; }
done
[[ $START_SEED -gt 0 && $END_SEED -ge $START_SEED && $CHECK_INTERVAL -gt 0 &&
   $SERVER_STARTUP_TIMEOUT_SECONDS -gt 0 && ${#COHERENCE_TIMES[@]} -gt 0 ]] || {
    echo '[watchdog-mimo] invalid seed range, poll interval or startup timeout' >&2; exit 2;
}
if [[ $START_SERVER == true && $SERVER_URL != tcp://localhost:5555 ]]; then
    echo '[watchdog-mimo] managed startup supports tcp://localhost:5555; use START_SERVER=false for another endpoint' >&2
    exit 2
fi

# Server and process-group lifecycle reused from performance-sionna-mimo.sh.
server_pid=''
run_pid=''
server_starts=0
stop_run() {
    if [[ -n $run_pid ]]; then
        kill -TERM -- "-$run_pid" 2>/dev/null || true
        kill -KILL -- "-$run_pid" 2>/dev/null || true
        kill -KILL "$run_pid" 2>/dev/null || true
        wait "$run_pid" 2>/dev/null || true
        run_pid=''
    fi
}
stop_server() {
    if [[ -n $server_pid ]]; then
        kill -TERM "$server_pid" 2>/dev/null || true
        kill -KILL "$server_pid" 2>/dev/null || true
        wait "$server_pid" 2>/dev/null || true
        server_pid=''
    fi
}
cleanup() { stop_run; stop_server; }
trap cleanup EXIT
trap 'exit 130' INT
trap 'exit 143' TERM
start_server() {
    "$SERVER_PYTHON" - <<'PORT_CHECK' || return 2
import socket
with socket.socket() as probe:
    probe.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    try:
        probe.bind(('127.0.0.1', 5555))
    except OSError as error:
        raise SystemExit('Port 5555 is already in use; use START_SERVER=false: ' + str(error))
PORT_CHECK
    server_starts=$((server_starts + 1))
    server_log="$OUTPUT_BASE/server.log"
    if [[ $server_starts -gt 1 ]]; then server_log="$OUTPUT_BASE/server-$server_starts.log"; fi
    local server_args=(--model_folder="$SERVER_DIR/models" --equidistant_walk_steps="$EQUIDISTANT_WALK_STEPS")
    if [[ $RT_FAST == true ]]; then server_args+=(--rt_fast); fi
    printf '[watchdog-mimo] starting server; log: %s\n' "$server_log"
    (cd -- "$SERVER_DIR"; exec "$SERVER_PYTHON" -u ns3sionna_server.py "${server_args[@]}") \
        > "$server_log" 2>&1 &
    server_pid=$!
    local deadline=$((SECONDS + SERVER_STARTUP_TIMEOUT_SECONDS))
    while ((SECONDS < deadline)); do
        if ! kill -0 "$server_pid" 2>/dev/null; then
            echo "[watchdog-mimo] server failed; see $server_log" >&2
            tail -n 30 -- "$server_log" >&2
            return 2
        fi
        if grep -q 'Sionna server socket ready' "$server_log"; then return 0; fi
        sleep 0.25
    done
    echo "[watchdog-mimo] server readiness timed out; see $server_log" >&2
    return 2
}

if [[ $DRY_RUN == false ]]; then
    command -v setsid >/dev/null || { echo '[watchdog-mimo] setsid is required' >&2; exit 2; }
    if [[ ! $RUN_TIMEOUT_SECONDS =~ ^0+$ ]]; then
        command -v timeout >/dev/null || { echo '[watchdog-mimo] GNU timeout is required' >&2; exit 2; }
    fi
    # Avoid overwriting server or experiment logs from a previous invocation.
    [[ ! -e $OUTPUT_BASE ]] || { echo "[watchdog-mimo] output root already exists: $OUTPUT_BASE" >&2; exit 2; }
    if [[ $BUILD == true ]]; then
        (cd -- "$NS3_DIR"; ./ns3 build ns3sionna-performance-sionna-mimo-monolith -j 2)
    fi
    mkdir -p -- "$OUTPUT_BASE"
    if [[ $START_SERVER == true ]]; then start_server || exit 2; fi
fi

for tc in "${COHERENCE_TIMES[@]}"; do
for ((seed=START_SEED; seed<=END_SEED; seed++)); do
    tag="tc${tc}_seed${seed}"
    run_dir="$OUTPUT_BASE/$tag"
    # The watchdog owns the timeout. Disable the inner timer so ns-3 and all
    # wrapper children stay in the one process group owned by this attempt.
    base_command=(env BUILD=false RUN_TIMEOUT_SECONDS=0 COHERENCE_TIME="$tc" SEED="$seed"
                  OUTPUT_DIR="$run_dir" bash "$MONOLITH_RUNNER" "${RUN_ARGS[@]}")
    if [[ $DRY_RUN == true ]]; then
        BUILD=false RUN_TIMEOUT_SECONDS="$RUN_TIMEOUT_SECONDS" COHERENCE_TIME="$tc" SEED="$seed" \
            OUTPUT_DIR="$run_dir" bash "$MONOLITH_RUNNER" --dry-run "${RUN_ARGS[@]}"
        continue
    fi
    attempt=0
    while true; do
        attempt=$((attempt + 1))
        attempt_log="$OUTPUT_BASE/$tag.attempt-$attempt.log"
        status=0
        crashed=false
        if [[ $START_SERVER == true ]] && ! kill -0 "$server_pid" 2>/dev/null; then
            if [[ $RESTART_SERVER_ON_CRASH == true ]]; then
                stop_server
                start_server || exit 2
            else
                echo '[watchdog-mimo] managed server is unavailable' >&2
                exit 125
            fi
        fi
        command=("${base_command[@]}")
        if [[ $PRINT_THROUGHPUT == true ]]; then
            command=(bash -c '
                log_file=$1
                shift
                "$@" 2>&1 | tee "$log_file"
                exit "${PIPESTATUS[0]}"
            ' bash "$attempt_log" "${command[@]}")
        fi
        # timeout is outside the optional tee wrapper, keeping a single owned
        # process group even when live throughput is enabled.
        if [[ ! $RUN_TIMEOUT_SECONDS =~ ^0+$ ]]; then
            command=(timeout --signal=TERM --kill-after=5 "$RUN_TIMEOUT_SECONDS" "${command[@]}")
        fi
        printf '[watchdog-mimo] %s, attempt %s; output: %s\n' "$tag" "$attempt" "$run_dir"
        if [[ $PRINT_THROUGHPUT == true ]]; then
            setsid "${command[@]}" &
        else
            setsid "${command[@]}" > "$attempt_log" 2>&1 &
        fi
        run_pid=$!
        while kill -0 "$run_pid" 2>/dev/null; do
            if [[ $START_SERVER == true ]] && ! kill -0 "$server_pid" 2>/dev/null; then
                crashed=true
                stop_run
                status=125
                break
            fi
            sleep "$CHECK_INTERVAL"
        done
        if [[ $crashed == false ]]; then
            wait "$run_pid" || status=$?
            if [[ $status != 0 ]]; then stop_run; else run_pid=''; fi
        fi
        if [[ $status == 0 ]]; then
            printf '[watchdog-mimo] completed %s\n' "$tag"
            break
        fi
        if [[ $START_SERVER == true ]] && ! kill -0 "$server_pid" 2>/dev/null; then crashed=true; fi
        if [[ $RESTART_SERVER_ON_CRASH == true &&
              ( $crashed == true || $status == 124 || $status == 137 ) && $attempt -le $MAX_RETRIES ]]; then
            if [[ -e $run_dir ]]; then mv -- "$run_dir" "$run_dir.failed-attempt-$attempt"; fi
            echo "[watchdog-mimo] server crash/timeout (exit $status); retry $attempt/$MAX_RETRIES" >&2
            if [[ $START_SERVER == true ]]; then stop_server; fi
            continue
        fi
        echo "[watchdog-mimo] failed (exit $status); see $attempt_log" >&2
        tail -n 30 -- "$attempt_log" >&2
        exit "$status"
    done
 done
 done
printf '[watchdog-mimo] outputs: %s\n' "$OUTPUT_BASE"
