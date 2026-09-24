// SPDX-License-Identifier: GPL-3.0-or-later
#include "jj_portal.h"
#include "cJSON.h"
#include "dc_portal.h"
#include "dc_prusa.h"
#include "jj_identity.h"
#include "esp_app_desc.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_random.h"
#include "esp_timer.h"
#include <errno.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define JJ_CONTROL_BODY_MAX 2048

static bool authorize(httpd_req_t *req, void *ctx);

static const char *TAG = "jj_portal";
static jj_interlock_t *s_interlock;
static jj_authority_t *s_authority;
static char s_device_id[32];
static char s_boot_id[33];

extern const unsigned char _binary_control_html_start[];
extern const unsigned char _binary_control_html_end[];
extern const unsigned char _binary_control_client_js_start[];
extern const unsigned char _binary_control_client_js_end[];

static uint64_t monotonic_ms(void)
{
    return (uint64_t)esp_timer_get_time() / 1000U;
}

static void random_token(char output[33])
{
    static const char digits[] = "0123456789abcdef";
    uint8_t bytes[16];
    esp_fill_random(bytes, sizeof bytes);
    for (size_t i = 0; i < sizeof bytes; ++i) {
        output[i * 2] = digits[bytes[i] >> 4];
        output[i * 2 + 1] = digits[bytes[i] & 0x0f];
    }
    output[32] = '\0';
}

static jj_inputs_t authoritative_inputs(const dc_prusa_status_t *printer)
{
    jj_inputs_t input = jj_inputs_safe_defaults();
    if (printer) {
        input.printer.online = printer->online;
        input.printer.printing = printer->online &&
            strcmp(printer->printer_state, "PRINTING") == 0;
        input.printer.bed_target_c = printer->bed_target;
    }
    /* Hardware commissioning and automatic target mapping remain unavailable. */
    return input;
}

static esp_err_t send_json(httpd_req_t *req, cJSON *json)
{
    char *body = cJSON_PrintUnformatted(json);
    cJSON_Delete(json);
    if (!body) return ESP_ERR_NO_MEM;
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    esp_err_t err = httpd_resp_sendstr(req, body);
    free(body);
    return err;
}

static esp_err_t send_error(
    httpd_req_t *req,
    const char *status,
    jj_control_result_t result,
    cJSON *state)
{
    cJSON *root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "error", jj_control_result_str(result));
    if (state) cJSON_AddItemToObject(root, "state", state);
    httpd_resp_set_status(req, status);
    return send_json(req, root);
}

static esp_err_t send_unauthorized(httpd_req_t *req)
{
    cJSON *root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "error", "unauthorized");
    httpd_resp_set_status(req, "403 Forbidden");
    return send_json(req, root);
}

static cJSON *read_json_body(httpd_req_t *req)
{
    char content_type[48];
    if (httpd_req_get_hdr_value_str(req, "Content-Type", content_type,
                                    sizeof content_type) != ESP_OK ||
        strncmp(content_type, "application/json", 16) != 0 ||
        (content_type[16] != '\0' && content_type[16] != ';' &&
         content_type[16] != ' ' && content_type[16] != '\t'))
        return NULL;
    if (req->content_len <= 0 || req->content_len > JJ_CONTROL_BODY_MAX)
        return NULL;
    char *body = malloc((size_t)req->content_len + 1);
    if (!body) return NULL;
    size_t received = 0;
    while (received < (size_t)req->content_len) {
        const int count = httpd_req_recv(
            req, body + received, (size_t)req->content_len - received);
        if (count <= 0) {
            free(body);
            return NULL;
        }
        received += (size_t)count;
    }
    body[received] = '\0';
    cJSON *json = cJSON_ParseWithLength(body, received);
    free(body);
    return json;
}

