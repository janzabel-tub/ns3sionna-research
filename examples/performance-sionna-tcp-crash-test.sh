#!/bin/bash
set -euo pipefail


cd ../../../

MODE=2
NUM_RES=16
NUM_STAS=1
# MAX_SEED=100

SEED_FILE="last_seed.txt"
CHECK_INTERVAL=60

if [ ! -f "$SEED_FILE" ]; then
  echo 0 > "$SEED_FILE"
fi

LAST_SEED=$(cat "$SEED_FILE")
START_SEED=$((LAST_SEED + 1))

# echo "Test mobility"

pushd contrib/sionna/model/ns3sionna > /dev/null
pkill -f ns3sionna_server.py
sleep 2
python ns3sionna_server.py &
SERVER_PID=$!
popd > /dev/null

sleep 10

for ((i=START_SEED; i<=100; i++))
do
  echo "Current Seed: $i"
  ./ns3 run "ns3sionna-performance-sionna-tcp --sim_max_stas=${NUM_STAS} --mobile_scenario=true --mobile_speed=1.0 --caching=true --mode=${MODE} --sub_mode=${NUM_RES} --minCoherenceTimeMs=10 --dataRate=150 --seed=$i"
  NS3_PID=$!

  while kill -0 "$NS3_PID" 2>/dev/null; do
    sleep "$CHECK_INTERVAL"
    if ! kill -0 "$SERVER_PID" 2>/dev/null; then
      echo "Server crashed! Restarting..."

      kill "$NS3_PID" 2>/dev/null
      wait "$NS3_PID" 2>/dev/null

      python ns3sionna_server.py &
      SERVER_PID=$!
      sleep 5

      echo "Retrying seed $i"
      continue 2
    fi
  done

  wait "$NS3_PID"
  EXIT_CODE=$?

  if [ "$EXIT_CODE" -ne 0 ]; then
    echo "ns-3 crashed, retrying seed $i"
    continue
  fi

  mv "throughput_results.csv" "tp_res/throughput_results_10_$i.csv"
  echo "$i" > "$SEED_FILE"

  sleep 3
done
