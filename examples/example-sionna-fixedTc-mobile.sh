#!/bin/bash

cd ../../../

echo "Running ns3"
./ns3 run "ns3sionna-example-sionna-fixedTc-mobile --minCoherenceTimeMs=100 --speed=1.0"