static bool json_u32(const cJSON *root, const char *name, uint32_t *out)
{
    const cJSON *item = cJSON_GetObjectItemCaseSensitive(root, name);
    if (!cJSON_IsNumber(item) || !isfinite(item->valuedouble) ||
        item->valuedouble < 0 || item->valuedouble > UINT32_MAX ||
        item->valuedouble != (double)(uint32_t)item->valuedouble)
        return false;
    *out = (uint32_t)item->valuedouble;
    return true;
}

static bool json_text(
    const cJSON *root,
    const char *name,
    char *out,
    size_t out_size,
    bool required)
{
    const cJSON *item = cJSON_GetObjectItemCaseSensitive(root, name);
    if (!item && !required) {
        out[0] = '\0';
        return true;
    }
    if (!cJSON_IsString(item) || !item->valuestring ||
        strlen(item->valuestring) >= out_size ||
        (required && item->valuestring[0] == '\0'))
        return false;
    snprintf(out, out_size, "%s", item->valuestring);
    return true;
}

static bool valid_request_id(const char *value)
{
    if (!value || value[0] == '\0') return true;
    if (strlen(value) != 36) return false;
    for (size_t i = 0; i < 36; ++i) {
        if (i == 8 || i == 13 || i == 18 || i == 23) {
            if (value[i] != '-') return false;
        } else if (!((value[i] >= '0' && value[i] <= '9') ||
                     (value[i] >= 'a' && value[i] <= 'f') ||
                     (value[i] >= 'A' && value[i] <= 'F'))) {
            return false;
        }
    }
    return true;
}

static esp_err_t info_get(httpd_req_t *req)
{
    cJSON *root = cJSON_CreateObject();
    cJSON_AddNumberToObject(root, "api_version", 2);
    cJSON_AddStringToObject(root, "device_id", s_device_id);
    cJSON_AddStringToObject(root, "firmware", esp_app_get_description()->version);
    cJSON_AddStringToObject(root, "project", esp_app_get_description()->project_name);
    cJSON *ui = cJSON_AddObjectToObject(root, "ui");
    cJSON_AddNumberToObject(ui, "schema", 1);
    cJSON_AddStringToObject(ui, "product", JJ_IDENTITY_PRODUCT_ID);
    cJSON_AddStringToObject(ui, "display_name", JJ_IDENTITY_DISPLAY_NAME);
    cJSON_AddStringToObject(ui, "control_path", "/control");
    cJSON *capabilities = cJSON_AddArrayToObject(root, "capabilities");
    cJSON_AddItemToArray(capabilities, cJSON_CreateString("source_status"));
    cJSON_AddItemToArray(capabilities, cJSON_CreateString("polling"));
    cJSON_AddItemToArray(capabilities, cJSON_CreateString("control_authority_v1"));
    cJSON_AddItemToArray(capabilities, cJSON_CreateString("atomic_mutations"));
    cJSON_AddItemToArray(capabilities, cJSON_CreateString("explicit_takeover"));
    return send_json(req, root);
}

