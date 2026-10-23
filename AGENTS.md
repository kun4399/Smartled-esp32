# Project working notes

- Read `docs/PROJECT_MEMORY.md` and `docs/ROADMAP.md` before changing the BLE protocol path.
- Distinguish compiled code, hardware-tested behavior, and unverified protocol hypotheses in documentation.
- Keep device identifiers, BLE keys, tokens, Wi-Fi and MQTT credentials, and raw capture logs out of the repository. Store captures under ignored `logs/` and redact before sharing.
- Do not invent Xiaomi control frames from unrelated models. Identify the exact light-strip model and inspect its broadcasts/GATT services first.
- Use ESP-IDF 6.x from the local `IDF_PATH`; default target is `esp32` for ESP32-WROOM-32.
- Do not flash or erase a board without confirming its port and intended use; repository build and read-only source inspection are safe.
