// SPDX-License-Identifier: GPL-3.0-or-later
#include "jj_control.h"
#include "jj_interlock.h"
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

#define CHECK(condition) do { if (!(condition)) { \
    fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #condition); \
    exit(1); } } while (0)

/*
 * TEST FIXTURE ONLY. Non-zero gains exist so lifecycle behavior is observable.
 * They are not tuning and say nothing about the Jump Jet thermal plant.
 */
static jj_controller_config_t fixture_config(void)
{
    jj_controller_config_t config = jj_controller_provisional_config();
    config.pid.kp = 4.0f;
    config.pid.ki = 0.5f;
    return config;
}

static jj_inputs_t manual_inputs(float chamber_c)
{
    return (jj_inputs_t){
        .commissioned = true,
        .mode = JJ_MODE_MANUAL,
        .manual_target_c = JJ_MANUAL_TARGET_DEFAULT_C,
        .active_authority = JJ_AUTHORITY_REMOTE,
        .manual_demand_authorized = true,
        .chamber = {.status = JJ_SENSOR_OK, .temperature_c = chamber_c},
        .outlet = {.status = JJ_SENSOR_OK, .temperature_c = 35.0f},
        .case_sensor = {.status = JJ_SENSOR_OK, .temperature_c = 36.0f},
        .printer = {.online = true, .printing = true, .bed_target_c = 100.0f},
        .fan_proof = JJ_FAN_PROOF_PROVEN,
    };
}

typedef struct {
    jj_controller_result_t controller;
    jj_outputs_t output;
} cycle_t;

/* The production ordering: Stage 1 -> controller -> Stage 2. */
static cycle_t run_cycle(jj_interlock_t *interlock, jj_controller_t *controller,
                         const jj_inputs_t *input)
{
    cycle_t c;
    const jj_eligibility_t eligibility = jj_interlock_evaluate(interlock, input);
    c.controller = jj_controller_step(controller, &eligibility, input);
    c.output = jj_interlock_authorize(interlock, &c.controller.report);
    return c;
}

static void check_cold(const cycle_t *c)
{
    CHECK(!c->output.heater_authorized);
    CHECK(c->output.allowed_duty_pct == 0.0f);
}

static void check_idle(const cycle_t *c)
{
    CHECK(!c->controller.stepped);
    CHECK(c->controller.report.state == JJ_CONTROLLER_IDLE);
    CHECK(c->output.controller_state == JJ_CONTROLLER_IDLE);
    CHECK(c->output.requested_duty_pct == 0.0f);
    check_cold(c);
}

static void setup(jj_interlock_t *interlock, jj_controller_t *controller,
                  jj_controller_config_t config)
{
    jj_interlock_init(interlock);
    jj_controller_init(controller, &config);
}

static void test_boot_and_off_are_idle(void)
{
    jj_interlock_t interlock;
    jj_controller_t controller;
    setup(&interlock, &controller, fixture_config());

    const jj_inputs_t boot = jj_inputs_safe_defaults();
    cycle_t c = run_cycle(&interlock, &controller, &boot);
    check_idle(&c);
    CHECK(c.output.block_reason == JJ_BLOCK_NOT_COMMISSIONED);

    jj_inputs_t off = manual_inputs(30.0f);
    off.mode = JJ_MODE_OFF;
    c = run_cycle(&interlock, &controller, &off);
    check_idle(&c);
    CHECK(c.output.block_reason == JJ_BLOCK_OFF);
    /* IDLE is ordinary: not a fault, not the controller-invalid reason. */
    CHECK(c.output.fault == JJ_FAULT_NONE);
    CHECK(c.output.block_reason != JJ_BLOCK_CONTROLLER_INVALID);
}

