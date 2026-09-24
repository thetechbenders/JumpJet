#include "jj_authority.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CHECK(condition) do { if (!(condition)) { \
    fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #condition); \
    exit(1); } } while (0)

#define LEASE_A "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"
#define LEASE_B "bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb"

static jj_inputs_t nominal_inputs(void)
{
    return (jj_inputs_t){
        .commissioned = true,
        .mode = JJ_MODE_OFF,
        .manual_target_c = JJ_MANUAL_TARGET_DEFAULT_C,
        .chamber = {.status = JJ_SENSOR_OK, .temperature_c = 30.0f},
        .outlet = {.status = JJ_SENSOR_OK, .temperature_c = 35.0f},
        .case_sensor = {.status = JJ_SENSOR_OK, .temperature_c = 36.0f},
        .printer = {.online = true, .printing = true, .bed_target_c = 100.0f},
        .fan_proof = JJ_FAN_PROOF_PROVEN,
    };
}

static jj_control_snapshot_t acquire(
    jj_authority_t *authority,
    const char *lease,
    const char *owner,
    bool takeover,
    uint64_t now_ms)
{
    jj_control_acquire_t request = {.takeover = takeover};
    snprintf(request.lease_id, sizeof request.lease_id, "%s", lease);
    snprintf(request.owner, sizeof request.owner, "%s", owner);
    jj_control_snapshot_t snapshot;
    CHECK(jj_authority_acquire(authority, &request, now_ms, &snapshot) ==
          JJ_CONTROL_OK);
    return snapshot;
}

static jj_control_snapshot_t mutate(
    jj_authority_t *authority,
    jj_interlock_t *interlock,
    const jj_control_snapshot_t *lease,
    jj_control_mutation_kind_t kind,
    float target_c,
    const char *request_id,
    const jj_inputs_t *inputs,
    uint64_t now_ms)
{
    jj_control_mutation_t request = {
        .generation = lease->generation,
        .expected_revision = lease->revision,
        .kind = kind,
        .target_c = target_c,
    };
    snprintf(request.lease_id, sizeof request.lease_id, "%s", lease->lease_id);
    snprintf(request.request_id, sizeof request.request_id, "%s",
             request_id ? request_id : "");
    jj_control_snapshot_t snapshot;
    CHECK(jj_authority_mutate(authority, interlock, &request, inputs, now_ms,
                              &snapshot) == JJ_CONTROL_OK);
    return snapshot;
}

static jj_control_snapshot_t refresh(
    jj_authority_t *authority,
    const jj_control_snapshot_t *lease,
    uint64_t now_ms)
{
    jj_control_snapshot_t snapshot;
    CHECK(jj_authority_refresh(authority, lease->lease_id, lease->generation,
                               now_ms, &snapshot) == JJ_CONTROL_OK);
    return snapshot;
}

static jj_controller_t s_controller;

/* Staged interlock cycle with the provisional (zero-gain) controller. */
static jj_outputs_t interlock_cycle(jj_interlock_t *interlock,
                                    const jj_inputs_t *inputs)
{
    const jj_eligibility_t eligibility = jj_interlock_evaluate(interlock, inputs);
    const jj_controller_result_t result =
        jj_controller_step(&s_controller, &eligibility, inputs);
    return jj_interlock_authorize(interlock, &result.report);
}

static jj_outputs_t evaluate(
    jj_interlock_t *interlock,
    const jj_control_snapshot_t *authority,
    jj_inputs_t inputs)
{
    jj_authority_apply_to_inputs(authority, &inputs);
    return interlock_cycle(interlock, &inputs);
}

