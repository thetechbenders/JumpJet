# Jump Jet roadmap

This roadmap is ordered by safety dependency, not by which feature is most fun to
demo. A phase may begin experimentally before the previous phase is complete, but
no phase passes its gate without the listed evidence.

## Phase 0 — cold-safe foundation

**Status:** complete

- establish a distinct ESP-IDF Jump Jet product
- pin compatible `dragon-core` components
- start the safety task before networking
- expose truthful Jump Jet identity and read-only status
- add host tests, static analysis, and an ESP32-S3 CI build
- accept only `jumpjet` OTA images, with heater-state checks before and after upload

**Gate:** the application builds for ESP32-S3 and contains no heater actuation path.

## Phase 1 — freeze the hardware contract

**Status:** active

- use Rev A.4.1 as the current authoritative schematic baseline
- freeze the hardware contract and create/review the first Rev A PCB during Phase 1
- review the BOM, enclosure, mechanical interfaces, and airflow path
- confirm the exact ESP32-S3 Super Mini revision, flash size, GPIO map, and boot-strap constraints
- implement a physically accessible, visibly labeled disconnect between
  board-derived MCU 5 V and the Super Mini's directly coupled 5 V/USB-VBUS node;
  seamless simultaneous board/USB power is out of scope for Rev A
- identify heater, MOSFET, characterized four-wire fan channel, thermistors,
  connectors, fuses, thermal cutoff, and switch
- characterize the prototype fan before freezing interface-derived firmware
  values or RPM/stall/proof thresholds
- calculate steady and worst-case current, connector loading, copper temperature rise,
  MOSFET dissipation, gate drive, voltage drop, ADC range, and protection margins
- create a system hazard analysis covering single faults and foreseeable misuse
- define the independent hardware cutoff and prove that firmware cannot defeat it

**Gate:** reviewed hardware baseline with no unresolved critical rating, pin, or
protection question. Firmware constants may not be finalized before this gate.

## Phase 2 — cold electronics bring-up

**Status:** blocked by Phase 1

- implement board identity and force all heater control pins to their safe level at reset
- implement calibrated ADC/thermistor conversion with open, short, rail, NaN, and
  implausible-reading classification
- implement the reviewed fan channel and tach-based fan proof
- define the bounded fan-start/proof sequence and explicit proof-failure criteria from release-intent hardware evidence
- implement the physical switch and define its electrical and software semantics
- persist settings with bounds, schema versioning, corrupt-value fallback, and factory reset
- add HIL hooks that cannot compile into production images

**Gate:** real-board telemetry is credible across the intended temperature range;
sensor and reset fault injection cannot energize the heater.

## Phase 3 — bench heater control and safety

**Status:** blocked by Phase 2

- add a product-local heater actuator with a single auditable enable boundary
- implement bounded PWM and anti-windup PID without bypassing the interlock
- latch overtemperature, sensor, fan/airflow, failure-to-heat, uncontrolled-rise,
  watchdog, and detectable stuck-on faults
- replace the provisional chamber, outlet, and case hard-trip ceilings with separately characterized limits
- define cooldown behavior for normal stop, fault, reboot, brownout, and lost control source
- require deliberate, safe-condition acknowledgement before clearing latched faults
- characterize thermal response and derive thresholds from measurements rather than guesses

**Gate:** current-limited bench testing and fault injection demonstrate safe shutdown
for every implemented fault. No printer installation at this phase.

## Phase 4 — printer policy, API, and UI

**Status:** partially scaffolded

- consume `dc_prusa`'s 15-second freshness result without a second product timer
- allowlist exact `PRINTING` and fail all other states cold
- derive and implement the bed-target-to-chamber-target production policy; keep
  AUTOMATIC unavailable until then
- implement validated, revision-aware API mutations and product settings
- show temperatures, target, duty, fans, mode, PrusaLink state, bed target,
  interlocks, faults, firmware, and network state
- advertise heating capabilities only after the corresponding hardware is commissioned

**Gate:** automated tests cover every policy transition and the UI/API cannot bypass
the firmware safety boundary.

## Phase 5 — integrated system validation

**Status:** blocked by Phases 3 and 4

- validate on a protected bench setup before installation in a CORE One
- measure supply current, connector and MOSFET temperature, airflow, hot spots,
  chamber uniformity, overshoot, and cooldown behavior
- inject thermistor open/short, fan failure, lost Wi-Fi, stale PrusaLink, printer stop,
  watchdog reset, brownout, corrupt settings, and interrupted OTA
- run unattended-duration soak tests only after all deliberate fault tests pass
- verify compatibility with the intended CORE One/+, INDX, and applicable Gen 2 configuration

**Gate:** signed validation record with no open critical safety defect and reproducible
results on release-intent hardware.

## Phase 6 — manufacturing and release

**Status:** blocked by Phase 5

- freeze schematic, PCB, BOM substitutions, firmware pins, and enclosure revision together
- publish assembly, wiring, flashing, recovery, test, and troubleshooting documentation
- define a production test fixture and per-board acceptance checks
- generate versioned firmware artifacts, checksums, provenance, and release notes
- document limitations, required independent protections, and supported hardware revisions

**Gate:** a new builder can assemble, inspect, flash, test, and recover a unit from
the released documentation without relying on undocumented project knowledge.
