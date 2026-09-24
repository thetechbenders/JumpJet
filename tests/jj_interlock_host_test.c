#include "jj_interlock.h"
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

#define CHECK(condition) do { if (!(condition)) { \
    fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #condition); \
    exit(1); } } while (0)

static jj_inputs_t nominal(jj_mode_t mode)
{
    jj_inputs_t input = {
        .commissioned = true,
        .mode = mode,
        .manual_target_c = JJ_MANUAL_TARGET_DEFAULT_C,
        .chamber = {.status = JJ_SENSOR_OK, .temperature_c = 30.0f},
        .outlet = {.status = JJ_SENSOR_OK, .temperature_c = 35.0f},
        .case_sensor = {.status = JJ_SENSOR_OK, .temperature_c = 36.0f},
        .printer = {.online = true, .printing = true, .bed_target_c = 100.0f},
        .fan_proof = JJ_FAN_PROOF_PROVEN,
    };
    if (mode == JJ_MODE_MANUAL) {
        input.active_authority = JJ_AUTHORITY_REMOTE;
        input.manual_demand_authorized = true;
    } else if (mode == JJ_MODE_AUTOMATIC) {
        input.active_authority = JJ_AUTHORITY_AUTOMATIC;
    }
    return input;
}

/*
 * One staged cycle with a well-formed VALID controller report. Exercises the
 * interlock alone: Stage 2 must treat this caller-built report as untrusted.
 */
static jj_outputs_t cycle_with(jj_interlock_t *state, const jj_inputs_t *input,
                               jj_controller_state_t controller, float duty)
{
    const jj_eligibility_t eligibility = jj_interlock_evaluate(state, input);
    const jj_controller_report_t report = {
        .eligibility_sequence = eligibility.sequence,
        .state = eligibility.disposition == JJ_DISPOSITION_SUSPEND
            ? JJ_CONTROLLER_IDLE : controller,
        .requested_duty_pct = eligibility.disposition == JJ_DISPOSITION_SUSPEND
            ? 0.0f : duty,
    };
    return jj_interlock_authorize(state, &report);
}

static jj_outputs_t cycle(jj_interlock_t *state, const jj_inputs_t *input)
{
    return cycle_with(state, input, JJ_CONTROLLER_VALID, 50.0f);
}

static jj_outputs_t step_once(jj_inputs_t input)
{
    jj_interlock_t state;
    jj_interlock_init(&state);
    return cycle(&state, &input);
}

static void check_cold(jj_outputs_t output, jj_block_reason_t reason)
{
    CHECK(!output.heater_authorized);
    CHECK(output.allowed_duty_pct == 0.0f);
    CHECK(output.effective_target_c == 0.0f);
    CHECK(output.block_reason == reason);
}

static void test_boot_defaults_and_modes(void)
{
    jj_inputs_t safe = jj_inputs_safe_defaults();
    CHECK(!safe.commissioned);
    CHECK(safe.mode == JJ_MODE_OFF);
    CHECK(safe.manual_target_c == JJ_MANUAL_TARGET_DEFAULT_C);
    check_cold(step_once(safe), JJ_BLOCK_NOT_COMMISSIONED);

    jj_inputs_t input = nominal(JJ_MODE_OFF);
    check_cold(step_once(input), JJ_BLOCK_OFF);
    input.mode = (jj_mode_t)99;
    check_cold(step_once(input), JJ_BLOCK_INVALID_MODE);
}

static void test_manual_target_is_rejected_not_clamped(void)
{
    const float accepted[] = {JJ_MANUAL_TARGET_MIN_C, JJ_MANUAL_TARGET_DEFAULT_C,
                              JJ_MANUAL_TARGET_MAX_C};
    for (size_t i = 0; i < sizeof(accepted) / sizeof(accepted[0]); ++i) {
        jj_inputs_t input = nominal(JJ_MODE_MANUAL);
        input.manual_target_c = accepted[i];
        jj_outputs_t output = step_once(input);
        CHECK(output.heater_authorized);
        CHECK(output.effective_target_c == accepted[i]);
    }
    const float rejected[] = {29.9f, 50.1f, NAN, INFINITY, -INFINITY};
    for (size_t i = 0; i < sizeof(rejected) / sizeof(rejected[0]); ++i) {
        jj_inputs_t input = nominal(JJ_MODE_MANUAL);
        input.manual_target_c = rejected[i];
        check_cold(step_once(input), JJ_BLOCK_MANUAL_TARGET_INVALID);
    }
}

