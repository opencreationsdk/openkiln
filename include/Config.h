#pragma once

#include <Arduino.h>

// ---------- Network ----------
// Leave WIFI_SSID empty to use access-point mode only.
constexpr char WIFI_SSID[] = "";
constexpr char WIFI_PASSWORD[] = "";
constexpr char AP_NAME[] = "KilnController";
constexpr char AP_PASSWORD[] = "change-this-ap-password";  // 8+ characters
constexpr char WEB_USER[] = "admin";
constexpr char WEB_PASSWORD[] = "admin";

// ---------- MQTT ----------
// Leave MQTT_HOST empty to disable MQTT. MQTT requires station-mode Wi-Fi.
constexpr char MQTT_HOST[] = "";
constexpr uint16_t MQTT_PORT = 1883;
constexpr char MQTT_USER[] = "";
constexpr char MQTT_PASSWORD[] = "";
constexpr char MQTT_TOPIC[] = "kiln/controller";  // No trailing slash.
constexpr uint32_t MQTT_PUBLISH_INTERVAL_MS = 5000;

// MQTT "stop" is always accepted because it can only remove heat.
// Other commands require MQTT_ALLOW_CONTROL; "start" additionally requires
// MQTT_ALLOW_REMOTE_START. Keep remote start disabled unless risk-assessed.
constexpr bool MQTT_ALLOW_CONTROL = false;
constexpr bool MQTT_ALLOW_REMOTE_START = false;

// ---------- Hardware ----------
// ESP32 VSPI defaults: SCK=18, MISO=19. MOSI=23 is only used by MAX31856.
constexpr uint8_t PIN_THERMO_SCK = 18;
constexpr uint8_t PIN_THERMO_MISO = 19;
constexpr uint8_t PIN_THERMO_MOSI = 23;
constexpr uint8_t PIN_THERMO_CS = 5;
constexpr uint8_t PIN_SSR = 26;
constexpr bool SSR_ACTIVE_HIGH = true;

enum class ThermocoupleChip { MAX31855, MAX31856 };
constexpr ThermocoupleChip THERMOCOUPLE_CHIP = ThermocoupleChip::MAX31855;

// MAX31856 only: 0=B, 1=E, 2=J, 3=K, 4=N, 5=R, 6=S, 7=T.
constexpr uint8_t MAX31856_TYPE = 3;
constexpr bool MAINS_IS_50_HZ = true;

// ---------- Thermocouple fault checking ----------
// true  = normal operation (recommended). Any sensor fault stops the firing.
// false = sensor checks disabled, for bench testing without a thermocouple or a
//         sensor that reports spurious faults (e.g. a grounded-junction thermocouple
//         makes a MAX31855 report "shorted to ground"). When false:
//           - amplifier fault bits are ignored and the reading is used if plausible
//           - sensor errors and timeouts never fault a firing; Start/Resume work without a sensor
//           - with no usable reading the SSR stays OFF and the profile clock still runs (dry run)
//         Still enforced: the SSR never switches on without a valid reading, and
//         EMERGENCY_SHUTOFF_C is always checked.
//         WARNING: with fault bits ignored, a broken thermocouple can return a plausible
//         but wrong value and the kiln would heat on it. Never fire unattended with false.
constexpr bool SENSOR_CHECK_ENABLED = true;

// ---------- Control and safety (all temperatures are Celsius) ----------
constexpr float THERMOCOUPLE_OFFSET_C = 0.0f;
constexpr float EMERGENCY_SHUTOFF_C = 1260.0f;
constexpr uint32_t SENSOR_INTERVAL_MS = 500;
constexpr uint32_t CONTROL_INTERVAL_MS = 1000;
constexpr uint32_t SSR_WINDOW_MS = 2000;  // Use an SSR, not a mechanical relay.
constexpr uint32_t SENSOR_TIMEOUT_MS = 5000;

// These are safe starting values only. Tune them for the actual kiln.
constexpr float PID_KP = 0.060f;
constexpr float PID_KI = 0.00040f;
constexpr float PID_KD = 0.250f;
constexpr float PID_CONTROL_WINDOW_C = 8.0f;
constexpr float INTEGRAL_LIMIT = 0.80f;

constexpr bool KILN_MUST_CATCH_UP = true;
constexpr float CATCH_UP_WINDOW_C = 15.0f;
constexpr float THROTTLE_BELOW_C = 150.0f;
constexpr float THROTTLE_MAX_DUTY = 0.25f;

constexpr bool AUTO_RESTART = true;
constexpr uint32_t AUTO_RESTART_WINDOW_SECONDS = 15 * 60;
constexpr uint32_t CHECKPOINT_INTERVAL_MS = 30000;
// Recovery needs NTP time to know how long power was off. Give up after this.
constexpr uint32_t AUTO_RESTART_TIME_WAIT_MS = 90000;

// ---------- Real-time control task ----------
// The sensor/PID/SSR loop runs in its own high-priority task so that slow
// network calls (TLS, MQTT reconnects, InfluxDB) can never hold the SSR on.
constexpr uint32_t CONTROL_TICK_MS = 20;        // SSR resolution: 1 % of a 2 s window
constexpr uint8_t SENSOR_FAULT_COUNT = 3;       // consecutive bad reads before a firing faults
constexpr float PID_D_FILTER = 0.30f;           // derivative low-pass factor (1.0 = unfiltered)
// Independent heat watchdog: if the control task stops running, force the SSR
// off after HEAT_WATCHDOG_MS and reboot after HEAT_WATCHDOG_RESET_MS.
constexpr uint32_t HEAT_WATCHDOG_MS = 1500;
constexpr uint32_t HEAT_WATCHDOG_RESET_MS = 10000;

// ---------- Network housekeeping ----------
constexpr uint32_t HTTP_TIMEOUT_MS = 5000;
constexpr uint32_t WIFI_RETRY_INTERVAL_MS = 30000;