static void test_manual_expiry_fails_cold_without_fault_and_keeps_cooldown(void)
{
    jj_authority_t authority;
    jj_interlock_t interlock;
    jj_authority_init(&authority, &interlock, 1000);
    jj_interlock_init(&interlock);
    jj_inputs_t inputs = nominal_inputs();
    jj_control_snapshot_t lease = acquire(&authority, LEASE_A, "phone-a", false, 0);
    lease = refresh(&authority, &lease, 0);
    jj_control_snapshot_t manual = mutate(
        &authority, &interlock, &lease, JJ_MUTATION_MANUAL, 45.0f,
        "manual-1", &inputs, 1);
    CHECK(evaluate(&interlock, &manual, inputs).heater_authorized);

    CHECK(jj_authority_tick(&authority, 1000));
    jj_outputs_t output = jj_interlock_snapshot(&interlock);
    CHECK(!output.heater_authorized);
    CHECK(output.thermal_state == JJ_THERMAL_COOLDOWN);
    jj_control_snapshot_t expired;
    jj_authority_snapshot(&authority, 1000, &expired);
    CHECK(expired.mode == JJ_MODE_MANUAL);
    CHECK(expired.active_authority == JJ_AUTHORITY_NONE);
    CHECK(expired.control_inhibit == JJ_CONTROL_INHIBIT_NO_AUTHORITY);
    CHECK(!expired.manual_demand_authorized);
    CHECK(expired.last_control_loss_reason ==
          JJ_CONTROL_LOSS_REMOTE_LEASE_EXPIRED);
    CHECK(strcmp(jj_control_loss_reason_str(expired.last_control_loss_reason),
                 "remote_lease_expired") == 0);
    inputs.cooldown_required = true;
    output = evaluate(&interlock, &expired, inputs);
    CHECK(!output.heater_authorized);
    CHECK(output.fault == JJ_FAULT_NONE);
    CHECK(output.control_inhibit == JJ_CONTROL_INHIBIT_NO_AUTHORITY);
    CHECK(output.thermal_state == JJ_THERMAL_COOLDOWN);
    CHECK(output.fan_percent == 100);
}

static void test_reacquire_requires_refresh_and_new_explicit_request(void)
{
    jj_authority_t authority;
    jj_interlock_t interlock;
    jj_authority_init(&authority, &interlock, 1000);
    jj_interlock_init(&interlock);
    jj_inputs_t inputs = nominal_inputs();
    jj_control_snapshot_t first = acquire(&authority, LEASE_A, "phone", false, 0);
    first = refresh(&authority, &first, 0);
    first = mutate(&authority, &interlock, &first, JJ_MUTATION_MANUAL, 44.0f,
                   "manual-a", &inputs, 1);
    CHECK(jj_authority_tick(&authority, 1001));

    jj_control_snapshot_t reacquired = acquire(
        &authority, LEASE_B, "phone", false, 1002);
    CHECK(reacquired.active_authority == JJ_AUTHORITY_REACQUIRING);
    CHECK(reacquired.control_inhibit ==
          JJ_CONTROL_INHIBIT_STATE_REFRESH_REQUIRED);
    CHECK(!reacquired.manual_demand_authorized);
    CHECK(!evaluate(&interlock, &reacquired, inputs).heater_authorized);

    jj_control_mutation_t premature = {
        .generation = reacquired.generation,
        .expected_revision = reacquired.revision,
        .kind = JJ_MUTATION_MANUAL,
        .target_c = 44.0f,
    };
    snprintf(premature.lease_id, sizeof premature.lease_id, "%s", LEASE_B);
    jj_control_snapshot_t rejected;
    CHECK(jj_authority_mutate(&authority, &interlock, &premature, &inputs,
                              1003, &rejected) ==
          JJ_CONTROL_AUTHORITY_REQUIRED);
    CHECK(!rejected.manual_demand_authorized);

    reacquired = refresh(&authority, &reacquired, 1004);
    CHECK(reacquired.active_authority == JJ_AUTHORITY_NONE);
    CHECK(reacquired.control_inhibit == JJ_CONTROL_INHIBIT_NO_AUTHORITY);

    jj_control_snapshot_t resumed = mutate(
        &authority, &interlock, &reacquired, JJ_MUTATION_MANUAL, 44.0f,
        "manual-b", &inputs, 1005);
    CHECK(resumed.active_authority == JJ_AUTHORITY_REMOTE);
    CHECK(resumed.manual_demand_authorized);
    CHECK(evaluate(&interlock, &resumed, inputs).heater_authorized);
}

