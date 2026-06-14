#!/bin/bash

cd ../../../

echo "Running ns3"
./ns3 run "ns3sionna-example-sionna-tcp-minstrel --minCoherenceTimeMs=100 --dataRate=150 --speed=1.0"
