# Sanyo Denki 9GA0424P3J001 characterization

> **PROTOTYPE ONLY / NOT PRODUCTION-FINAL.** This record does not select the
> production fan, freeze firmware constants, or authorize fan/heater actuation.

## 1. Scope and status

This is the evidence record and bench plan for the ordered Sanyo Denki
`9GA0424P3J001` four-wire fan. Published facts apply to this model only. All
measurement fields remain TBD until the physical specimen arrives and is tested.

## 2. Specimen identity and provenance

- Model ordered: `9GA0424P3J001`
- Status: prototype candidate, not BOM-final
- Expected arrival: 2026-09-15
- Received specimen label, lot/date code, supplier, and traceability: TBD
- Photograph and lead/connector identification: TBD

Manufacturer sources:

- [9GA0424P3J001 product page](https://products.sanyodenki.com/en/sanace/dc/dc-fan/9GA0424P3J001/)
- [San Ace PWM technical material](https://products.sanyodenki.com/info/sanace/en/technical_material/pwm.html)
- [San Ace DC sensor technical material](https://products.sanyodenki.com/info/sanace/en/technical_material/dcsensor.html)
- San Ace installation/safety manual `M0011876C`, linked from the product page

## 3. Pinout

The manufacturer manual states the general San Ace convention: red is positive,
black or blue is ground, yellow is the sensor lead, and brown is the PWM-control
lead. The delivered model must still be inspected because this convention is not
a substitute for specimen-specific verification.

| Lead | Provisional function | Verification |
|---|---|---|
| Red | +24 V | Inspect delivered specimen and continuity/power test |
| Black | GND | Inspect delivered specimen and continuity/power test |
| Yellow | Pulse sensor / tach | Verify waveform and electrical limits |
| Brown | PWM control | Verify input behavior and safe drive topology |

## 4. Published model-specific specifications

The model-specific product page confirms:

| Property | Published value |
|---|---:|
| Rated voltage | 24 V |
| Operating range | 21.6–26.4 V |
| Rated current | 0.27 A |
| Rated input | 6.48 W |
| Rated speed | 18,000 min⁻¹ |
| Maximum airflow | 0.67 m³/min (23.7 CFM) |
| Maximum static pressure | 535 Pa (2.15 inH₂O) |
| Operating temperature | −20 to +70 °C |
| Sensor | Pulse sensor |
| PWM control | Yes |

The published model page does not state the exact PWM frequency, tach output
circuit, pulses per revolution, or model-specific open/0% behavior. Manufacturer
technical material supports open-collector/open-drain PWM drive generally, but
also says voltage, frequency, and response differ by model. Those values remain
provisional pending a model-specific drawing or bench evidence.

## 5. Bench setup

Required equipment:

- current-limited 24 V supply
- DMM and oscilloscope
- open-drain PWM source with adjustable frequency/duty
- logic/frequency capture
- thermal measurement suitable for hub, wire, and connector temperatures
- independent optical tachometer preferred
- anemometer optional

Record wiring, current limit, ambient temperature, airflow fixture, oscilloscope
probe reference, and all instrument identifiers/calibration status. Do not power
the fan until lead identity has been verified.

## 6. PWM behavior

- High-impedance/default control behavior: TBD
- Accepted voltage levels: TBD
- Compatible frequency range; test 25 kHz as a candidate point: TBD
- 0% behavior: TBD
- Upward and downward duty sweeps: TBD
- Noise/vibration observations: TBD

## 7. Tach behavior

- Output circuit and voltage: TBD
- Required pull-up and sink-current limits: TBD
- Pulses per revolution: TBD
- Jitter and pulse integrity: TBD
- Startup tach-acquisition latency: TBD

## 8. RPM versus duty

Record commanded duty, measured RPM, supply current, and tach frequency for both
upward and downward sweeps. Results: TBD.

## 9. Startup / minimum reliable command

- Startup current and duration: TBD
- Minimum reliable startup duty: TBD
- Minimum stable running duty: TBD
- Restart hysteresis: TBD

## 10. Stall / locked rotor

The manufacturer installation manual describes cyclic current cutoff and
automatic restart as the general DC-fan burnout-protection approach. Verify the
delivered model rather than treating protection as fan proof.

- Locked-rotor current behavior: TBD
- Tach waveform/semantics while stalled: TBD
- Automatic restart behavior and timing: TBD
- Safe stall-detection latency: TBD

## 11. Current

- 24 V steady current: TBD (published rated current is 0.27 A)
- Startup peak: TBD
- Duty-dependent current: TBD
- Locked-rotor current profile: TBD

## 12. Thermal behavior

Record ambient, hub, lead, connector, and interface-component temperatures during
steady operation and locked-rotor testing. Results: TBD.

## 13. Airflow / CZ4060 cooldown

Record the fan fixture, restriction/static-pressure condition, installed CZ4060
airflow, cooldown curve, noise, and vibration. Results: TBD.

## 14. Derived candidate thresholds

No minimum duty, minimum RPM, stall threshold, proof timeout, or thermal limit is
defined. Derive candidates only after repeatable bench and installed measurements.

## 15. Open questions

- exact delivered lead/connector construction and AWG
- exact PWM electrical limits and frequency range
- open-control and 0% behavior
- tach circuit, pull-up, pulses/revolution, and stopped/locked semantics
- startup, stable-speed, and restart behavior
- fan-branch fuse rating and connector/interface component values
- production-fan suitability and acceptable substitutions

## 16. Installed-system validation

After bench characterization, repeat the relevant tests in the production-intent
heater/duct/enclosure assembly. Establish airflow margin, hub/wiring/connector
temperature, chamber performance, cooldown effectiveness, EMI/noise behavior,
and trustworthy fan-proof semantics. Installed results: TBD.
