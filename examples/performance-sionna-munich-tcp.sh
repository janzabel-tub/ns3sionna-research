#!/bin/bash
# Single-shot simulation runner for the Munich outdoor scenario.
# All parameters are supplied via environment variables by watchdog-munich.sh.
# Can also be run standalone by exporting variables manually.
#
# Required env vars (all have defaults for standalone use):
#   MODE, NUM_RES, NUM_STAS, MOBILE_SPEED, COHERENCE_TIME, SEED,
#   DATA_RATE, OUTPUT_DIR
#
# NOTE: Unlike the indoor runner, the Munich binary has SNR logging always
# enabled (hardcoded) and does not support --snr / --positions flags.
# snr_results.csv is therefore always moved to OUTPUT_DIR.

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
OUTPUT_DIR=${OUTPUT_DIR:-"results-munich/tc${COHERENCE_TIME}"}

# ── Prepare output directory ──────────────────────────────────────────────────
mkdir -p "$OUTPUT_DIR"

echo "[runner-munich] coherence=${COHERENCE_TIME}ms seed=${SEED} → ${OUTPUT_DIR}"

# ── Run simulation ────────────────────────────────────────────────────────────
# The Munich binary does not accept --snr or --positions flags.
./ns3 run "ns3sionna-performance-sionna-munich-tcp \
  --sim_max_stas=${NUM_STAS} \
  --mobile_scenario=true \
  --mobile_speed=${MOBILE_SPEED}.0 \
  --caching=true \
  --mode=${MODE} \
  --sub_mode=${NUM_RES} \
  --minCoherenceTimeMs=${COHERENCE_TIME} \
  --dataRate=${DATA_RATE} \
  --seed=${SEED}"

# ── Move outputs into the run-specific directory ──────────────────────────────
TAG="${COHERENCE_TIME}_${SEED}"

mv "throughput_results.csv" "${OUTPUT_DIR}/throughput_results_${TAG}.csv"

# SNR logging is always active in the Munich binary — move unconditionally.
mv "snr_results.csv" "${OUTPUT_DIR}/snr_results_${TAG}.csv"

echo "[runner-munich] done → outputs saved to ${OUTPUT_DIR}/"
