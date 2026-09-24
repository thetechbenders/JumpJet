// SPDX-License-Identifier: GPL-3.0-or-later
#include "jj_authority.h"

#include <math.h>
#include <string.h>

#ifdef ESP_PLATFORM
#include "freertos/FreeRTOS.h"
static portMUX_TYPE s_authority_lock = portMUX_INITIALIZER_UNLOCKED;
#define AUTHORITY_LOCK() portENTER_CRITICAL(&s_authority_lock)
#define AUTHORITY_UNLOCK() portEXIT_CRITICAL(&s_authority_lock)
#else
#define AUTHORITY_LOCK() ((void)0)
#define AUTHORITY_UNLOCK() ((void)0)
#endif

static void copy_text(char *destination, size_t size, const char *source)
{
    if (!destination || size == 0) return;
    size_t i = 0;
    if (source) {
        while (i + 1 < size && source[i] != '\0') {
            destination[i] = source[i];
            ++i;
        }
    }
    destination[i] = '\0';
}

static void advance(uint32_t *value)
{
    (*value)++;
    if (*value == 0) *value = 1;
}

static void snapshot_unlocked(
    const jj_authority_t *state,
    uint64_t now_ms,
    jj_control_snapshot_t *out)
{
    if (!out) return;
    *out = state->state;
    if (out->lease_active && state->lease_deadline_ms > now_ms) {
        const uint64_t remaining = state->lease_deadline_ms - now_ms;
        out->lease_expires_in_ms = remaining > UINT32_MAX
            ? UINT32_MAX : (uint32_t)remaining;
    } else {
        out->lease_expires_in_ms = 0;
    }
}

/*
 * Lock ordering is AUTHORITY_LOCK -> jj_interlock's state lock. Interlock code
 * never calls back into jj_authority. Everything below this lock is bounded
 * in-memory work: no logging, allocation, networking, or blocking services.
 */
static void remove_remote_authorization_unlocked(
    jj_authority_t *state,
    jj_control_authority_t authority,
    jj_control_inhibit_t inhibit,
    jj_block_reason_t reason)
{
    jj_interlock_remove_remote_authorization(
        state->interlock, authority, inhibit, reason);
}

static bool lease_expire_unlocked(jj_authority_t *state, uint64_t now_ms)
{
    if (!state->state.lease_active || now_ms < state->lease_deadline_ms)
        return false;
    state->state.lease_active = false;
    state->state.lease_id[0] = '\0';
    state->state.lease_owner[0] = '\0';
    state->lease_deadline_ms = 0;
    advance(&state->state.generation);
    advance(&state->state.revision);
    state->state.last_control_loss_ms = now_ms;
    state->state.last_control_loss_reason =
        JJ_CONTROL_LOSS_REMOTE_LEASE_EXPIRED;
    if (state->state.mode == JJ_MODE_MANUAL) {
        state->state.manual_demand_authorized = false;
        state->state.active_authority = JJ_AUTHORITY_NONE;
        state->state.control_inhibit = JJ_CONTROL_INHIBIT_NO_AUTHORITY;
        remove_remote_authorization_unlocked(
            state, JJ_AUTHORITY_NONE, JJ_CONTROL_INHIBIT_NO_AUTHORITY,
            JJ_BLOCK_CONTROL_NO_AUTHORITY);
    }
    return true;
}

void jj_authority_init(
    jj_authority_t *state,
    jj_interlock_t *interlock,
    uint32_t lease_ttl_ms)
{
    if (!state || !interlock) return;
    AUTHORITY_LOCK();
    memset(state, 0, sizeof *state);
    state->lease_ttl_ms = lease_ttl_ms ? lease_ttl_ms : JJ_REMOTE_LEASE_TTL_MS;
    state->interlock = interlock;
    state->state.revision = 1;
    state->state.generation = 1;
    state->state.mode = JJ_MODE_OFF;
    state->state.configured_target_c = JJ_MANUAL_TARGET_DEFAULT_C;
    state->state.active_authority = JJ_AUTHORITY_NONE;
    state->state.control_inhibit = JJ_CONTROL_INHIBIT_NONE;
    AUTHORITY_UNLOCK();
}