static void test_off_to_manual_steps_and_authorizes(void)
{
    jj_interlock_t interlock;
    jj_controller_t controller;
    setup(&interlock, &controller, fixture_config());
    jj_inputs_t input = manual_inputs(30.0f);
    input.mode = JJ_MODE_OFF;
    (void)run_cycle(&interlock, &controller, &input);

    input.mode = JJ_MODE_MANUAL;
    const cycle_t c = run_cycle(&interlock, &controller, &input);
    CHECK(c.controller.stepped);
    CHECK(c.controller.integrating);
    CHECK(c.controller.setpoint_c == JJ_MANUAL_TARGET_DEFAULT_C);
    CHECK(c.output.controller_state == JJ_CONTROLLER_VALID);
    CHECK(c.output.heater_authorized);
    CHECK(c.output.requested_duty_pct == c.controller.pid.output);
    CHECK(c.output.requested_duty_pct > 0.0f);
    CHECK(c.output.allowed_duty_pct == c.output.requested_duty_pct);
}

static void test_provisional_config_is_valid_and_requests_nothing(void)
{
    jj_interlock_t interlock;
    jj_controller_t controller;
    setup(&interlock, &controller, jj_controller_provisional_config());
    const jj_inputs_t input = manual_inputs(30.0f);
    const cycle_t c = run_cycle(&interlock, &controller, &input);
    CHECK(c.output.controller_state == JJ_CONTROLLER_VALID);
    CHECK(c.output.requested_duty_pct == 0.0f);
    CHECK(c.output.allowed_duty_pct == 0.0f);
}

static void test_rejected_step_is_invalid_and_cold(void)
{
    jj_controller_config_t bad[3];
    bad[0] = fixture_config();
    bad[0].pid.kd = 1.0f;              /* kd without derivative_alpha */
    bad[1] = fixture_config();
    bad[1].sample_period_s = 0.0f;     /* dt must be positive */
    bad[2] = fixture_config();
    bad[2].pid.output_max = NAN;
    for (size_t i = 0; i < sizeof bad / sizeof bad[0]; ++i) {
        jj_interlock_t interlock;
        jj_controller_t controller;
        setup(&interlock, &controller, bad[i]);
        const jj_inputs_t input = manual_inputs(30.0f);
        for (int n = 0; n < 5; ++n) {
            const cycle_t c = run_cycle(&interlock, &controller, &input);
            CHECK(c.controller.stepped);
            CHECK(c.controller.report.state == JJ_CONTROLLER_INVALID);
            CHECK(c.output.controller_state == JJ_CONTROLLER_INVALID);
            CHECK(c.output.requested_duty_pct == 0.0f);
            check_cold(&c);
            CHECK(c.output.block_reason == JJ_BLOCK_CONTROLLER_INVALID);
        }
    }
}

static void test_automatic_is_idle_before_phase_four(void)
{
    jj_interlock_t interlock;
    jj_controller_t controller;
    setup(&interlock, &controller, fixture_config());
    jj_inputs_t input = manual_inputs(30.0f);
    input.mode = JJ_MODE_AUTOMATIC;
    input.active_authority = JJ_AUTHORITY_AUTOMATIC;
    input.automatic_target_available = true;
    input.automatic_target_c = 42.0f;
    for (int n = 0; n < 5; ++n) {
        const cycle_t c = run_cycle(&interlock, &controller, &input);
        check_idle(&c);
        CHECK(c.output.block_reason == JJ_BLOCK_AUTO_POLICY_UNAVAILABLE);
    }
    /* Even if eligibility said RUN, AUTOMATIC has no setpoint to control. */
    const jj_eligibility_t run = {.disposition = JJ_DISPOSITION_RUN};
    const jj_controller_result_t r = jj_controller_step(&controller, &run, &input);
    CHECK(!r.stepped && r.report.state == JJ_CONTROLLER_IDLE);
}

