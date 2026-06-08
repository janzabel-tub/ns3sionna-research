#!/bin/bash

cd ../../../

echo "Running ns3"
./ns3 run "ns3sionna-example-sionna-fixedTc --minCoherenceTimeMs=100 --dataRate=150"
