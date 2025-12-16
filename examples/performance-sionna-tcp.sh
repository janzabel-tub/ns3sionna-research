#!/bin/bash

cd ../../../

echo "Running ns3"
./ns3 run ns3sionna-performance-sionna-tcp

echo "Plotting results"
python3 ./contrib/sionna/examples/plot_csi.py csi_node0.csv csi_node1.csv
