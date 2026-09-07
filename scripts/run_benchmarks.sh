#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
BUILD_DIR=${BUILD_DIR:-"$ROOT_DIR/build"}
ACTIVE_PIDS=()

cleanup() {
  if ((${#ACTIVE_PIDS[@]})); then
    kill -TERM "${ACTIVE_PIDS[@]}" 2>/dev/null || true
    wait "${ACTIVE_PIDS[@]}" 2>/dev/null || true
  fi
}
trap cleanup EXIT INT TERM

for executable in streamvault_recorder imu_producer gps_producer temperature_producer; do
  [[ -x "$BUILD_DIR/$executable" ]] || { echo "Missing $BUILD_DIR/$executable; build the project first." >&2; exit 1; }
done

run_scenario() {
  local name=$1 port=$2 imu_hz=$3 gps_hz=$4 temp_hz=$5 drop=$6 delay=$7 jitter=$8
  local session="$ROOT_DIR/recordings/${name}.dat"
  local summary="$ROOT_DIR/results/${name}.json"
  echo "Running $name scenario..."
  "$BUILD_DIR/streamvault_recorder" --port "$port" --output "$session" --summary "$summary" >"$ROOT_DIR/results/${name}.log" 2>&1 &
  local recorder=$!
  ACTIVE_PIDS=("$recorder")
  sleep 0.3
  "$BUILD_DIR/imu_producer" --port "$port" --hz "$imu_hz" --drop-rate "$drop" --delay-ms "$delay" --jitter-ms "$jitter" --seed 101 >/dev/null & ACTIVE_PIDS+=("$!")
  "$BUILD_DIR/gps_producer" --port "$port" --hz "$gps_hz" --drop-rate "$drop" --delay-ms "$delay" --jitter-ms "$jitter" --seed 202 >/dev/null & ACTIVE_PIDS+=("$!")
  "$BUILD_DIR/temperature_producer" --port "$port" --hz "$temp_hz" --drop-rate "$drop" --delay-ms "$delay" --jitter-ms "$jitter" --seed 303 >/dev/null & ACTIVE_PIDS+=("$!")
  sleep 4
  kill -TERM "${ACTIVE_PIDS[@]:1}"
  wait "${ACTIVE_PIDS[@]:1}"
  kill -TERM "$recorder"
  wait "$recorder"
  ACTIVE_PIDS=()
  [[ -s "$session" && -s "$summary" ]]
  python3 -m json.tool "$summary" >/dev/null
  echo "$name completed: $summary"
}

run_scenario nominal 9101 100 10 2 0 0 0
run_scenario latency 9102 100 10 2 0 5 3
run_scenario packet_loss 9103 100 10 2 0.05 0 0
run_scenario high_rate 9104 1000 200 50 0 0 0

echo "All benchmark scenarios completed successfully."
