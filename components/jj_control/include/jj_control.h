// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "dc_pid.h"
#include "jj_interlock.h"
#include <stdbool.h>

/*
 * Jump Jet product control policy around dc_pid.
 *
 * Owns setpoint resolution, dc_pid state/config lifecycle, integrate
 * decisions, and the raw requested duty. Owns no safety eligibility,
 * authorization, fault handling, limiting, or actuation: it consumes the
 * interlock's Stage-1 jj_eligibility_t and reports back through
 * jj_controller_report_t for Stage 2.
 */

typedef struct {
    dc_pid_config_t pid;
    float sample_period_s; /* dt passed to dc_pid_step() each control step */
} jj_controller_config_t;

typedef struct {
    jj_controller_config_t config;
    dc_pid_state_t pid;
} jj_controller_t;

typedef struct {
    jj_controller_report_t report; /* the only part Stage 2 consumes */
    /* Advanced diagnostics; not primary HMI. Zeroed unless a step ran. */
    dc_pid_result_t pid;
    bool stepped;
    bool integrating;
    float setpoint_c;              /* NAN when no setpoint applies */
} jj_controller_result_t;

/*
 * PROVISIONAL, UNCHARACTERIZED. Zero gains: a step produces a valid 0 % request.
 * Output/integral bounds are the 0..100 % duty unit, not tuning. The sample
 * period mirrors the existing 250 ms control-task cadence and is not an
 * approved controller interval. Replace only from Phase 3 plant evidence.
 */
jj_controller_config_t jj_controller_provisional_config(void);

void jj_controller_init(jj_controller_t *controller,
                        const jj_controller_config_t *config);

jj_controller_result_t jj_controller_step(jj_controller_t *controller,
                                          const jj_eligibility_t *eligibility,
                                          const jj_inputs_t *input);
