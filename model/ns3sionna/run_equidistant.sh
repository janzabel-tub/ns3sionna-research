#!/bin/bash

EQUIDISTANT_WALK_STEPS=${EQUIDISTANT_WALK_STEPS:-100000}

echo "Starting server component of ns3sionna: equidistant walk step=${EQUIDISTANT_WALK_STEPS} ns"
python ns3sionna_server.py --equidistant_walk_steps "$EQUIDISTANT_WALK_STEPS"