static cJSON *control_state_json(
    const jj_control_snapshot_t *authority,
    const jj_outputs_t *output,
    const dc_prusa_status_t *printer,
    bool include_lease_id)
{
    cJSON *root = cJSON_CreateObject();
    cJSON_AddNumberToObject(root, "api_version", 2);
    cJSON_AddStringToObject(root, "boot_id", s_boot_id);
    cJSON_AddNumberToObject(root, "state_revision", authority->revision);
    cJSON_AddStringToObject(root, "mode", jj_mode_str(authority->mode));
    const char *controller = authority->active_authority == JJ_AUTHORITY_REMOTE
        ? "remote" :
        authority->active_authority == JJ_AUTHORITY_AUTOMATIC ? "prusalink" :
        authority->active_authority == JJ_AUTHORITY_REACQUIRING
            ? "reacquiring" : "none";
    cJSON_AddStringToObject(root, "controller", controller);
    cJSON *control = cJSON_AddObjectToObject(root, "control");
    cJSON_AddStringToObject(control, "authority",
                           jj_control_authority_str(authority->active_authority));
    cJSON_AddStringToObject(control, "inhibit",
                           jj_control_inhibit_str(output->control_inhibit));
    cJSON_AddNumberToObject(control, "generation", authority->generation);
    cJSON_AddNumberToObject(control, "expires_in_ms",
                           authority->lease_expires_in_ms);
    cJSON_AddBoolToObject(control, "lease_active", authority->lease_active);
    cJSON_AddStringToObject(control, "lease_owner", authority->lease_owner);
    if (include_lease_id)
        cJSON_AddStringToObject(control, "lease_id", authority->lease_id);
    if (authority->last_control_loss_ms) {
        cJSON_AddNumberToObject(control, "last_loss_monotonic_ms",
                               (double)authority->last_control_loss_ms);
        cJSON_AddStringToObject(control, "last_loss_reason",
                               jj_control_loss_reason_str(
                                   authority->last_control_loss_reason));
    }
    cJSON_AddNumberToObject(root, "configured_target_c",
                           authority->configured_target_c);
    cJSON_AddNumberToObject(root, "target_c", authority->configured_target_c);
    if (output->effective_target_c > 0.0f)
        cJSON_AddNumberToObject(root, "effective_target_c",
                               output->effective_target_c);
    else
        cJSON_AddNullToObject(root, "effective_target_c");
    cJSON *heater = cJSON_AddObjectToObject(root, "heater");
    const bool raw_request = authority->mode == JJ_MODE_MANUAL &&
        authority->manual_demand_authorized;
    cJSON_AddBoolToObject(heater, "requested", raw_request);
    cJSON_AddStringToObject(heater, "control_loop",
                           jj_controller_state_str(output->controller_state));
    cJSON_AddNumberToObject(heater, "requested_duty_percent",
                           output->requested_duty_pct);
    cJSON_AddBoolToObject(heater, "allowed", output->heater_authorized);
    cJSON_AddNumberToObject(heater, "allowed_duty_percent",
                           output->allowed_duty_pct);
    cJSON_AddBoolToObject(heater, "delivered", false);
    cJSON_AddNumberToObject(heater, "delivered_percent", 0);
    cJSON_AddBoolToObject(heater, "available", false);
    cJSON *fan = cJSON_AddObjectToObject(root, "fan");
    cJSON_AddNumberToObject(fan, "requested_percent", output->fan_percent);
    cJSON_AddNumberToObject(fan, "delivered_percent", 0);
    cJSON_AddBoolToObject(fan, "available", false);
    cJSON_AddStringToObject(root, "thermal_state",
                           jj_thermal_state_str(output->thermal_state));
    cJSON_AddStringToObject(root, "dominant_constraint",
                           jj_block_reason_str(output->block_reason));
    cJSON *safety = cJSON_AddObjectToObject(root, "safety");
    cJSON_AddStringToObject(safety, "health",
                           output->fault == JJ_FAULT_NONE ? "degraded" : "fault");
    cJSON_AddBoolToObject(safety, "commissioned", false);
    cJSON_AddBoolToObject(safety, "fault_latched",
                          output->fault != JJ_FAULT_NONE);
    cJSON_AddStringToObject(safety, "fault", jj_fault_str(output->fault));
    cJSON_AddStringToObject(safety, "interlock",
                           jj_block_reason_str(output->block_reason));
    cJSON *prusa = cJSON_AddObjectToObject(root, "printer");
    cJSON_AddStringToObject(prusa, "source", "prusalink");
    cJSON_AddStringToObject(prusa, "connection", dc_prusa_state_str(printer->state));
    cJSON_AddStringToObject(prusa, "state", printer->printer_state);
    if (printer->online) {
        cJSON_AddNumberToObject(prusa, "bed_temperature_c", printer->bed_temp);
        cJSON_AddNumberToObject(prusa, "bed_target_c", printer->bed_target);
    } else {
        cJSON_AddNullToObject(prusa, "bed_temperature_c");
        cJSON_AddNullToObject(prusa, "bed_target_c");
    }
    return root;
}

