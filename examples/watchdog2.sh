#!/bin/bash
# watchdog.sh — manages the Sionna server and sweeps over coherence times × seeds.
#
# State is persisted so the sweep can resume exactly where it left off after any
# crash or manual restart. The inner runner is a single-shot script; all looping
# lives here.
#
# Usage: ./watchdog.sh [--reset]
#   --reset   delete saved state and start the sweep from the beginning

set -uo pipefail

EXAMPLES="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
SERVER_DIR="$EXAMPLES/../model/ns3sionna"

# ═══════════════════════════════════════════════════════════════════════════════
# SWEEP CONFIGURATION — edit these to change what gets simulated
# ═══════════════════════════════════════════════════════════════════════════════

# Coherence times (ms) to sweep over. Add or remove values freely.
COHERENCE_TIMES=(1 10 100 1000)

# Seed range (inclusive)
START_SEED=1
END_SEED=10

# Simulation parameters exported to the runner script
export MODE=2
export NUM_RES=16
export NUM_STAS=1
export MOBILE_SPEED=1
export DATA_RATE=600

# Output: each coherence time gets its own subdirectory under OUTPUT_BASE
OUTPUT_BASE="$EXAMPLES/results"

# Optional features passed through to the C++ binary
export ENABLE_SNR=true
export ENABLE_POSITIONS=true

# How many seconds between health-check polls
CHECK_INTERVAL=60

# Max consecutive crashes before a seed is abandoned and the sweep continues
MAX_RETRIES=3

# ═══════════════════════════════════════════════════════════════════════════════

STATE_FILE="$EXAMPLES/watchdog_state.txt"
SERVER_PID=""
EXP_PID=""

# ── State persistence ─────────────────────────────────────────────────────────
# State file format: "<coherence_index> <seed>"
# coherence_index is an index into COHERENCE_TIMES, not the value itself,
# so the array can be freely reordered without corrupting saved progress.

save_state() {
    echo "$1 $2" > "$STATE_FILE"
}

load_state() {
    if [[ -f "$STATE_FILE" ]]; then
        read -r COHERENCE_IDX CURRENT_SEED < "$STATE_FILE"
        echo "[watchdog] Resuming from state: coherence_idx=${COHERENCE_IDX}" \
             "(${COHERENCE_TIMES[$COHERENCE_IDX]}ms), seed=${CURRENT_SEED}"
    else
        COHERENCE_IDX=0
        CURRENT_SEED=$START_SEED
        echo "[watchdog] No state file found, starting from the beginning"
    fi
}

clear_state() {
    rm -f "$STATE_FILE"
    echo "[watchdog] State cleared"
}

# ── Process management ────────────────────────────────────────────────────────

start_server() {
    echo "[watchdog] Starting Sionna server..."
    cd "$SERVER_DIR" || { echo "Cannot cd to $SERVER_DIR"; exit 1; }
    export NS3SIONNA_MODEL_FOLDER="$SERVER_DIR/models"
    python ns3sionna_server.py > "$SERVER_DIR/server.log" 2>&1 &
    SERVER_PID=$!
    echo "[watchdog] Server PID=${SERVER_PID}, waiting for init..."
    sleep 8
    if ! kill -0 "$SERVER_PID" 2>/dev/null; then
        echo "[watchdog] Server failed to start. Check $SERVER_DIR/server.log"
        exit 1
    fi
    echo "[watchdog] Server running"
    cd "$EXAMPLES" || exit 1
}

stop_server() {
    if [[ -n "$SERVER_PID" ]] && kill -0 "$SERVER_PID" 2>/dev/null; then
        echo "[watchdog] Stopping server (PID=${SERVER_PID})"
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
    echo "[watchdog] Run started (PID=${EXP_PID}): tc=${tc}ms seed=${seed}"
}

stop_run() {
    if [[ -n "$EXP_PID" ]] && kill -0 "$EXP_PID" 2>/dev/null; then
        kill "$EXP_PID" 2>/dev/null
        wait "$EXP_PID" 2>/dev/null || true
    fi
    # Also kill any ns3 child that may have been spawned
    pkill -f "ns3sionna-performance-sionna-tcp" 2>/dev/null || true
    EXP_PID=""
}

# ── Cleanup on Ctrl-C / SIGTERM ───────────────────────────────────────────────

