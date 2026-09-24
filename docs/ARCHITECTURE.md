# Jump Jet firmware architecture

Jump Jet is an ESP-IDF Dragon-family product, not a pin-swapped DragonBreath build.

## Ownership boundary

Pinned `dragon-core` v0.32.0 services provide board-neutral Wi-Fi, portal, UI,
event logging, and read-only PrusaLink status. `dc_prusa` applies its authoritative
15-second freshness behavior; Jump Jet does not run another freshness timer.

Jump Jet owns mode and target settings, AUTOMATIC policy, diagnostics, OTA product
guards, cooldown, recovery, all hardware definitions, sensing, actuation, and
safety decisions. See [PRODUCT_SAFETY_CONTRACT.md](PRODUCT_SAFETY_CONTRACT.md).

## Boot order

1. Initialize logging and NVS.
2. Initialize control authority and the interlock in OFF with physical actuation unavailable.
3. Start the control/safety task.
4. Start Wi-Fi, read-only PrusaLink polling, and the management portal.
5. Wait for a synchronized cold-safe interlock cycle.
6. Verify zero requested heat and zero effective target.
7. Only then mark a pending OTA image valid.

Mode is not persisted. Reset always returns to OFF. The 45 °C retained manual
target is configuration, not permission to heat.

## Control and diagnostics

Operating mode, active authority, control inhibit, latched safety fault, and
thermal handling are independent dimensions. `jj_authority` owns remote leases,
generation/revision checks, explicit takeover, and complete mode/target
mutations. `jj_interlock` independently applies product safety vetoes and owns
the final logical heater permission. Physical delivery remains unavailable.

MANUAL demand requires a live REMOTE lease. Expiry or takeover synchronously
removes that demand and advances the control generation without creating a
hardware fault. The configured mode and target may remain visible, while
cooldown/fault fan requests continue. Acquisition enters REACQUIRING; a separate
authoritative refresh handshake must complete before any mutation can commit,
and a new MANUAL request is required to resume logical heat.

Once a validated transition into AUTOMATIC commits, product-local AUTOMATIC
authority owns demand. The browser lease may expire without changing that
authority. Fresh exact `PRINTING` status and every product safety/policy input
are still re-evaluated on each interlock cycle and any loss fails cold.

All operator mutations carry an opaque lease ID, control generation, expected
state revision, a requested final semantic state, and optionally a request UUID.
Validation and commit occur in one authority critical section. Stable error
classes distinguish missing/lost authority, stale generations, revision
conflicts, ineligible requests, idempotency conflicts, and latched faults.

The control page discards its local authority assumption on foreground/resume,
then acquires and refreshes before enabling controls. Visible-page heartbeats
are a usability mechanism only; server lease expiry is the fail-cold mechanism.
Responses are ordered by boot ID, generation, and revision so a late response
from an older authority generation cannot replace newer UI state.

Only the exact PrusaLink state `PRINTING` passes the AUTOMATIC state allowlist.
Even then, AUTOMATIC remains cold because the bed-target-to-chamber-target policy
is deliberately undefined. Manual targets outside 30–50 °C are rejected.

The abstract interlock can represent logical heat demand, fan proof, cooldown,
fault thermal management, and authoritative overtemperature detection without
inventing GPIOs or temperature thresholds. The production image has no physical
output path, so API delivery remains zero/unavailable.

The control task is the sole production owner of ordinary interlock evaluation.
It first copies the independently polled, cached PrusaLink status, then performs
authority expiry/sampling, authority input application, and the final interlock
decision as one short ordered control step. The cached-status getter may wait on
its own mutex and therefore runs before authority is sampled; neither it nor any
network work runs under the authority `portMUX`.

Authorization removal is directional and synchronous. Expiry, takeover,
reacquisition, and OFF immediately cold the current logical REMOTE decision,
without discarding cooldown/fan-afterrun state. Grants remain inert until the
next control step. Authority-to-interlock nesting always locks authority first;
interlock code never acquires authority. Authority critical sections contain
only bounded state checks/copies, the eight-entry cache scan, and short
interlock operations—no logging, formatted I/O, allocation, or blocking calls.

## PID-family control loop

Normal heater control is PID-family closed-loop control; hysteretic bang-bang
exists only as a future Phase 3 validation comparator. `dc_pid` (pinned
dragon-core) provides controller mathematics only. `jj_control` owns setpoint
resolution, `dc_pid` state/config lifecycle, integration decisions, and the raw
request. `jj_interlock` remains the sole safety authority.

Inside the one ordered control step, under the authority lock:

1. `jj_interlock_evaluate()` (Stage 1) runs the single set of eligibility
   predicates and returns a dominant block reason for diagnostics plus a
   controller disposition aggregated over every applicable inhibit
   (SUSPEND > HOLD > RUN). Diagnostic priority never decides disposition. Stage 1
   immediately publishes a de-authorized snapshot and retains the evaluation
   under a sequence token.
2. `jj_controller_step()` consumes that result. SUSPEND: controller IDLE, state
   reset. HOLD: step with integration frozen, state preserved. RUN: step with
   integration. It never re-derives eligibility from `jj_inputs_t`.
3. `jj_interlock_authorize()` (Stage 2) consumes the retained evaluation exactly
   once, matched by token. It authorizes only RUN with a VALID controller and a
   finite 0–100 % request; `allowed_duty_pct` is then the raw request. Any other
   combination, a stale/replayed/unknown token, or a revocation or fault clear
   between the stages fails cold.

Controller state is IDLE (no step attempted; ordinary), VALID, or INVALID (step
expected but rejected; reported as `controller_invalid`). `requested_duty_pct`
is the unmodified `dc_pid` output and 0 unless VALID. No product code compares
temperature to target; there is no target-crossing override.

Only fan-proof-pending HOLDs. Every other inhibit, including NOT_COMMISSIONED
and MANUAL_TARGET_INVALID, SUSPENDs; the classification is an exhaustive switch,
so a new block reason does not compile until classified. Gains, sample interval,
integral/output limits, target-change integral handling, and any product duty
limiter are provisional pending Phase 3 plant evidence. The shipped
configuration has zero gains and the production image is uncommissioned, so it
never produces a heat request.

## OTA invariant

The portal rejects OTA when heat is authorized or active thermal management is
required. The same authoritative guard is rerun immediately before boot selection,
then project identity is checked. Browser/API controls cannot override interlocks.
