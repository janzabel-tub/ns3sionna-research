#!/bin/bash

cd ../../../

MODE=2
NUM_RES=16
NUM_STAS=32

echo "Test stationary / high load"

./ns3 run "ns3sionna-performance-sionna-tcp --sim_max_stas=${NUM_STAS} --mobile_scenario=false --mobile_speed=0.0 --caching=true --mode=${MODE} --sub_mode=${NUM_RES}"

echo "Test high mobility / higher load"

./ns3 run "ns3sionna-performance-sionna-tcp --sim_max_stas=${NUM_STAS} --mobile_scenario=true --mobile_speed=7.0 --caching=true --mode=${MODE} --sub_mode=${NUM_RES}"
