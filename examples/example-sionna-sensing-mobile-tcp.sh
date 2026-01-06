#!/bin/bash

cd ../../../

echo "Running ns3"
./ns3 run "ns3sionna-example-sionna-sensing-mobile-tcp --minCoherenceTimeMs=0 --dataRate=10 --speed=5.0"

./ns3 run "ns3sionna-example-sionna-sensing-mobile-tcp --minCoherenceTimeMs=10 --dataRate=10 --speed=5.0"

./ns3 run "ns3sionna-example-sionna-sensing-mobile-tcp --minCoherenceTimeMs=100 --dataRate=10 --speed=5.0"

./ns3 run "ns3sionna-example-sionna-sensing-mobile-tcp --minCoherenceTimeMs=250 --dataRate=10 --speed=5.0"

./ns3 run "ns3sionna-example-sionna-sensing-mobile-tcp --minCoherenceTimeMs=500 --dataRate=10 --speed=5.0"

echo "Plotting results"
python3 ./contrib/sionna/examples/plot3d_mobile_csi.py example-sionna-sensing-mobile.csv example-sionna-sensing-mobile-pathloss.csv example-sionna-sensing-mobile-time-pos.csv
