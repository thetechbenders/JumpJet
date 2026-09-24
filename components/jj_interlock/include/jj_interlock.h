// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <stdbool.h>
#include <stdint.h>

#define JJ_MANUAL_TARGET_MIN_C     30.0f
#define JJ_MANUAL_TARGET_DEFAULT_C 45.0f
#define JJ_MANUAL_TARGET_MAX_C     50.0f

typedef enum { JJ_MODE_OFF = 0, JJ_MODE_MANUAL, JJ_MODE_AUTOMATIC } jj_mode_t;
typedef enum {
    JJ_AUTHORITY_NONE = 0,
    JJ_AUTHORITY_REMOTE,
    JJ_AUTHORITY_AUTOMATIC,
    JJ_AUTHORITY_REACQUIRING,
} jj_control_authority_t;
typedef enum {
    JJ_CONTROL_INHIBIT_NONE = 0,
    JJ_CONTROL_INHIBIT_NO_AUTHORITY,
    JJ_CONTROL_INHIBIT_STATE_REFRESH_REQUIRED,
    JJ_CONTROL_INHIBIT_NOT_ELIGIBLE,
} jj_control_inhibit_t;
typedef enum {
    JJ_THERMAL_IDLE = 0,
    JJ_THERMAL_HEATING,
    JJ_THERMAL_COOLDOWN,
} jj_thermal_state_t;
typedef enum {
    JJ_SENSOR_UNAVAILABLE = 0, JJ_SENSOR_OK, JJ_SENSOR_OPEN,
    JJ_SENSOR_SHORT, JJ_SENSOR_IMPLAUSIBLE,
} jj_sensor_status_t;
typedef enum {
    JJ_FAN_PROOF_UNAVAILABLE = 0, JJ_FAN_PROOF_PENDING,
    JJ_FAN_PROOF_PROVEN, JJ_FAN_PROOF_FAILED,
} jj_fan_proof_t;
typedef enum {
    JJ_FAULT_NONE = 0, JJ_FAULT_SENSOR, JJ_FAULT_OVERTEMPERATURE,
    JJ_FAULT_FAN, JJ_FAULT_NO_HEAT, JJ_FAULT_UNCONTROLLED_RISE,
    JJ_FAULT_CONFIG, JJ_FAULT_STUCK_ON, JJ_FAULT_COMMANDED_OFF_PROOF,
    JJ_FAULT_WATCHDOG_RESET, JJ_FAULT_UNEXPECTED_RESET,
    JJ_FAULT_BROWNOUT_RESET,
} jj_fault_t;
typedef enum {
    JJ_REMOTE_ACK_NEVER = 0,
    JJ_REMOTE_ACK_WHEN_HEALTHY,
    JJ_REMOTE_ACK_AFTER_REVALIDATION,
} jj_remote_ack_policy_t;
typedef enum {
    JJ_BLOCK_NONE = 0, JJ_BLOCK_OFF, JJ_BLOCK_NOT_COMMISSIONED,
    JJ_BLOCK_FAULT_LATCHED, JJ_BLOCK_PRINTER_UNAVAILABLE,
    JJ_BLOCK_PRINTER_NOT_PRINTING, JJ_BLOCK_AUTO_POLICY_UNAVAILABLE,
    JJ_BLOCK_MANUAL_TARGET_INVALID, JJ_BLOCK_FAN_PROOF_PENDING,
    JJ_BLOCK_INVALID_MODE, JJ_BLOCK_CONTROL_NO_AUTHORITY,
    JJ_BLOCK_STATE_REFRESH_REQUIRED, JJ_BLOCK_AUTO_AUTHORITY_UNAVAILABLE,
    /* Stage-2 denials: produced only by jj_interlock_authorize(). */
    JJ_BLOCK_CONTROLLER_INVALID, JJ_BLOCK_CONTROL_SEQUENCE_INVALID,
    JJ_BLOCK_REASON_COUNT, /* sentinel; not a reason */
} jj_block_reason_t;
_Static_assert(JJ_BLOCK_REASON_COUNT <= 32, "inhibit_mask is 32 bits wide");

/*
 * Controller lifecycle. IDLE is an ordinary state (no step was attempted);
 * INVALID means a step was expected but the controller could not produce a
 * valid request. Zero values are the safe defaults.
 */
typedef enum {
    JJ_CONTROLLER_IDLE = 0,
    JJ_CONTROLLER_VALID,
    JJ_CONTROLLER_INVALID,
} jj_controller_state_t;

/*
 * What the controller may do this cycle. Numeric order is precedence:
 * SUSPEND (most restrictive) < HOLD < RUN, so aggregation takes the minimum.
 *   SUSPEND: do not step; controller IDLE; controller state reset.
 *   HOLD:    step with integration frozen; state preserved; heat not allowed.
 *   RUN:     step with integration enabled; heat may be authorized in Stage 2.
 */