static void test_automatic_is_whitelisted_and_policy_blocked(void)
{
    jj_inputs_t input = nominal(JJ_MODE_AUTOMATIC);
    input.printer.online = false; /* includes dc_prusa missing/stale result */
    check_cold(step_once(input), JJ_BLOCK_PRINTER_UNAVAILABLE);

    input = nominal(JJ_MODE_AUTOMATIC);
    input.printer.printing = false; /* every state other than exact PRINTING */
    check_cold(step_once(input), JJ_BLOCK_PRINTER_NOT_PRINTING);

    input = nominal(JJ_MODE_AUTOMATIC);
    input.printer.bed_target_c = 0.0f;
    check_cold(step_once(input), JJ_BLOCK_AUTO_POLICY_UNAVAILABLE);
    input.printer.bed_target_c = 200.0f;
    check_cold(step_once(input), JJ_BLOCK_AUTO_POLICY_UNAVAILABLE);
    input.printer.bed_target_c = NAN;
    check_cold(step_once(input), JJ_BLOCK_AUTO_POLICY_UNAVAILABLE);
}

static void test_automatic_authority_and_eligibility_are_independent(void)
{
    jj_inputs_t input = nominal(JJ_MODE_AUTOMATIC);
    input.automatic_target_available = true;
    input.automatic_target_c = 42.0f;
    jj_outputs_t output = step_once(input);
    check_cold(output, JJ_BLOCK_AUTO_POLICY_UNAVAILABLE); /* fully eligible; still
                                                             cold until the bed-
                                                             target policy exists */

    input.active_authority = JJ_AUTHORITY_NONE;
    check_cold(step_once(input), JJ_BLOCK_AUTO_AUTHORITY_UNAVAILABLE);
    input.active_authority = JJ_AUTHORITY_AUTOMATIC;
    input.printer.online = false;
    output = step_once(input);
    check_cold(output, JJ_BLOCK_PRINTER_UNAVAILABLE);
    CHECK(output.control_inhibit == JJ_CONTROL_INHIBIT_NOT_ELIGIBLE);
    CHECK(output.fault == JJ_FAULT_NONE);
}

static void test_faults_latch_and_need_explicit_safe_clear(void)
{
    jj_interlock_t state;
    jj_interlock_init(&state);
    jj_inputs_t input = nominal(JJ_MODE_MANUAL);
    input.chamber.status = JJ_SENSOR_OPEN;
    jj_outputs_t output = cycle(&state, &input);
    CHECK(output.fault == JJ_FAULT_SENSOR);
    check_cold(output, JJ_BLOCK_FAULT_LATCHED);

    input = nominal(JJ_MODE_MANUAL);
    CHECK(cycle(&state, &input).fault == JJ_FAULT_SENSOR);
    CHECK(!jj_interlock_clear_fault(&state, &input));
    input.mode = JJ_MODE_OFF;
    input.cooldown_required = true;
    CHECK(!jj_interlock_clear_fault(&state, &input));
    input.cooldown_required = false;
    CHECK(jj_interlock_clear_fault(&state, &input));

    jj_interlock_init(&state);
    input = nominal(JJ_MODE_MANUAL);
    input.overtemperature_detected = true;
    output = cycle(&state, &input);
    CHECK(output.fault == JJ_FAULT_OVERTEMPERATURE);
    check_cold(output, JJ_BLOCK_FAULT_LATCHED);
}

static void test_off_keeps_thermal_management_request(void)
{
    jj_inputs_t input = nominal(JJ_MODE_OFF);
    input.cooldown_required = true;
    jj_outputs_t output = step_once(input);
    check_cold(output, JJ_BLOCK_OFF);
    CHECK(output.thermal_management_required);
    CHECK(output.fan_percent == 100);

    input.cooldown_required = false;
    input.fault_requires_thermal_management = true;
    output = step_once(input);
    CHECK(output.thermal_management_required);
    CHECK(output.fan_percent == 100);
}

