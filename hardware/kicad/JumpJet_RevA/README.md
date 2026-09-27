# JumpJet Rev A KiCad source

This directory contains the checked-in source from the authoritative Rev A.4.1
schematic package, `JumpJet_Controller_RevA4_1_J3_JST_XH.zip`.

Open `JumpJet_Controller_RevA4.kicad_pro`, then
`JumpJet_Controller_RevA4.kicad_sch`. The root schematic references four child
sheets in this directory. Project-local symbols and footprints resolve through
`sym-lib-table` and `fp-lib-table`.

## Authority and limitations

- Rev A.4.1 is the current authoritative **schematic** baseline.
- That baseline originally contained the obsolete two-wire low-side-switched fan
  block. Phase-1 source now carries the reviewed provisional four-wire interface:
  continuous fused 24 V and ground, open-drain PWM, and 3.3 V tach provisions.
  The fan and interface remain prototype/candidate dependent and uncharacterized.
- No `.kicad_pcb` exists in this source set. The first Rev A PCB remains active
  Phase 1 implementation work, not a missing artifact to recover.
- This source is not fabrication-ready and does not authorize heater or fan
  actuation.
- Bench characterization establishes that the actual Super Mini's exposed 5 V
  pin and USB VBUS are directly coupled for system-design purposes. Rev A now
  routes the shared `+5V_SYS_GATE` source through D4 reverse isolation and the
  physical SW1 RUN / USB SERVICE disconnect before the module `+5V_MCU` node.
  D4 and SW1 are strong-candidate functional classes only; their exact MPNs and
  footprints remain TBD. TPS2116-only automatic selection is not a valid
  solution for nodes already tied on the module.
- `+5V_SYS_GATE` remains a separate 24-V-derived actuator rail. USB must have no
  path to that rail, the gate buffer supply, or heater actuation.
- Rev A retains remote `T_CHAMBER`, `T_OUTLET`, and `T_CASE_EXTERNAL` sensing and
  adds onboard `T_PCB` through the existing thermistor-front-end philosophy.
  `T_PCB` placement, exact NTC characteristics, ADC/GPIO assignment, conversion
  constants, and firmware thresholds remain blocked; it is not chamber sensing
  and does not supersede `T_CASE_EXTERNAL` in Rev A.
- `BOM_PRELIMINARY.csv` is archive context only. It contains placeholders and
  does not override statuses in
  [`docs/PHASE1_HARDWARE_REGISTER.md`](../../../docs/PHASE1_HARDWARE_REGISTER.md).
- Native KiCad 10.0.6 opens the root hierarchy successfully, and native ERC is
  reusable with the installed standard library tables. ERC intentionally still
  reports the unresolved `+5V_MCU` source path, unassigned control nets, TBD
  footprints, intentional legacy NC nets, and reviewed cached-symbol differences.
- Hierarchical-sheet and project-local library paths pass static resolution.
  Placeholder footprint references `TBD:High_Current_2Pin`,
  `TBD:DC_DC_Module`, `TBD:Aux_2Pin`, and `Package_DirectFET:TBD` remain
  intentionally unresolved and must be replaced before PCB implementation.

## Footprint status

`JumpJet.pretty/ESP32-S3_SuperMini_2x9.kicad_mod` is the drilled legacy
footprint referenced by Rev A.4.1. It is retained so the current schematic
resolves, but it is obsolete for the selected production mounting method.

`JumpJet.pretty/ESP32-S3_SuperMini_2x9_Castellated_PROVISIONAL.kicad_mod` is an
unassigned iteration footprint for direct castellated soldering. Its body,
USB shell, nominal pitch/row spacing, and derived longitudinal registration use
the current physical evidence. Its receiving-land depth, antenna graphic, and
underside-contact exclusion remain provisional. It deliberately has no locating
holes because the previous coordinates do not reconcile with the updated
longitudinal datum. Do not release or assign it without the required 1:1 fit
check and completion of the blockers in the hardware register.

Current evidence supports a solid carrier PCB beneath the ESP32 module: there
are no underside mounted components, but exposed underside contacts require an
isolation-controlled region. A mandatory central window is not supported by the
evidence. Underside contact mapping and the provisional antenna keepout remain
fabrication blockers.

## Source-package exclusions

The archive's repair, warning-cleanup, presentation, polish, and connector
changelog text files were not imported because they are historical packaging
notes rather than project dependencies. Lock files, autosaves, backups,
fabrication outputs, and temporary exports are also intentionally excluded.
The archive README is retained under `references/` as provenance.