jj_control_result_t jj_authority_acquire(
    jj_authority_t *state,
    const jj_control_acquire_t *request,
    uint64_t now_ms,
    jj_control_snapshot_t *out)
{
    if (!state || !request || strlen(request->lease_id) != JJ_CONTROL_LEASE_ID_LEN ||
        request->owner[0] == '\0')
        return JJ_CONTROL_INVALID;
    AUTHORITY_LOCK();
    (void)lease_expire_unlocked(state, now_ms);
    if (state->state.lease_active &&
        strcmp(state->state.lease_owner, request->owner) != 0 &&
        !request->takeover) {
        snapshot_unlocked(state, now_ms, out);
        AUTHORITY_UNLOCK();
        return JJ_CONTROL_AUTHORITY_REQUIRED;
    }
    const bool revoked_remote_demand =
        state->state.mode == JJ_MODE_MANUAL &&
        state->state.manual_demand_authorized;
    state->state.manual_demand_authorized = false;
    state->state.lease_active = true;
    copy_text(state->state.lease_id, sizeof state->state.lease_id,
              request->lease_id);
    copy_text(state->state.lease_owner, sizeof state->state.lease_owner,
              request->owner);
    state->lease_deadline_ms = now_ms + state->lease_ttl_ms;
    advance(&state->state.generation);
    advance(&state->state.revision);
    if (state->state.mode != JJ_MODE_AUTOMATIC) {
        state->state.active_authority = JJ_AUTHORITY_REACQUIRING;
        state->state.control_inhibit =
            JJ_CONTROL_INHIBIT_STATE_REFRESH_REQUIRED;
        remove_remote_authorization_unlocked(
            state, JJ_AUTHORITY_REACQUIRING,
            JJ_CONTROL_INHIBIT_STATE_REFRESH_REQUIRED,
            JJ_BLOCK_STATE_REFRESH_REQUIRED);
    }
    if (revoked_remote_demand || request->takeover) {
        state->state.last_control_loss_ms = now_ms;
        state->state.last_control_loss_reason = request->takeover
            ? JJ_CONTROL_LOSS_EXPLICIT_TAKEOVER
            : JJ_CONTROL_LOSS_REMOTE_REACQUIRED;
    }
    snapshot_unlocked(state, now_ms, out);
    AUTHORITY_UNLOCK();
    return JJ_CONTROL_OK;
}

jj_control_result_t jj_authority_heartbeat(
    jj_authority_t *state,
    const char *lease_id,
    uint32_t generation,
    uint64_t now_ms,
    jj_control_snapshot_t *out)
{
    if (!state || !lease_id) return JJ_CONTROL_INVALID;
    AUTHORITY_LOCK();
    const bool expired = lease_expire_unlocked(state, now_ms);
    if (generation != state->state.generation) {
        snapshot_unlocked(state, now_ms, out);
        AUTHORITY_UNLOCK();
        return expired ? JJ_CONTROL_AUTHORITY_LOST : JJ_CONTROL_GENERATION_STALE;
    }
    if (!state->state.lease_active ||
        strcmp(lease_id, state->state.lease_id) != 0) {
        snapshot_unlocked(state, now_ms, out);
        AUTHORITY_UNLOCK();
        return JJ_CONTROL_AUTHORITY_LOST;
    }
    state->lease_deadline_ms = now_ms + state->lease_ttl_ms;
    snapshot_unlocked(state, now_ms, out);
    AUTHORITY_UNLOCK();
    return JJ_CONTROL_OK;
}

jj_control_result_t jj_authority_refresh(
    jj_authority_t *state,
    const char *lease_id,
    uint32_t generation,
    uint64_t now_ms,
    jj_control_snapshot_t *out)
{
    if (!state || !lease_id) return JJ_CONTROL_INVALID;
    AUTHORITY_LOCK();
    const bool expired = lease_expire_unlocked(state, now_ms);
    if (generation != state->state.generation) {
        snapshot_unlocked(state, now_ms, out);
        AUTHORITY_UNLOCK();
        return expired ? JJ_CONTROL_AUTHORITY_LOST : JJ_CONTROL_GENERATION_STALE;
    }
    if (!state->state.lease_active ||
        strcmp(lease_id, state->state.lease_id) != 0) {
        snapshot_unlocked(state, now_ms, out);
        AUTHORITY_UNLOCK();
        return JJ_CONTROL_AUTHORITY_LOST;
    }
    state->lease_deadline_ms = now_ms + state->lease_ttl_ms;
    if (state->state.active_authority == JJ_AUTHORITY_REACQUIRING) {
        state->state.active_authority = JJ_AUTHORITY_NONE;
        state->state.control_inhibit = state->state.mode == JJ_MODE_MANUAL
            ? JJ_CONTROL_INHIBIT_NO_AUTHORITY : JJ_CONTROL_INHIBIT_NONE;
        remove_remote_authorization_unlocked(
            state, JJ_AUTHORITY_NONE, state->state.control_inhibit,
            state->state.mode == JJ_MODE_MANUAL
                ? JJ_BLOCK_CONTROL_NO_AUTHORITY : JJ_BLOCK_OFF);
        advance(&state->state.revision);
    }
    snapshot_unlocked(state, now_ms, out);
    AUTHORITY_UNLOCK();
    return JJ_CONTROL_OK;
}

