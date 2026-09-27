# Jump Jet TODO

This is the working queue. The [roadmap](ROADMAP.md) owns sequencing and phase
gates; this file owns concrete tasks. Move substantial work into GitHub issues as
implementation starts, and keep only the summary checkbox here.

## Needed from the current hardware design

- [x] Review the Rev A.4.1 schematic package and record it as the current
  authoritative schematic baseline in the
  [Phase 1 hardware register](docs/PHASE1_HARDWARE_REGISTER.md)
- [x] Check the authoritative Rev A.4.1 KiCad project, hierarchical sheets,
  project symbol/footprint libraries, and preliminary BOM into
  `hardware/kicad/JumpJet_RevA/`
- [ ] Create the first Rev A `.kicad_pcb` after the mechanical and electrical
  layout blockers close
- [ ] Add the current BOM with manufacturer part numbers and acceptable substitutions
- [x] Identify the physical controller module as an ESP32-S3 Super Mini
- [x] Record the measured ESP32-S3 Super Mini envelope and provisional nominal
  pin geometry
- [ ] Verify the installed ESP32-S3 Super Mini flash size
- [ ] Complete the production castellated footprint after the provisional land
  geometry and exact registration, underside-contact isolation map, antenna
  keepout, USB access envelope, and physical pin numbering are verified
- [ ] Confirm the complete GPIO/ADC assignment, including boot strapping and USB pins
- [x] Record standalone CZ4060 rated and measured power/current at 24 V
- [ ] Validate the full Jump Jet Q1/F2/PCB/connector/wiring path at the 200 W / 8.33 A design basis
- [x] Select the native four-wire fan architecture: continuous fused 24 V power,
  separate open-drain PWM, and separate tach feedback
- [x] Create the
  [9GA0424P3J001 characterization record](docs/hardware/9ga0424p3j001-characterization.md)
- [ ] Characterize the physical 9GA0424P3J001 specimen on arrival
- [ ] Characterize the 9GA0424P3J001 prototype and finalize candidate-specific
  pinout, current, connector, PWM/tach electrical details, RPM behavior, and
  stall thresholds; it remains prototype-only
- [ ] Record every thermistor part number, beta/Steinhart-Hart data, tolerance, and location
- [ ] Confirm the MOSFET part, gate network, thermal environment, and default-off circuitry
- [ ] Confirm fuse type/rating, independent thermal-cutoff part/rating, and physical placement
- [ ] Define what the physical switch interrupts or commands
- [ ] Add the latest enclosure/duct model and intended airflow direction

### Current PCB/fabrication blockers

- [ ] Define the board outline, mounting-hole coordinates, connector-facing
  edges, and USB mechanical datum
- [x] Measure ESP32 castellation width and the non-USB edge-to-first-castellation datum
- [ ] Verify exact ESP32 body/pad registration on a 1:1 production-footprint plot
- [x] Confirm the ESP32 underside has no mounted components
- [ ] Map the exposed underside contacts and define the carrier isolation region
- [ ] Identify and measure the actual ESP32 antenna keepout
- [x] Measure the ESP32 USB-C shell envelope
- [ ] Measure the USB-C mating-plug/cable-head envelope and define carrier-edge,
  enclosure, and service-access clearances
- [x] Establish the external 5 V / USB-C VBUS topology: the actual Super Mini's
  exposed 5 V pin and USB VBUS are directly coupled for system-design purposes
- [ ] Select, rate, and place the Rev A physical board-5-V disconnect; verify USB
  access, service labeling, jumper/switch clearance, retention, and misuse visibility
- [ ] Verify the Q1 footprint and multipad mapping
- [ ] Verify the F2 holder footprint and service/removal clearance
- [ ] Select TF1/TS1 parts and their mounting/thermal-coupling architecture
- [ ] Select and verify J1, F1, and MOD1 parts and footprints
- [ ] Characterize the production PSU current limit and complete fuse coordination
- [ ] Close the aligned local mechanical constraint map before creating
  fabrication-intent `Edge.Cuts`

## Firmware — immediate

- [x] Add last-success sample age to `dc_prusa_status_t` in Dan's `dragon-core` fork
- [x] Test that only a complete, atomically parsed PrusaLink sample refreshes freshness
- [x] Submit the freshness API upstream in `dragon-core` PR #51 and pin Jump Jet to its green commit
- [x] After PR #51 merges, repin Jump Jet to the resulting upstream commit or release tag
- [x] Remove provisional AUTO thresholds and keep production AUTOMATIC heat unavailable
- [x] Allowlist exact `PRINTING`; fail every other known or future state cold
- [ ] Add configuration validation tests for PrusaLink host, port, and secret retention
- [x] Use `dc_prusa`'s 15-second freshness result directly; remove the second product timer
- [x] Add host tests for manual target rejection, AUTOMATIC fail-cold policy, and safe fault clearing
- [x] Make zero-initialized interlock inputs non-authoritative and provide named cold-safe defaults
- [x] Distinguish fan-proof pending from an explicit proof failure in the abstract interlock
- [x] Remove uncharacterized numeric trip ceilings from production source
- [ ] Derive sensor-specific release limits from full-system evidence before heater actuation
- [x] Add API contract tests for truthful capabilities, semantic ordering, and cold-safe delivery