static esp_err_t state_get(httpd_req_t *req)
{
    const uint64_t now_ms = monotonic_ms();
    (void)jj_authority_tick(s_authority, now_ms);
    jj_control_snapshot_t authority;
    jj_authority_snapshot(s_authority, now_ms, &authority);
    dc_prusa_status_t printer = {0};
    (void)dc_prusa_get_status(&printer);
    const jj_outputs_t output = jj_interlock_snapshot(s_interlock);
    return send_json(req, control_state_json(
        &authority, &output, &printer, false));
}

static esp_err_t health_get(httpd_req_t *req)
{
    cJSON *root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "status", "cold_safe");
    cJSON_AddBoolToObject(root, "heater_available", false);
    return send_json(req, root);
}

static esp_err_t control_page_get(httpd_req_t *req)
{
    httpd_resp_set_type(req, "text/html; charset=utf-8");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    return httpd_resp_send(req, (const char *)_binary_control_html_start,
                           (uintptr_t)_binary_control_html_end -
                           (uintptr_t)_binary_control_html_start);
}

static esp_err_t control_script_get(httpd_req_t *req)
{
    httpd_resp_set_type(req, "application/javascript; charset=utf-8");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    return httpd_resp_send(req,
                           (const char *)_binary_control_client_js_start,
                           (uintptr_t)_binary_control_client_js_end -
                           (uintptr_t)_binary_control_client_js_start);
}

static const char *result_status(jj_control_result_t result)
{
    if (result == JJ_CONTROL_INVALID) return "400 Bad Request";
    if (result == JJ_CONTROL_HARDWARE_FAULT) return "423 Locked";
    return "409 Conflict";
}

static esp_err_t send_control_result(
    httpd_req_t *req,
    jj_control_result_t result,
    const jj_control_snapshot_t *authority,
    bool include_lease_id)
{
    dc_prusa_status_t printer = {0};
    (void)dc_prusa_get_status(&printer);
    const jj_outputs_t output = jj_interlock_snapshot(s_interlock);
    cJSON *state = control_state_json(
        authority, &output, &printer, include_lease_id);
    if (result != JJ_CONTROL_OK)
        return send_error(req, result_status(result), result, state);
    return send_json(req, state);
}

static esp_err_t acquire_post(httpd_req_t *req)
{
    if (!authorize(req, NULL)) return send_unauthorized(req);
    cJSON *json = read_json_body(req);
    jj_control_acquire_t request = {0};
    const cJSON *takeover = json
        ? cJSON_GetObjectItemCaseSensitive(json, "takeover") : NULL;
    if (!json || !json_text(json, "owner", request.owner,
                            sizeof request.owner, true) ||
        (takeover && !cJSON_IsBool(takeover))) {
        cJSON_Delete(json);
        return send_error(req, "400 Bad Request", JJ_CONTROL_INVALID, NULL);
    }
    request.takeover = cJSON_IsTrue(takeover);
    random_token(request.lease_id);
    cJSON_Delete(json);
    jj_control_snapshot_t authority;
    const jj_control_result_t result = jj_authority_acquire(
        s_authority, &request, monotonic_ms(), &authority);
    return send_control_result(req, result, &authority,
                               result == JJ_CONTROL_OK);
}

static bool parse_lease_request(
    const cJSON *json,
    char lease_id[JJ_CONTROL_LEASE_ID_LEN + 1],
    uint32_t *generation)
{
    return json &&
        json_text(json, "lease_id", lease_id,
                  JJ_CONTROL_LEASE_ID_LEN + 1, true) &&
        strlen(lease_id) == JJ_CONTROL_LEASE_ID_LEN &&
        json_u32(json, "control_generation", generation);
}