cleanup() {
    echo ""
    echo "[watchdog] Caught signal — shutting down cleanly"
    stop_run
    stop_server
    echo "[watchdog] State preserved in ${STATE_FILE} — re-run watchdog.sh to resume"
    exit 0
}

trap cleanup SIGINT SIGTERM

# ── run_until_success ─────────────────────────────────────────────────────────
# Runs one (tc, seed) pair. Handles server crashes by restarting the server
# and re-launching the run. Gives up after MAX_RETRIES consecutive failures
# that are NOT caused by a server crash.
#
# Returns 0 when the run succeeds, 1 if MAX_RETRIES was hit.

run_until_success() {
    local tc=$1 seed=$2
    local retries=0

    start_run "$tc" "$seed"

    while true; do
        sleep "$CHECK_INTERVAL"

        # ── Server health check ───────────────────────────────────────────────
        if ! kill -0 "$SERVER_PID" 2>/dev/null; then
            echo "[watchdog] Server crashed — restarting server and re-running seed=${seed} tc=${tc}ms"
            stop_run
            sleep 2
            start_server
            start_run "$tc" "$seed"
            # A server crash doesn't count against the retry budget
            continue
        fi

        # ── Experiment health check ───────────────────────────────────────────
        if ! kill -0 "$EXP_PID" 2>/dev/null; then
            wait "$EXP_PID"
            local status=$?

            if [[ $status -eq 0 ]]; then
                echo "[watchdog] Run succeeded: tc=${tc}ms seed=${seed}"
                return 0
            else
                ((retries++)) || true
                echo "[watchdog] Run failed (attempt ${retries}/${MAX_RETRIES}): tc=${tc}ms seed=${seed}"

                if [[ $retries -ge $MAX_RETRIES ]]; then
                    echo "[watchdog] Max retries reached for tc=${tc}ms seed=${seed} — skipping"
                    return 1
                fi

                echo "[watchdog] Retrying in 5s..."
                sleep 5
                start_run "$tc" "$seed"
            fi
        fi
    done
}

# ═══════════════════════════════════════════════════════════════════════════════
# Entry point
# ═══════════════════════════════════════════════════════════════════════════════

# Handle --reset flag
if [[ "${1:-}" == "--reset" ]]; then
    clear_state
fi

# Abort if a server is already running (keeps original behaviour)
EXISTING_PID=$(pgrep -f "ns3sionna_server.py" || true)
if [[ -n "$EXISTING_PID" ]]; then
    echo "[watchdog] A Sionna server is already running (PID=${EXISTING_PID}). Stop it first."
    exit 1
fi

echo "[watchdog] ════════════════════════════════════════════"
echo "[watchdog] Coherence times: ${COHERENCE_TIMES[*]} ms"
echo "[watchdog] Seeds:           ${START_SEED}..${END_SEED}"
echo "[watchdog] SNR logging:     ${ENABLE_SNR}"
echo "[watchdog] Position logging: ${ENABLE_POSITIONS}"
echo "[watchdog] Output base:     ${OUTPUT_BASE}"
echo "[watchdog] ════════════════════════════════════════════"

load_state
start_server

# ── Outer loop: coherence times ───────────────────────────────────────────────
while [[ $COHERENCE_IDX -lt ${#COHERENCE_TIMES[@]} ]]; do
    TC="${COHERENCE_TIMES[$COHERENCE_IDX]}"

    echo ""
    echo "[watchdog] ── Coherence time ${TC}ms (${COHERENCE_IDX}/${#COHERENCE_TIMES[@]}) ──"

    # ── Inner loop: seeds ─────────────────────────────────────────────────────
    while [[ $CURRENT_SEED -le $END_SEED ]]; do
        echo ""
        # Persist state before each run so a crash can resume here
        save_state "$COHERENCE_IDX" "$CURRENT_SEED"

        run_until_success "$TC" "$CURRENT_SEED"

        sleep 3  # brief pause between runs (matches original script)
        ((CURRENT_SEED++)) || true
    done

    # Advance to next coherence time, reset seed counter
    ((COHERENCE_IDX++)) || true
    CURRENT_SEED=$START_SEED
done

# ── All done ──────────────────────────────────────────────────────────────────
echo ""
echo "[watchdog] ════════════════════════════════════════════"
echo "[watchdog] All sweeps complete!"
echo "[watchdog] Results are in: ${OUTPUT_BASE}/"
echo "[watchdog] ════════════════════════════════════════════"

stop_server
clear_state
exit 0
