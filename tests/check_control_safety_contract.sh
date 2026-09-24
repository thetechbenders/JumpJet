#!/bin/sh
set -eu

root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
app="$root/main/app_main.c"
authority="$root/components/jj_authority/jj_authority.c"
portal="$root/components/jj_portal/jj_portal.c"
interlock="$root/components/jj_interlock/jj_interlock.c"

prusa_line=$(grep -n -m1 'dc_prusa_get_status(&printer)' "$app" | cut -d: -f1)
control_line=$(grep -n -m1 'jj_authority_control_step(' "$app" | cut -d: -f1)
[ "$prusa_line" -lt "$control_line" ]

if grep -q 'jj_authority_snapshot' "$app"; then
    echo "control task must not snapshot authority before cached inputs" >&2
    exit 1
fi
if grep -Eq 'jj_interlock_(evaluate|authorize)|jj_controller_step' "$app" "$portal"; then
    echo "ordinary production interlock evaluation must remain control-task owned" >&2
    exit 1
fi

if rg -n '(ESP_LOG[A-Z]*|\b(printf|snprintf|vfprintf|malloc|calloc|realloc|free|xSemaphoreTake|vTaskDelay)[[:space:]]*\(|esp_http)' \
    "$authority" "$root/components/jj_control/jj_control.c" >/dev/null; then
    echo "authority critical-section unit contains forbidden unbounded work" >&2
    exit 1
fi

grep -q 'Lock ordering is AUTHORITY_LOCK -> jj_interlock' "$authority"
grep -q 'return JJ_REMOTE_ACK_WHEN_HEALTHY' "$interlock"
grep -q 'return JJ_REMOTE_ACK_AFTER_REVALIDATION' "$interlock"
grep -q 'return JJ_REMOTE_ACK_NEVER' "$interlock"

for fn in acquire_post refresh_post heartbeat_post mutate_post; do
    body=$(awk -v fn="$fn" '
        $0 ~ "^static esp_err_t "fn"\\(" { capture=1 }
        capture { print }
        capture && /^}/ { exit }
    ' "$portal")
    if ! printf '%s\n' "$body" | grep -q 'authorize(req'; then
        echo "control handler $fn must call authorize(req) before mutating authority state" >&2
        exit 1
    fi
done

echo "control ordering/locking/fault-policy contract check: PASS"
