#!/bin/bash

cd ../../../

echo "Running ns3"
./ns3 run "ns3sionna-example-sionna-fixedTc-mobile-sensing --minCoherenceTimeMs=100 --speed=1.0"

echo "Plotting results"
python3 ./contrib/sionna/examples/plot_snr.py example-sionna-fixedTc-mobile-sensing-snr-position.csv
