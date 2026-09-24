// SPDX-License-Identifier: GPL-3.0-or-later
#include "jj_interlock.h"
#include <math.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#ifdef ESP_PLATFORM
#include "freertos/FreeRTOS.h"
static portMUX_TYPE s_state_lock = portMUX_INITIALIZER_UNLOCKED;
#define STATE_LOCK() portENTER_CRITICAL(&s_state_lock)
#define STATE_UNLOCK() portEXIT_CRITICAL(&s_state_lock)
#else
#define STATE_LOCK() ((void)0)
#define STATE_UNLOCK() ((void)0)
#endif

static bool sensor_ok(const jj_sensor_sample_t *sensor)
{
    return sensor->status == JJ_SENSOR_OK && isfinite(sensor->temperature_c);
}

static bool sensors_ok(const jj_inputs_t *input)
{
    return sensor_ok(&input->chamber) && sensor_ok(&input->outlet) &&
           sensor_ok(&input->case_sensor);
}

static bool manual_target_valid(float target_c)
{
    return isfinite(target_c) && target_c >= JJ_MANUAL_TARGET_MIN_C &&
           target_c <= JJ_MANUAL_TARGET_MAX_C;
}

jj_inputs_t jj_inputs_safe_defaults(void)
{
    return (jj_inputs_t){
        .commissioned = false,
        .mode = JJ_MODE_OFF,
        .manual_target_c = JJ_MANUAL_TARGET_DEFAULT_C,
        .active_authority = JJ_AUTHORITY_NONE,
        .control_inhibit = JJ_CONTROL_INHIBIT_NONE,
        .chamber = {.status = JJ_SENSOR_UNAVAILABLE},
        .outlet = {.status = JJ_SENSOR_UNAVAILABLE},
        .case_sensor = {.status = JJ_SENSOR_UNAVAILABLE},
        .fan_proof = JJ_FAN_PROOF_UNAVAILABLE,
    };
}

void jj_interlock_init(jj_interlock_t *state)
{
    if (!state) return;
    STATE_LOCK();
    memset(state, 0, sizeof(*state));
    state->last_output.block_reason = JJ_BLOCK_OFF;
    STATE_UNLOCK();
}

/*
 * Controller disposition for every block reason. Exhaustive with no default:
 * adding a jj_block_reason_t without deciding its disposition fails the
 * -Werror build (-Wswitch). Only an explicitly approved transient inhibit may
 * HOLD; everything else SUSPENDs (idle + reset).
 */
jj_controller_disposition_t jj_block_reason_disposition(jj_block_reason_t reason)
{
    switch (reason) {
    case JJ_BLOCK_NONE:
        return JJ_DISPOSITION_RUN;
    case JJ_BLOCK_FAN_PROOF_PENDING:
        /* Approved Category A: transient authorization loss. */
        return JJ_DISPOSITION_HOLD;
    case JJ_BLOCK_OFF:
    case JJ_BLOCK_NOT_COMMISSIONED:       /* approved: suspend + reset */
    case JJ_BLOCK_FAULT_LATCHED:          /* suspended for the whole latch */
    case JJ_BLOCK_INVALID_MODE:
    case JJ_BLOCK_MANUAL_TARGET_INVALID:  /* approved: no valid objective */
    case JJ_BLOCK_PRINTER_UNAVAILABLE:    /* AUTOMATIC: no Phase-4 setpoint */
    case JJ_BLOCK_PRINTER_NOT_PRINTING:
    case JJ_BLOCK_AUTO_POLICY_UNAVAILABLE:
    case JJ_BLOCK_AUTO_AUTHORITY_UNAVAILABLE:
    case JJ_BLOCK_CONTROL_NO_AUTHORITY:
    case JJ_BLOCK_STATE_REFRESH_REQUIRED:
    case JJ_BLOCK_CONTROLLER_INVALID:
    case JJ_BLOCK_CONTROL_SEQUENCE_INVALID:
    case JJ_BLOCK_REASON_COUNT:
        return JJ_DISPOSITION_SUSPEND;
    }
    return JJ_DISPOSITION_SUSPEND; /* out-of-range value */
}