static void test_fan_proof_and_null_inputs_fail_cold(void)
{
    jj_inputs_t input = nominal(JJ_MODE_MANUAL);
    input.fan_proof = JJ_FAN_PROOF_PENDING;
    check_cold(step_once(input), JJ_BLOCK_FAN_PROOF_PENDING);
    input.fan_proof = JJ_FAN_PROOF_FAILED;
    jj_outputs_t output = step_once(input);
    CHECK(output.fault == JJ_FAULT_FAN);
    check_cold(output, JJ_BLOCK_FAULT_LATCHED);

    jj_interlock_t state;
    jj_interlock_init(&state);
    const jj_eligibility_t null_eligibility = jj_interlock_evaluate(NULL, &input);
    CHECK(null_eligibility.disposition == JJ_DISPOSITION_SUSPEND);
    CHECK(null_eligibility.dominant_reason == JJ_BLOCK_FAULT_LATCHED);
    CHECK(jj_interlock_evaluate(&state, NULL).disposition == JJ_DISPOSITION_SUSPEND);
    const jj_controller_report_t report = {.state = JJ_CONTROLLER_VALID,
                                           .requested_duty_pct = 50.0f};
    check_cold(jj_interlock_authorize(NULL, &report), JJ_BLOCK_FAULT_LATCHED);
    CHECK(!jj_interlock_authorize(&state, &report).heater_authorized);
    CHECK(!jj_interlock_authorize(&state, NULL).heater_authorized);
}

static void test_remote_fault_acknowledgement_policy(void)
{
    CHECK(jj_fault_remote_ack_policy(JJ_FAULT_SENSOR) ==
          JJ_REMOTE_ACK_WHEN_HEALTHY);
    CHECK(jj_fault_remote_ack_policy(JJ_FAULT_OVERTEMPERATURE) ==
          JJ_REMOTE_ACK_AFTER_REVALIDATION);
    CHECK(jj_fault_remote_ack_policy(JJ_FAULT_FAN) ==
          JJ_REMOTE_ACK_AFTER_REVALIDATION);
    CHECK(jj_fault_remote_ack_policy(JJ_FAULT_NO_HEAT) ==
          JJ_REMOTE_ACK_AFTER_REVALIDATION);
    CHECK(jj_fault_remote_ack_policy(JJ_FAULT_WATCHDOG_RESET) ==
          JJ_REMOTE_ACK_AFTER_REVALIDATION);
    CHECK(jj_fault_remote_ack_policy(JJ_FAULT_UNEXPECTED_RESET) ==
          JJ_REMOTE_ACK_AFTER_REVALIDATION);
    CHECK(jj_fault_remote_ack_policy(JJ_FAULT_BROWNOUT_RESET) ==
          JJ_REMOTE_ACK_AFTER_REVALIDATION);
    CHECK(jj_fault_remote_ack_policy(JJ_FAULT_UNCONTROLLED_RISE) ==
          JJ_REMOTE_ACK_NEVER);
    CHECK(jj_fault_remote_ack_policy(JJ_FAULT_CONFIG) == JJ_REMOTE_ACK_NEVER);
    CHECK(jj_fault_remote_ack_policy(JJ_FAULT_STUCK_ON) == JJ_REMOTE_ACK_NEVER);
    CHECK(jj_fault_remote_ack_policy(JJ_FAULT_COMMANDED_OFF_PROOF) ==
          JJ_REMOTE_ACK_NEVER);
    CHECK(jj_fault_remote_ack_policy((jj_fault_t)999) == JJ_REMOTE_ACK_NEVER);

    jj_interlock_t state;
    jj_interlock_init(&state);
    jj_inputs_t input = nominal(JJ_MODE_OFF);

    state.fault_latched = JJ_FAULT_SENSOR;
    CHECK(jj_interlock_clear_fault(&state, &input));

    state.fault_latched = JJ_FAULT_SENSOR;
    input.chamber.status = JJ_SENSOR_IMPLAUSIBLE;
    CHECK(!jj_interlock_clear_fault(&state, &input));
    input.chamber.status = JJ_SENSOR_OPEN;
    CHECK(!jj_interlock_clear_fault(&state, &input));
    input.chamber.status = JJ_SENSOR_SHORT;
    CHECK(!jj_interlock_clear_fault(&state, &input));
    input.chamber.status = JJ_SENSOR_OK;
    input.chamber.temperature_c = NAN;
    CHECK(!jj_interlock_clear_fault(&state, &input));
    input.chamber.temperature_c = 30.0f;

    state.fault_latched = JJ_FAULT_FAN;
    input.fan_proof = JJ_FAN_PROOF_PENDING;
    CHECK(!jj_interlock_clear_fault(&state, &input));
    input.fan_proof = JJ_FAN_PROOF_PROVEN;
    CHECK(jj_interlock_clear_fault(&state, &input));

    state.fault_latched = JJ_FAULT_OVERTEMPERATURE;
    CHECK(!jj_interlock_clear_fault(&state, &input));
    input.overtemperature_reset_proven = true;
    input.cooldown_required = true;
    CHECK(!jj_interlock_clear_fault(&state, &input));
    input.cooldown_required = false;
    CHECK(jj_interlock_clear_fault(&state, &input));

    state.fault_latched = JJ_FAULT_NO_HEAT;
    CHECK(!jj_interlock_clear_fault(&state, &input));
    input.no_heat_revalidation_proven = true;
    CHECK(jj_interlock_clear_fault(&state, &input));

    state.fault_latched = JJ_FAULT_UNCONTROLLED_RISE;
    input.reset_revalidation_proven = true;
    CHECK(!jj_interlock_clear_fault(&state, &input));
    state.fault_latched = JJ_FAULT_CONFIG;
    CHECK(!jj_interlock_clear_fault(&state, &input));
    state.fault_latched = (jj_fault_t)999;
    CHECK(!jj_interlock_clear_fault(&state, &input));

    state.fault_latched = JJ_FAULT_WATCHDOG_RESET;
    CHECK(jj_interlock_clear_fault(&state, &input));
}


