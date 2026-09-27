# Jump Jet product and safety contract

This is the authoritative behavioral contract for Jump Jet. It distinguishes
required product behavior from what the current cold-safe foundation implements.
Hardware-dependent values remain unknown until evidence supports them.

## Operating modes and retained configuration

Jump Jet has exactly three operating modes: **OFF**, **MANUAL**, and
**AUTOMATIC**. Boot and reset always select OFF; mode is never restored across a
reboot. OFF removes the heating request but does not erase retained settings.
Cooldown or fault-related fan activity may continue while mode is OFF.

The retained MANUAL target persists across mode changes and reboot. Its allowed
range is 30–50 °C inclusive and its default is 45 °C. Inputs outside that range
are rejected, never clamped. The foundation models these semantics but exposes
no mode/target mutation yet; NVS persistence is a later implementation gate.

## AUTOMATIC and PrusaLink

`dc_prusa` is read-only and owns status freshness. Jump Jet consumes its existing
15-second freshness result directly and must not add a second freshness timer.
Missing or stale status is therefore presented to Jump Jet as unavailable.

Eligibility is an allowlist: only exact `PRINTING` is eligible. `PAUSED`, `BUSY`,
`ATTENTION`, `IDLE`, `READY`, `FINISHED`, `STOPPED`, `ERROR`, `UNKNOWN`, empty,
and every future unrecognized state fail cold.

The production bed-target-to-chamber-target policy is TBD. It must be derived and
reviewed rather than inferred from legacy behavior. Until that policy exists,
AUTOMATIC production heating is unavailable even when `PRINTING` is fresh.

## Safety and recovery

- Boot/reset commands heater OFF before any other product behavior.
- Missing/stale printer data, invalid sensors, faults, and unsupported policy
  inhibit heating.
- Watchdog, panic, and brownout resets create a latched inhibit. After reboot,
  health must be revalidated and a user must explicitly acknowledge it.
- Suspected stuck-on heating requires a power cycle, explicit acknowledgement,
  and evidence of sane commanded-OFF behavior before heat may be enabled again.
- Cooldown and fault fan policy is product-owned and may operate in OFF.
- Browser and API authorization are management controls, never the safety boundary.
- The actual Super Mini directly couples USB VBUS to its exposed 5 V pin. Rev A
  therefore requires a physical disconnect between board-derived MCU 5 V and
  the module node, opened before powered USB service. USB must never power
  `+5V_SYS_GATE`, the heater gate driver, the MOSFET actuation path, or any other
  safety-critical actuator domain. Removal of 24 V must collapse gate power even
  if USB keeps the MCU alive.

Reset-cause persistence, stuck-on proof, physical sensors, and physical fan proof
are not implemented in this heater-incapable foundation. They are mandatory gates
before an actuator is introduced.

## OTA guard

OTA must be rejected while heating, during active cooldown, or while a fault
requires active thermal management. The authoritative product guard is checked
before upload and again immediately before selecting the uploaded image for boot.
An uploaded image must also carry the Jump Jet product identity.

## Diagnostic semantics

Diagnostics use this order so a client cannot confuse demand with delivery:

1. mode and controller
2. controlling temperature and source
3. target
4. requested output
5. allowed output
6. delivered output
7. dominant constraint
8. health, degraded state, and fault

Capabilities are truthful. Heating or fan capability must not be advertised while
physical actuation is unavailable.

## Heater-incapable foundation invariant

Production code contains no heater GPIO, placeholder heater pin, fan GPIO, fake
board constants, PWM hardware, ADC, thermistor conversion, or MOSFET actuation.
Logical demand and thermal-management requests may be modeled for host tests, but
delivered physical output remains zero and unavailable.

## Confirmed hardware facts

The standalone CZ4060 characterization at 24 V found:

- conservative design basis: 200 W / 8.33 A
- measured sustained load: about 189 W / 7.89 A
- no high-current cold inrush
- heater/PTC region around 130 °C steady
- chassis around 74–75 °C steady

The evidence and setup are in
[`hardware/cz4060-characterization.md`](hardware/cz4060-characterization.md).
Those temperatures are observations, not firmware thresholds.

## Hardware-dependent TBDs

The full Q1/F2/PCB copper/connectors/wiring path is not validated, and the
Rev A.4.1 schematic is the current hardware authority. The Rev A PCB layout does
not yet exist and must be implemented and validated during Phase 1. Hardware
actuation remains blocked until the resulting PCB/current path and the other
safety-critical hardware are validated. Final GPIO, ADC, thermistor conversion,
protection thresholds, cooldown criteria, and recovery thresholds must not be
invented. Sanyo Denki 9GA0424P3J001 is a prototype fan candidate only and is not
BOM-final.

The intended replacement-fan architecture is continuous fused 24 V plus ground,
separate open-drain PWM, and tach feedback. The Rev A.4.1 low-side switched fan-
power block is obsolete for production. No production pulses/revolution,
minimum duty, RPM/stall threshold, or proof timeout exists before
characterization; a candidate-specific 25 kHz test point does not freeze final
fan policy. Physical fan proof remains mandatory before heater actuation.