typedef struct {
    jj_block_reason_t dominant;
    uint32_t mask;
    jj_controller_disposition_t disposition;
} inhibit_accumulator_t;

/*
 * Record one applicable inhibit. The first recorded reason is the dominant
 * diagnostic reason (unchanged PR #7 priority). Disposition is the minimum over
 * every recorded reason, so it does not depend on check order. Returns true
 * once SUSPEND is reached: nothing later can change the disposition.
 */
static bool record(inhibit_accumulator_t *acc, jj_block_reason_t reason)
{
    if (acc->dominant == JJ_BLOCK_NONE) acc->dominant = reason;
    acc->mask |= UINT32_C(1) << (uint32_t)reason;
    const jj_controller_disposition_t disposition =
        jj_block_reason_disposition(reason);
    if (disposition < acc->disposition) acc->disposition = disposition;
    return acc->disposition == JJ_DISPOSITION_SUSPEND;
}

/*
 * The single implementation of heat-eligibility predicates. Check order and
 * fault-latching side effects are unchanged from PR #7; only the early
 * returns now continue past non-SUSPEND inhibits.
 */
static void collect_inhibits(jj_interlock_t *state, const jj_inputs_t *input,
                             inhibit_accumulator_t *acc)
{
    if (!input->commissioned && record(acc, JJ_BLOCK_NOT_COMMISSIONED)) return;
    if (!sensors_ok(input)) state->fault_latched = JJ_FAULT_SENSOR;
    if (input->overtemperature_detected)
        state->fault_latched = JJ_FAULT_OVERTEMPERATURE;
    if (state->fault_latched != JJ_FAULT_NONE &&
        record(acc, JJ_BLOCK_FAULT_LATCHED)) return;
    if (input->mode == JJ_MODE_OFF && record(acc, JJ_BLOCK_OFF)) return;
    if (input->mode != JJ_MODE_MANUAL && input->mode != JJ_MODE_AUTOMATIC &&
        record(acc, JJ_BLOCK_INVALID_MODE)) return;
    if (input->fan_proof == JJ_FAN_PROOF_FAILED ||
        (input->fan_proof != JJ_FAN_PROOF_UNAVAILABLE &&
         input->fan_proof != JJ_FAN_PROOF_PENDING &&
         input->fan_proof != JJ_FAN_PROOF_PROVEN)) {
        state->fault_latched = JJ_FAULT_FAN;
        if (record(acc, JJ_BLOCK_FAULT_LATCHED)) return;
    }
    if (input->fan_proof != JJ_FAN_PROOF_PROVEN &&
        record(acc, JJ_BLOCK_FAN_PROOF_PENDING)) return;

    if (input->mode == JJ_MODE_AUTOMATIC) {
        if (input->active_authority != JJ_AUTHORITY_AUTOMATIC &&
            record(acc, JJ_BLOCK_AUTO_AUTHORITY_UNAVAILABLE)) return;
        if (!input->printer.online &&
            record(acc, JJ_BLOCK_PRINTER_UNAVAILABLE)) return;
        if (!input->printer.printing &&
            record(acc, JJ_BLOCK_PRINTER_NOT_PRINTING)) return;
        /* dc_prusa owns its 15 s freshness decision. No second timer lives here.
         * Exact bed-target mapping is intentionally undefined, so AUTO is cold. */
        (void)record(acc, JJ_BLOCK_AUTO_POLICY_UNAVAILABLE);
        return;
    }
    if ((input->active_authority == JJ_AUTHORITY_REACQUIRING ||
         input->control_inhibit == JJ_CONTROL_INHIBIT_STATE_REFRESH_REQUIRED) &&
        record(acc, JJ_BLOCK_STATE_REFRESH_REQUIRED)) return;
    if ((input->active_authority != JJ_AUTHORITY_REMOTE ||
         !input->manual_demand_authorized) &&
        record(acc, JJ_BLOCK_CONTROL_NO_AUTHORITY)) return;
    if (!manual_target_valid(input->manual_target_c))
        (void)record(acc, JJ_BLOCK_MANUAL_TARGET_INVALID);
}

