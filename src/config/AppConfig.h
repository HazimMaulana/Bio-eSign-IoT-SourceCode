#pragma once

#include <Arduino.h>

namespace AppConfig {
  // MQTT
  static const char* MQTT_HOST = "43.156.127.102";
  static constexpr uint16_t MQTT_PORT = 1883;
  static const char* MQTT_USERNAME = "backend_service";
  static const char* MQTT_PASSWORD = "passwordbe";
  static const char* DEVICE_ID_PREFIX = "EPS-32 S3";
  static constexpr uint16_t MQTT_BUFFER_SIZE = 6144;

  inline String macAddressCompact() {
    uint64_t mac = ESP.getEfuseMac();
    char buffer[13];
    snprintf(
      buffer,
      sizeof(buffer),
      "%04X%08X",
      (uint16_t)(mac >> 32),
      (uint32_t)mac
    );
    return String(buffer);
  }

  inline const String& deviceId() {
    static String id = String(DEVICE_ID_PREFIX) + "-" + macAddressCompact();
    return id;
  }

  inline String deviceTopic(const char* suffix) {
    return String("presence/devices/") + deviceId() + "/" + suffix;
  }

  inline String serverTopic(const char* suffix) {
    return String("presence/server/") + deviceId() + "/" + suffix;
  }

  // Topics
  static const char* TOPIC_PROVISIONING_DISCOVER = "presence/provisioning/discover";
  static const char* TOPIC_REGISTRASI        = "presence/mahasiswa/registrasi";
  static const char* TOPIC_SESSION_CLEAR     = "presence/mahasiswa/session/clear";
  static const char* TOPIC_REGISTRASI_TEMPLATE_PREFIX = "presence/mahasiswa/registrasi/template";

  inline String topicDeviceConfig() { return deviceTopic("config"); }
  inline String topicAttendanceAck() { return serverTopic("attendance/ack"); }
  inline String topicPresensi() { return deviceTopic("attendance"); }
  inline String topicMahasiswa() { return deviceTopic("catalog"); }
  inline String topicTemplateReq() { return deviceTopic("template/request"); }
  inline String topicTemplateManifest() { return deviceTopic("template/manifest"); }
  inline String topicTemplateChunk() { return deviceTopic("template/chunk"); }
  inline String topicTemplateChunkWildcard() { return deviceTopic("template/chunk/#"); }
  inline String topicTemplateAck() { return deviceTopic("template/ack"); }
  inline String topicStatus() { return deviceTopic("status"); }
  inline String topicCommand() { return deviceTopic("command"); }

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
