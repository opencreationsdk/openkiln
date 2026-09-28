# OpenKiln

Standalone OpenKiln controller with local web UI and persistent settings.

## Pages
- `/` Oven dashboard: status, temperature, target, SSR duty, controls, saved-profile selector and selected profile graph.
- `/profiles` Profile library/editor: create, edit, graph, save and delete profiles (the profile to fire is chosen on the dashboard).
- `/setup` Persistent configuration for Wi-Fi/AP, web access, MQTT, InfluxDB 2.x, Pushover, NTP/timezone and firmware OTA.

## MQTT
MQTT is optional. The configured topic prefix publishes status, state, temperature, target, duty, fault and availability. The compatibility topic `kiln/profile_json` is also retained. Commands are received on `<prefix>/command`. STOP is accepted remotely; pause/resume and remote START require their respective Setup switches.

## InfluxDB
Supports InfluxDB 2.x HTTP write API. Configure URL, organisation, bucket, token and interval. Measurement: `kiln`; fields include temperature, target, duty, elapsed_minutes and sensor_ok, tagged by profile.

## Pushover
Optional notifications for start, completion, manual stop and fault. Configure application API token and user/group key.

## NTP
When station Wi-Fi has Internet access, time is synchronized using the configured NTP server. Default timezone is Denmark/Central Europe: `CET-1CEST,M3.5.0,M10.5.0/3`. Kiln operation does not depend on NTP.

## OTA firmware
Open Setup -> Firmware and use *Check for update* (GitHub releases), or upload PlatformIO `.pio/build/esp32dev/firmware.bin` under Advanced -> Manual firmware upload. Firmware update is blocked during RUNNING/PAUSED, starting a firing is blocked during an update, and heating is forced off throughout.

## First boot / recovery
The controller always starts its fallback AP. Defaults are still in `include/Config.h`; after first login use Setup and reboot. Keep the AP password at least 8 characters.

## Build
Open in VS Code + PlatformIO, build environment `esp32dev`, then upload over USB for the initial installation. Subsequent firmware can be uploaded from Setup.


## Thermocouple fault checking
`SENSOR_CHECK_ENABLED` in `include/Config.h` (default `true`). Set it to `false` only for bench testing without a thermocouple, or for a sensor that reports spurious faults (e.g. a grounded-junction thermocouple on a MAX31855). Sensor faults then no longer stop a firing, and with no usable reading the SSR stays off while the profile clock runs (dry run). The emergency limit is still enforced. A warning banner is shown on the dashboard. Never fire unattended with checks disabled.

## Architecture
- `src/main.cpp` – firmware. Sensor, PID and SSR run in a dedicated high-priority FreeRTOS task (core 1, 20 ms tick); all networking (MQTT, InfluxDB, Pushover, update checks/OTA) runs in a separate task on core 0, so a slow or unreachable server can never hold the heating element on. An independent esp_timer watchdog forces the SSR off if the control task ever stalls.
- `web/` – the web UI (HTML/CSS/JS). Edit these files directly.
- `tools/embed_web.py` – runs automatically before every PlatformIO build; gzips `web/` into `include/WebAssets.h` (generated, not committed). Pages are served gzip-compressed with ETags, so repeat visits get a 304.

## Hardware notes
- Fit a pull-down resistor (10 kΩ) on the SSR input. During reset/boot and flashing, ESP32 pins float, and a floating SSR input can switch on.
- A thermal fuse or independent over-temperature cut-out in series with the contactor is strongly recommended. Firmware is not a safety device.

## Firmware version
Change only `OPENKILN_VERSION` in `include/Version.h` before publishing a new firmware release.