## Electrical and safety analysis

- [ ] Draw the complete 24 V power and return path from supply through protection to loads
- [ ] Calculate normal, startup, and credible fault current for the heater plus
  all final fan and auxiliary loads
- [ ] Calculate connector/contact loading and wire ampacity with temperature derating
- [ ] Calculate MOSFET conduction and switching loss at worst-case voltage, current, duty, and ambient
- [x] Implement separate `+5V_SYS_GATE` / `+5V_MCU` schematic domains and a
  hardware-default-disabled heater-driver OE topology
- [ ] Implement the adopted physical disconnect between board-derived MCU 5 V
  and the Super Mini-side `+5V_MCU` / USB-VBUS tied node; removable jumper/shunt
  is preferred but its part, rating, and footprint remain unresolved
- [ ] Bench-verify that USB/service power cannot energize the gate-driver domain
  or Q1 through every power-up, power-down, reset, brownout, and mixed-source state
- [ ] Verify gate-driver partial-power-down behavior or provide equivalent isolation
- [x] Retain a direct provisional Q1 gate pull-down independent of MCU and buffer state
- [x] Add schematic test points for both 5 V domains, gate-driver OE, heater PWM
  input, Q1 gate, and Q1 source reference
- [ ] Check gate voltage/current, MCU reset behavior, switching waveform, and MOSFET SOA
- [ ] Calculate trace/via current capacity and expected copper temperature rise
- [ ] Calculate thermistor-divider voltage and ADC counts over open, short, and operating range
- [ ] Define independent thermal-cutoff trip temperature from measured enclosure hot spots
- [ ] Establish measured margins for: normal operating ceiling < firmware hard trip < independent hardware cutoff < lowest applicable material/component limit
- [ ] Produce a hazard/FMEA table with detection, response, latch behavior, and independent mitigation

## Firmware — after hardware review

- [ ] Implement `jj_board` with safe-at-reset GPIO initialization
- [ ] Implement ADC sampling, calibration, filtering, and explicit sensor status
- [ ] Implement the characterized fan channel and available fan/airflow proof
- [ ] Define the bounded fan spin-up/proof timeout and the evidence that changes proof from pending to proven or failed
- [ ] Implement physical-switch handling and debouncing
- [ ] Implement bounded settings persistence with corrupt-NVS recovery
- [ ] Implement the heater actuator behind one interlock-controlled interface
- [ ] Verify the chamber, outlet, and case hard-trip ordering against measured normal temperatures, material limits, and the independent cutoff
- [ ] Implement PWM/PID with output clamp, anti-windup, and bumpless stop/restart behavior
- [ ] Implement and persist latched safety faults
- [ ] Implement failure-to-heat and uncontrolled-rise detection from measured thermal behavior
- [ ] Add control-task watchdog and reset-reason reporting
- [ ] Define brownout and reboot cooldown behavior

## UI, API, and OTA

- [ ] Finalize Jump Jet API v2 state and mutation schemas
- [ ] Add chamber, outlet, and case temperature telemetry with sensor status
- [ ] Add target, heater duty, fan command/proof state, mode, and commissioning state
- [ ] Add PrusaLink connection, printer state, bed temperature/target, and sample age
- [ ] Define optional Klipper/Moonraker integration for read-only printer state and bed-target telemetry, with the same stale-data fail-cold semantics as PrusaLink
- [ ] Add visible interlocks, latched fault reason, and safe recovery guidance
- [ ] Add settings for PrusaLink and the bed-target policy with strict bounds
- [ ] Add revision/lease semantics for remote heat commands if manual remote heat is retained
- [ ] Test immediate pre-upload OTA rejection while heating
- [ ] Test the second heater-state guard before selecting the uploaded image for boot
- [ ] Test wrong-product, corrupt, interrupted, and rollback OTA paths
- [ ] Advertise `power_on` or `auto` only after those paths pass HIL commissioning

## Bench and HIL validation

- [ ] Build a dummy-load/current-limited bring-up fixture before connecting the heater
- [ ] Add HIL scenarios for every sensor rail/open/short fault
- [ ] Add HIL scenarios for fan failure, heater no-response, abnormal rise, and stuck-on indication
- [ ] Add HIL scenarios for stale/offline PrusaLink and printer stop/error transitions
- [ ] Add watchdog, brownout, corrupt-NVS, factory-reset, and interrupted-OTA scenarios
- [ ] Characterize heat-up, overshoot, steady-state duty, hot spots, airflow, and cooldown
- [ ] Record connector, wire, PCB, MOSFET, heater-case, outlet, and chamber temperatures
- [ ] Run extended soak testing only after deliberate fault injection passes

## Documentation and release

- [ ] Maintain schematic, wiring diagram, BOM, pin map, and PCB-revision compatibility table
- [ ] Write first-flash, OTA, rollback, factory-reset, and recovery procedures
- [ ] Write assembly, inspection, commissioning, and troubleshooting procedures
- [ ] Define production-test fixture steps and acceptance limits
- [ ] Package versioned binaries, checksums, source provenance, and release notes
- [ ] Publish known limitations and required external safety hardware
