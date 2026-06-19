#!/bin/bash

cd ../../../

echo "Running ns3"
./ns3 run "ns3sionna-example-sionna-tcp-sensing-low --fixedCoherenceTimeMs=100 --dataRate=150 --speed=1.0"

echo "Plotting lower-layer log"
python3 ./contrib/sionna/examples/plot_lower_layer.py \
    --input example-sionna-tcp-sensing-low-l1.csv
