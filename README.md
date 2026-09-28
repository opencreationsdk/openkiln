# OpenKiln

Standalone OpenKiln controller with local web UI and persistent settings.

## Pages
- `/` Oven dashboard: status, temperature, target, SSR duty, controls, saved-profile selector and selected profile graph.
- `/profiles` Profile library/editor: create, edit, graph, save, select and delete profiles.
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
Open Setup -> Firmware OTA and upload PlatformIO `.pio/build/esp32dev/firmware.bin`. Firmware update is blocked during RUNNING/PAUSED and heating is forced off during update.

## First boot / recovery
The controller always starts its fallback AP. Defaults are still in `include/Config.h`; after first login use Setup and reboot. Keep the AP password at least 8 characters.

## Build
Open in VS Code + PlatformIO, build environment `esp32dev`, then upload over USB for the initial installation. Subsequent firmware can be uploaded from Setup.


## Firmware version
Change only `OPENKILN_VERSION` in `include/Version.h` before publishing a new firmware release.