static esp_err_t refresh_post(httpd_req_t *req)
{
    if (!authorize(req, NULL)) return send_unauthorized(req);
    cJSON *json = read_json_body(req);
    char lease_id[JJ_CONTROL_LEASE_ID_LEN + 1] = {0};
    uint32_t generation = 0;
    if (!parse_lease_request(json, lease_id, &generation)) {
        cJSON_Delete(json);
        return send_error(req, "400 Bad Request", JJ_CONTROL_INVALID, NULL);
    }
    cJSON_Delete(json);
    jj_control_snapshot_t authority;
    const jj_control_result_t result = jj_authority_refresh(
        s_authority, lease_id, generation, monotonic_ms(), &authority);
    return send_control_result(req, result, &authority,
                               result == JJ_CONTROL_OK);
}

static esp_err_t heartbeat_post(httpd_req_t *req)
{
    if (!authorize(req, NULL)) return send_unauthorized(req);
    cJSON *json = read_json_body(req);
    char lease_id[JJ_CONTROL_LEASE_ID_LEN + 1] = {0};
    uint32_t generation = 0;
    if (!parse_lease_request(json, lease_id, &generation)) {
        cJSON_Delete(json);
        return send_error(req, "400 Bad Request", JJ_CONTROL_INVALID, NULL);
    }
    cJSON_Delete(json);
    jj_control_snapshot_t authority;
    const jj_control_result_t result = jj_authority_heartbeat(
        s_authority, lease_id, generation, monotonic_ms(), &authority);
    return send_control_result(req, result, &authority,
                               result == JJ_CONTROL_OK);
}

static bool parse_mutation(
    const cJSON *json,
    jj_control_mutation_t *request)
{
    if (!parse_lease_request(json, request->lease_id,
                             &request->generation) ||
        !json_u32(json, "expected_state_revision",
                  &request->expected_revision) ||
        !json_text(json, "request_id", request->request_id,
                   sizeof request->request_id, false) ||
        !valid_request_id(request->request_id))
        return false;
    const cJSON *action = cJSON_GetObjectItemCaseSensitive(json, "action");
    if (!cJSON_IsString(action) || !action->valuestring) return false;
    if (strcmp(action->valuestring, "off") == 0) {
        request->kind = JJ_MUTATION_OFF;
    } else if (strcmp(action->valuestring, "manual") == 0) {
        const cJSON *target = cJSON_GetObjectItemCaseSensitive(json, "target_c");
        if (!cJSON_IsNumber(target) || !isfinite(target->valuedouble))
            return false;
        request->kind = JJ_MUTATION_MANUAL;
        request->target_c = (float)target->valuedouble;
    } else if (strcmp(action->valuestring, "automatic") == 0) {
        request->kind = JJ_MUTATION_AUTOMATIC;
    } else if (strcmp(action->valuestring, "clear_fault") == 0) {
        request->kind = JJ_MUTATION_CLEAR_FAULT;
    } else {
        return false;
    }
    return true;
}

static esp_err_t mutate_post(httpd_req_t *req)
{
    if (!authorize(req, NULL)) return send_unauthorized(req);
    cJSON *json = read_json_body(req);
    jj_control_mutation_t request = {0};
    if (!json || !parse_mutation(json, &request)) {
        cJSON_Delete(json);
        return send_error(req, "400 Bad Request", JJ_CONTROL_INVALID, NULL);
    }
    cJSON_Delete(json);
    dc_prusa_status_t printer = {0};
    (void)dc_prusa_get_status(&printer);
    const jj_inputs_t input = authoritative_inputs(&printer);
    jj_control_snapshot_t authority;
    const jj_control_result_t result = jj_authority_mutate(
        s_authority, s_interlock, &request, &input, monotonic_ms(),
        &authority);
    return send_control_result(req, result, &authority,
                               result == JJ_CONTROL_OK);
}