static jj_outputs_t blocked(jj_interlock_t *state, jj_block_reason_t reason,
                            const jj_inputs_t *input)
{
    jj_control_inhibit_t control_inhibit = input->control_inhibit;
    if (reason == JJ_BLOCK_CONTROL_NO_AUTHORITY)
        control_inhibit = JJ_CONTROL_INHIBIT_NO_AUTHORITY;
    else if (reason == JJ_BLOCK_STATE_REFRESH_REQUIRED)
        control_inhibit = JJ_CONTROL_INHIBIT_STATE_REFRESH_REQUIRED;
    else if (reason == JJ_BLOCK_PRINTER_UNAVAILABLE ||
             reason == JJ_BLOCK_PRINTER_NOT_PRINTING ||
             reason == JJ_BLOCK_AUTO_POLICY_UNAVAILABLE ||
             reason == JJ_BLOCK_AUTO_AUTHORITY_UNAVAILABLE ||
             reason == JJ_BLOCK_MANUAL_TARGET_INVALID ||
             reason == JJ_BLOCK_FAN_PROOF_PENDING)
        control_inhibit = JJ_CONTROL_INHIBIT_NOT_ELIGIBLE;
    const bool thermal_management = input->cooldown_required ||
        input->fault_requires_thermal_management || state->fault_latched == JJ_FAULT_FAN ||
        reason == JJ_BLOCK_FAN_PROOF_PENDING;
    return (jj_outputs_t){
        .fan_percent = thermal_management ? 100 : 0,
        .effective_target_c = 0.0f,
        .thermal_management_required = thermal_management,
        .active_authority = input->active_authority,
        .control_inhibit = control_inhibit,
        .thermal_state = thermal_management ? JJ_THERMAL_COOLDOWN : JJ_THERMAL_IDLE,
        .fault = state->fault_latched,
        .block_reason = reason,
    };
}

/* Strip any authorization from an output; optionally name the denial. */
static jj_outputs_t deauthorized(jj_outputs_t output, jj_block_reason_t reason)
{
    output.heater_authorized = false;
    output.allowed_duty_pct = 0.0f;
    output.effective_target_c = 0.0f;
    output.thermal_state = output.thermal_management_required
        ? JJ_THERMAL_COOLDOWN : JJ_THERMAL_IDLE;
    if (output.block_reason == JJ_BLOCK_NONE) output.block_reason = reason;
    return output;
}

static jj_eligibility_t evaluate_unlocked(jj_interlock_t *state,
                                          const jj_inputs_t *input)
{
    inhibit_accumulator_t acc = {
        .dominant = JJ_BLOCK_NONE,
        .mask = 0,
        .disposition = JJ_DISPOSITION_RUN,
    };
    collect_inhibits(state, input, &acc);

    jj_outputs_t output;
    if (acc.dominant == JJ_BLOCK_NONE) {
        /* Eligible form; Stage 2 publishes it only if it authorizes. */
        output = (jj_outputs_t){
            .fan_percent = 100,
            .effective_target_c = input->manual_target_c,
            .thermal_management_required = true,
            .active_authority = JJ_AUTHORITY_REMOTE,
            .control_inhibit = JJ_CONTROL_INHIBIT_NONE,
            .thermal_state = JJ_THERMAL_HEATING,
            .fault = JJ_FAULT_NONE,
            .block_reason = JJ_BLOCK_NONE,
        };
    } else {
        output = blocked(state, acc.dominant, input);
    }
    output.controller_disposition = acc.disposition;

    const jj_eligibility_t eligibility = {
        .sequence = ++state->next_sequence,
        .dominant_reason = acc.dominant,
        .inhibit_mask = acc.mask,
        .disposition = acc.disposition,
    };
    state->pending = eligibility;
    state->pending_output = output;
    state->pending_valid = true;
    /* Never leave an earlier authorization visible while Stage 2 is pending. */
    state->last_output = deauthorized(output, JJ_BLOCK_NONE);
    return eligibility;
}

