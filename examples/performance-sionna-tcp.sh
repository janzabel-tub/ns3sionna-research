#!/bin/bash

cd ../../../

MODE=2
NUM_RES=16
NUM_STAS=1
COHERENCE_TIME=1
MOBILE_SPEED=7

echo "Test mobility"

# 1 Start from 7
# 10 start from 57
# 10 high mobility start from 1
# 100 Done
# 1000 Done
for i in {7..100}
do
  echo "Current Seed: $i"
  ./ns3 run "ns3sionna-performance-sionna-tcp --sim_max_stas=${NUM_STAS} --mobile_scenario=true --mobile_speed=${MOBILE_SPEED}.0 --caching=true --mode=${MODE} --sub_mode=${NUM_RES} --minCoherenceTimeMs=${COHERENCE_TIME} --dataRate=150 --seed=$i"
  mv "throughput_results.csv" "tp_res_${MOBILE_SPEED}/throughput_results_${COHERENCE_TIME}_${i}.csv"
  # Waiting some time reduces crashes for some reason
  # Program still crashes after 30-60 Minutes.
  # Not sure the sleep does anything
  sleep 3
done

# echo "Test high mobility"

# ./ns3 run "ns3sionna-performance-sionna-tcp --sim_max_stas=${NUM_STAS} --mobile_scenario=true --mobile_speed=7.0 --caching=true --mode=${MODE} --sub_mode=${NUM_RES}"