static void test_fan_proof_pending_holds_and_freezes_integral(void)
{
    jj_interlock_t interlock;
    jj_controller_t controller;
    setup(&interlock, &controller, fixture_config());
    jj_inputs_t input = manual_inputs(40.0f);
    for (int n = 0; n < 4; ++n) (void)run_cycle(&interlock, &controller, &input);
    const float integral = controller.pid.integral;
    CHECK(integral > 0.0f);

    input.fan_proof = JJ_FAN_PROOF_PENDING;
    for (int n = 0; n < 3; ++n) {
        const cycle_t c = run_cycle(&interlock, &controller, &input);
        CHECK(c.output.controller_disposition == JJ_DISPOSITION_HOLD);
        CHECK(c.controller.stepped);
        CHECK(!c.controller.integrating);
        CHECK(controller.pid.integral == integral);
        CHECK(c.output.controller_state == JJ_CONTROLLER_VALID);
        CHECK(c.output.requested_duty_pct > 0.0f); /* raw request, not zeroed */
        check_cold(&c);
        CHECK(c.output.block_reason == JJ_BLOCK_FAN_PROOF_PENDING);
    }

    /* Overlap with a SUSPEND-class inhibit suspends and resets. */
    jj_inputs_t no_authority = input;
    no_authority.manual_demand_authorized = false;
    cycle_t c = run_cycle(&interlock, &controller, &no_authority);
    CHECK(c.output.block_reason == JJ_BLOCK_FAN_PROOF_PENDING);
    CHECK(c.output.controller_disposition == JJ_DISPOSITION_SUSPEND);
    check_idle(&c);
    CHECK(controller.pid.integral == 0.0f);

    /* Resuming from a pure HOLD keeps the frozen integral. */
    setup(&interlock, &controller, fixture_config());
    input = manual_inputs(40.0f);
    for (int n = 0; n < 4; ++n) (void)run_cycle(&interlock, &controller, &input);
    const float kept = controller.pid.integral;
    input.fan_proof = JJ_FAN_PROOF_PENDING;
    (void)run_cycle(&interlock, &controller, &input);
    input.fan_proof = JJ_FAN_PROOF_PROVEN;
    c = run_cycle(&interlock, &controller, &input);
    CHECK(c.controller.integrating);
    CHECK(controller.pid.integral > kept);
    CHECK(c.output.heater_authorized);
}

static void test_fault_latch_keeps_controller_idle_until_safe_clear(void)
{
    jj_interlock_t interlock;
    jj_controller_t controller;
    setup(&interlock, &controller, fixture_config());
    jj_inputs_t input = manual_inputs(40.0f);
    for (int n = 0; n < 4; ++n) (void)run_cycle(&interlock, &controller, &input);
    CHECK(controller.pid.integral > 0.0f);

    jj_inputs_t faulted = input;
    faulted.overtemperature_detected = true;
    cycle_t c = run_cycle(&interlock, &controller, &faulted);
    check_idle(&c);
    CHECK(c.output.fault == JJ_FAULT_OVERTEMPERATURE);
    CHECK(controller.pid.integral == 0.0f);

    /* Condition gone, MANUAL still has a target: the latch keeps it IDLE. */
    for (int n = 0; n < 20; ++n) {
        c = run_cycle(&interlock, &controller, &input);
        check_idle(&c);
        CHECK(c.output.block_reason == JJ_BLOCK_FAULT_LATCHED);
        CHECK(c.output.controller_disposition == JJ_DISPOSITION_SUSPEND);
        CHECK(controller.pid.integral == 0.0f);
        CHECK(!controller.pid.initialized);
    }

    input.overtemperature_reset_proven = true;
    CHECK(!jj_interlock_clear_fault(&interlock, &input)); /* not OFF */
    jj_inputs_t off = input;
    off.mode = JJ_MODE_OFF;
    CHECK(jj_interlock_clear_fault(&interlock, &off));
    c = run_cycle(&interlock, &controller, &off);
    check_idle(&c);
    c = run_cycle(&interlock, &controller, &input);
    CHECK(c.output.controller_state == JJ_CONTROLLER_VALID);
    CHECK(c.output.heater_authorized);
}