jj_eligibility_t jj_interlock_evaluate(jj_interlock_t *state, const jj_inputs_t *input)
{
    if (!state || !input)
        return (jj_eligibility_t){
            .dominant_reason = JJ_BLOCK_FAULT_LATCHED,
            .disposition = JJ_DISPOSITION_SUSPEND,
        };
    STATE_LOCK();
    const jj_eligibility_t eligibility = evaluate_unlocked(state, input);
    STATE_UNLOCK();
    return eligibility;
}

static bool report_state_known(jj_controller_state_t value)
{
    switch (value) {
    case JJ_CONTROLLER_IDLE:
    case JJ_CONTROLLER_VALID:
    case JJ_CONTROLLER_INVALID:
        return true;
    }
    return false;
}

static jj_outputs_t authorize_unlocked(jj_interlock_t *state,
                                       const jj_controller_report_t *report)
{
    if (!state->pending_valid || !report ||
        report->eligibility_sequence != state->pending.sequence) {
        /* No matching Stage 1 for this report: fail cold, consume nothing. */
        state->pending_valid = false;
        state->last_output = deauthorized(state->last_output,
                                          JJ_BLOCK_CONTROL_SEQUENCE_INVALID);
        return state->last_output;
    }
    /* Use the interlock-retained evaluation, exactly once. */
    const jj_eligibility_t eligibility = state->pending;
    jj_outputs_t output = state->pending_output;
    state->pending_valid = false;

    jj_controller_state_t controller = report_state_known(report->state)
        ? report->state : JJ_CONTROLLER_INVALID;
    const float request = report->requested_duty_pct;
    if (controller == JJ_CONTROLLER_VALID &&
        !(isfinite(request) && request >= 0.0f && request <= 100.0f))
        controller = JJ_CONTROLLER_INVALID;
    if (eligibility.disposition == JJ_DISPOSITION_SUSPEND &&
        controller != JJ_CONTROLLER_IDLE)
        controller = JJ_CONTROLLER_INVALID; /* stepped while suspended */
    output.controller_state = controller;
    /* Raw request is reported unmodified; safety only gates allowed duty. */
    output.requested_duty_pct = controller == JJ_CONTROLLER_VALID ? request : 0.0f;

    if (eligibility.disposition == JJ_DISPOSITION_RUN &&
        controller == JJ_CONTROLLER_VALID) {
        output.heater_authorized = true;
        /* No non-safety product limiter exists yet (ownership unresolved). */
        output.allowed_duty_pct = output.requested_duty_pct;
    } else {
        output = deauthorized(output, JJ_BLOCK_CONTROLLER_INVALID);
    }
    state->last_output = output;
    return output;
}

jj_outputs_t jj_interlock_authorize(jj_interlock_t *state,
                                    const jj_controller_report_t *report)
{
    if (!state)
        return (jj_outputs_t){.fault = JJ_FAULT_SENSOR,
                              .block_reason = JJ_BLOCK_FAULT_LATCHED};
    STATE_LOCK();
    const jj_outputs_t output = authorize_unlocked(state, report);
    STATE_UNLOCK();
    return output;
}