static void test_stale_generation_and_revision_are_atomic(void)
{
    jj_authority_t authority;
    jj_interlock_t interlock;
    jj_authority_init(&authority, &interlock, 1000);
    jj_interlock_init(&interlock);
    jj_inputs_t inputs = nominal_inputs();
    jj_control_snapshot_t lease = acquire(&authority, LEASE_A, "phone", false, 0);
    lease = refresh(&authority, &lease, 0);
    jj_control_mutation_t request = {
        .generation = lease.generation - 1,
        .expected_revision = lease.revision,
        .kind = JJ_MUTATION_MANUAL,
        .target_c = 45.0f,
    };
    snprintf(request.lease_id, sizeof request.lease_id, "%s", LEASE_A);
    jj_control_snapshot_t rejected;
    CHECK(jj_authority_mutate(&authority, &interlock, &request, &inputs, 1,
                              &rejected) == JJ_CONTROL_GENERATION_STALE);
    CHECK(rejected.mode == JJ_MODE_OFF);
    CHECK(!rejected.manual_demand_authorized);

    request.generation = lease.generation;
    request.expected_revision = lease.revision - 1;
    CHECK(jj_authority_mutate(&authority, &interlock, &request, &inputs, 2,
                              &rejected) == JJ_CONTROL_REVISION_CONFLICT);
    CHECK(rejected.mode == JJ_MODE_OFF);
    CHECK(!rejected.manual_demand_authorized);
}

static void test_explicit_takeover_revokes_old_remote_demand(void)
{
    jj_authority_t authority;
    jj_interlock_t interlock;
    jj_authority_init(&authority, &interlock, 1000);
    jj_interlock_init(&interlock);
    jj_inputs_t inputs = nominal_inputs();
    jj_control_snapshot_t first = acquire(&authority, LEASE_A, "phone-a", false, 0);
    first = refresh(&authority, &first, 0);
    first = mutate(&authority, &interlock, &first, JJ_MUTATION_MANUAL, 45.0f,
                   "manual-a", &inputs, 1);
    CHECK(evaluate(&interlock, &first, inputs).heater_authorized);

    jj_control_acquire_t denied = {.takeover = false};
    snprintf(denied.lease_id, sizeof denied.lease_id, "%s", LEASE_B);
    snprintf(denied.owner, sizeof denied.owner, "%s", "phone-b");
    jj_control_snapshot_t unchanged;
    CHECK(jj_authority_acquire(&authority, &denied, 2, &unchanged) ==
          JJ_CONTROL_AUTHORITY_REQUIRED);
    CHECK(unchanged.manual_demand_authorized);

    jj_control_snapshot_t takeover = acquire(
        &authority, LEASE_B, "phone-b", true, 3);
    CHECK(takeover.generation > first.generation);
    CHECK(!takeover.manual_demand_authorized);
    CHECK(takeover.active_authority == JJ_AUTHORITY_REACQUIRING);
    CHECK(takeover.last_control_loss_reason ==
          JJ_CONTROL_LOSS_EXPLICIT_TAKEOVER);
    CHECK(strcmp(jj_control_loss_reason_str(takeover.last_control_loss_reason),
                 "explicit_takeover") == 0);
    CHECK(!jj_interlock_snapshot(&interlock).heater_authorized);
    CHECK(!evaluate(&interlock, &takeover, inputs).heater_authorized);
    jj_control_snapshot_t heartbeat;
    CHECK(jj_authority_heartbeat(&authority, LEASE_A, first.generation, 4,
                                 &heartbeat) == JJ_CONTROL_GENERATION_STALE);
}

