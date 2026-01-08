#!/bin/bash

cd ../../../

MODE=2
NUM_RES=16
NUM_STAS=1

echo "Test low mobility"

./ns3 run "ns3sionna-performance-sionna-tcp --sim_max_stas=${NUM_STAS} --mobile_scenario=true --mobile_speed=15.0 --caching=true --mode=${MODE} --sub_mode=${NUM_RES} --minCoherenceTimeMs=0 --dataRate=150"

# echo "Test high mobility"

# ./ns3 run "ns3sionna-performance-sionna-tcp --sim_max_stas=${NUM_STAS} --mobile_scenario=true --mobile_speed=7.0 --caching=true --mode=${MODE} --sub_mode=${NUM_RES}"
