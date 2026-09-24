// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "jj_control.h"
#include "jj_interlock.h"
#include <stdbool.h>
#include <stdint.h>

#define JJ_CONTROL_LEASE_ID_LEN 32
#define JJ_CONTROL_OWNER_LEN 31
#define JJ_CONTROL_REQUEST_ID_LEN 63
#define JJ_CONTROL_REQUEST_CACHE_SIZE 8
#define JJ_REMOTE_LEASE_TTL_MS 60000U

typedef enum {
    JJ_CONTROL_OK = 0,
    JJ_CONTROL_AUTHORITY_REQUIRED,
    JJ_CONTROL_AUTHORITY_LOST,
    JJ_CONTROL_GENERATION_STALE,
    JJ_CONTROL_REVISION_CONFLICT,
    JJ_CONTROL_REQUEST_INELIGIBLE,
    JJ_CONTROL_REQUEST_CONFLICT,
    JJ_CONTROL_HARDWARE_FAULT,
    JJ_CONTROL_INVALID,
} jj_control_result_t;

typedef enum {
    JJ_MUTATION_OFF = 0,
    JJ_MUTATION_MANUAL,
    JJ_MUTATION_AUTOMATIC,
    JJ_MUTATION_CLEAR_FAULT,
} jj_control_mutation_kind_t;

typedef enum {
    JJ_CONTROL_LOSS_NONE = 0,
    JJ_CONTROL_LOSS_REMOTE_LEASE_EXPIRED,
    JJ_CONTROL_LOSS_EXPLICIT_TAKEOVER,
    JJ_CONTROL_LOSS_REMOTE_REACQUIRED,
} jj_control_loss_reason_t;

typedef struct {
    char lease_id[JJ_CONTROL_LEASE_ID_LEN + 1];
    char owner[JJ_CONTROL_OWNER_LEN + 1];
    bool takeover;
} jj_control_acquire_t;

typedef struct {
    char lease_id[JJ_CONTROL_LEASE_ID_LEN + 1];
    uint32_t generation;
    uint32_t expected_revision;
    char request_id[JJ_CONTROL_REQUEST_ID_LEN + 1];
    jj_control_mutation_kind_t kind;
    float target_c;
} jj_control_mutation_t;

typedef struct {
    uint32_t revision;
    uint32_t generation;
    jj_mode_t mode;
    float configured_target_c;
    bool manual_demand_authorized;
    jj_control_authority_t active_authority;
    jj_control_inhibit_t control_inhibit;
    bool lease_active;
    char lease_id[JJ_CONTROL_LEASE_ID_LEN + 1];
    char lease_owner[JJ_CONTROL_OWNER_LEN + 1];
    uint32_t lease_expires_in_ms;
    uint64_t last_control_loss_ms;
    jj_control_loss_reason_t last_control_loss_reason;
} jj_control_snapshot_t;

typedef struct {
    char request_id[JJ_CONTROL_REQUEST_ID_LEN + 1];
    uint64_t signature;
    jj_control_result_t result;
} jj_control_request_cache_entry_t;

typedef struct {
    jj_control_snapshot_t state;
    uint64_t lease_deadline_ms;
    uint32_t lease_ttl_ms;
    jj_interlock_t *interlock;
    jj_control_request_cache_entry_t request_cache[JJ_CONTROL_REQUEST_CACHE_SIZE];
    uint8_t next_request_cache_entry;
} jj_authority_t;

void jj_authority_init(
    jj_authority_t *state,
    jj_interlock_t *interlock,
    uint32_t lease_ttl_ms);
jj_control_result_t jj_authority_acquire(
    jj_authority_t *state,
    const jj_control_acquire_t *request,
    uint64_t now_ms,
    jj_control_snapshot_t *out);
jj_control_result_t jj_authority_heartbeat(
    jj_authority_t *state,
    const char *lease_id,
    uint32_t generation,
    uint64_t now_ms,
    jj_control_snapshot_t *out);
jj_control_result_t jj_authority_refresh(
    jj_authority_t *state,
    const char *lease_id,
    uint32_t generation,
    uint64_t now_ms,
    jj_control_snapshot_t *out);
jj_control_result_t jj_authority_mutate(
    jj_authority_t *state,
    jj_interlock_t *interlock,
    const jj_control_mutation_t *request,
    const jj_inputs_t *authoritative_inputs,
    uint64_t now_ms,
    jj_control_snapshot_t *out);
bool jj_authority_tick(jj_authority_t *state, uint64_t now_ms);
void jj_authority_snapshot(
    const jj_authority_t *state,
    uint64_t now_ms,
    jj_control_snapshot_t *out);
void jj_authority_apply_to_inputs(
    const jj_control_snapshot_t *authority,
    jj_inputs_t *inputs);
/*
 * One ordered control step under the authority lock: authority -> interlock
 * Stage 1 -> controller -> interlock Stage 2. A NULL controller fails cold.
 */
jj_outputs_t jj_authority_control_step(
    jj_authority_t *state,
    jj_controller_t *controller,
    const jj_inputs_t *authoritative_inputs,
    uint64_t now_ms,
    jj_control_snapshot_t *out);
const char *jj_control_result_str(jj_control_result_t result);
const char *jj_mode_str(jj_mode_t mode);
const char *jj_control_loss_reason_str(jj_control_loss_reason_t reason);