static void test_automatic_survives_browser_loss_but_eligibility_fails_cold(void)
{
    jj_authority_t authority;
    jj_interlock_t interlock;
    jj_authority_init(&authority, &interlock, 1000);
    jj_interlock_init(&interlock);
    jj_inputs_t inputs = nominal_inputs();
    inputs.automatic_target_available = true;
    inputs.automatic_target_c = 42.0f;
    jj_control_snapshot_t lease = acquire(&authority, LEASE_A, "phone", false, 0);
    lease = refresh(&authority, &lease, 0);
    jj_control_snapshot_t automatic = mutate(
        &authority, &interlock, &lease, JJ_MUTATION_AUTOMATIC, 0.0f,
        "auto-a", &inputs, 1);
    jj_outputs_t output = evaluate(&interlock, &automatic, inputs);
    CHECK(!output.heater_authorized);
    CHECK(output.block_reason == JJ_BLOCK_AUTO_POLICY_UNAVAILABLE);

    CHECK(jj_authority_tick(&authority, 1000));
    jj_control_snapshot_t expired;
    jj_authority_snapshot(&authority, 1000, &expired);
    CHECK(expired.mode == JJ_MODE_AUTOMATIC);
    CHECK(expired.active_authority == JJ_AUTHORITY_AUTOMATIC);
    output = evaluate(&interlock, &expired, inputs);
    CHECK(!output.heater_authorized);
    CHECK(output.block_reason == JJ_BLOCK_AUTO_POLICY_UNAVAILABLE);

    inputs.printer.online = false;
    output = evaluate(&interlock, &expired, inputs);
    CHECK(!output.heater_authorized);
    CHECK(output.fault == JJ_FAULT_NONE);
    CHECK(output.block_reason == JJ_BLOCK_PRINTER_UNAVAILABLE);
    CHECK(output.control_inhibit == JJ_CONTROL_INHIBIT_NOT_ELIGIBLE);
    inputs.printer.online = true;
    inputs.printer.printing = false;
    output = evaluate(&interlock, &expired, inputs);
    CHECK(!output.heater_authorized);
    CHECK(output.block_reason == JJ_BLOCK_PRINTER_NOT_PRINTING);
    inputs.printer.printing = true;
    inputs.automatic_target_available = false;
    output = evaluate(&interlock, &expired, inputs);
    CHECK(!output.heater_authorized);
    CHECK(output.block_reason == JJ_BLOCK_AUTO_POLICY_UNAVAILABLE);
    inputs.automatic_target_available = true;
    inputs.fan_proof = JJ_FAN_PROOF_PENDING;
    output = evaluate(&interlock, &expired, inputs);
    CHECK(!output.heater_authorized);
    CHECK(output.block_reason == JJ_BLOCK_FAN_PROOF_PENDING);
}

static void test_automatic_transition_requires_current_product_eligibility(void)
{
    jj_authority_t authority;
    jj_interlock_t interlock;
    jj_authority_init(&authority, &interlock, 1000);
    jj_interlock_init(&interlock);
    jj_inputs_t inputs = nominal_inputs();
    jj_control_snapshot_t lease = acquire(&authority, LEASE_A, "phone", false, 0);
    lease = refresh(&authority, &lease, 0);
    jj_control_mutation_t request = {
        .generation = lease.generation,
        .expected_revision = lease.revision,
        .kind = JJ_MUTATION_AUTOMATIC,
    };
    snprintf(request.lease_id, sizeof request.lease_id, "%s", LEASE_A);
    jj_control_snapshot_t rejected;
    CHECK(jj_authority_mutate(&authority, &interlock, &request, &inputs, 1,
                              &rejected) == JJ_CONTROL_REQUEST_INELIGIBLE);
    CHECK(rejected.mode == JJ_MODE_OFF);
    inputs.automatic_target_available = true;
    inputs.automatic_target_c = 42.0f;
    inputs.printer.printing = false;
    request.expected_revision = rejected.revision;
    CHECK(jj_authority_mutate(&authority, &interlock, &request, &inputs, 2,
                              &rejected) == JJ_CONTROL_REQUEST_INELIGIBLE);
    CHECK(rejected.mode == JJ_MODE_OFF);
}

