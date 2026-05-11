#pragma once

#include <Arduino.h>

namespace AppConfig {
  // WiFi
  static const char* WIFI_SSID = "Jimsss";
  static const char* WIFI_PASS = "12345678";

  // MQTT
  static const char* MQTT_HOST = "172.235.244.139";
  static constexpr uint16_t MQTT_PORT = 1883;
  static const char* MQTT_USERNAME = "backend_service";
  static const char* MQTT_PASSWORD = "passwordbe";
  static const char* DEVICE_ID = "ESP32S3_R302_01";
  static constexpr uint16_t MQTT_BUFFER_SIZE = 6144;

  // Topics
  static const char* TOPIC_PRESENSI          = "presence/presensi";
  static const char* TOPIC_MAHASISWA         = "presence/mahasiswa/catalog";
  static const char* TOPIC_REGISTRASI        = "presence/mahasiswa/registrasi";
  static const char* TOPIC_REGISTER_ACK      = "presence/mahasiswa/templates/register_ack/ESP32S3_R302_01";
  static const char* TOPIC_TEMPLATE_REQ      = "presence/mahasiswa/templates/request";
  static const char* TOPIC_TEMPLATE_MANIFEST = "presence/mahasiswa/templates/manifest";
  static const char* TOPIC_TEMPLATE_CHUNK    = "presence/mahasiswa/templates/chunk";
  static const char* TOPIC_TEMPLATE_CHUNK_WILDCARD = "presence/mahasiswa/templates/chunk/#";
  static const char* TOPIC_TEMPLATE_ACK      = "presence/mahasiswa/templates/ack";
  static const char* TOPIC_STATUS            = "presence/device/ESP32S3_R302_01/status";
  static const char* TOPIC_SESSION_CLEAR     = "presence/mahasiswa/session/clear";
  static const char* TOPIC_COMMAND           = "presence/device/ESP32S3_R302_01/command";
  static const char* TOPIC_ATTENDANCE        = "presence/attendance";
  static const char* TOPIC_REGISTRASI_TEMPLATE_PREFIX = "presence/mahasiswa/registrasi/template";

  // Fingerprint R302
  static constexpr int FP_RX = 41;
  static constexpr int FP_TX = 40;
  static constexpr uint32_t FP_BAUD = 57600;

  // LCD
  static constexpr uint16_t LCD_FORCE_ID = 0x6814;

  // Touch R302
  static constexpr int TOUCH_PIN = 21;
  static constexpr uint8_t TOUCH_ACTIVE_LEVEL = LOW;
  static constexpr uint32_t TOUCH_DEBOUNCE_MS = 30;
  static constexpr uint32_t TOUCH_COOLDOWN_MS = 1000;
  static constexpr uint32_t FP_FALLBACK_INTERVAL_MS = 120;
  static constexpr uint32_t FP_SCAN_COOLDOWN_MS = 1200;

  // Buzzer
  static constexpr int BUZZER_PIN = 42;
  static constexpr int BUZZER_CH = 0;
  static constexpr int BUZZER_RES_BITS = 10;
  static constexpr int BUZZER_BASE_FREQ = 2000;

  // Button
  static constexpr int BTN_PIN = 39;
  static constexpr uint32_t DEBOUNCE_MS = 30;
  static constexpr uint32_t SERIAL_INPUT_TIMEOUT_MS = 120000;

  // Runtime behavior
  static constexpr bool CLEAR_SENSOR_TEMPLATES_ON_BOOT = false;
  static constexpr uint8_t ENROLL_RETRY_PER_SLOT = 3;
  static constexpr uint32_t EEZ_BOOT_DURATION_MS = 7000;

  // Limits
  static constexpr size_t MAX_MAHASISWA = 80;
  static constexpr size_t MAX_SYNC_TEMPLATES = 50;
  static constexpr size_t MAX_CHUNKS_PER_TEMPLATE = 16;
  static constexpr size_t MAX_TEMPLATE_BYTES = 2048;
}
