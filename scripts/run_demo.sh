#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
BUILD_DIR=${BUILD_DIR:-"$ROOT_DIR/build"}
PORT=${PORT:-9000}
SESSION="$ROOT_DIR/recordings/demo.dat"
SUMMARY="$ROOT_DIR/results/demo.json"
PIDS=()

cleanup() {
  if ((${#PIDS[@]})); then
    kill -TERM "${PIDS[@]}" 2>/dev/null || true
    wait "${PIDS[@]}" 2>/dev/null || true
  fi
}
trap cleanup EXIT INT TERM

for executable in streamvault_recorder imu_producer gps_producer temperature_producer streamvault_replay; do
  [[ -x "$BUILD_DIR/$executable" ]] || { echo "Missing $BUILD_DIR/$executable; build the project first." >&2; exit 1; }
done

"$BUILD_DIR/streamvault_recorder" --port "$PORT" --output "$SESSION" --summary "$SUMMARY" &
RECORDER_PID=$!
PIDS+=("$RECORDER_PID")
sleep 0.4

"$BUILD_DIR/imu_producer" --port "$PORT" --seed 11 & PIDS+=("$!")
"$BUILD_DIR/gps_producer" --port "$PORT" --seed 22 & PIDS+=("$!")
"$BUILD_DIR/temperature_producer" --port "$PORT" --seed 33 & PIDS+=("$!")
sleep 5

kill -TERM "${PIDS[@]:1}"
wait "${PIDS[@]:1}"
kill -TERM "$RECORDER_PID"
wait "$RECORDER_PID"
PIDS=()

[[ -s "$SESSION" ]] || { echo "Demo session was not created." >&2; exit 1; }
[[ -s "$SUMMARY" ]] || { echo "Demo summary was not created." >&2; exit 1; }
python3 -m json.tool "$SUMMARY" >/dev/null

echo "Demo session: $SESSION"
echo "Demo summary: $SUMMARY"
echo "Accelerated replay at 2x:"
"$BUILD_DIR/streamvault_replay" "$SESSION" --speed 2.0
