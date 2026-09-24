// SPDX-License-Identifier: GPL-3.0-or-later
#include "jj_control.h"
#include <math.h>
#include <string.h>

/*
 * This unit runs inside jj_authority's critical section: bounded work only.
 * It must not test fan proof, faults, printer state, authority, sensor status,
 * or target validity. Those predicates live only in jj_interlock.
 */

jj_controller_config_t jj_controller_provisional_config(void)
{
    return (jj_controller_config_t){
        .pid = {
            .kp = 0.0f, .ki = 0.0f, .kd = 0.0f, /* PROVISIONAL: untuned */
            .derivative_alpha = 0.0f,
            .output_min = 0.0f, .output_max = 100.0f,
            .integral_min = 0.0f, .integral_max = 100.0f,
        },
        .sample_period_s = 0.25f, /* PROVISIONAL: inherited task cadence */
    };
}

void jj_controller_init(jj_controller_t *controller,
                        const jj_controller_config_t *config)
{
    if (!controller) return;
    memset(controller, 0, sizeof(*controller));
    if (config) controller->config = *config;
    dc_pid_reset(&controller->pid);
}

/* Setpoint resolution is product policy owned here, from mode alone. */
static bool resolve_setpoint(const jj_inputs_t *input, float *setpoint_c)
{
    switch (input->mode) {
    case JJ_MODE_MANUAL:
        *setpoint_c = input->manual_target_c;
        return true;
    case JJ_MODE_OFF:
    case JJ_MODE_AUTOMATIC: /* no Phase-4 chamber-target policy exists */
        return false;
    }
    return false;
}

jj_controller_result_t jj_controller_step(jj_controller_t *controller,
                                          const jj_eligibility_t *eligibility,
                                          const jj_inputs_t *input)
{
    jj_controller_result_t result = {
        .report = {.state = JJ_CONTROLLER_IDLE, .requested_duty_pct = 0.0f},
        .setpoint_c = NAN,
    };
    if (!controller || !eligibility || !input) {
        result.report.state = JJ_CONTROLLER_INVALID;
        return result;
    }
    result.report.eligibility_sequence = eligibility->sequence;

    bool known = false, integrate = false;
    switch (eligibility->disposition) { /* exhaustive: no default */
    case JJ_DISPOSITION_SUSPEND:
        /* IDLE for as long as suspension lasts; never resume on stale state. */
        dc_pid_reset(&controller->pid);
        return result;
    case JJ_DISPOSITION_HOLD:
        known = true; /* preserve state, freeze integral */
        break;
    case JJ_DISPOSITION_RUN:
        known = true;
        integrate = true;
        break;
    }
    if (!known) {
        result.report.state = JJ_CONTROLLER_INVALID;
        return result;
    }

    float setpoint_c;
    if (!resolve_setpoint(input, &setpoint_c)) {
        dc_pid_reset(&controller->pid);
        return result; /* IDLE: no applicable setpoint */
    }
    result.setpoint_c = setpoint_c;

    /*
     * PROVISIONAL: no special handling of target changes. dc_pid's
     * derivative-on-measurement avoids setpoint kick; whether the integral
     * should be preserved or reinitialized across a target change is open
     * pending Phase 3. dc_pid output is used unmodified: no target-crossing
     * override exists here.
     */
    result.stepped = true;
    if (dc_pid_step(&controller->pid, &controller->config.pid, setpoint_c,
                    input->chamber.temperature_c,
                    controller->config.sample_period_s, integrate,
                    &result.pid)) {
        result.report.state = JJ_CONTROLLER_VALID;
        result.report.requested_duty_pct = result.pid.output;
        result.integrating = integrate;
    } else {
        result.report.state = JJ_CONTROLLER_INVALID;
        result.report.requested_duty_pct = 0.0f;
    }
    return result;
}
