// SPDX-License-Identifier: GPL-3.0-or-later
// Jump Jet — Dragon-family firmware for the Prusa CORE One chamber heater.
// Foundation milestone: no actuator driver exists, so this image cannot energize heat.

#include "dc_evlog.h"
#include "dc_prusa.h"
#include "dc_wifi.h"
#include "jj_authority.h"
#include "jj_control.h"
#include "jj_identity.h"
#include "jj_interlock.h"
#include "jj_portal.h"
#include "esp_check.h"
#include "esp_log.h"
#include "esp_ota_ops.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nvs_flash.h"
#include <string.h>

static const char *TAG = JJ_IDENTITY_PRODUCT_ID;
static jj_interlock_t s_interlock;
static jj_authority_t s_authority;
static jj_controller_t s_controller;

static bool printer_is_printing(const char *state)
{
    return state && strcmp(state, "PRINTING") == 0;
}

static void control_task(void *arg)
{
    TaskHandle_t startup_task = (TaskHandle_t)arg;
    bool startup_reported = false;
    jj_block_reason_t previous = JJ_BLOCK_NONE;
    for (;;) {
        jj_inputs_t input = jj_inputs_safe_defaults();
        dc_prusa_status_t printer = {0};
        if (dc_prusa_get_status(&printer) == ESP_OK) {
            input.printer.online = printer.online;
            input.printer.printing = printer_is_printing(printer.printer_state);
            input.printer.bed_target_c = printer.bed_target;
            // dc_prusa has already applied its authoritative 15-second
            // freshness rule. Jump Jet deliberately has no second timer.
        }
        /*
         * Cached Prusa status is read before authority. The final authority
         * sample and interlock decision are one short, ordered control step.
         */
        const uint64_t now_ms = (uint64_t)esp_timer_get_time() / 1000U;
        const jj_outputs_t output = jj_authority_control_step(
            &s_authority, &s_controller, &input, now_ms, NULL);
        if (!startup_reported) {
            startup_reported = true;
            xTaskNotifyGive(startup_task);
        }
        if (output.block_reason != previous) {
            dc_evlog_add("interlock: %s", jj_block_reason_str(output.block_reason));
            previous = output.block_reason;
        }
        // Intentionally no GPIO/PWM write in the foundation image.
        vTaskDelay(pdMS_TO_TICKS(250));
    }
}

void app_main(void)
{
    dc_evlog_console_init();
    dc_evlog_init();
    ESP_LOGI(TAG, "%s cold-safe foundation starting", JJ_IDENTITY_DISPLAY_NAME);
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    ESP_ERROR_CHECK(err);
    jj_interlock_init(&s_interlock);
    jj_authority_init(&s_authority, &s_interlock, JJ_REMOTE_LEASE_TTL_MS);
    const jj_controller_config_t controller_config =
        jj_controller_provisional_config();
    jj_controller_init(&s_controller, &controller_config);
    BaseType_t created = xTaskCreate(control_task, "jj_control", 4096,
                                     xTaskGetCurrentTaskHandle(), 8, NULL);
    ESP_ERROR_CHECK(created == pdPASS ? ESP_OK : ESP_ERR_NO_MEM);
    const dc_wifi_identity_t identity = {
        .hostname = JJ_IDENTITY_HOSTNAME,
        .instance_name = JJ_IDENTITY_DISPLAY_NAME,
        .ap_ssid_prefix = JJ_IDENTITY_AP_SSID_PREFIX,
        .ap_password = DC_WIFI_DEFAULT_AP_PASSWORD,
    };
    ESP_ERROR_CHECK(dc_wifi_set_identity(&identity));
    ESP_ERROR_CHECK(dc_wifi_start());
    ESP_ERROR_CHECK(dc_prusa_start());
    ESP_ERROR_CHECK(jj_portal_start(&s_interlock, &s_authority));
    ESP_ERROR_CHECK(ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(2000)) > 0 ?
                    ESP_OK : ESP_ERR_TIMEOUT);
    const jj_outputs_t startup_output = jj_interlock_snapshot(&s_interlock);
    ESP_ERROR_CHECK(!startup_output.heater_authorized &&
                    startup_output.allowed_duty_pct == 0.0f &&
                    startup_output.effective_target_c == 0.0f ?
                    ESP_OK : ESP_ERR_INVALID_STATE);
    const esp_partition_t *running = esp_ota_get_running_partition();
    esp_ota_img_states_t state;
    if (running && esp_ota_get_state_partition(running, &state) == ESP_OK &&
        state == ESP_OTA_IMG_PENDING_VERIFY) {
        ESP_ERROR_CHECK(esp_ota_mark_app_valid_cancel_rollback());
    }
    dc_evlog_add("boot complete: actuator unavailable; heater forced off");
    ESP_LOGW(TAG, "heater actuator is intentionally not implemented");
}