static void test_disposition_classification_is_pinned(void)
{
    /* Changing any disposition must be a deliberate, reviewed test change. */
    for (int r = 0; r <= (int)JJ_BLOCK_REASON_COUNT; ++r) {
        const jj_block_reason_t reason = (jj_block_reason_t)r;
        jj_controller_disposition_t expected = JJ_DISPOSITION_SUSPEND;
        if (reason == JJ_BLOCK_NONE) expected = JJ_DISPOSITION_RUN;
        if (reason == JJ_BLOCK_FAN_PROOF_PENDING) expected = JJ_DISPOSITION_HOLD;
        CHECK(jj_block_reason_disposition(reason) == expected);
    }
    CHECK(jj_block_reason_disposition((jj_block_reason_t)999) ==
          JJ_DISPOSITION_SUSPEND);
    CHECK(JJ_DISPOSITION_SUSPEND < JJ_DISPOSITION_HOLD);
    CHECK(JJ_DISPOSITION_HOLD < JJ_DISPOSITION_RUN);
    CHECK(JJ_DISPOSITION_SUSPEND == 0 && JJ_CONTROLLER_IDLE == 0);
}

static void test_overlapping_inhibits_aggregate_conservatively(void)
{
    jj_interlock_t state;
    jj_interlock_init(&state);
    jj_inputs_t input = nominal(JJ_MODE_MANUAL);
    CHECK(jj_interlock_evaluate(&state, &input).disposition == JJ_DISPOSITION_RUN);

    input.fan_proof = JJ_FAN_PROOF_PENDING;
    jj_eligibility_t e = jj_interlock_evaluate(&state, &input);
    CHECK(e.disposition == JJ_DISPOSITION_HOLD);
    CHECK(e.dominant_reason == JJ_BLOCK_FAN_PROOF_PENDING);

    /* Same dominant diagnostic reason, but disposition sees every inhibit. */
    jj_inputs_t no_authority = input;
    no_authority.manual_demand_authorized = false;
    e = jj_interlock_evaluate(&state, &no_authority);
    CHECK(e.dominant_reason == JJ_BLOCK_FAN_PROOF_PENDING);
    CHECK(e.disposition == JJ_DISPOSITION_SUSPEND);
    CHECK(e.inhibit_mask & (UINT32_C(1) << JJ_BLOCK_FAN_PROOF_PENDING));
    CHECK(e.inhibit_mask & (UINT32_C(1) << JJ_BLOCK_CONTROL_NO_AUTHORITY));

    jj_inputs_t refresh = input;
    refresh.active_authority = JJ_AUTHORITY_REACQUIRING;
    CHECK(jj_interlock_evaluate(&state, &refresh).disposition ==
          JJ_DISPOSITION_SUSPEND);

    jj_inputs_t bad_target = input;
    bad_target.manual_target_c = 60.0f;
    e = jj_interlock_evaluate(&state, &bad_target);
    CHECK(e.dominant_reason == JJ_BLOCK_FAN_PROOF_PENDING);
    CHECK(e.disposition == JJ_DISPOSITION_SUSPEND);

    jj_inputs_t automatic = nominal(JJ_MODE_AUTOMATIC);
    automatic.fan_proof = JJ_FAN_PROOF_PENDING;
    CHECK(jj_interlock_evaluate(&state, &automatic).disposition ==
          JJ_DISPOSITION_SUSPEND);

    /* Fault latch dominates and suspends regardless of other conditions. */
    jj_inputs_t faulted = input;
    faulted.overtemperature_detected = true;
    e = jj_interlock_evaluate(&state, &faulted);
    CHECK(e.dominant_reason == JJ_BLOCK_FAULT_LATCHED);
    CHECK(e.disposition == JJ_DISPOSITION_SUSPEND);
}