static esp_err_t register_routes(httpd_handle_t server, void *ctx)
{
    (void)ctx;
    const httpd_uri_t routes[] = {
        {.uri = "/control", .method = HTTP_GET, .handler = control_page_get},
        {.uri = "/jumpjet-control.js", .method = HTTP_GET,
         .handler = control_script_get},
        {.uri = "/api/v2/info", .method = HTTP_GET, .handler = info_get},
        {.uri = "/api/v2/state", .method = HTTP_GET, .handler = state_get},
        {.uri = "/api/v2/health", .method = HTTP_GET, .handler = health_get},
        {.uri = "/api/v2/control/acquire", .method = HTTP_POST,
         .handler = acquire_post},
        {.uri = "/api/v2/control/refresh", .method = HTTP_POST,
         .handler = refresh_post},
        {.uri = "/api/v2/control/heartbeat", .method = HTTP_POST,
         .handler = heartbeat_post},
        {.uri = "/api/v2/control/mutate", .method = HTTP_POST,
         .handler = mutate_post},
    };
    for (size_t i = 0; i < sizeof(routes) / sizeof(routes[0]); ++i) {
        esp_err_t err = httpd_register_uri_handler(server, &routes[i]);
        if (err != ESP_OK) return err;
    }
    return ESP_OK;
}

static cJSON *field(const char *key, const char *label, const char *type,
                    const char *value, bool secret)
{
    cJSON *item = cJSON_CreateObject();
    cJSON_AddStringToObject(item, "key", key);
    cJSON_AddStringToObject(item, "label", label);
    cJSON_AddStringToObject(item, "type", type);
    cJSON_AddStringToObject(item, "value", secret ? "" : value);
    if (secret) cJSON_AddBoolToObject(item, "secret", true);
    return item;
}

static cJSON *describe_product(void *ctx)
{
    (void)ctx;
    dc_prusa_config_t config = {0};
    (void)dc_prusa_get_config(&config);
    char port[8];
    snprintf(port, sizeof(port), "%u", (unsigned)config.port);
    cJSON *root = cJSON_CreateObject();
    cJSON *sections = cJSON_AddArrayToObject(root, "sections");
    cJSON *section = cJSON_CreateObject();
    cJSON_AddStringToObject(section, "title", "PrusaLink (read-only)");
    cJSON *fields = cJSON_AddArrayToObject(section, "fields");
    cJSON_AddItemToArray(fields, field("pr_host", "Printer host", "text", config.host, false));
    cJSON_AddItemToArray(fields, field("pr_port", "Port", "number", port, false));
    cJSON_AddItemToArray(fields, field("pr_key", "API key (blank keeps saved key)",
                                       "password", "", true));
    cJSON_AddItemToArray(sections, section);
    return root;
}

static bool valid_host(const char *host)
{
    if (!host || !host[0] || strlen(host) >= sizeof(((dc_prusa_config_t *)0)->host))
        return false;
    for (const unsigned char *p = (const unsigned char *)host; *p; ++p)
        if (*p <= 0x20 || *p == '/' || *p == '\\' || *p == ':' || *p == '#') return false;
    return true;
}

static esp_err_t apply_product(const cJSON *values, void *ctx,
                               char *message, size_t message_size)
{
    (void)ctx;
    dc_prusa_config_t config = {0};
    (void)dc_prusa_get_config(&config);
    const cJSON *host = cJSON_GetObjectItemCaseSensitive(values, "pr_host");
    const cJSON *port = cJSON_GetObjectItemCaseSensitive(values, "pr_port");
    const cJSON *key = cJSON_GetObjectItemCaseSensitive(values, "pr_key");
    if (!cJSON_IsString(host) || !valid_host(host->valuestring)) {
        snprintf(message, message_size, "Printer host is required and must not contain a URL path.");
        return ESP_ERR_INVALID_ARG;
    }
    long parsed_port = config.port ? config.port : DC_PRUSA_DEFAULT_PORT;
    if (cJSON_IsNumber(port)) {
        parsed_port = port->valueint;
        if (port->valuedouble != parsed_port) parsed_port = -1;
    } else if (cJSON_IsString(port)) {
        char *end = NULL;
        errno = 0;
        parsed_port = strtol(port->valuestring, &end, 10);
        if (errno || !end || *end) parsed_port = -1;
    } else {
        parsed_port = -1;
    }
    if (parsed_port < 1 || parsed_port > UINT16_MAX) {
        snprintf(message, message_size, "PrusaLink port must be 1-65535.");
        return ESP_ERR_INVALID_ARG;
    }
    if (key && !cJSON_IsString(key)) {
        snprintf(message, message_size, "PrusaLink API key must be text.");
        return ESP_ERR_INVALID_ARG;
    }
    if (key && strlen(key->valuestring) >= sizeof(config.api_key)) {
        snprintf(message, message_size, "PrusaLink API key is too long.");
        return ESP_ERR_INVALID_ARG;
    }
    snprintf(config.host, sizeof(config.host), "%s", host->valuestring);
    config.port = (uint16_t)parsed_port;
    if (key && key->valuestring[0])
        snprintf(config.api_key, sizeof(config.api_key), "%s", key->valuestring);
    esp_err_t err = dc_prusa_set_config(&config);
    if (err == ESP_OK)
        snprintf(message, message_size, "PrusaLink settings saved; restart to apply.");
    return err;
}

