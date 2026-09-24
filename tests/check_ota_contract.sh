#!/bin/sh
set -eu

root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
portal="$root/components/jj_portal/jj_portal.c"
app="$root/main/app_main.c"
interlock="$root/components/jj_interlock/jj_interlock.c"

grep -q 'guard_operation = guard_operation' "$portal"
grep -q 'validate_image = validate_image' "$portal"
grep -q 'esp_err_t err = thermal_state_guard(message, message_size);' "$portal"
grep -q '!output.heater_authorized && !output.thermal_management_required' "$portal"
grep -q 'strcmp(image->project_name, JJ_IDENTITY_PRODUCT_ID)' "$portal"
grep -q '"commissioned", false' "$portal"
grep -q '"heater_available", false' "$portal"
grep -q 'jj_inputs_safe_defaults()' "$app"
grep -q 'dc_prusa_get_status(&printer) == ESP_OK' "$app"
grep -q 'xTaskNotifyGive(startup_task)' "$app"
grep -q 'ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(2000))' "$app"
grep -q 'jj_interlock_snapshot(&s_interlock)' "$app"
grep -q 'portENTER_CRITICAL(&s_state_lock)' "$interlock"
grep -q 'portEXIT_CRITICAL(&s_state_lock)' "$interlock"

portal_line=$(grep -n 'jj_portal_start(&s_interlock, &s_authority)' "$app" | cut -d: -f1)
health_line=$(grep -n 'ulTaskNotifyTake(pdTRUE' "$app" | cut -d: -f1)
valid_line=$(grep -n 'esp_ota_mark_app_valid_cancel_rollback' "$app" | cut -d: -f1)
[ "$portal_line" -lt "$health_line" ]
[ "$health_line" -lt "$valid_line" ]

echo "OTA/status product contract check: PASS"
