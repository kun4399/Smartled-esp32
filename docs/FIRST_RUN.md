# First build and local setup

## Requirements

- ESP-IDF 6.x with the ESP32 target support installed.
- Python 3.10 or newer for host-side BLE tools.
- A compatible ESP32-WROOM-32 board and a USB data cable for optional flashing.

## Build the firmware

```sh
cd Smartled-esp32
. "$IDF_PATH/export.sh"
idf.py set-target esp32
idf.py build
```

`set-target` is needed for a new build directory or when changing chip targets. Build output is written under `build/` and is ignored by Git.

## Configure the gateway

Use `idf.py menuconfig` and open **SmartLED local gateway** to set the Wi-Fi and MQTT values for your network. These settings live in the local `sdkconfig`; do not commit or share it. The network must provide 2.4 GHz Wi-Fi for the ESP32-WROOM-32.

Generate `main/gatt_ltmk_private.h` from a local, permission-restricted key file:

```sh
python3 tools/provision_firmware_key.py
```

The key is compiled into firmware. Keep generated binaries and flash backups private. The provisioning tool does not display the key.

## Optional host-side tools

Create an isolated Python environment and install the listed dependencies:

```sh
python3 -m venv .venv
.venv/bin/python -m pip install -r tools/requirements.txt
```

The MIoT probe is read-only unless an explicit write option is supplied. The key-fetch utility prompts locally for account authentication and writes the resulting device key outside the repository. Review `--help` before running a tool against hardware or an account.

## Flash only the intended board

List the serial devices provided by your operating system, identify the board, then pass its port explicitly:

```sh
idf.py -p <serial-port> flash
```

Flashing replaces the application on that board. Confirm the port and board before running the command. This guide intentionally contains no machine-specific port or user-directory paths.
