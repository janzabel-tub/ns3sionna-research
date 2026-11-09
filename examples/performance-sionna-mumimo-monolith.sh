#!/usr/bin/env bash
# --test_scenario=1 (default): one 4-antenna AP, four 1-antenna STAs.
# --test_scenario=2: one 4-antenna AP, two 2-antenna STAs; currently unsupported.
# Also accepts the environment variable test_scenario=1 or 2.
# Four-user MU TCP monolith. Supervision/retries reuse performance-sionna-mimo.sh;
# arguments, output ownership and timing follow performance-sionna-mimo-monolith.sh.
set -euo pipefail
test_scenario=${test_scenario:-1}
EXAMPLES_DIR=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
NS3_DIR=$(cd -- "$EXAMPLES_DIR/../../.." && pwd)

# Scenario and traffic (environment overrides; CLI --name=value overrides below).
ENVIRONMENT=${ENVIRONMENT:-apartment/apartment.xml}
AP_POSITION=${AP_POSITION:-}
STA_POSITIONS=${STA_POSITIONS:-}
MOBILE_SCENARIO=${MOBILE_SCENARIO:-false}
MOBILE_SPEED=${MOBILE_SPEED:-1}
DATA_RATE=${DATA_RATE:-600} # Mb/s offered to EACH STA, using TCP OnOff.
TCP_VARIANT=${TCP_VARIANT:-ns3::TcpCubic}
SIMULATION_TIME_SECONDS=${SIMULATION_TIME_SECONDS:-10}
TRAFFIC_START_SECONDS=${TRAFFIC_START_SECONDS:-1}
REPORT_INTERVAL_MS=${REPORT_INTERVAL_MS:-100}
SEED=${SEED:-1}
RNG_RUN=${RNG_RUN:-1}

# Default topology: 4 AP antennas, 4 one-antenna STAs, NSS=1/user.
MU_GROUPING=${MU_GROUPING:-adaptive} # adaptive or fixed
MU_RATE_CONTROL=${MU_RATE_CONTROL:-ideal-csi} # ideal-csi or station-manager
PRECODING=${PRECODING:-zf} # zf or rzf; RZF_ALPHA must be supplied for rzf.
RZF_ALPHA=${RZF_ALPHA:-}
ARRAY_ROWS=${ARRAY_ROWS:-1}
ANTENNA_SPACING_LAMBDA=${ANTENNA_SPACING_LAMBDA:-0.5}
CHANNEL_WIDTH_MHZ=${CHANNEL_WIDTH_MHZ:-20} # HE: 20, 40, 80 or 160.
CHANNEL=${CHANNEL:-0} # Select the default channel matching the width.
TX_POWER=${TX_POWER:-20} # Total AP power, shared across four MU streams.
NOISE_FIGURE_DB=${NOISE_FIGURE_DB:-7}
RATE_ADAPTATION=${RATE_ADAPTATION:-minstrel} # AP rate policy.
REVERSE_PATH=${REVERSE_PATH:-reciprocal-he} # reciprocal-he or legacy24; STA uplink policy.
MCS=${MCS:-9} # Fixed-mode HE MCS 0--9. Native MU scheduler uses BCC.
LDPC=${LDPC:-false} # Optional SU support; MU remains BCC.