static void test_authorization_requires_valid_controller(void)
{
    jj_inputs_t input = nominal(JJ_MODE_MANUAL);
    jj_interlock_t state;

    jj_interlock_init(&state);
    jj_outputs_t out = cycle_with(&state, &input, JJ_CONTROLLER_INVALID, 80.0f);
    check_cold(out, JJ_BLOCK_CONTROLLER_INVALID);
    CHECK(out.controller_state == JJ_CONTROLLER_INVALID);
    CHECK(out.requested_duty_pct == 0.0f);

    jj_interlock_init(&state);
    out = cycle_with(&state, &input, JJ_CONTROLLER_IDLE, 0.0f);
    check_cold(out, JJ_BLOCK_CONTROLLER_INVALID);

    const float malformed[] = {NAN, INFINITY, -1.0f, 100.5f};
    for (size_t i = 0; i < sizeof malformed / sizeof malformed[0]; ++i) {
        jj_interlock_init(&state);
        out = cycle_with(&state, &input, JJ_CONTROLLER_VALID, malformed[i]);
        check_cold(out, JJ_BLOCK_CONTROLLER_INVALID);
        CHECK(out.controller_state == JJ_CONTROLLER_INVALID);
        CHECK(out.requested_duty_pct == 0.0f);
    }
    jj_interlock_init(&state);
    out = cycle_with(&state, &input, (jj_controller_state_t)77, 50.0f);
    check_cold(out, JJ_BLOCK_CONTROLLER_INVALID);

    /* A legitimate VALID 0 % request is authorized and distinct from denial. */
    jj_interlock_init(&state);
    out = cycle_with(&state, &input, JJ_CONTROLLER_VALID, 0.0f);
    CHECK(out.heater_authorized);
    CHECK(out.controller_state == JJ_CONTROLLER_VALID);
    CHECK(out.allowed_duty_pct == 0.0f);
    CHECK(out.block_reason == JJ_BLOCK_NONE);

    jj_interlock_init(&state);
    out = cycle_with(&state, &input, JJ_CONTROLLER_VALID, 37.5f);
    CHECK(out.heater_authorized);
    CHECK(out.requested_duty_pct == 37.5f);
    CHECK(out.allowed_duty_pct == 37.5f);
    CHECK(out.effective_target_c == JJ_MANUAL_TARGET_DEFAULT_C);
    CHECK(out.thermal_state == JJ_THERMAL_HEATING);
}

static void test_safety_never_rewrites_valid_request(void)
{
    jj_interlock_t state;
    jj_interlock_init(&state);
    jj_inputs_t input = nominal(JJ_MODE_MANUAL);
    input.fan_proof = JJ_FAN_PROOF_PENDING;
    jj_outputs_t out = cycle_with(&state, &input, JJ_CONTROLLER_VALID, 42.0f);
    check_cold(out, JJ_BLOCK_FAN_PROOF_PENDING);
    CHECK(out.controller_disposition == JJ_DISPOSITION_HOLD);
    CHECK(out.controller_state == JJ_CONTROLLER_VALID);
    CHECK(out.requested_duty_pct == 42.0f);

    /* Stepping while suspended is a controller contract violation. */
    jj_inputs_t off = nominal(JJ_MODE_OFF);
    const jj_eligibility_t e = jj_interlock_evaluate(&state, &off);
    const jj_controller_report_t rogue = {
        .eligibility_sequence = e.sequence,
        .state = JJ_CONTROLLER_VALID,
        .requested_duty_pct = 90.0f,
    };
    out = jj_interlock_authorize(&state, &rogue);
    check_cold(out, JJ_BLOCK_OFF);
    CHECK(out.controller_state == JJ_CONTROLLER_INVALID);
}

