#!/bin/bash

cd ../../../

echo "Running ns3"
./ns3 run "ns3sionna-example-sionna-fixedTc-mobile --fixedCoherenceTimeMs=100 --dataRate=150 --speed=1.0"
