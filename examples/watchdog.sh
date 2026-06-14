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
COHERENCE_TIMES=(0 1 10 100 1000)

# Seed range (inclusive)
START_SEED=1
END_SEED=1

# Simulation parameters exported to the runner script
export RUN_LABEL=${RUN_LABEL:-$(date +%Y%m%d_%H%M%S)}
export MODE=2
export NUM_RES=16
export NUM_STAS=1
export MOBILE_SPEED=7
export DATA_RATE=150
export ENVIRONMENT="apartment/apartment.xml"  # Use "munich/munich.xml" for Munich positions
export TX_POWER=30

# Output: each coherence time gets its own subdirectory under OUTPUT_BASE
OUTPUT_BASE="$EXAMPLES/results_sionna2/regApartmenthighSpeed30dbm"
WB_LOSS_LOG="$OUTPUT_BASE/wb_loss_${RUN_LABEL}.csv"

# Optional features passed through to the C++ binary
export ENABLE_SNR=true
export ENABLE_POSITIONS=true
export ENABLE_RATE_TRACE=false
export EQUIDISTANT_WALK_STEPS=${EQUIDISTANT_WALK_STEPS:-100000}

# How many seconds between health-check polls
CHECK_INTERVAL=60

# Max wall-clock seconds one run may take before it is treated like a crash.
# Set to 0 to disable.
RUN_TIMEOUT_SECONDS=${RUN_TIMEOUT_SECONDS:-1800}

# Max consecutive failures before a seed is abandoned and the sweep continues
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
    mkdir -p "$OUTPUT_BASE"
    cd "$SERVER_DIR" || { echo "Cannot cd to $SERVER_DIR"; exit 1; }
    export NS3SIONNA_MODEL_FOLDER="$SERVER_DIR/models"
    python ns3sionna_server.py \
        --equidistant_walk_steps "$EQUIDISTANT_WALK_STEPS" \
        --wb_loss_log "$WB_LOSS_LOG" > "$SERVER_DIR/server.log" 2>&1 &
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
# Runs one (tc, seed) pair. Handles server crashes and run timeouts by
# restarting the server and re-launching the run. Gives up after MAX_RETRIES
# consecutive failures, including repeated server crashes/timeouts.
#
# Returns 0 when the run succeeds, 1 if MAX_RETRIES was hit.

run_until_success() {
    local tc=$1 seed=$2
    local retries=0
    local run_started_at elapsed

    start_run "$tc" "$seed"
    run_started_at=$(date +%s)

    while true; do
        sleep "$CHECK_INTERVAL"

        # Treat a stuck run like a crash: kill run + server, then relaunch both.
        elapsed=$(( $(date +%s) - run_started_at ))
        if [[ $RUN_TIMEOUT_SECONDS -gt 0 && $elapsed -ge $RUN_TIMEOUT_SECONDS ]] && kill -0 "$EXP_PID" 2>/dev/null; then
            ((retries++)) || true
            echo "[watchdog] Run timed out after ${elapsed}s (limit ${RUN_TIMEOUT_SECONDS}s) — restarting server and re-running seed=${seed} tc=${tc}ms"

            if [[ $retries -ge $MAX_RETRIES ]]; then
                echo "[watchdog] Max retries reached after repeated timeouts for tc=${tc}ms seed=${seed} — skipping"
                stop_run
                stop_server
                sleep 2
                start_server
                return 1
            fi

            stop_run
            stop_server
            sleep 2
            start_server
            start_run "$tc" "$seed"
            run_started_at=$(date +%s)
            continue
        fi

        # ── Server health check ───────────────────────────────────────────────
        if ! kill -0 "$SERVER_PID" 2>/dev/null; then
            ((retries++)) || true
            echo "[watchdog] Server crashed — restarting server and re-running seed=${seed} tc=${tc}ms"
            if [[ $retries -ge $MAX_RETRIES ]]; then
                echo "[watchdog] Max retries reached after repeated server crashes for tc=${tc}ms seed=${seed} — skipping"
                stop_run
                sleep 2
                start_server
                return 1
            fi
            stop_run
            sleep 2
            start_server
            start_run "$tc" "$seed"
            run_started_at=$(date +%s)
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
                run_started_at=$(date +%s)
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
echo "[watchdog] Run label:       ${RUN_LABEL}"
echo "[watchdog] Environment:     ${ENVIRONMENT}"
echo "[watchdog] Tx power:        ${TX_POWER} dBm"
echo "[watchdog] SNR logging:     ${ENABLE_SNR}"
echo "[watchdog] Position logging: ${ENABLE_POSITIONS}"
echo "[watchdog] WB loss logging:  ${WB_LOSS_LOG}"
echo "[watchdog] Equidistant walk step: ${EQUIDISTANT_WALK_STEPS} ns"
echo "[watchdog] Run timeout:     ${RUN_TIMEOUT_SECONDS} s"
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
