#!/bin/bash

cd ../../../

SEED_FILE="contrib/sionna/examples/last_seed.txt"

MODE=2
NUM_RES=16
NUM_STAS=1
COHERENCE_TIME=1
MOBILE_SPEED=7

if [[ -f "$SEED_FILE" ]]; then
  START_SEED=$(cat "$SEED_FILE")
else
  echo "Seed file not found. Please create a file called last_seed.txt with the value 1 inside."
  START_SEED=1
fi

echo "Starting simulation"

for ((i=START_SEED; i<=100; i++))
do
  echo "Current Seed: $i"
  # Saving the seed in the file to restart from this position.
  echo "$i" > "$SEED_FILE"
  # TODO: Munich Scenario --environment=munich/munich.xml
  ./ns3 run "ns3sionna-performance-sionna-tcp --sim_max_stas=${NUM_STAS} --mobile_scenario=true --mobile_speed=${MOBILE_SPEED}.0 --caching=true --mode=${MODE} --sub_mode=${NUM_RES} --minCoherenceTimeMs=${COHERENCE_TIME} --dataRate=150 --seed=$i"

  # Crash test
  if [[ $? -ne 0 ]]; then
    echo "Crashed. Last seed: $i"
    exit 1
  fi

  # Rename file here, I was too lazy to do it in the cpp script.
  # mv "throughput_results.csv" "tp_res_${MOBILE_SPEED}/throughput_results_${COHERENCE_TIME}_${i}.csv"

  # I have a theory the server crashes less if there is a small waiting time between simulations
  # I have not tested this theory. Just a feeling. This may waste 3 seconds every time.
  sleep 3
done