# Live Sionna channel and optional local server ownership.
MODE=${MODE:-2}
NUM_RES=${NUM_RES:-16}
CACHING=${CACHING:-true}
COHERENCE_TIME=${COHERENCE_TIME:-0}
SERVER_URL=${SERVER_URL:-tcp://localhost:5555}
START_SERVER=${START_SERVER:-false}
SERVER_PYTHON=${SERVER_PYTHON:-python3}
RT_FAST=${RT_FAST:-true}
RESTART_SERVER_ON_CRASH=${RESTART_SERVER_ON_CRASH:-true}
MAX_RETRIES=${MAX_RETRIES:-3}
RUN_TIMEOUT_SECONDS=${RUN_TIMEOUT_SECONDS:-1800} # Per attempt; 0 disables.
BUILD=${BUILD:-true}

# Recording: per-STA interval/summary CSVs always on; details opt-in.
OUTPUT_DIR=${OUTPUT_DIR:-"$NS3_DIR/results/mumimo-monolith/$(date +%Y%m%d-%H%M%S)-$$"}
RUN_LABEL=${RUN_LABEL:-manual}
ENABLE_SNR=${ENABLE_SNR:-false}
ENABLE_POSITIONS=${ENABLE_POSITIONS:-false}
ENABLE_RATE_TRACE=${ENABLE_RATE_TRACE:-false}
ENABLE_PCAP=${ENABLE_PCAP:-false}
ENABLE_MIMO_TRACE=${ENABLE_MIMO_TRACE:-false}
ENABLE_MU_DIAGNOSTICS=${ENABLE_MU_DIAGNOSTICS:-false}
ENABLE_THROUGHPUT_10MS=${ENABLE_THROUGHPUT_10MS:-false}
PRINT_THROUGHPUT=${PRINT_THROUGHPUT:-false}
VERBOSE=${VERBOSE:-false}

if [[ ${1:-} == --help || ${1:-} == -h ]]; then
    cat <<'HELP'
Usage: [ENV=value ...] bash performance-sionna-mumimo-monolith.sh [--name=value ...]
       bash performance-sionna-mumimo-monolith.sh --dry-run [--name=value ...]

One four-antenna AP sends TCP to four one-antenna STAs over a shared full-band
full-band RU at CHANNEL_WIDTH_MHZ=20/40/80/160 (default 20).
CHANNEL=0 selects a matching channel. PRECODING=zf is default. For RZF set
PRECODING=rzf RZF_ALPHA=<positive value>; no calibration beta is needed.
MU_GROUPING=adaptive uses eligible groups of 2--N; fixed retains the strict group.
REVERSE_PATH=reciprocal-he (default) uses reciprocal STA-1/AP-N HE reception
and STA Minstrel with native aggregation. legacy24 restores legacy TCP ACK data.
MU_RATE_CONTROL=ideal-csi selects MU MCS 0--9 from the actual group channel.
AP Minstrel selects SU rates independently; MU uses one stream/user and 800 ns GI.
For the legacy baseline use MU_GROUPING=fixed MU_RATE_CONTROL=station-manager.
RATE_ADAPTATION=fixed MCS=5 selects a fixed HE MCS. STA TCP ACK data uses
legacy OfdmRate24Mbps because the server has global TX/RX array dimensions.

All settings are grouped at the top of this file. Common overrides:
  MOBILE_SCENARIO=true MOBILE_SPEED=1 SEED=1 RNG_RUN=1 DATA_RATE=600
  ENVIRONMENT=apartment/apartment.xml AP_POSITION='x,y,z'
  STA_POSITIONS='x,y,z;x,y,z;x,y,z;x,y,z' TX_POWER=20 ARRAY_ROWS=1
  SIMULATION_TIME_SECONDS=10 TRAFFIC_START_SECONDS=1 REPORT_INTERVAL_MS=100
  SERVER_URL=tcp://localhost:5555 START_SERVER=false SERVER_PYTHON=python3
  RUN_TIMEOUT_SECONDS=1800 BUILD=true MAX_RETRIES=3 RT_FAST=true
  RESTART_SERVER_ON_CRASH=true OUTPUT_DIR=<new directory>
  ENABLE_SNR=false ENABLE_POSITIONS=false ENABLE_RATE_TRACE=false
  ENABLE_PCAP=false ENABLE_MIMO_TRACE=false ENABLE_THROUGHPUT_10MS=false
  ENABLE_MU_DIAGNOSTICS=false  # scheduler/PPDU/ACK/CW CSVs; also enables MIMO traces
  PRINT_THROUGHPUT=false VERBOSE=false

Default uses an existing server. START_SERVER=true owns a local server,
waits for readiness, monitors it, and retries server crashes/timeouts up to
MAX_RETRIES. External servers are left running and runs are not retried.
Timeout applies per simulation attempt; excludes build/server startup;
TERM then KILL after 5 s. Failed attempts and server logs are retained.
Ctrl-C/TERM cleans up the owned simulation process group and owned server.

Outputs: OUTPUT_DIR/run/stas-4/{throughput_results.csv,summary.csv,
mu_summary.csv,config.txt,computationtime.csv}, plus OUTPUT_DIR/run/run.log
and optional server logs at OUTPUT_DIR. Failed runs move to
OUTPUT_DIR/run.failed-attempt-N. Details are disabled by default.
MU uses eligible queues with established BlockAck agreements and feasible channels;
fixed grouping requires the complete configured group. Native SU handles fallback.
mu_summary.csv records full-group, total MU and HE-SU counts in the measurement
window; mu_group_sizes.csv reports actual group sizes. Throughput includes both.
Use --PrintHelp for C++ flags without building or starting a server.
See performance-sionna-mumimo-monolith.md and ns3sionna_mimo_model.md.
HELP
    exit 0
fi
DRY_RUN=false
if [[ ${1:-} == --dry-run ]]; then DRY_RUN=true; shift; fi
# Resolve runner-owned options before constructing the server and log paths.
forwarded=()
for option in "$@"; do
    case "$option" in
        --test_scenario=*|test_scenario=*) test_scenario=${option#*=}; continue ;;
        --serverUrl=*) SERVER_URL=${option#*=} ;;
        --outputDirectory=*) OUTPUT_DIR=${option#*=} ;;
        --prtTput|--prtTput=true|--prtTput=1) PRINT_THROUGHPUT=true ;;
        --prtTput=false|--prtTput=0) PRINT_THROUGHPUT=false ;;
    esac
    forwarded+=("$option")
done
set -- "${forwarded[@]}"
case "$test_scenario" in
    1) ;;
    2) echo '[runner] scenario 2 requires extending the MU helper/scheduler for two antennas and streams per STA.' >&2; exit 2 ;;
    *) echo '[runner] test_scenario must be 1 or 2' >&2; exit 2 ;;
