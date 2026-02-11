#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
BUILD_DIR="${BUILD_DIR:-$ROOT_DIR/build}"
CONFIG_PATH="${1:-$ROOT_DIR/config/config.json}"
RESTART_ON_EXIT="${RESTART_ON_EXIT:-1}"
RESTART_DELAY_SEC="${RESTART_DELAY_SEC:-2}"
LOG_DIR="${LOG_DIR:-$ROOT_DIR/logs}"
LOG_FILE="${LOG_FILE:-$LOG_DIR/rpi_calendar.log}"

if command -v nproc >/dev/null 2>&1; then
  DEFAULT_JOBS="$(nproc)"
else
  DEFAULT_JOBS=4
fi
JOBS="${JOBS:-$DEFAULT_JOBS}"

if [[ ! -f "$CONFIG_PATH" ]]; then
  echo "Config file not found: $CONFIG_PATH" >&2
  exit 1
fi

cmake -S "$ROOT_DIR" -B "$BUILD_DIR"
cmake --build "$BUILD_DIR" -j"$JOBS"

mkdir -p "$LOG_DIR"

disable_screen_blanking() {
  if [[ -n "${DISPLAY:-}" ]] && command -v xset >/dev/null 2>&1; then
    xset s off || true
    xset -dpms || true
    xset s noblank || true
  fi
  if command -v vcgencmd >/dev/null 2>&1; then
    vcgencmd display_power 1 >/dev/null 2>&1 || true
  fi
}

run_clock() {
  local -a cmd=("$BUILD_DIR/rpi_calendar" "$CONFIG_PATH")
  if command -v systemd-inhibit >/dev/null 2>&1; then
    cmd=(
      systemd-inhibit
      --what=idle:sleep:shutdown
      --who="rpi_calendar"
      --why="Keep kiosk clock running"
      --mode=block
      "${cmd[@]}"
    )
  fi

  disable_screen_blanking
  "${cmd[@]}"
}

while true; do
  echo "[$(date '+%Y-%m-%d %H:%M:%S')] Starting rpi_calendar" | tee -a "$LOG_FILE"
  if run_clock >>"$LOG_FILE" 2>&1; then
    exit 0
  fi
  exit_code=$?
  echo "[$(date '+%Y-%m-%d %H:%M:%S')] rpi_calendar exited with code $exit_code" | tee -a "$LOG_FILE"
  if [[ "$RESTART_ON_EXIT" != "1" ]]; then
    exit "$exit_code"
  fi
  sleep "$RESTART_DELAY_SEC"
done
