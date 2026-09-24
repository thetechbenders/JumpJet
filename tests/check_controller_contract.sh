#!/bin/sh
# Static guards for the PID-family controller/safety boundary.
set -eu

root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
control="$root/components/jj_control/jj_control.c"
control_h="$root/components/jj_control/include/jj_control.h"
interlock="$root/components/jj_interlock/jj_interlock.c"
manifest="$root/main/idf_component.yml"
command -v rg >/dev/null 2>&1 || { echo "controller contract check requires rg" >&2; exit 1; }

fail() { echo "$1" >&2; exit 1; }

# 1. jj_control holds no safety/eligibility predicates; jj_interlock owns them.
# Comments are stripped first so prose cannot mask or trip the check.
strip() { cc -fpreprocessed -dD -E -P "$1" 2>/dev/null; }
for file in "$control" "$control_h"; do
    if strip "$file" | rg -n '\b(fan_proof|fault\w*|printer|commissioned|overtemperature\w*|active_authority|manual_demand_authorized|control_inhibit|cooldown\w*|status|JJ_SENSOR_\w+|JJ_FAULT_\w+|JJ_MANUAL_TARGET_(MIN|MAX)_C|JJ_BLOCK_\w+|JJ_AUTHORITY_\w+)\b'; then
        fail "jj_control must not implement safety/eligibility predicates ($file)"
    fi
done

# 2. No product-level target-crossing override anywhere in firmware sources.
if rg -n --pcre2 '(temperature_c|measurement)[^;{}]*[<>]=?[^;{}]*(setpoint|target)|(setpoint|target)[^;{}]*[<>]=?[^;{}]*(temperature_c|measurement)' \
    "$root/components" "$root/main" -g '*.c' -g '*.h'; then
    fail "temperature-vs-target comparisons belong only inside dc_pid"
fi

# 3. The raw dc_pid output is the request, unmodified.
grep -q 'result.report.requested_duty_pct = result.pid.output;' "$control" ||
    fail "requested duty must be the unmodified dc_pid output"
[ "$(grep -c 'dc_pid_step(' "$control")" -eq 1 ] ||
    fail "jj_control must have exactly one dc_pid_step call site"

# 4. The retired boolean must not return.
if rg -n 'heater_requested' "$root/components" "$root/main" "$root/tests" -g '!check_controller_contract.sh'; then
    fail "heater_requested was replaced by heater_authorized/allowed_duty_pct"
fi

# 5. Stage 2 authorizes only RUN + VALID, from interlock-retained state.
grep -q 'eligibility.disposition == JJ_DISPOSITION_RUN &&' "$interlock" ||
    fail "Stage 2 must require RUN disposition"
grep -q 'const jj_eligibility_t eligibility = state->pending;' "$interlock" ||
    fail "Stage 2 must use the interlock-retained Stage-1 result"

# 6. dc_pid is pinned to the same dragon-core commit as every other dc_* dep.
pins=$(awk '/^    version:/ { print $2 }' "$manifest" | sort -u)
[ "$(printf '%s\n' "$pins" | wc -l)" -eq 1 ] ||
    fail "all dragon-core components must share one pinned commit"
grep -q '^  dc_pid:' "$manifest" || fail "dc_pid dependency missing"

echo "controller/safety boundary contract check: PASS"
