#!/bin/bash

cd ../../../

echo "Running ns3"
./ns3 run "ns3sionna-example-sionna-tcp-minstrel --fixedCoherenceTimeMs=100 --dataRate=150 --speed=1.0"

echo "Plotting Minstrel logs"
python3 ./contrib/sionna/examples/plot_minstrel.py \
    --tx-vectors example-sionna-tcp-minstrel-tx-vectors.csv \
    --rate-changes example-sionna-tcp-minstrel-rate-changes.csv