static void test_faults_are_not_cleared_by_authority_and_client_cannot_bypass(void)
{
    jj_authority_t authority;
    jj_interlock_t interlock;
    jj_authority_init(&authority, &interlock, 1000);
    jj_interlock_init(&interlock);
    jj_inputs_t inputs = nominal_inputs();
    jj_inputs_t faulted = inputs;
    faulted.chamber.status = JJ_SENSOR_OPEN;
    CHECK(interlock_cycle(&interlock, &faulted).fault == JJ_FAULT_SENSOR);

    jj_control_snapshot_t lease = acquire(&authority, LEASE_A, "phone", false, 0);
    lease = refresh(&authority, &lease, 0);
    jj_control_mutation_t request = {
        .generation = lease.generation,
        .expected_revision = lease.revision,
        .kind = JJ_MUTATION_MANUAL,
        .target_c = 45.0f,
    };
    snprintf(request.lease_id, sizeof request.lease_id, "%s", LEASE_A);
    jj_control_snapshot_t rejected;
    CHECK(jj_authority_mutate(&authority, &interlock, &request, &inputs, 1,
                              &rejected) == JJ_CONTROL_HARDWARE_FAULT);
    CHECK(jj_interlock_snapshot(&interlock).fault == JJ_FAULT_SENSOR);

    jj_interlock_init(&interlock);
    inputs.chamber.status = JJ_SENSOR_OPEN;
    lease = acquire(&authority, LEASE_B, "phone", false, 2);
    lease = refresh(&authority, &lease, 2);
    request.generation = lease.generation;
    request.expected_revision = lease.revision;
    snprintf(request.lease_id, sizeof request.lease_id, "%s", LEASE_B);
    CHECK(jj_authority_mutate(&authority, &interlock, &request, &inputs, 3,
                              &rejected) == JJ_CONTROL_REQUEST_INELIGIBLE);
    CHECK(rejected.mode == JJ_MODE_OFF);
    CHECK(!rejected.manual_demand_authorized);
}

static void test_request_uuid_retry_is_exactly_once_and_content_bound(void)
{
    jj_authority_t authority;
    jj_interlock_t interlock;
    jj_authority_init(&authority, &interlock, 1000);
    jj_interlock_init(&interlock);
    jj_inputs_t inputs = nominal_inputs();
    jj_control_snapshot_t lease = acquire(&authority, LEASE_A, "phone", false, 0);
    lease = refresh(&authority, &lease, 0);
    jj_control_mutation_t request = {
        .generation = lease.generation,
        .expected_revision = lease.revision,
        .kind = JJ_MUTATION_MANUAL,
        .target_c = 45.0f,
    };
    snprintf(request.lease_id, sizeof request.lease_id, "%s", LEASE_A);
    snprintf(request.request_id, sizeof request.request_id, "%s", "same-request");
    jj_control_snapshot_t first;
    CHECK(jj_authority_mutate(&authority, &interlock, &request, &inputs, 1,
                              &first) == JJ_CONTROL_OK);
    jj_control_snapshot_t retry;
    CHECK(jj_authority_mutate(&authority, &interlock, &request, &inputs, 2,
                              &retry) == JJ_CONTROL_OK);
    CHECK(retry.revision == first.revision);
    jj_control_mutation_t intervening = request;
    snprintf(intervening.request_id, sizeof intervening.request_id, "%s",
             "different-request");
    intervening.expected_revision = first.revision;
    intervening.kind = JJ_MUTATION_OFF;
    jj_control_snapshot_t off;
    CHECK(jj_authority_mutate(&authority, &interlock, &intervening, &inputs, 3,
                              &off) == JJ_CONTROL_OK);
    request.target_c = 45.0f;
    CHECK(jj_authority_mutate(&authority, &interlock, &request, &inputs, 4,
                              &retry) == JJ_CONTROL_OK);
    CHECK(retry.revision == off.revision);
    CHECK(retry.mode == JJ_MODE_OFF);
    request.target_c = 46.0f;
    CHECK(jj_authority_mutate(&authority, &interlock, &request, &inputs, 5,
                              &retry) == JJ_CONTROL_REQUEST_CONFLICT);
    CHECK(off.mode == JJ_MODE_OFF);
}