static bool sensors_valid(const jj_inputs_t *input)
{
    return input->chamber.status == JJ_SENSOR_OK &&
        isfinite(input->chamber.temperature_c) &&
        input->outlet.status == JJ_SENSOR_OK &&
        isfinite(input->outlet.temperature_c) &&
        input->case_sensor.status == JJ_SENSOR_OK &&
        isfinite(input->case_sensor.temperature_c);
}

static bool base_heat_eligible(
    jj_fault_t fault,
    const jj_inputs_t *input)
{
    return input->commissioned && sensors_valid(input) &&
        !input->overtemperature_detected &&
        fault == JJ_FAULT_NONE;
}

static uint64_t request_signature(const jj_control_mutation_t *request)
{
    uint64_t hash = UINT64_C(1469598103934665603);
    const unsigned char *text = (const unsigned char *)request->lease_id;
    while (*text) {
        hash ^= *text++;
        hash *= UINT64_C(1099511628211);
    }
    const uint32_t values[] = {
        request->generation,
        request->expected_revision,
        (uint32_t)request->kind,
    };
    for (size_t i = 0; i < sizeof values / sizeof values[0]; ++i) {
        for (unsigned shift = 0; shift < 32; shift += 8) {
            hash ^= (values[i] >> shift) & UINT32_C(0xff);
            hash *= UINT64_C(1099511628211);
        }
    }
    uint32_t target_bits = 0;
    memcpy(&target_bits, &request->target_c, sizeof target_bits);
    for (unsigned shift = 0; shift < 32; shift += 8) {
        hash ^= (target_bits >> shift) & UINT32_C(0xff);
        hash *= UINT64_C(1099511628211);
    }
    return hash;
}

static void cache_result(
    jj_authority_t *state,
    const jj_control_mutation_t *request,
    uint64_t signature,
    jj_control_result_t result,
    uint64_t now_ms,
    jj_control_snapshot_t *out)
{
    if (request->request_id[0]) {
        jj_control_request_cache_entry_t *entry =
            &state->request_cache[state->next_request_cache_entry];
        state->next_request_cache_entry =
            (uint8_t)((state->next_request_cache_entry + 1U) %
                      JJ_CONTROL_REQUEST_CACHE_SIZE);
        copy_text(entry->request_id, sizeof entry->request_id,
                  request->request_id);
        entry->signature = signature;
        entry->result = result;
    }
    snapshot_unlocked(state, now_ms, out);
}

