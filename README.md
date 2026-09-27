# Jump Jet

Jump Jet is a PCBWay-sponsored chamber-heater project for the Prusa CORE One
family, developed by [Dan](https://github.com/danielbrownjr) and
[Mauker](https://github.com/Mauker1). It redesigns the electronics and firmware
of Philip Sørensen's original Jetpack project while retaining its practical,
printer-integrated form factor.

> **Development status: cold-safe foundation.** The production firmware has no
> heater or fan GPIO, PWM, ADC, thermistor conversion, or actuator driver. It
> cannot energize a heater and is not installable heater-control firmware.

## Project lineage and credit

Jump Jet began as a fork of Philip Sørensen's
[`esp32-chamber-heater-core-one`](https://github.com/philip-soerensen/esp32-chamber-heater-core-one).
Philip created the original mechanical concept, Arduino firmware, PrusaLink
automation, PID approach, and three-sensor safety concept. The retained history
and legacy source remain GPL-3.0. See [UPSTREAM.md](UPSTREAM.md) for provenance.

The new firmware is a distinct ESP-IDF/Dragon-family product. It consumes pinned,
board-neutral services from [`dragon-core`](https://github.com/justinh-rahb/dragon-core)
while keeping product policy, hardware, control, settings, API behavior, and all
heater safety decisions local to Jump Jet.

## Current foundation

- ESP32-S3 ESP-IDF application skeleton
- `dc_wifi`, `dc_portal`, `dc_ui`, `dc_evlog`, and read-only `dc_prusa` pinned to
  dragon-core v0.32.0 (`4e041d864763d468a50e9649807827dd83dd54bc`)
- exactly three product modes: OFF, MANUAL, and AUTOMATIC; boot/reset is always OFF
- manual target policy of 30–50 °C inclusive with a 45 °C default and rejection,
  never clamping, outside that range
- `dc_prusa`'s existing 15-second freshness result used directly, with no second
  Jump Jet freshness timer
- whitelist-only AUTOMATIC eligibility: only exact `PRINTING` is eligible, and
  production automatic heat remains unavailable until the bed-target mapping is defined
- truthful read-only API diagnostics and capabilities
- pre-upload and immediate pre-boot-selection OTA thermal-state guards
- host tests, API/static contract checks, UBSan, and ESP32-S3 CI build
- no heater or fan actuation path

The authoritative product and safety behavior is defined in
[the product/safety contract](docs/PRODUCT_SAFETY_CONTRACT.md).

## Hardware evidence and limits

Standalone [CZ4060 characterization](docs/hardware/cz4060-characterization.md)
measured about 189 W / 7.89 A sustained at 24 V, no high-current cold inrush,
about 130 °C at the heater/PTC region, and about 74–75 °C at the chassis. Design
sizing remains the rated 24 V / 200 W / 8.33 A case.

That test did **not** validate the complete Jump Jet Q1/F2/PCB/connector/wiring
path or installed operation. Rev A.4.1 is the authoritative schematic baseline,
and its [KiCad project source](hardware/kicad/JumpJet_RevA/README.md) is checked
into this repository. The first Rev A PCB is Phase-1 work in progress; no
fabrication-approved PCB or outputs exist. Checked-in source does not authorize
heater or fan actuation. Detailed status and blockers are tracked in the
[Phase 1 hardware register](docs/PHASE1_HARDWARE_REGISTER.md). GPIO, ADC,
thermistor, protection, and cooldown thresholds therefore remain TBD.

The replacement-fan direction is continuous fused 24 V and ground with separate
open-drain PWM and tach feedback. The Sanyo Denki 9GA0424P3J001 remains a
prototype candidate, not BOM-final; published facts and pending measurements are
tracked in its [characterization record](docs/hardware/9ga0424p3j001-characterization.md).

## Safety boundary

Firmware is only one layer. A release also requires rated wiring/connectors,
input protection and fusing, an independent thermal cutoff, a derated MOSFET,
appropriate PCB copper and clearances, proven airflow, suitable materials, and
fault-injection evidence. The browser and API are never the safety boundary.

## Validation

```sh
./tests/run_interlock_host_test.sh
sh tests/check_identity_contract.sh
sh tests/check_api_contract.sh
sh tests/check_actuation_allowlist.sh
sh tests/check_ota_contract.sh
```

For ESP-IDF 5.3 or newer:

```sh
./tools/idf-build.sh . esp32s3
```

## Documentation

- [Product and safety contract](docs/PRODUCT_SAFETY_CONTRACT.md)
- [Firmware architecture](docs/ARCHITECTURE.md)
- [Hardware baseline](docs/HARDWARE_BASELINE.md)
- [CZ4060 characterization](docs/hardware/cz4060-characterization.md)
- [Safety verification](docs/SAFETY_VERIFICATION.md)
- [Roadmap](ROADMAP.md) and [work queue](TODO.md)
- [Migration map](docs/MIGRATION.md)
- [Attribution and provenance](UPSTREAM.md)

## Contributors and sponsorship

- [Dan](https://github.com/danielbrownjr) — project, firmware, and hardware
- [Mauker](https://github.com/Mauker1) — project and mechanical design
- Original Jetpack project: [Philip Sørensen](https://github.com/philip-soerensen)
- PCB fabrication sponsored by [PCBWay](https://www.pcbway.com/)

Jump Jet is independent and is not affiliated with or endorsed by Prusa Research
or the original Jetpack author.

## License

GPL-3.0, inherited from the upstream Jetpack repository. `dragon-core` is consumed
as an MIT-licensed dependency.
