# MQTT control interface

The ESP32-WROOM-32 gateway controls the light over BLE. X-Knob and other clients communicate with the gateway through a local MQTT broker. Home Assistant is optional and is not required for this setup.

```text
X-Knob / MQTT client ── Wi-Fi ── MQTT broker ── ESP32-WROOM-32 ── BLE ── light
```

## Configure both devices

1. Run an MQTT broker on a host reachable from both ESP32 boards. Use a dedicated account and restrict the broker to the trusted local network.
2. In the gateway project, open `idf.py menuconfig` → **SmartLED local gateway**. Enter the 2.4 GHz Wi-Fi network, broker URI, broker credentials, and topic prefix. These values are stored in the local ignored `sdkconfig` and the firmware image.
3. In X-Knob, enter the same Wi-Fi network, broker host/port, credentials, and topic prefix in its local configuration page. The knob stores values in NVS. Do not commit `src/secrets.h` if you use it for initial defaults.
4. Build each project separately. Confirm the target board and serial port before flashing.

Use a private local address in your own configuration; documentation and examples should use placeholders only. ESP32-WROOM-32 and ESP32-S3 boards require 2.4 GHz Wi-Fi.

## Topics and payloads

Replace `<prefix>` with the same prefix on both devices. The example prefix below is `home`.

| Purpose | Topic | Payload |
| --- | --- | --- |
| Command | `home/smartled/strip/set` | `state`, `power 0`, `power 1`, `brightness 1..100`, `temperature 2700..6500` |
| State | `home/smartled/strip/state` | JSON described below |

The V1 interface is available in parallel with these legacy topics. Keep the
legacy topics until the new firmware is verified against the physical light.

| Purpose | Topic | Retained |
| --- | --- | --- |
| Absolute command | `xknob/v1/light/main/set` | No |
| Command result | `xknob/v1/light/main/ack` | No |
| BLE readback | `xknob/v1/light/main/state` | Yes |
| Gateway availability | `xknob/v1/light/main/availability` | Yes |

Example command:

```json
{"v":1,"command_id":"knob-1234","desired":{"power":true,"brightness_percent":52,"color_temperature_kelvin":4200}}
```

At least one desired field is required. IDs contain letters, digits, `-` or
`_` and are at most 63 characters. Brightness is 1..100 and temperature is
2700..6500 K. A duplicate successful command ID is acknowledged without
repeating the BLE write; clients should use a new ID for a genuinely new
command. A command's publish receipt is not proof of execution.

The ACK includes `command_id`, `success`, and `ack_stage`. Success means the
gateway read back every requested field and found a match. The retained state
includes the readback values, `verification: "device_readback"`, `ok`, and
`last_command_id` when a command was processed. Queue overflow and BLE failures
produce a failure ACK. `availability` is `online`/`offline` via MQTT last will;
physical BLE availability is reported separately in state.

Commands are plain text and are not retained. State is retained by the gateway. Example shape:

```json
{"online":true,"ok":true,"power":true,"brightness":50,"temperature":4000}
```

`online` indicates whether the BLE device state is currently available. `ok` reports whether the last write/readback succeeded. The reported values come from BLE reads; a value previewed on the knob is not confirmed until the gateway publishes matching state. The gateway publishes an MQTT last-will message when its broker connection drops.

## X-Knob interaction

The Always Ready v3 face is the knob's default page. Rotate clockwise for
absolute ON and counterclockwise for absolute OFF, including in DARK without
waking the LCD. Short-press enters brightness precision control when the light
is on; the next short press selects color temperature. Long-press opens local
scene preview; double-click on Face opens apps. Scene selection is a preview
until a short press applies it. The separate **Smart LED** app remains.

Idle BLE polling is 30 seconds; writes retain immediate verification readback.
The knob uses a 90-second light TTL. It must confirm matching state and command
ID regardless of ACK arrival order; ACK or publish receipt alone cannot
confirm. Its old queued/UI intent is dropped on disconnect, timeout or dark
transition rather than replayed later. This gateway image was flashed after a
private backup, and a same-value V1 command produced matching BLE readback/ACK;
the one observed 0.983s sample is not switch latency P95. Duplicate ID handling
passed live. Dark knob input, reconnection and P95 latency still need testing.

Before this Always Ready change, the V1 protocol passed an ESP-IDF 6.0.1 build and was flashed to the
confirmed 4 MB ESP32-WROOM-32 on 2026-10-24. The live gateway published a V1
state with `verification: "device_readback"` and matching retained legacy
state. A same-value absolute power command received a successful ACK and
matching BLE readback. The same command ID was acknowledged as a duplicate;
an out-of-range brightness was rejected; the legacy `state` request still
returned BLE readback. Brightness, color-temperature and on/off gestures from
the physical knob need separate observation.

The V1 JSON parser and 16-entry successful-command deduplication ring also
have host tests in `tests/test_gateway_protocol.c`. Run them with
`sh tests/run_host_tests.sh` after ESP-IDF dependencies are installed.

Before flashing, confirm the serial port belongs to the intended WROOM-32.
Keep a full 4 MB Flash backup outside Git because it contains private device
configuration. `idf.py -p PORT -b 115200 flash` writes the bootloader, partition
table and application without a full-chip erase. To restore the prior image,
confirm the same board and use `python -m esptool --chip esp32 --port PORT
write-flash 0 PRIVATE_FULLFLASH_BACKUP.bin`. Do not publish the backup or raw
serial/MQTT captures.

## Troubleshooting

- No connection: check that both boards use the same 2.4 GHz network and broker settings.
- Knob says offline: subscribe to the state topic and verify the gateway is publishing retained state.
- Preview does not become confirmed: check broker permissions and the gateway's BLE connection.
- Do not share `sdkconfig`, `src/secrets.h`, NVS dumps, device keys, firmware images, or raw serial logs. They may contain credentials or device identifiers.