static bool authorize(httpd_req_t *req, void *ctx)
{
    (void)ctx;
    char value[65];
    return httpd_req_get_hdr_value_str(req, "X-Dragon-Auth", value, sizeof(value)) == ESP_OK &&
           value[0] != '\0';
}

static esp_err_t thermal_state_guard(char *message, size_t message_size)
{
    const jj_outputs_t output = jj_interlock_snapshot(s_interlock);
    if (!output.heater_authorized && !output.thermal_management_required)
        return ESP_OK;
    snprintf(message, message_size,
             "OTA is unavailable while heating or active thermal management is required.");
    return ESP_ERR_INVALID_STATE;
}

static esp_err_t guard_operation(dc_portal_operation_t operation, void *ctx,
                                 char *message, size_t message_size)
{
    (void)operation;
    (void)ctx;
    return thermal_state_guard(message, message_size);
}

static esp_err_t validate_image(const esp_app_desc_t *image, void *ctx,
                                char *message, size_t message_size)
{
    (void)ctx;
    // Independent post-upload recheck immediately before core selects the image.
    esp_err_t err = thermal_state_guard(message, message_size);
    if (err != ESP_OK) return err;
    if (strcmp(image->project_name, JJ_IDENTITY_PRODUCT_ID) == 0) return ESP_OK;
    snprintf(message, message_size, "Not a %s firmware image.", JJ_IDENTITY_DISPLAY_NAME);
    return ESP_ERR_INVALID_ARG;
}

static esp_err_t factory_reset(void *ctx)
{
    (void)ctx;
    return dc_prusa_clear_config();
}

esp_err_t jj_portal_start(jj_interlock_t *interlock, jj_authority_t *authority)
{
    if (!interlock || !authority) return ESP_ERR_INVALID_ARG;
    s_interlock = interlock;
    s_authority = authority;
    random_token(s_boot_id);
    uint8_t mac[6] = {0};
    esp_read_mac(mac, ESP_MAC_WIFI_STA);
    snprintf(s_device_id, sizeof(s_device_id), JJ_IDENTITY_DEVICE_ID_PREFIX "%02x%02x%02x",
             mac[3], mac[4], mac[5]);
    httpd_config_t http = HTTPD_DEFAULT_CONFIG();
    http.max_uri_handlers = 32;
    http.lru_purge_enable = true;
    const dc_portal_config_t config = {
        .product = JJ_IDENTITY_PRODUCT_ID,
        .display_name = JJ_IDENTITY_DISPLAY_NAME,
        .register_product_routes = register_routes,
        .describe_product = describe_product,
        .apply_product = apply_product,
        .authorize = authorize,
        .guard_operation = guard_operation,
        .validate_image = validate_image,
        .factory_reset = factory_reset,
        .httpd_config = &http,
    };
    esp_err_t err = dc_portal_start(&config);
    if (err == ESP_OK) ESP_LOGI(TAG, "Dragon-family portal started");
    return err;
}