static bool clear_fault_unlocked(jj_interlock_t *state, const jj_inputs_t *input)
{
    if (input->mode != JJ_MODE_OFF || !sensors_ok(input) ||
        input->overtemperature_detected || input->cooldown_required ||
        input->fault_requires_thermal_management)
        return false;
    if (jj_fault_remote_ack_policy(state->fault_latched) ==
        JJ_REMOTE_ACK_NEVER)
        return false;
    switch (state->fault_latched) {
    case JJ_FAULT_SENSOR:
        break;
    case JJ_FAULT_OVERTEMPERATURE:
        if (!input->overtemperature_reset_proven) return false;
        break;
    case JJ_FAULT_FAN:
        if (input->fan_proof != JJ_FAN_PROOF_PROVEN) return false;
        break;
    case JJ_FAULT_NO_HEAT:
        if (input->fan_proof != JJ_FAN_PROOF_PROVEN ||
            !input->no_heat_revalidation_proven)
            return false;
        break;
    case JJ_FAULT_WATCHDOG_RESET:
    case JJ_FAULT_UNEXPECTED_RESET:
    case JJ_FAULT_BROWNOUT_RESET:
        if (!input->reset_revalidation_proven) return false;
        break;
    case JJ_FAULT_NONE:
    case JJ_FAULT_UNCONTROLLED_RISE:
    case JJ_FAULT_CONFIG:
    case JJ_FAULT_STUCK_ON:
    case JJ_FAULT_COMMANDED_OFF_PROOF:
    default:
        return false;
    }
    state->fault_latched = JJ_FAULT_NONE;
    state->pending_valid = false;
    state->last_output = (jj_outputs_t){.block_reason = JJ_BLOCK_OFF};
    return true;
}

bool jj_interlock_clear_fault(jj_interlock_t *state, const jj_inputs_t *input)
{
    if (!state || !input) return false;
    STATE_LOCK();
    bool cleared = clear_fault_unlocked(state, input);
    STATE_UNLOCK();
    return cleared;
}

void jj_interlock_remove_remote_authorization(
    jj_interlock_t *state,
    jj_control_authority_t authority,
    jj_control_inhibit_t inhibit,
    jj_block_reason_t reason)
{
    if (!state) return;
    STATE_LOCK();
    /* Revocation also voids any Stage-1 result not yet authorized. */
    state->pending_valid = false;
    state->last_output.heater_authorized = false;
    state->last_output.allowed_duty_pct = 0.0f;
    state->last_output.effective_target_c = 0.0f;
    state->last_output.active_authority = authority;
    state->last_output.control_inhibit = inhibit;
    state->last_output.block_reason = reason;
    if (state->last_output.thermal_management_required)
        state->last_output.thermal_state = JJ_THERMAL_COOLDOWN;
    else
        state->last_output.thermal_state = JJ_THERMAL_IDLE;
    STATE_UNLOCK();
}

jj_outputs_t jj_interlock_snapshot(const jj_interlock_t *state)
{
    if (!state)
        return (jj_outputs_t){.fault = JJ_FAULT_SENSOR,
                              .block_reason = JJ_BLOCK_FAULT_LATCHED};
    STATE_LOCK();
    jj_outputs_t output = state->last_output;
    STATE_UNLOCK();
    return output;
}

const char *jj_fault_str(jj_fault_t fault)
{
    switch (fault) {
    case JJ_FAULT_NONE: return "none";
    case JJ_FAULT_SENSOR: return "sensor";
    case JJ_FAULT_OVERTEMPERATURE: return "overtemperature";
    case JJ_FAULT_FAN: return "fan";
    case JJ_FAULT_NO_HEAT: return "no_heat";
    case JJ_FAULT_UNCONTROLLED_RISE: return "uncontrolled_rise";
    case JJ_FAULT_CONFIG: return "config";
    case JJ_FAULT_STUCK_ON: return "stuck_on";
    case JJ_FAULT_COMMANDED_OFF_PROOF: return "commanded_off_proof";
    case JJ_FAULT_WATCHDOG_RESET: return "watchdog_reset";
    case JJ_FAULT_UNEXPECTED_RESET: return "unexpected_reset";
    case JJ_FAULT_BROWNOUT_RESET: return "brownout_reset";
    default: return "unknown";
    }
}

