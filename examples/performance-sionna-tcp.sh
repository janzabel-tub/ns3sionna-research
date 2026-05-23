#!/bin/bash
# Single-shot simulation runner.
# All parameters are supplied via environment variables by watchdog.sh.
# Can also be run standalone by exporting variables manually.
#
# Required env vars (all have defaults for standalone use):
#   MODE, NUM_RES, NUM_STAS, MOBILE_SPEED, COHERENCE_TIME, SEED,
#   DATA_RATE, ENVIRONMENT, TX_POWER, OUTPUT_DIR, ENABLE_SNR, ENABLE_POSITIONS

set -euo pipefail

cd ../../../

# ── Parameter defaults (overridden by watchdog via env) ──────────────────────
MODE=${MODE:-2}
NUM_RES=${NUM_RES:-16}
NUM_STAS=${NUM_STAS:-1}
MOBILE_SPEED=${MOBILE_SPEED:-1}
COHERENCE_TIME=${COHERENCE_TIME:-0}
SEED=${SEED:-1}
DATA_RATE=${DATA_RATE:-600}
ENVIRONMENT=${ENVIRONMENT:-"apartment/apartment.xml"}
TX_POWER=${TX_POWER:-20}
OUTPUT_DIR=${OUTPUT_DIR:-"results/tc${COHERENCE_TIME}"}
RUN_LABEL=${RUN_LABEL:-manual}
RUN_ID=${RUN_ID:-$(date +%Y%m%d_%H%M%S)_$$}

# Optional feature flags (set to "true" to enable)
ENABLE_SNR=${ENABLE_SNR:-false}
ENABLE_POSITIONS=${ENABLE_POSITIONS:-false}

# ── Prepare output directory ──────────────────────────────────────────────────
mkdir -p "$OUTPUT_DIR"
echo "[runner] label=${RUN_LABEL} run=${RUN_ID}"

echo "[runner] coherence=${COHERENCE_TIME}ms seed=${SEED} → ${OUTPUT_DIR}"

# ── Build optional flags ──────────────────────────────────────────────────────
EXTRA_FLAGS=""
[[ "$ENABLE_SNR"       == "true" ]] && EXTRA_FLAGS="$EXTRA_FLAGS --snr=true"
[[ "$ENABLE_POSITIONS" == "true" ]] && EXTRA_FLAGS="$EXTRA_FLAGS --positions=true"

# ── Run simulation ────────────────────────────────────────────────────────────
./ns3 run "ns3sionna-performance-sionna-tcp \
  --sim_max_stas=${NUM_STAS} \
  --environment=${ENVIRONMENT} \
  --mobile_scenario=true \
  --mobile_speed=${MOBILE_SPEED}.0 \
  --caching=true \
  --mode=${MODE} \
  --sub_mode=${NUM_RES} \
  --minCoherenceTimeMs=${COHERENCE_TIME} \
  --dataRate=${DATA_RATE} \
  --txPower=${TX_POWER} \
  --seed=${SEED} \
  ${EXTRA_FLAGS}"

# ── Move outputs into the run-specific directory ──────────────────────────────
TAG="${COHERENCE_TIME}_${SEED}_${RUN_LABEL}_${RUN_ID}"

mv "throughput_results.csv" "${OUTPUT_DIR}/throughput_results_${TAG}.csv"

[[ -f "computationtime.csv" ]] && \
    mv "computationtime.csv" "${OUTPUT_DIR}/computationtime_${TAG}.csv"

[[ "$ENABLE_SNR"       == "true" && -f "snr_results.csv"      ]] && \
    mv "snr_results.csv"       "${OUTPUT_DIR}/snr_results_${TAG}.csv"

[[ "$ENABLE_POSITIONS" == "true" && -f "position_results.csv" ]] && \
    mv "position_results.csv"  "${OUTPUT_DIR}/position_results_${TAG}.csv"

echo "[runner] done → outputs saved to ${OUTPUT_DIR}/"