static void test_stage_two_consumes_only_its_own_stage_one(void)
{
    jj_interlock_t state;
    jj_inputs_t input = nominal(JJ_MODE_MANUAL);
    const jj_controller_report_t any = {.state = JJ_CONTROLLER_VALID,
                                        .requested_duty_pct = 50.0f};

    /* Authorize with no Stage 1 at all. */
    jj_interlock_init(&state);
    CHECK(!jj_interlock_authorize(&state, &any).heater_authorized);

    /* Wrong token: denied, and the pending evaluation is consumed. */
    jj_interlock_init(&state);
    jj_eligibility_t e = jj_interlock_evaluate(&state, &input);
    jj_controller_report_t report = {.eligibility_sequence = e.sequence + 1,
                                     .state = JJ_CONTROLLER_VALID,
                                     .requested_duty_pct = 50.0f};
    jj_outputs_t out = jj_interlock_authorize(&state, &report);
    check_cold(out, JJ_BLOCK_CONTROL_SEQUENCE_INVALID);
    report.eligibility_sequence = e.sequence;
    CHECK(!jj_interlock_authorize(&state, &report).heater_authorized);

    /* Replay: one Stage 1 authorizes at most one Stage 2. */
    jj_interlock_init(&state);
    e = jj_interlock_evaluate(&state, &input);
    report.eligibility_sequence = e.sequence;
    CHECK(jj_interlock_authorize(&state, &report).heater_authorized);
    out = jj_interlock_authorize(&state, &report);
    check_cold(out, JJ_BLOCK_CONTROL_SEQUENCE_INVALID);
    CHECK(!jj_interlock_snapshot(&state).heater_authorized);

    /* Stale: a newer Stage 1 supersedes an older one. */
    jj_interlock_init(&state);
    const jj_eligibility_t older = jj_interlock_evaluate(&state, &input);
    (void)jj_interlock_evaluate(&state, &input);
    report.eligibility_sequence = older.sequence;
    CHECK(!jj_interlock_authorize(&state, &report).heater_authorized);

    /* A caller cannot manufacture eligibility: Stage 2 uses its own copy. */
    jj_interlock_init(&state);
    jj_inputs_t off = nominal(JJ_MODE_OFF);
    e = jj_interlock_evaluate(&state, &off);
    jj_eligibility_t forged = e;
    forged.disposition = JJ_DISPOSITION_RUN;
    forged.dominant_reason = JJ_BLOCK_NONE;
    report.eligibility_sequence = forged.sequence;
    CHECK(!jj_interlock_authorize(&state, &report).heater_authorized);

    /* Revocation between the stages voids the pending evaluation. */
    jj_interlock_init(&state);
    e = jj_interlock_evaluate(&state, &input);
    jj_interlock_remove_remote_authorization(
        &state, JJ_AUTHORITY_NONE, JJ_CONTROL_INHIBIT_NO_AUTHORITY,
        JJ_BLOCK_CONTROL_NO_AUTHORITY);
    report.eligibility_sequence = e.sequence;
    out = jj_interlock_authorize(&state, &report);
    CHECK(!out.heater_authorized);
    CHECK(out.block_reason == JJ_BLOCK_CONTROL_NO_AUTHORITY);

    /* Fault clear between the stages also voids it. */
    jj_interlock_init(&state);
    jj_inputs_t clear = nominal(JJ_MODE_OFF);
    e = jj_interlock_evaluate(&state, &input);
    state.fault_latched = JJ_FAULT_SENSOR;
    CHECK(jj_interlock_clear_fault(&state, &clear));
    report.eligibility_sequence = e.sequence;
    CHECK(!jj_interlock_authorize(&state, &report).heater_authorized);

    /* Stage 1 immediately withdraws any earlier authorization. */
    jj_interlock_init(&state);
    CHECK(cycle(&state, &input).heater_authorized);
    CHECK(jj_interlock_snapshot(&state).heater_authorized);
    (void)jj_interlock_evaluate(&state, &input);
    CHECK(!jj_interlock_snapshot(&state).heater_authorized);
    CHECK(jj_interlock_snapshot(&state).allowed_duty_pct == 0.0f);
}

int main(void)
{
    test_boot_defaults_and_modes();
    test_manual_target_is_rejected_not_clamped();
    test_automatic_is_whitelisted_and_policy_blocked();
    test_automatic_authority_and_eligibility_are_independent();
    test_faults_latch_and_need_explicit_safe_clear();
    test_off_keeps_thermal_management_request();
    test_fan_proof_and_null_inputs_fail_cold();
    test_remote_fault_acknowledgement_policy();
    test_disposition_classification_is_pinned();
    test_overlapping_inhibits_aggregate_conservatively();
    test_authorization_requires_valid_controller();
    test_safety_never_rewrites_valid_request();
    test_stage_two_consumes_only_its_own_stage_one();
    puts("jj_interlock_host_test: PASS");
    return 0;
}
