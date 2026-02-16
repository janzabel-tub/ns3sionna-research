#!/bin/bash

set -uo pipefail

EXAMPLES="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
SERVER_DIR="$EXAMPLES/../model/ns3sionna"

SERVER_CMD="python ns3sionna_server.py"
EXPERIMENT_CMD="./performance-sionna-tcp.sh"

CHECK_INTERVAL=60   # seconds

# check if experiment is killed after everything is over.

cd "$EXAMPLES" || exit 1

start_server() {
  echo "=== Starting Sionna server ==="
  # The watchdog script is not in the same directory as the server
  # When executing the watchdog script, the working directory is sionna/examples
  # When running the server using python ../model/ns3sionna/ns3sionna_server.py this is still true
  # The server uses relative paths within itself (Line 51: model_folder='./models/'
  # When executing without changing to the ns3sionna directory, it looks for examples/models, which does not exist
  # Changing to sionna/model/ns3sionna is necessary
  cd "$SERVER_DIR" || exit 1

  # Absolute model folder to avoid cwd issues
  export NS3SIONNA_MODEL_FOLDER="$SERVER_DIR/models"

  python ns3sionna_server.py > server.log 2>&1 &
  SERVER_PID=$!

  # Give server time to fully initialize
  sleep 8
}

start_experiment() {
  echo "=== Starting experiment ==="
  cd "$EXAMPLES" || exit 1
  "./performance-sionna-tcp.sh" &
  EXP_PID=$!
}

# Stopping the program early is an issue.
cleanup() {
  echo "Stopping program"
  [[ -n "$SERVER_PID"  ]] && kill $SERVER_PID 2>/dev/null
  [[ -n "$EXP_PID"  ]] && kill $EXP_PID 2>/dev/null
  pkill -f "ns3 run ns3sionna-performance-sionna-tcp"
  pkill -f "ns3.40-ns3sionna-performance-sionna-tcp-default"
  wait
  exit 0
}

trap cleanup SIGINT SIGTERM

# Program has problem if server is already running.
# Stops server before starting
EXIST_SERVER_PID=$(pgrep -f "ns3sionna_server.py")
if [[ -n "$EXIST_SERVER_PID"  ]]; then
  echo "Server running."
  exit 1
fi

start_server
start_experiment

while true; do
  sleep $CHECK_INTERVAL

  # Check server
  if ! kill -0 $SERVER_PID 2>/dev/null; then
    echo "=== Server crashed. Restarting ==="

    # Kill experiment if still running
    if kill -0 $EXP_PID 2>/dev/null; then
      kill $EXP_PID
      wait $EXP_PID 2>/dev/null
      pkill -f "ns3 run ns3sionna-performance-sionna-tcp"
      pkill -f "ns3.40-ns3sionna-performance-sionna-tcp-default"
    fi

    start_server
    start_experiment
  fi

  # Check experiment
  if ! kill -0 $EXP_PID 2>/dev/null; then
    wait $EXP_PID
    STATUS=$?

    if [[ $STATUS -eq 0 ]]; then
      echo "=== Experiment finished successfully ==="
      kill $SERVER_PID 2>/dev/null
      pkill -f "ns3 run ns3sionna-performance-sionna-tcp"
      pkill -f "ns3.40-ns3sionna-performance-sionna-tcp-default"
      exit 0
    else
      echo "=== Experiment crashed. Restarting ==="
      start_experiment
    fi
  fi
done