static void test_category_b_inhibits_suspend_and_reset(void)
{
    jj_interlock_t interlock;
    jj_controller_t controller;
    setup(&interlock, &controller, fixture_config());
    jj_inputs_t input = manual_inputs(40.0f);
    for (int n = 0; n < 4; ++n) (void)run_cycle(&interlock, &controller, &input);
    CHECK(controller.pid.integral > 0.0f);

    jj_inputs_t bad_target = input;
    bad_target.manual_target_c = 50.5f;
    cycle_t c = run_cycle(&interlock, &controller, &bad_target);
    check_idle(&c);
    CHECK(c.output.block_reason == JJ_BLOCK_MANUAL_TARGET_INVALID);
    CHECK(controller.pid.integral == 0.0f);

    for (int n = 0; n < 4; ++n) (void)run_cycle(&interlock, &controller, &input);
    CHECK(controller.pid.integral > 0.0f);
    jj_inputs_t uncommissioned = input;
    uncommissioned.commissioned = false;
    c = run_cycle(&interlock, &controller, &uncommissioned);
    check_idle(&c);
    CHECK(c.output.block_reason == JJ_BLOCK_NOT_COMMISSIONED);
    CHECK(controller.pid.integral == 0.0f);
}

static void test_no_target_crossing_override_outside_dc_pid(void)
{
    jj_interlock_t interlock;
    jj_controller_t controller;
    const jj_controller_config_t config = fixture_config();
    setup(&interlock, &controller, config);
    dc_pid_state_t shadow;
    dc_pid_reset(&shadow);

    /* Rise through the target and fall back through it. */
    const float trajectory[] = {38, 40, 42, 44, 44.8f, 45.2f, 45.6f, 45.9f,
                                45.6f, 45.2f, 44.9f, 44.6f};
    bool nonzero_above_target = false;
    for (size_t i = 0; i < sizeof trajectory / sizeof trajectory[0]; ++i) {
        const jj_inputs_t input = manual_inputs(trajectory[i]);
        const cycle_t c = run_cycle(&interlock, &controller, &input);
        dc_pid_result_t expected;
        CHECK(dc_pid_step(&shadow, &config.pid, JJ_MANUAL_TARGET_DEFAULT_C,
                          trajectory[i], config.sample_period_s, true, &expected));
        /* Requested and allowed duty are exactly dc_pid's output, every cycle. */
        CHECK(c.output.requested_duty_pct == expected.output);
        CHECK(c.output.allowed_duty_pct == expected.output);
        CHECK(c.output.heater_authorized);
        if (trajectory[i] > JJ_MANUAL_TARGET_DEFAULT_C && expected.output > 0.0f)
            nonzero_above_target = true;
    }
    CHECK(nonzero_above_target); /* the fixture actually exercises the case */
}

static void test_controller_defers_to_eligibility(void)
{
    /*
     * jj_control has no fan/fault/printer/authority/sensor-status predicates:
     * given RUN it steps even on inputs the interlock would refuse, and given
     * SUSPEND it idles on inputs the interlock would allow.
     */
    jj_controller_t controller;
    const jj_controller_config_t config = fixture_config();
    jj_controller_init(&controller, &config);
    jj_inputs_t hostile = manual_inputs(40.0f);
    hostile.fan_proof = JJ_FAN_PROOF_FAILED;
    hostile.overtemperature_detected = true;
    hostile.commissioned = false;
    hostile.manual_demand_authorized = false;
    hostile.outlet.status = JJ_SENSOR_OPEN;
    const jj_eligibility_t run = {.sequence = 7, .disposition = JJ_DISPOSITION_RUN};
    jj_controller_result_t r = jj_controller_step(&controller, &run, &hostile);
    CHECK(r.stepped && r.report.state == JJ_CONTROLLER_VALID);
    CHECK(r.report.eligibility_sequence == 7);

    const jj_eligibility_t suspend = {.disposition = JJ_DISPOSITION_SUSPEND};
    const jj_inputs_t fine = manual_inputs(40.0f);
    r = jj_controller_step(&controller, &suspend, &fine);
    CHECK(!r.stepped && r.report.state == JJ_CONTROLLER_IDLE);

    const jj_eligibility_t garbage = {.disposition = (jj_controller_disposition_t)9};
    r = jj_controller_step(&controller, &garbage, &fine);
    CHECK(!r.stepped && r.report.state == JJ_CONTROLLER_INVALID);
    CHECK(jj_controller_step(NULL, &run, &fine).report.state ==
          JJ_CONTROLLER_INVALID);
}

