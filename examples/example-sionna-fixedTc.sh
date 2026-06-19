#!/bin/bash

cd ../../../

echo "Running ns3"
./ns3 run "ns3sionna-example-sionna-fixedTc --fixedCoherenceTimeMs=100 --dataRate=150"
