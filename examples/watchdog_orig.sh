#!/bin/bash
# watchdog_orig.sh - runs the original Python Sionna server with the same
# single-shot TCP runner used by watchdog.sh.
#
# Usage:
#   RUN_LABEL=origA ./watchdog_orig.sh --reset
#   RUN_LABEL=origB ./watchdog_orig.sh --reset
#
# Notes:
# - The original server has no --equidistant_walk_steps CLI option.
# - COHERENCE_TIME is still passed to the ns-3 TCP example as
#   --fixedCoherenceTimeMs. The original server receives that value through the
#   simulation init message, but parts of the original server mark it as unused.

set -uo pipefail

EXAMPLES="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
SERVER_DIR="$EXAMPLES/../model/ns3sionna"
SERVER_SCRIPT="ns3sionna_server_orig.py"

# Sweep configuration. Keep this aligned with watchdog.sh for reproducibility
# checks against the modified server.
COHERENCE_TIMES=(1)
START_SEED=1
END_SEED=1

export RUN_LABEL=${RUN_LABEL:-orig_$(date +%Y%m%d_%H%M%S)}
export MODE=2
export NUM_RES=16
export NUM_STAS=1
export MOBILE_SPEED=7
export DATA_RATE=600

OUTPUT_BASE="$EXAMPLES/results_BBOX/OrigSpeedTest_${RUN_LABEL}"

export ENABLE_SNR=true
export ENABLE_POSITIONS=true

CHECK_INTERVAL=60
MAX_RETRIES=3

STATE_FILE="$EXAMPLES/watchdog_orig_state.txt"
SERVER_LOG="$SERVER_DIR/server_orig.log"
SERVER_PID=""
EXP_PID=""

save_state() {
    echo "$1 $2" > "$STATE_FILE"
}

load_state() {
    if [[ -f "$STATE_FILE" ]]; then
        read -r COHERENCE_IDX CURRENT_SEED < "$STATE_FILE"
        echo "[watchdog-orig] Resuming from state: coherence_idx=${COHERENCE_IDX}" \
             "(${COHERENCE_TIMES[$COHERENCE_IDX]}ms), seed=${CURRENT_SEED}"
    else
        COHERENCE_IDX=0
        CURRENT_SEED=$START_SEED
        echo "[watchdog-orig] No state file found, starting from the beginning"
    fi
}

clear_state() {
    rm -f "$STATE_FILE"
    echo "[watchdog-orig] State cleared"
}

start_server() {
    echo "[watchdog-orig] Starting original Sionna server..."
    cd "$SERVER_DIR" || { echo "Cannot cd to $SERVER_DIR"; exit 1; }
    export NS3SIONNA_MODEL_FOLDER="$SERVER_DIR/models"
    python "$SERVER_SCRIPT" > "$SERVER_LOG" 2>&1 &
    SERVER_PID=$!
    echo "[watchdog-orig] Server PID=${SERVER_PID}, waiting for init..."
    sleep 8
    if ! kill -0 "$SERVER_PID" 2>/dev/null; then
        echo "[watchdog-orig] Server failed to start. Check $SERVER_LOG"
        exit 1
    fi
    echo "[watchdog-orig] Server running"
    cd "$EXAMPLES" || exit 1
}

stop_server() {
    if [[ -n "$SERVER_PID" ]] && kill -0 "$SERVER_PID" 2>/dev/null; then
        echo "[watchdog-orig] Stopping server (PID=${SERVER_PID})"
        kill "$SERVER_PID" 2>/dev/null
        wait "$SERVER_PID" 2>/dev/null || true
    fi
    SERVER_PID=""
}

start_run() {
    local tc=$1 seed=$2
    export COHERENCE_TIME="$tc"
    export SEED="$seed"
    export OUTPUT_DIR="${OUTPUT_BASE}/tc${tc}"
    mkdir -p "$OUTPUT_DIR"

    cd "$EXAMPLES" || exit 1
    bash "./performance-sionna-tcp.sh" &
    EXP_PID=$!
    echo "[watchdog-orig] Run started (PID=${EXP_PID}): tc=${tc}ms seed=${seed}"
}

