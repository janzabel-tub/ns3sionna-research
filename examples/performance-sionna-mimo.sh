#!/bin/bash
set -euo pipefail

# The beta must be calibrated for the selected MCS. The default 2 is only the
# existing MIMO test value; it is not a production calibration.
cd ../../../

EESM_BETA_LINEAR=${EESM_BETA_LINEAR:-2}
MCS=${MCS:-11}
MOBILE_SPEED_MPS=${MOBILE_SPEED_MPS:-1}
TX_POWER_DBM=${TX_POWER_DBM:-20}
SIMULATION_TIME_SECONDS=${SIMULATION_TIME_SECONDS:-10}
SEED=${SEED:-1}

./ns3 run "ns3sionna-performance-sionna-mimo \
  --environment=apartment/apartment.xml \
  --mobileSpeedMps=${MOBILE_SPEED_MPS} \
  --txPowerDbm=${TX_POWER_DBM} \
  --simulationTimeSeconds=${SIMULATION_TIME_SECONDS} \
  --mcs=${MCS} \
  --eesmBetaLinear=${EESM_BETA_LINEAR} \
  --offeredRate=1Gbps \
