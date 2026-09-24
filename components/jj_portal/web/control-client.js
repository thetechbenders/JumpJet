// SPDX-License-Identifier: GPL-3.0-or-later
(function (root, factory) {
  const api = factory();
  if (typeof module === "object" && module.exports) module.exports = api;
  else root.JumpJetControl = api;
})(typeof globalThis !== "undefined" ? globalThis : this, function () {
  "use strict";

  const CONTROL_LOSS_ERRORS = new Set([
    "control_authority_required",
    "control_authority_lost",
    "control_generation_stale",
  ]);

  class ControlError extends Error {
    constructor(code, state) {
      super(code);
      this.name = "ControlError";
      this.code = code;
      this.state = state || null;
    }
  }

  function titleCase(value) {
    return value ? value[0].toUpperCase() + value.slice(1) : "None";
  }

  function controllerLabel(state) {
    const authority = state && state.control ? state.control.authority : "none";
    const controller = authority === "remote" ? "Remote" :
      authority === "automatic" ? "PrusaLink" :
      authority === "reacquiring" ? "Reacquiring" : "None";
    return titleCase(state && state.mode) + " · " + controller;
  }

  function constraintMessage(code) {
    const messages = {
      none: "No active control constraint",
      off: "Heating is off",
      not_commissioned: "Heating unavailable: product not commissioned",
      fault_latched: "Heating inhibited: safety fault latched",
      printer_unavailable: "Heating inhibited: PrusaLink status unavailable or stale",
      printer_not_printing: "Heating inhibited: printer is not printing",
      automatic_policy_unavailable: "Heating inhibited: Automatic target policy unavailable",
      manual_target_invalid: "Heating inhibited: Manual target is invalid",
      fan_proof_pending: "Heating inhibited: airflow is not proven",
      invalid_mode: "Heating inhibited: operating mode is invalid",
      control_no_authority: "Control inhibited: remote controller disconnected",
      state_refresh_required: "Control inhibited: authoritative state refresh required",
      automatic_authority_unavailable: "Heating inhibited: Automatic controller unavailable",
      controller_invalid: "Heating inhibited: controller could not produce a valid request",
      control_sequence_invalid: "Heating inhibited: control evaluation was out of sequence",
    };
    return messages[code] || "Heating inhibited: see advanced diagnostics";
  }

  const CONTROL_LOOP_LABELS = {
    idle: {text: "Idle", className: ""},
    valid: {text: "Running", className: ""},
    invalid: {text: "Invalid: controller failure", className: "fault"},
  };

  function dutyText(value) {
    return typeof value === "number" && Number.isFinite(value)
      ? value.toFixed(1) + " %" : "—";
  }

  /*
   * Primary heater presentation. Controller state is shown on its own so an
   * INVALID controller stays visible whatever the dominant constraint is.
   */
  function heaterPresentation(heater) {
    const h = heater || {};
    const loop = Object.prototype.hasOwnProperty.call(CONTROL_LOOP_LABELS, h.control_loop)
      ? CONTROL_LOOP_LABELS[h.control_loop]
      : {text: "Unknown", className: "inhibit"};
    return {
      controllerText: loop.text,
      controllerClass: loop.className,
      requestedDuty: dutyText(h.requested_duty_percent),
      allowedDuty: dutyText(h.allowed_duty_percent),
    };
  }

  function mutationErrorMessage(code) {
    if (code === "state_revision_conflict")
      return "Authoritative state changed. Refresh and verify the current result; the earlier request may have completed.";
    return "Request rejected: " + code;
  }

  class Client {
    constructor(options) {
      this.fetch = options.fetch;
      this.owner = options.owner;
      this.onState = options.onState || function () {};
      this.onReady = options.onReady || function () {};
      this.onControlLost = options.onControlLost || function () {};
      this.uuid = options.uuid || function () {
        return crypto.randomUUID();
      };
      this.state = null;
      this.leaseId = "";
      this.generation = 0;
      this.revision = 0;
      this.ready = false;
      this.resumeSequence = 0;
    }

    invalidate(code) {
      this.ready = false;
      this.leaseId = "";
      this.onReady(false);
      this.onControlLost(code);
    }

    acceptState(state, options) {
      if (!state || typeof state !== "object" || !state.control) return false;
      const allowBootChange = options && options.allowBootChange;
      const generation = Number(state.control.generation);
      const revision = Number(state.state_revision);
      if (!Number.isSafeInteger(generation) || !Number.isSafeInteger(revision))
        return false;
      if (this.state && state.boot_id !== this.state.boot_id && !allowBootChange)
        return false;
      if (this.state && state.boot_id === this.state.boot_id &&
          (generation < this.generation ||
           (generation === this.generation && revision < this.revision)))
        return false;
      this.state = state;
      this.generation = generation;
      this.revision = revision;
      this.onState(state);
      return true;
    }

    async post(path, body) {
      const response = await this.fetch(path, {
        method: "POST",
        headers: {"Content-Type": "application/json"},
        body: JSON.stringify(body),
        cache: "no-store",
      });
      const payload = await response.json();
      if (!response.ok) throw new ControlError(payload.error || "request_failed",
                                                payload.state);
      return payload;
    }

    async resume(takeover) {
      const sequence = ++this.resumeSequence;
      this.ready = false;
      this.leaseId = "";
      this.onReady(false);
      try {
        const acquired = await this.post("/api/v2/control/acquire", {
          owner: this.owner,
          takeover: takeover === true,
        });
        if (sequence !== this.resumeSequence) return null;
        if (!this.acceptState(acquired, {allowBootChange: true}))
          throw new ControlError("invalid_authoritative_state");
        this.leaseId = acquired.control.lease_id;
        const refreshed = await this.post("/api/v2/control/refresh", {
          lease_id: this.leaseId,
          control_generation: this.generation,
        });
        if (sequence !== this.resumeSequence) return null;
        if (!this.acceptState(refreshed, {allowBootChange: true}))
          throw new ControlError("invalid_authoritative_state");
        this.leaseId = refreshed.control.lease_id;
        this.ready = true;
        this.onReady(true);
        return refreshed;
      } catch (error) {
        if (sequence === this.resumeSequence) {
          if (error.state) this.acceptState(error.state, {allowBootChange: true});
          this.invalidate(error.code);
        }
        throw error;
      }
    }

    async mutate(action, targetC) {
      if (!this.ready || !this.leaseId)
        throw new ControlError("control_authority_required");
      const requestGeneration = this.generation;
      const body = {
        lease_id: this.leaseId,
        control_generation: requestGeneration,
        expected_state_revision: this.revision,
        request_id: this.uuid(),
        action: action,
      };
      if (action === "manual") body.target_c = targetC;
      try {
        const state = await this.post("/api/v2/control/mutate", body);
        if (requestGeneration !== this.generation) return this.state;
        this.acceptState(state);
        return state;
      } catch (error) {
        if (requestGeneration !== this.generation) return this.state;
        if (error.state) this.acceptState(error.state);
        if (CONTROL_LOSS_ERRORS.has(error.code)) this.invalidate(error.code);
        throw error;
      }
    }

    async heartbeat() {
      if (!this.ready || !this.leaseId) return null;
      const requestGeneration = this.generation;
      try {
        const state = await this.post("/api/v2/control/heartbeat", {
          lease_id: this.leaseId,
          control_generation: requestGeneration,
        });
        if (requestGeneration === this.generation) this.acceptState(state);
        return state;
      } catch (error) {
        if (requestGeneration === this.generation &&
            CONTROL_LOSS_ERRORS.has(error.code)) this.invalidate(error.code);
        throw error;
      }
    }
  }

  return {
    Client,
    ControlError,
    CONTROL_LOSS_ERRORS,
    controllerLabel,
    constraintMessage,
    heaterPresentation,
    mutationErrorMessage,
  };
});
