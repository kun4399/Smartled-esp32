# Roadmap

## Completed

- Identify the target product family and inspect its BLE services.
- Implement and hardware-test MIoT BLE v2 login, state reads, power control, brightness control, and color-temperature control.
- Add the Wi-Fi/MQTT gateway on ESP32-WROOM-32.
- Add the MQTT Smart LED page and rotary feedback to X-Knob.
- Build both firmware targets and complete a user-reported end-to-end test.

## Next improvements

1. Make BLE reconnection and MQTT state recovery predictable after power or network loss.
2. Surface command timeout, stale-state, and broker-connection conditions clearly on the knob.
3. Add repeatable host-side tests for message parsing and command boundaries.
4. Document hardware variants, setup screenshots, and a concise troubleshooting flow as they are confirmed.

## Verification standard

- A successful build confirms compilation only.
- A test is hardware-verified only when the board and device behavior were observed directly.
- Protocol behavior inferred from another product or public specification must remain labeled as an assumption until tested on the exact target model.
- Never test undocumented write or pairing flows on a live device merely to complete a checklist.
