# Implementation and verification notes

This page records project-level evidence without storing household identifiers, device addresses, credentials, keys, or raw captures. Update it when implementation or verification status changes.

## Verified on hardware

- The target light uses the public model identifier `lemesh.light.wy0d02` (product ID 19733). Model matching identifies a product family; it does not uniquely identify a physical device.
- The ESP32-WROOM-32 can connect to the target, negotiate ATT MTU, complete MIoT BLE v2 login, and read power, brightness, and color temperature.
- Power, brightness, and color temperature writes have been accepted and verified by reading the attributes back from the light.
- The MQTT gateway and X-Knob client have been flashed and the user reported successful operation. The exact test environment and measurements were not retained in this public log.

## Build evidence

- ESP-IDF firmware has built for the `esp32` target.
- X-Knob firmware has built for `esp32-s3-devkitc-1` using PlatformIO.
- A successful build confirms compilation only. It does not establish BLE reliability, MQTT reconnect behavior, UI readability, or haptic quality on every board.

## Protocol evidence and limits

- The target's MIoT BLE v2 authentication and property operations were exercised on hardware.
- Public model specifications and third-party implementations were useful for forming implementation hypotheses. They are not evidence for unsupported models or undocumented control frames.
- Do not add pairing, reset, or raw-frame writes without model-specific evidence and an explicit need.

## Handling private test data

- Keep `sdkconfig`, `main/gatt_ltmk_private.h`, device key files, captured logs, and firmware images containing keys outside Git.
- Put temporary captures under the ignored `logs/` directory and redact identifiers and payloads before sharing.
- When updating this page, summarize only the result needed to explain project status; do not paste terminal output or account/device details.