jj_remote_ack_policy_t jj_fault_remote_ack_policy(jj_fault_t fault)
{
    switch (fault) {
    case JJ_FAULT_SENSOR:
        return JJ_REMOTE_ACK_WHEN_HEALTHY;
    case JJ_FAULT_OVERTEMPERATURE:
    case JJ_FAULT_FAN:
    case JJ_FAULT_NO_HEAT:
    case JJ_FAULT_WATCHDOG_RESET:
    case JJ_FAULT_UNEXPECTED_RESET:
    case JJ_FAULT_BROWNOUT_RESET:
        return JJ_REMOTE_ACK_AFTER_REVALIDATION;
    case JJ_FAULT_NONE:
    case JJ_FAULT_UNCONTROLLED_RISE:
    case JJ_FAULT_CONFIG:
    case JJ_FAULT_STUCK_ON:
    case JJ_FAULT_COMMANDED_OFF_PROOF:
    default:
        return JJ_REMOTE_ACK_NEVER;
    }
}

const char *jj_block_reason_str(jj_block_reason_t reason)
{
    switch (reason) {
    case JJ_BLOCK_NONE: return "none";
    case JJ_BLOCK_OFF: return "off";
    case JJ_BLOCK_NOT_COMMISSIONED: return "not_commissioned";
    case JJ_BLOCK_FAULT_LATCHED: return "fault_latched";
    case JJ_BLOCK_PRINTER_UNAVAILABLE: return "printer_unavailable";
    case JJ_BLOCK_PRINTER_NOT_PRINTING: return "printer_not_printing";
    case JJ_BLOCK_AUTO_POLICY_UNAVAILABLE: return "automatic_policy_unavailable";
    case JJ_BLOCK_MANUAL_TARGET_INVALID: return "manual_target_invalid";
    case JJ_BLOCK_FAN_PROOF_PENDING: return "fan_proof_pending";
    case JJ_BLOCK_INVALID_MODE: return "invalid_mode";
    case JJ_BLOCK_CONTROL_NO_AUTHORITY: return "control_no_authority";
    case JJ_BLOCK_STATE_REFRESH_REQUIRED: return "state_refresh_required";
    case JJ_BLOCK_AUTO_AUTHORITY_UNAVAILABLE: return "automatic_authority_unavailable";
    case JJ_BLOCK_CONTROLLER_INVALID: return "controller_invalid";
    case JJ_BLOCK_CONTROL_SEQUENCE_INVALID: return "control_sequence_invalid";
    default: return "unknown";
    }
}

const char *jj_control_authority_str(jj_control_authority_t authority)
{
    switch (authority) {
    case JJ_AUTHORITY_NONE: return "none";
    case JJ_AUTHORITY_REMOTE: return "remote";
    case JJ_AUTHORITY_AUTOMATIC: return "automatic";
    case JJ_AUTHORITY_REACQUIRING: return "reacquiring";
    default: return "unknown";
    }
}

const char *jj_control_inhibit_str(jj_control_inhibit_t inhibit)
{
    switch (inhibit) {
    case JJ_CONTROL_INHIBIT_NONE: return "none";
    case JJ_CONTROL_INHIBIT_NO_AUTHORITY: return "no_authority";
    case JJ_CONTROL_INHIBIT_STATE_REFRESH_REQUIRED: return "state_refresh_required";
    case JJ_CONTROL_INHIBIT_NOT_ELIGIBLE: return "not_eligible";
    default: return "unknown";
    }
}

const char *jj_thermal_state_str(jj_thermal_state_t state)
{
    switch (state) {
    case JJ_THERMAL_IDLE: return "idle";
    case JJ_THERMAL_HEATING: return "heating";
    case JJ_THERMAL_COOLDOWN: return "cooldown";
    default: return "unknown";
    }
}

const char *jj_controller_state_str(jj_controller_state_t state)
{
    switch (state) {
    case JJ_CONTROLLER_IDLE: return "idle";
    case JJ_CONTROLLER_VALID: return "valid";
    case JJ_CONTROLLER_INVALID: return "invalid";
    default: return "unknown";
    }
}

const char *jj_controller_disposition_str(jj_controller_disposition_t disposition)
{
    switch (disposition) {
    case JJ_DISPOSITION_SUSPEND: return "suspend";
    case JJ_DISPOSITION_HOLD: return "hold";
    case JJ_DISPOSITION_RUN: return "run";
    default: return "unknown";
    }
}