typedef enum {
    JJ_DISPOSITION_SUSPEND = 0,
    JJ_DISPOSITION_HOLD,
    JJ_DISPOSITION_RUN,
} jj_controller_disposition_t;

typedef struct { jj_sensor_status_t status; float temperature_c; } jj_sensor_sample_t;
typedef struct {
    bool online;
    bool printing; /* True only for dc_prusa's exact PRINTING state. */
    float bed_target_c;
} jj_printer_sample_t;
typedef struct {
    bool commissioned;
    jj_mode_t mode;
    float manual_target_c;
    jj_control_authority_t active_authority;
    jj_control_inhibit_t control_inhibit;
    bool manual_demand_authorized;
    bool automatic_target_available;
    float automatic_target_c;
    jj_sensor_sample_t chamber;
    jj_sensor_sample_t outlet;
    jj_sensor_sample_t case_sensor;
    bool overtemperature_detected;
    bool overtemperature_reset_proven;
    bool cooldown_required;
    bool fault_requires_thermal_management;
    bool no_heat_revalidation_proven;
    bool reset_revalidation_proven;
    jj_printer_sample_t printer;
    jj_fan_proof_t fan_proof;
} jj_inputs_t;
/*
 * Stage-1 result. Produced only by jj_interlock_evaluate(); the controller
 * reads it but Stage 2 never trusts a caller's copy of it.
 */
typedef struct {
    uint32_t sequence;                   /* interlock-issued evaluation token */
    jj_block_reason_t dominant_reason;   /* diagnostic/HMI priority only */
    uint32_t inhibit_mask;               /* 1u << reason for each inhibit found */
    jj_controller_disposition_t disposition; /* aggregate over all inhibits */
} jj_eligibility_t;

/* The only controller data Stage 2 consumes. Treated as untrusted input. */
typedef struct {
    uint32_t eligibility_sequence;       /* copied from the jj_eligibility_t used */
    jj_controller_state_t state;
    float requested_duty_pct;            /* raw controller request; 0 unless VALID */
} jj_controller_report_t;

typedef struct {
    uint8_t fan_percent;
    float effective_target_c;
    bool thermal_management_required;
    jj_control_authority_t active_authority;
    jj_control_inhibit_t control_inhibit;
    jj_thermal_state_t thermal_state;
    jj_fault_t fault;
    jj_block_reason_t block_reason;
    jj_controller_disposition_t controller_disposition;
    jj_controller_state_t controller_state;
    float requested_duty_pct;  /* raw controller request, never safety-modified */
    bool heater_authorized;    /* safety authorization, independent of magnitude */
    float allowed_duty_pct;    /* 0 unless heater_authorized */
} jj_outputs_t;
typedef struct {
    jj_fault_t fault_latched;
    jj_outputs_t last_output;
    /* Stage-1 evaluation awaiting its single Stage-2 consumption. */
    bool pending_valid;
    uint32_t next_sequence;
    jj_eligibility_t pending;
    jj_outputs_t pending_output;
} jj_interlock_t;

jj_inputs_t jj_inputs_safe_defaults(void);
void jj_interlock_init(jj_interlock_t *state);
/*
 * Staged evaluation. Both stages run inside one control step. Stage 1 publishes
 * a de-authorized snapshot immediately; Stage 2 consumes that exact evaluation
 * once. Revocation, fault clear, or a newer Stage 1 invalidates it.
 */
jj_eligibility_t jj_interlock_evaluate(jj_interlock_t *state, const jj_inputs_t *input);
jj_outputs_t jj_interlock_authorize(jj_interlock_t *state,
                                    const jj_controller_report_t *report);
jj_controller_disposition_t jj_block_reason_disposition(jj_block_reason_t reason);
bool jj_interlock_clear_fault(jj_interlock_t *state, const jj_inputs_t *input);
void jj_interlock_remove_remote_authorization(
    jj_interlock_t *state,
    jj_control_authority_t authority,
    jj_control_inhibit_t inhibit,
    jj_block_reason_t reason);
jj_outputs_t jj_interlock_snapshot(const jj_interlock_t *state);
jj_remote_ack_policy_t jj_fault_remote_ack_policy(jj_fault_t fault);
const char *jj_fault_str(jj_fault_t fault);
const char *jj_block_reason_str(jj_block_reason_t reason);
const char *jj_control_authority_str(jj_control_authority_t authority);
const char *jj_control_inhibit_str(jj_control_inhibit_t inhibit);
const char *jj_thermal_state_str(jj_thermal_state_t state);
const char *jj_controller_state_str(jj_controller_state_t state);
const char *jj_controller_disposition_str(jj_controller_disposition_t disposition);