stop_run() {
    if [[ -n "$EXP_PID" ]] && kill -0 "$EXP_PID" 2>/dev/null; then
        kill "$EXP_PID" 2>/dev/null
        wait "$EXP_PID" 2>/dev/null || true
    fi
    pkill -f "ns3sionna-performance-sionna-tcp" 2>/dev/null || true
    EXP_PID=""
}

cleanup() {
    echo ""
    echo "[watchdog-orig] Caught signal - shutting down cleanly"
    stop_run
    stop_server
    echo "[watchdog-orig] State preserved in ${STATE_FILE} - re-run watchdog_orig.sh to resume"
    exit 0
}

trap cleanup SIGINT SIGTERM

run_until_success() {
    local tc=$1 seed=$2
    local retries=0

    start_run "$tc" "$seed"

    while true; do
        sleep "$CHECK_INTERVAL"

        if ! kill -0 "$SERVER_PID" 2>/dev/null; then
            ((retries++)) || true
            echo "[watchdog-orig] Server crashed - restarting server and re-running seed=${seed} tc=${tc}ms"
            if [[ $retries -ge $MAX_RETRIES ]]; then
                echo "[watchdog-orig] Max retries reached after repeated server crashes for tc=${tc}ms seed=${seed} - skipping"
                stop_run
                sleep 2
                start_server
                return 1
            fi
            stop_run
            sleep 2
            start_server
            start_run "$tc" "$seed"
            continue
        fi

        if ! kill -0 "$EXP_PID" 2>/dev/null; then
            wait "$EXP_PID"
            local status=$?

            if [[ $status -eq 0 ]]; then
                echo "[watchdog-orig] Run succeeded: tc=${tc}ms seed=${seed}"
                return 0
            else
                ((retries++)) || true
                echo "[watchdog-orig] Run failed (attempt ${retries}/${MAX_RETRIES}): tc=${tc}ms seed=${seed}"

                if [[ $retries -ge $MAX_RETRIES ]]; then
                    echo "[watchdog-orig] Max retries reached for tc=${tc}ms seed=${seed} - skipping"
                    return 1
                fi

                echo "[watchdog-orig] Retrying in 5s..."
                sleep 5
                start_run "$tc" "$seed"
            fi
        fi
    done
}

if [[ "${1:-}" == "--reset" ]]; then
    clear_state
fi

EXISTING_PID=$(pgrep -f "ns3sionna_server.*\.py" || true)
if [[ -n "$EXISTING_PID" ]]; then
    echo "[watchdog-orig] A Sionna server is already running (PID=${EXISTING_PID}). Stop it first."
    exit 1
fi

echo "[watchdog-orig] ============================================"
echo "[watchdog-orig] Server script:   ${SERVER_SCRIPT}"
echo "[watchdog-orig] Coherence times: ${COHERENCE_TIMES[*]} ms"
echo "[watchdog-orig] Seeds:           ${START_SEED}..${END_SEED}"
echo "[watchdog-orig] Run label:       ${RUN_LABEL}"
echo "[watchdog-orig] Speed:           ${MOBILE_SPEED} m/s"
echo "[watchdog-orig] SNR logging:     ${ENABLE_SNR}"
echo "[watchdog-orig] Position logging: ${ENABLE_POSITIONS}"
echo "[watchdog-orig] Output base:     ${OUTPUT_BASE}"
echo "[watchdog-orig] ============================================"

load_state
start_server

while [[ $COHERENCE_IDX -lt ${#COHERENCE_TIMES[@]} ]]; do
    TC="${COHERENCE_TIMES[$COHERENCE_IDX]}"

    echo ""
    echo "[watchdog-orig] -- Coherence time ${TC}ms (${COHERENCE_IDX}/${#COHERENCE_TIMES[@]}) --"

    while [[ $CURRENT_SEED -le $END_SEED ]]; do
        echo ""
        save_state "$COHERENCE_IDX" "$CURRENT_SEED"

        run_until_success "$TC" "$CURRENT_SEED"

        sleep 3
        ((CURRENT_SEED++)) || true
    done

    ((COHERENCE_IDX++)) || true
    CURRENT_SEED=$START_SEED
done

echo ""
echo "[watchdog-orig] ============================================"
echo "[watchdog-orig] All sweeps complete!"
echo "[watchdog-orig] Results are in: ${OUTPUT_BASE}/"
echo "[watchdog-orig] ============================================"

stop_server
clear_state
exit 0