static void test_invalid_never_yields_allowed_duty(void)
{
    /* Sweep modes, dispositions, and configs: allowed > 0 only when VALID+RUN. */
    const jj_mode_t modes[] = {JJ_MODE_OFF, JJ_MODE_MANUAL, JJ_MODE_AUTOMATIC};
    const jj_fan_proof_t fans[] = {JJ_FAN_PROOF_PROVEN, JJ_FAN_PROOF_PENDING};
    for (int bad = 0; bad < 2; ++bad)
        for (size_t m = 0; m < 3; ++m)
            for (size_t f = 0; f < 2; ++f) {
                jj_interlock_t interlock;
                jj_controller_t controller;
                jj_controller_config_t config = fixture_config();
                if (bad) config.sample_period_s = -1.0f;
                setup(&interlock, &controller, config);
                jj_inputs_t input = manual_inputs(30.0f);
                input.mode = modes[m];
                input.fan_proof = fans[f];
                const cycle_t c = run_cycle(&interlock, &controller, &input);
                if (c.output.allowed_duty_pct > 0.0f || c.output.heater_authorized) {
                    CHECK(c.output.controller_state == JJ_CONTROLLER_VALID);
                    CHECK(c.output.controller_disposition == JJ_DISPOSITION_RUN);
                }
                if (c.output.controller_state != JJ_CONTROLLER_VALID) check_cold(&c);
            }
}

static void test_target_change_behavior_is_provisional(void)
{
    /*
     * PROVISIONAL: documents current mechanics only. No target-change policy
     * is implemented; dc_pid keeps its integral across a setpoint change.
     * Preserve vs reinitialize is open pending Phase 3 evidence.
     */
    jj_interlock_t interlock;
    jj_controller_t controller;
    setup(&interlock, &controller, fixture_config());
    jj_inputs_t input = manual_inputs(40.0f);
    for (int n = 0; n < 4; ++n) (void)run_cycle(&interlock, &controller, &input);
    const float before = controller.pid.integral;
    input.manual_target_c = 35.0f;
    const cycle_t c = run_cycle(&interlock, &controller, &input);
    CHECK(c.output.controller_state == JJ_CONTROLLER_VALID);
    CHECK(c.controller.pid.d == 0.0f); /* no derivative term, so no kick here */
    CHECK(controller.pid.initialized);
    CHECK(before > 0.0f);
}

int main(void)
{
    test_boot_and_off_are_idle();
    test_off_to_manual_steps_and_authorizes();
    test_provisional_config_is_valid_and_requests_nothing();
    test_rejected_step_is_invalid_and_cold();
    test_automatic_is_idle_before_phase_four();
    test_fan_proof_pending_holds_and_freezes_integral();
    test_fault_latch_keeps_controller_idle_until_safe_clear();
    test_category_b_inhibits_suspend_and_reset();
    test_no_target_crossing_override_outside_dc_pid();
    test_controller_defers_to_eligibility();
    test_invalid_never_yields_allowed_duty();
    test_target_change_behavior_is_provisional();
    puts("jj_control_host_test: PASS");
    return 0;
}
