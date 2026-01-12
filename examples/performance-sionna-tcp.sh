#!/bin/bash

cd ../../../

MODE=2
NUM_RES=16
NUM_STAS=1

echo "Test mobility"

# Start from 58

for i in {86..100}
do
  echo "Current Seed: $i"
  ./ns3 run "ns3sionna-performance-sionna-tcp --sim_max_stas=${NUM_STAS} --mobile_scenario=true --mobile_speed=1.0 --caching=true --mode=${MODE} --sub_mode=${NUM_RES} --minCoherenceTimeMs=1000 --dataRate=150 --seed=$i"
  mv "throughput_results.csv" "tp_res/throughput_results_1000_$i.csv"
  sleep 3
done

# echo "Test high mobility"

# ./ns3 run "ns3sionna-performance-sionna-tcp --sim_max_stas=${NUM_STAS} --mobile_scenario=true --mobile_speed=7.0 --caching=true --mode=${MODE} --sub_mode=${NUM_RES}"
