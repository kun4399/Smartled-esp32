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

Commands are plain text and are not retained. State is retained by the gateway. Example shape:

```json
{"online":true,"ok":true,"power":true,"brightness":50,"temperature":4000}
```

`online` indicates whether the BLE device state is currently available. `ok` reports whether the last write/readback succeeded. The reported values come from BLE reads; a value previewed on the knob is not confirmed until the gateway publishes matching state. The gateway publishes an MQTT last-will message when its broker connection drops.

## X-Knob interaction

Open **Smart LED** from the knob menu. Turn to adjust brightness or color temperature; short-press to cycle brightness → temperature → power. Long-press returns to the previous screen. The display shows the local preview and whether the gateway confirmed it. The motor uses bounded detents for brightness and temperature and two strong positions for power.

## Troubleshooting

- No connection: check that both boards use the same 2.4 GHz network and broker settings.
- Knob says offline: subscribe to the state topic and verify the gateway is publishing retained state.
- Preview does not become confirmed: check broker permissions and the gateway's BLE connection.
- Do not share `sdkconfig`, `src/secrets.h`, NVS dumps, device keys, firmware images, or raw serial logs. They may contain credentials or device identifiers.