esac
[[ $MAX_RETRIES =~ ^[0-9]+$ ]] || { echo 'MAX_RETRIES must be a non-negative integer' >&2; exit 2; }
[[ $RUN_TIMEOUT_SECONDS =~ ^[0-9]+$ ]] || { echo 'RUN_TIMEOUT_SECONDS must be a non-negative integer' >&2; exit 2; }
if [[ -z $OUTPUT_DIR ]]; then echo '[runner] output directory cannot be empty' >&2; exit 2; fi
if [[ $OUTPUT_DIR != /* ]]; then OUTPUT_DIR="$PWD/$OUTPUT_DIR"; fi
run_dir="$OUTPUT_DIR/run"
tag="$RUN_LABEL"
args=(
    "--sim_min_stas=4" "--sim_max_stas=4" "--antennaCount=4" "--streams=1"
    "--channelWidthMhz=$CHANNEL_WIDTH_MHZ" "--channel=$CHANNEL" "--precoding=$PRECODING"
    "--muGrouping=$MU_GROUPING" "--muRateControl=$MU_RATE_CONTROL" "--reversePath=$REVERSE_PATH"
    "--arrayRows=$ARRAY_ROWS" "--antennaSpacingLambda=$ANTENNA_SPACING_LAMBDA"
    "--environment=$ENVIRONMENT" "--serverUrl=$SERVER_URL"
    "--mobile_scenario=$MOBILE_SCENARIO" "--mobile_speed=$MOBILE_SPEED"
    "--caching=$CACHING" "--mode=$MODE" "--sub_mode=$NUM_RES"
    "--fixedCoherenceTimeMs=$COHERENCE_TIME" "--dataRate=$DATA_RATE"
    "--rateAdaptation=$RATE_ADAPTATION" "--mcs=$MCS" "--tcpVariant=$TCP_VARIANT"
    "--txPower=$TX_POWER" "--noiseFigureDb=$NOISE_FIGURE_DB" "--ldpc=$LDPC"
    "--seed=$SEED" "--run=$RNG_RUN"
    "--simulationTimeSeconds=$SIMULATION_TIME_SECONDS"
    "--trafficStartSeconds=$TRAFFIC_START_SECONDS" "--reportIntervalMs=$REPORT_INTERVAL_MS"
    "--snr=$ENABLE_SNR" "--positions=$ENABLE_POSITIONS"
    "--minstrelRateTrace=$ENABLE_RATE_TRACE" "--pcap=$ENABLE_PCAP"
    "--mimoTrace=$ENABLE_MIMO_TRACE" "--throughput10ms=$ENABLE_THROUGHPUT_10MS"
    "--muDiagnostics=$ENABLE_MU_DIAGNOSTICS"
    "--prtTput=$PRINT_THROUGHPUT" "--verbose=$VERBOSE"
)
if [[ -n $RZF_ALPHA ]]; then args+=("--rzfAlpha=$RZF_ALPHA"); fi
if [[ -n $AP_POSITION ]]; then args+=("--apPosition=$AP_POSITION"); fi
if [[ -n $STA_POSITIONS ]]; then args+=("--staPositions=$STA_POSITIONS"); fi
args+=("$@" "--outputDirectory=$run_dir")
command=(./ns3 run --no-build ns3sionna-performance-sionna-mumimo-monolith -- "${args[@]}")
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
command -v setsid >/dev/null || { echo '[runner] setsid is required for process supervision' >&2; exit 2; }
if [[ ! $RUN_TIMEOUT_SECONDS =~ ^0+$ ]]; then
    command -v timeout >/dev/null || { echo '[runner] GNU timeout is required' >&2; exit 2; }
fi
if [[ $START_SERVER == true && $SERVER_URL != tcp://localhost:5555 ]]; then
    echo 'START_SERVER supports the local tcp://localhost:5555 endpoint' >&2
    exit 2
fi
cd -- "$NS3_DIR"
if [[ $BUILD == true ]]; then ./ns3 build ns3sionna-performance-sionna-mumimo-monolith -j 2; fi
mkdir -p -- "$OUTPUT_DIR"
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
    mkdir -p -- "$OUTPUT_DIR" || return 2
    server_starts=$((server_starts + 1))
    server_log="$OUTPUT_DIR/server.log"
    if [[ $server_starts -gt 1 ]]; then server_log="$OUTPUT_DIR/server-$server_starts.log"; fi
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
if [[ $START_SERVER == true ]]; then
    managed_server=true
    start_server || exit 2
fi
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
        if [[ $PRINT_THROUGHPUT == true ]]; then
            # Keep the full output in run.log, while forwarding interval
            # throughput lines to the terminal.
            setsid bash -c '
                log_file=$1
                shift
                "$@" 2>&1 | tee "$log_file" | grep --line-buffered "Mbit/s"
                exit "${PIPESTATUS[0]}"
            ' bash "$run_dir/run.log" "${command[@]}" &
        else
            setsid "${command[@]}" > "$run_dir/run.log" 2>&1 &
        fi
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
        if [[ $PRINT_THROUGHPUT == true ]]; then cat -- "$run_dir/stas-4/summary.csv"; fi
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
printf '[runner] outputs: %s\n' "$OUTPUT_DIR"