static void test_retry_after_expiry_never_replays_historical_manual_demand(void)
{
    jj_authority_t authority;
    jj_interlock_t interlock;
    jj_authority_init(&authority, &interlock, 1000);
    jj_interlock_init(&interlock);
    jj_inputs_t inputs = nominal_inputs();
    jj_control_snapshot_t lease = acquire(&authority, LEASE_A, "phone", false, 0);
    lease = refresh(&authority, &lease, 0);
    jj_control_mutation_t request = {
        .generation = lease.generation,
        .expected_revision = lease.revision,
        .kind = JJ_MUTATION_MANUAL,
        .target_c = 45.0f,
    };
    snprintf(request.lease_id, sizeof request.lease_id, "%s", LEASE_A);
    snprintf(request.request_id, sizeof request.request_id, "%s", "lost-response");
    jj_control_snapshot_t accepted;
    CHECK(jj_authority_mutate(&authority, &interlock, &request, &inputs, 1,
                              &accepted) == JJ_CONTROL_OK);
    CHECK(accepted.manual_demand_authorized);
    jj_control_snapshot_t retry;
    CHECK(jj_authority_mutate(&authority, &interlock, &request, &inputs, 1001,
                              &retry) == JJ_CONTROL_OK);
    CHECK(!retry.lease_active);
    CHECK(!retry.manual_demand_authorized);
    CHECK(retry.active_authority == JJ_AUTHORITY_NONE);
    CHECK(!evaluate(&interlock, &retry, inputs).heater_authorized);
}

