#!/bin/bash

EQUIDISTANT_WALK_STEPS=${EQUIDISTANT_WALK_STEPS:-100000}
MOBILITY_BBOX_MARGIN=${MOBILITY_BBOX_MARGIN:-1.0}

echo "Starting server component of ns3sionna: equidistant walk step=${EQUIDISTANT_WALK_STEPS} ns, bbox margin=${MOBILITY_BBOX_MARGIN} m"
python ns3sionna_server.py \
    --equidistant_walk_steps "$EQUIDISTANT_WALK_STEPS" \
    --mobility_bbox_toggle true \
    --mobility_bbox_margin "$MOBILITY_BBOX_MARGIN"
