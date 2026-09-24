#!/usr/bin/env bash
set -euo pipefail
root=$(cd "$(dirname "$0")/.." && pwd)
dc_pid=$("$root/tests/fetch_dc_pid.sh")
out=$(mktemp)
trap 'rm -f "$out"' EXIT
cc -std=c11 -Wall -Wextra -Werror \
  -fsanitize=undefined -fno-sanitize-recover=undefined \
  -I"$root/components/jj_interlock/include" \
  -I"$root/components/jj_control/include" \
  -I"$dc_pid/include" \
  "$root/components/jj_interlock/jj_interlock.c" \
  "$root/components/jj_control/jj_control.c" \
  "$dc_pid/dc_pid.c" \
  "$root/tests/jj_control_host_test.c" -lm -o "$out"
"$out"