static void test_directional_authority_sampling(void)
{
    jj_authority_t authority;
    jj_interlock_t interlock;
    jj_authority_init(&authority, &interlock, 1000);
    jj_interlock_init(&interlock);
    jj_inputs_t cached_prusa_inputs = nominal_inputs();
    jj_control_snapshot_t lease = acquire(&authority, LEASE_A, "phone-a", false, 0);
    lease = refresh(&authority, &lease, 0);
    jj_control_snapshot_t manual = mutate(
        &authority, &interlock, &lease, JJ_MUTATION_MANUAL, 45.0f,
        "directional-manual", &cached_prusa_inputs, 1);

    /* Grants do not rewrite the current interlock decision. */
    CHECK(!jj_interlock_snapshot(&interlock).heater_authorized);
    CHECK(jj_authority_control_step(
        &authority, &s_controller, &cached_prusa_inputs, 2, &manual).heater_authorized);

    /* A takeover after the cached Prusa read revokes that decision now. */
    jj_control_snapshot_t takeover = acquire(
        &authority, LEASE_B, "phone-b", true, 3);
    CHECK(!takeover.manual_demand_authorized);
    CHECK(!jj_interlock_snapshot(&interlock).heater_authorized);
    CHECK(!jj_authority_control_step(
        &authority, &s_controller, &cached_prusa_inputs, 3, NULL).heater_authorized);

    takeover = refresh(&authority, &takeover, 4);
    jj_control_snapshot_t second_manual = mutate(
        &authority, &interlock, &takeover, JJ_MUTATION_MANUAL, 45.0f,
        "directional-second-manual", &cached_prusa_inputs, 5);
    CHECK(!jj_interlock_snapshot(&interlock).heater_authorized);
    CHECK(jj_authority_control_step(
        &authority, &s_controller, &cached_prusa_inputs, 5, NULL).heater_authorized);

    jj_control_snapshot_t reacquiring = acquire(
        &authority, LEASE_A, "phone-b", false, 6);
    CHECK(reacquiring.last_control_loss_reason ==
          JJ_CONTROL_LOSS_REMOTE_REACQUIRED);
    CHECK(strcmp(jj_control_loss_reason_str(
                     reacquiring.last_control_loss_reason),
                 "remote_reacquired") == 0);
    CHECK(!jj_interlock_snapshot(&interlock).heater_authorized);

    reacquiring = refresh(&authority, &reacquiring, 7);
    second_manual = mutate(
        &authority, &interlock, &reacquiring, JJ_MUTATION_MANUAL, 45.0f,
        "directional-third-manual", &cached_prusa_inputs, 8);
    CHECK(jj_authority_control_step(
        &authority, &s_controller, &cached_prusa_inputs, 8, NULL).heater_authorized);
    jj_control_snapshot_t off = mutate(
        &authority, &interlock, &second_manual, JJ_MUTATION_OFF, 0.0f,
        "directional-off", &cached_prusa_inputs, 9);
    CHECK(off.mode == JJ_MODE_OFF);
    CHECK(!jj_interlock_snapshot(&interlock).heater_authorized);
}

static void test_evicted_retry_reports_revision_conflict_with_current_state(void)
{
    jj_authority_t authority;
    jj_interlock_t interlock;
    jj_authority_init(&authority, &interlock, 10000);
    jj_interlock_init(&interlock);
    jj_inputs_t inputs = nominal_inputs();
    jj_control_snapshot_t lease = acquire(&authority, LEASE_A, "phone", false, 0);
    lease = refresh(&authority, &lease, 0);
    jj_control_mutation_t original = {
        .generation = lease.generation,
        .expected_revision = lease.revision,
        .kind = JJ_MUTATION_MANUAL,
        .target_c = 45.0f,
    };
    snprintf(original.lease_id, sizeof original.lease_id, "%s", LEASE_A);
    snprintf(original.request_id, sizeof original.request_id, "%s", "original");
    jj_control_snapshot_t current;
    CHECK(jj_authority_mutate(&authority, &interlock, &original, &inputs, 1,
                              &current) == JJ_CONTROL_OK);

    for (size_t i = 0; i < JJ_CONTROL_REQUEST_CACHE_SIZE; ++i) {
        jj_control_mutation_t next = {
            .generation = current.generation,
            .expected_revision = current.revision,
            .kind = i % 2 == 0 ? JJ_MUTATION_OFF : JJ_MUTATION_MANUAL,
            .target_c = 45.0f,
        };
        snprintf(next.lease_id, sizeof next.lease_id, "%s", LEASE_A);
        snprintf(next.request_id, sizeof next.request_id, "evict-%zu", i);
        CHECK(jj_authority_mutate(&authority, &interlock, &next, &inputs,
                                  2 + i, &current) == JJ_CONTROL_OK);
    }

    jj_control_snapshot_t retry_state;
    CHECK(jj_authority_mutate(&authority, &interlock, &original, &inputs, 20,
                              &retry_state) == JJ_CONTROL_REVISION_CONFLICT);
    CHECK(retry_state.revision == current.revision);
    CHECK(retry_state.mode == current.mode);
}

