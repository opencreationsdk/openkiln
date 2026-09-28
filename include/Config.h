#pragma once

#include <Arduino.h>

// ---------- Network ----------
// Leave WIFI_SSID empty to use access-point mode only.
constexpr char WIFI_SSID[] = "";
constexpr char WIFI_PASSWORD[] = "";
constexpr char AP_NAME[] = "KilnController";
constexpr char AP_PASSWORD[] = "change-this-ap-password";  // 8+ characters
constexpr char WEB_USER[] = "admin";
constexpr char WEB_PASSWORD[] = "change-me";

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
