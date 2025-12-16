#!/bin/bash

echo "Starting server component of ns3sionna: dirty fix slow normal mode"

export PROTOCOL_BUFFERS_PYTHON_IMPLEMENTATION=python
python ns3sionna_server.py