static void test_remote_clear_obeys_fault_class_policy(void)
{
    jj_authority_t authority;
    jj_interlock_t interlock;
    jj_authority_init(&authority, &interlock, 1000);
    jj_interlock_init(&interlock);
    jj_inputs_t inputs = nominal_inputs();
    jj_control_snapshot_t lease = acquire(&authority, LEASE_A, "phone", false, 0);
    lease = refresh(&authority, &lease, 0);
    jj_control_mutation_t clear = {
        .generation = lease.generation,
        .expected_revision = lease.revision,
        .kind = JJ_MUTATION_CLEAR_FAULT,
    };
    snprintf(clear.lease_id, sizeof clear.lease_id, "%s", LEASE_A);

    interlock.fault_latched = JJ_FAULT_FAN;
    inputs.fan_proof = JJ_FAN_PROOF_PENDING;
    jj_control_snapshot_t result;
    CHECK(jj_authority_mutate(&authority, &interlock, &clear, &inputs, 1,
                              &result) == JJ_CONTROL_HARDWARE_FAULT);
    CHECK(interlock.fault_latched == JJ_FAULT_FAN);
    inputs.fan_proof = JJ_FAN_PROOF_PROVEN;
    CHECK(jj_authority_mutate(&authority, &interlock, &clear, &inputs, 2,
                              &result) == JJ_CONTROL_OK);
    CHECK(interlock.fault_latched == JJ_FAULT_NONE);

    interlock.fault_latched = JJ_FAULT_UNCONTROLLED_RISE;
    clear.expected_revision = result.revision;
    inputs.overtemperature_reset_proven = true;
    inputs.no_heat_revalidation_proven = true;
    inputs.reset_revalidation_proven = true;
    CHECK(jj_authority_mutate(&authority, &interlock, &clear, &inputs, 3,
                              &result) == JJ_CONTROL_HARDWARE_FAULT);
    CHECK(interlock.fault_latched == JJ_FAULT_UNCONTROLLED_RISE);
}


static void test_control_step_without_controller_fails_cold(void)
{
    jj_authority_t authority;
    jj_interlock_t interlock;
    jj_authority_init(&authority, &interlock, 1000);
    jj_interlock_init(&interlock);
    jj_inputs_t inputs = nominal_inputs();
    jj_control_snapshot_t lease = acquire(&authority, LEASE_A, "phone-a", false, 0);
    lease = refresh(&authority, &lease, 0);
    (void)mutate(&authority, &interlock, &lease, JJ_MUTATION_MANUAL, 45.0f,
                 "null-controller", &inputs, 1);
    CHECK(jj_authority_control_step(&authority, &s_controller, &inputs, 2, NULL)
              .heater_authorized);
    const jj_outputs_t output =
        jj_authority_control_step(&authority, NULL, &inputs, 3, NULL);
    CHECK(!output.heater_authorized);
    CHECK(output.allowed_duty_pct == 0.0f);
    CHECK(output.block_reason == JJ_BLOCK_CONTROL_SEQUENCE_INVALID);
    CHECK(!jj_interlock_snapshot(&interlock).heater_authorized);
}

int main(void)
{
    const jj_controller_config_t config = jj_controller_provisional_config();
    jj_controller_init(&s_controller, &config);
    test_manual_expiry_fails_cold_without_fault_and_keeps_cooldown();
    test_reacquire_requires_refresh_and_new_explicit_request();
    test_stale_generation_and_revision_are_atomic();
    test_explicit_takeover_revokes_old_remote_demand();
    test_automatic_survives_browser_loss_but_eligibility_fails_cold();
    test_automatic_transition_requires_current_product_eligibility();
    test_faults_are_not_cleared_by_authority_and_client_cannot_bypass();
    test_request_uuid_retry_is_exactly_once_and_content_bound();
    test_retry_after_expiry_never_replays_historical_manual_demand();
    test_directional_authority_sampling();
    test_evicted_retry_reports_revision_conflict_with_current_state();
    test_remote_clear_obeys_fault_class_policy();
    test_control_step_without_controller_fails_cold();
    puts("jj_authority_host_test: PASS");
    return 0;
}