jj_control_result_t jj_authority_mutate(
    jj_authority_t *state,
    jj_interlock_t *interlock,
    const jj_control_mutation_t *request,
    const jj_inputs_t *authoritative_inputs,
    uint64_t now_ms,
    jj_control_snapshot_t *out)
{
    if (!state || !interlock || interlock != state->interlock || !request ||
        !authoritative_inputs)
        return JJ_CONTROL_INVALID;
    const uint64_t signature = request_signature(request);
    AUTHORITY_LOCK();
    const bool expired = lease_expire_unlocked(state, now_ms);
    if (request->request_id[0]) {
        for (size_t i = 0; i < JJ_CONTROL_REQUEST_CACHE_SIZE; ++i) {
            const jj_control_request_cache_entry_t *entry =
                &state->request_cache[i];
            if (strcmp(request->request_id, entry->request_id) != 0) continue;
            if (signature != entry->signature) {
                snapshot_unlocked(state, now_ms, out);
                AUTHORITY_UNLOCK();
                return JJ_CONTROL_REQUEST_CONFLICT;
            }
            snapshot_unlocked(state, now_ms, out);
            const jj_control_result_t cached = entry->result;
            AUTHORITY_UNLOCK();
            return cached;
        }
    }
    if (request->generation != state->state.generation) {
        const jj_control_result_t result = expired
            ? JJ_CONTROL_AUTHORITY_LOST : JJ_CONTROL_GENERATION_STALE;
        cache_result(state, request, signature, result, now_ms, out);
        AUTHORITY_UNLOCK();
        return result;
    }
    if (!state->state.lease_active ||
        strcmp(request->lease_id, state->state.lease_id) != 0) {
        cache_result(state, request, signature, JJ_CONTROL_AUTHORITY_LOST,
                     now_ms, out);
        AUTHORITY_UNLOCK();
        return JJ_CONTROL_AUTHORITY_LOST;
    }
    if (request->expected_revision != state->state.revision) {
        cache_result(state, request, signature, JJ_CONTROL_REVISION_CONFLICT,
                     now_ms, out);
        AUTHORITY_UNLOCK();
        return JJ_CONTROL_REVISION_CONFLICT;
    }
    if (state->state.active_authority == JJ_AUTHORITY_REACQUIRING) {
        cache_result(state, request, signature,
                     JJ_CONTROL_AUTHORITY_REQUIRED, now_ms, out);
        AUTHORITY_UNLOCK();
        return JJ_CONTROL_AUTHORITY_REQUIRED;
    }

    jj_control_result_t result = JJ_CONTROL_OK;
    if (request->kind == JJ_MUTATION_OFF) {
        state->state.mode = JJ_MODE_OFF;
        state->state.manual_demand_authorized = false;
        state->state.active_authority = JJ_AUTHORITY_NONE;
        state->state.control_inhibit = JJ_CONTROL_INHIBIT_NONE;
        remove_remote_authorization_unlocked(
            state, JJ_AUTHORITY_NONE, JJ_CONTROL_INHIBIT_NONE, JJ_BLOCK_OFF);
    } else if (request->kind == JJ_MUTATION_MANUAL) {
        const jj_outputs_t output = jj_interlock_snapshot(interlock);
        if (output.fault != JJ_FAULT_NONE) {
            result = JJ_CONTROL_HARDWARE_FAULT;
        } else if (!base_heat_eligible(output.fault, authoritative_inputs) ||
                   authoritative_inputs->fan_proof != JJ_FAN_PROOF_PROVEN ||
                   !isfinite(request->target_c) ||
                   request->target_c < JJ_MANUAL_TARGET_MIN_C ||
                   request->target_c > JJ_MANUAL_TARGET_MAX_C) {
            result = JJ_CONTROL_REQUEST_INELIGIBLE;
        } else {
            state->state.mode = JJ_MODE_MANUAL;
            state->state.configured_target_c = request->target_c;
            state->state.manual_demand_authorized = true;
            state->state.active_authority = JJ_AUTHORITY_REMOTE;
            state->state.control_inhibit = JJ_CONTROL_INHIBIT_NONE;
        }
    } else if (request->kind == JJ_MUTATION_AUTOMATIC) {
        const jj_outputs_t output = jj_interlock_snapshot(interlock);
        if (output.fault != JJ_FAULT_NONE) {
            result = JJ_CONTROL_HARDWARE_FAULT;
        } else if (!base_heat_eligible(output.fault, authoritative_inputs) ||
                   authoritative_inputs->fan_proof != JJ_FAN_PROOF_PROVEN ||
                   !authoritative_inputs->printer.online ||
                   !authoritative_inputs->printer.printing ||
                   !authoritative_inputs->automatic_target_available ||
                   !isfinite(authoritative_inputs->automatic_target_c) ||
                   authoritative_inputs->automatic_target_c <= 0.0f) {
            result = JJ_CONTROL_REQUEST_INELIGIBLE;
        } else {
            state->state.mode = JJ_MODE_AUTOMATIC;
            state->state.manual_demand_authorized = false;
            state->state.active_authority = JJ_AUTHORITY_AUTOMATIC;
            state->state.control_inhibit = JJ_CONTROL_INHIBIT_NONE;
        }
    } else if (request->kind == JJ_MUTATION_CLEAR_FAULT) {
        if (state->state.mode != JJ_MODE_OFF) {
            result = JJ_CONTROL_REQUEST_INELIGIBLE;
        } else {
            jj_inputs_t clear_inputs = *authoritative_inputs;
            clear_inputs.mode = JJ_MODE_OFF;
            if (!jj_interlock_clear_fault(interlock, &clear_inputs))
                result = JJ_CONTROL_HARDWARE_FAULT;
        }
    } else {
        result = JJ_CONTROL_INVALID;
    }

    if (result == JJ_CONTROL_OK) advance(&state->state.revision);
    cache_result(state, request, signature, result, now_ms, out);
    AUTHORITY_UNLOCK();
    return result;
}

bool jj_authority_tick(jj_authority_t *state, uint64_t now_ms)
{
    if (!state) return false;
    AUTHORITY_LOCK();
    const bool changed = lease_expire_unlocked(state, now_ms);
    AUTHORITY_UNLOCK();
    return changed;
}

void jj_authority_snapshot(
    const jj_authority_t *state,
    uint64_t now_ms,
    jj_control_snapshot_t *out)
{
    if (!state || !out) return;
    AUTHORITY_LOCK();
    snapshot_unlocked(state, now_ms, out);
    AUTHORITY_UNLOCK();
}

void jj_authority_apply_to_inputs(
    const jj_control_snapshot_t *authority,
    jj_inputs_t *inputs)
{
    if (!authority || !inputs) return;
    inputs->mode = authority->mode;
    inputs->manual_target_c = authority->configured_target_c;
    inputs->active_authority = authority->active_authority;
    inputs->control_inhibit = authority->control_inhibit;
    inputs->manual_demand_authorized = authority->manual_demand_authorized;
}

jj_outputs_t jj_authority_control_step(
    jj_authority_t *state,
    jj_controller_t *controller,
    const jj_inputs_t *authoritative_inputs,
    uint64_t now_ms,
    jj_control_snapshot_t *out)
{
    if (!state || !state->interlock || !authoritative_inputs)
        return (jj_outputs_t){.fault = JJ_FAULT_SENSOR,
                              .block_reason = JJ_BLOCK_FAULT_LATCHED};
    jj_inputs_t input = *authoritative_inputs;
    AUTHORITY_LOCK();
    (void)lease_expire_unlocked(state, now_ms);
    jj_authority_apply_to_inputs(&state->state, &input);
    /*
     * Both interlock stages and the controller step run under this one lock,
     * so no authority mutation or revocation can interleave between them.
     */
    const jj_eligibility_t eligibility =
        jj_interlock_evaluate(state->interlock, &input);
    const jj_controller_result_t controller_result =
        jj_controller_step(controller, &eligibility, &input);
    const jj_outputs_t output = jj_interlock_authorize(
        state->interlock, controller ? &controller_result.report : NULL);
    snapshot_unlocked(state, now_ms, out);
    AUTHORITY_UNLOCK();
    return output;
}

const char *jj_control_result_str(jj_control_result_t result)
{
    switch (result) {
    case JJ_CONTROL_OK: return "ok";
    case JJ_CONTROL_AUTHORITY_REQUIRED: return "control_authority_required";
    case JJ_CONTROL_AUTHORITY_LOST: return "control_authority_lost";
    case JJ_CONTROL_GENERATION_STALE: return "control_generation_stale";
    case JJ_CONTROL_REVISION_CONFLICT: return "state_revision_conflict";
    case JJ_CONTROL_REQUEST_INELIGIBLE: return "control_request_ineligible";
    case JJ_CONTROL_REQUEST_CONFLICT: return "control_request_conflict";
    case JJ_CONTROL_HARDWARE_FAULT: return "hardware_fault_latched";
    case JJ_CONTROL_INVALID: return "invalid_request";
    default: return "unknown";
    }
}

const char *jj_mode_str(jj_mode_t mode)
{
    switch (mode) {
    case JJ_MODE_OFF: return "off";
    case JJ_MODE_MANUAL: return "manual";
    case JJ_MODE_AUTOMATIC: return "automatic";
    default: return "unknown";
    }
}

const char *jj_control_loss_reason_str(jj_control_loss_reason_t reason)
{
    switch (reason) {
    case JJ_CONTROL_LOSS_NONE: return "none";
    case JJ_CONTROL_LOSS_REMOTE_LEASE_EXPIRED:
        return "remote_lease_expired";
    case JJ_CONTROL_LOSS_EXPLICIT_TAKEOVER: return "explicit_takeover";
    case JJ_CONTROL_LOSS_REMOTE_REACQUIRED: return "remote_reacquired";
    default: return "unknown";
    }
}
