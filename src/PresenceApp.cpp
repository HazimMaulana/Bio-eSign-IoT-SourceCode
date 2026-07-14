#include "PresenceApp.h"
#include "config/AppConfig.h"
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <Update.h>
#include <WiFiClientSecure.h>

void PresenceApp::mqttThunk(void* context, char* topic, byte* payload, unsigned int length) {
  ((PresenceApp*)context)->handleMqttMessage(topic, payload, length);
}

void PresenceApp::backgroundThunk(void* context) {
  ((PresenceApp*)context)->serviceBackground();
}

void PresenceApp::begin() {
  Serial.begin(115200);
  delay(100);
  Serial.println("\n=== ESP32-S3 R302 + MQTT + EEZ UI OOP ===");

  ui_.begin();

  pinMode(AppConfig::BTN_PIN, INPUT_PULLUP);
  pinMode(AppConfig::TOUCH_PIN, INPUT_PULLUP);
  Serial.printf("Touch GPIO=%d (active=%s)\n", AppConfig::TOUCH_PIN, (AppConfig::TOUCH_ACTIVE_LEVEL == HIGH ? "HIGH" : "LOW"));
  Serial.printf("Button GPIO=%d (ke GND saat ditekan)\n", AppConfig::BTN_PIN);
  Serial.printf("Buzzer GPIO=%d\n", AppConfig::BUZZER_PIN);

  buzzer_.begin();
  fingerprint_.setBuzzer(&buzzer_);
  fingerprint_.setBackgroundCallback(PresenceApp::backgroundThunk, this);

  ui_.sensorChecking();
  fingerprint_.begin();
  if (!fingerprint_.verifyPassword()) {
    Serial.println("[FAIL] Sensor tidak terdeteksi. Cek VCC/GND dan RX/TX.");
    ui_.showFail("R302 not detected", "Check VCC, GND, TX, RX");
    buzzer_.fail();
    while (true) ui_.delayWithUi(1000);
  }
  Serial.println("[OK] Sensor terdeteksi");
  fingerprint_.configurePacketSize();
  ui_.sensorReady();

  wifi_.begin(&ui_);
  mqtt_.setMessageHandler(PresenceApp::mqttThunk, this);
  mqtt_.begin(&ui_);
  discoveryPublished_ = false;

  sync_.begin(&mqtt_, &fingerprint_, &ui_);
  registration_.begin(&mqtt_, &fingerprint_, &buzzer_, &ui_, &catalog_);
  attendance_.begin(&fingerprint_, &catalog_, &mqtt_, &buzzer_, &ui_, &sync_);
  attendance_.setBackgroundCallback(PresenceApp::backgroundThunk, this);

  sync_.reset("boot");
  if (AppConfig::CLEAR_SENSOR_TEMPLATES_ON_BOOT) {
    Serial.println("[BOOT] Mengosongkan template sensor...");
    fingerprint_.clearAllTemplates();
  }

  ui_.showSyncOnlySensorPanel("Waiting sync...");
  if (wifi_.isOnline() && mqtt_.isConnected()) {
    mqtt_.publishDiscovery(wifi_.ipString());
    discoveryPublished_ = true;
  }

  fingerprint_.printInfo(nextId_);

  Serial.println("\nInstruksi:");
  Serial.println("  Tempelkan jari ke sensor -> presensi otomatis");
}

void PresenceApp::loop() {
  serviceBackground();
  processPendingConfigSyncRequest();

  registration_.process(sync_.done(), standbyMode_, activeClassName_);
  attendance_.poll(standbyMode_, registration_.inProgress(), registration_.requested());

  if (!standbyMode_ && !registration_.inProgress() && readButtonPressedOnce()) {
    registration_.startSerialWizard(nextId_);
    fingerprint_.printInfo(nextId_);
  }

  static uint32_t lastStatusMs = 0;
  if (millis() - lastStatusMs > 10000) {
    lastStatusMs = millis();
    publishStatus(standbyMode_ ? "standby" : (sync_.done() ? "ready" : "preparing"));
  }

  ui_.delayWithUi(10);
}

void PresenceApp::serviceBackground() {
  ui_.tick();
  wifi_.ensureConnected(&ui_);
  mqtt_.setSendingEnabled(wifi_.isOnline());
  if (wifi_.isOnline()) {
    mqtt_.ensureConnected(&ui_);
  }
  mqtt_.loop();
  if (wifi_.isOnline() && mqtt_.isConnected() && !discoveryPublished_) {
    if (mqtt_.publishDiscovery(wifi_.ipString())) {
      discoveryPublished_ = true;
    }
  }
  if (!wifi_.isOnline()) {
    discoveryPublished_ = false;
  }
  sync_.processPendingFinalization(activeClassName_);
}

void PresenceApp::publishStatus(const char* status) {
  mqtt_.publishStatus(status, wifi_.ipString(), true);
}

void PresenceApp::enterStandbyMode(bool clearTemplates) {
  Serial.println("[STANDBY] Enter standby mode");
  pendingConfigSync_ = false;
  standbyMode_ = true;
  sync_.reset("standby");
  activeClassCode_ = "";
  activeClassName_ = "";
  catalog_.clear();
  attendance_.setActiveClass(activeClassCode_, activeClassName_);
  ui_.showStandbyPanel();

  if (clearTemplates) {
    Serial.println("[STANDBY] Clearing sensor templates");
    fingerprint_.clearAllTemplates();
  }

  uint16_t storedCount = fingerprint_.templateCount();
  Serial.printf("[STANDBY] Templates currently stored in sensor: %d\n", storedCount);

  publishStatus("standby");
}

void PresenceApp::setActiveClass(const String& code, const String& name) {
  if (code.length() == 0) return;
  if (standbyMode_) {
    Serial.println("[STANDBY] Exit standby for class sync");
    standbyMode_ = false;
  }
  activeClassCode_ = code;
  activeClassName_ = name.length() > 0 ? name : code;
  attendance_.setActiveClass(activeClassCode_, activeClassName_);

  Serial.print("[INFO] Kelas aktif diubah via command: ");
  Serial.println(activeClassCode_);

  ui_.showSyncOnlySensorPanel("Checking...");
  sync_.reset("class_change");
  Serial.println("[SYNC] Wiping sensor templates before class load");
  fingerprint_.clearAllTemplates();
  sync_.setExpected(true);
}

void PresenceApp::scheduleConfigSyncRequest() {
  pendingConfigSync_ = true;
  pendingConfigSyncAtMs_ = millis() + 1500;
}

void PresenceApp::processPendingConfigSyncRequest() {
  if (!pendingConfigSync_) return;
  if ((int32_t)(millis() - pendingConfigSyncAtMs_) < 0) return;

  if (activeClassCode_.length() == 0) {
    pendingConfigSync_ = false;
    return;
  }

  if (sync_.requestSync(activeClassCode_)) {
    Serial.print("[SYNC] Requested sync from assigned config: ");
    Serial.println(activeClassCode_);
    pendingConfigSync_ = false;
  } else {
    Serial.println("[SYNC] Config sync request failed, retrying");
    pendingConfigSyncAtMs_ = millis() + 5000;
  }
}

bool PresenceApp::performFirmwareUpdate(const String& firmwareUrl, const String& firmwareVersion, const String& checksum, bool force) {
  if (!wifi_.isOnline()) {
    Serial.println("[OTA] Update skipped: WiFi is not online");
    return false;
  }

  if (!force && firmwareVersion.length() > 0 && firmwareVersion == AppConfig::FIRMWARE_VERSION) {
    Serial.print("[OTA] Skipped: device already running version ");
    Serial.println(firmwareVersion);
    return false;
  }

  Serial.print("[OTA] Downloading firmware version: ");
  Serial.println(firmwareVersion.length() > 0 ? firmwareVersion : "unknown");

  HTTPClient http;
  WiFiClientSecure secureClient;
  WiFiClient plainClient;
  bool started = false;

  if (firmwareUrl.startsWith("https://")) {
    secureClient.setInsecure();
    started = http.begin(secureClient, firmwareUrl);
  } else {
    started = http.begin(plainClient, firmwareUrl);
  }

  if (!started) {
    Serial.println("[OTA] Failed to start HTTP client");
    return false;
  }

  int httpCode = http.GET();
  if (httpCode != HTTP_CODE_OK) {
    Serial.printf("[OTA] HTTP GET failed: %d\n", httpCode);
    http.end();
    return false;
  }

  int contentLength = http.getSize();
  Stream* stream = http.getStreamPtr();

  if (checksum.length() == 32) {
    Update.setMD5(checksum.c_str());
  }

  if (!Update.begin(contentLength > 0 ? (size_t)contentLength : UPDATE_SIZE_UNKNOWN)) {
    Serial.printf("[OTA] Update.begin failed: %s\n", Update.errorString());
    http.end();
    return false;
  }

  Serial.println("[OTA] Flashing firmware...");
  size_t written = Update.writeStream(*stream);

  if (!Update.end(true)) {
    Serial.printf("[OTA] Update failed after %u bytes: %s\n", (unsigned int)written, Update.errorString());
    http.end();
    return false;
  }

  Serial.printf("[OTA] Update successful, written %u bytes\n", (unsigned int)written);
  http.end();
  delay(1000);
  ESP.restart();
  return true;
}

void PresenceApp::handleMqttMessage(char* topic, byte* payload, unsigned int length) {
  char* raw = (char*)malloc(length + 1);
  if (!raw) return;
  memcpy(raw, payload, length);
  raw[length] = '\0';

  Serial.print("[MQTT] Topic: ");
  Serial.println(topic);

  if (strcmp(topic, AppConfig::topicMahasiswa().c_str()) == 0) {
    if (standbyMode_) {
      Serial.println("[STANDBY] Catalog ignored");
      free(raw);
      return;
    }
    catalog_.handlePayload(raw, length);
    nextId_ = catalog_.updateNextId(nextId_);
  } else if (strcmp(topic, AppConfig::TOPIC_SESSION_CLEAR) == 0) {
    sync_.reset("session_clear");
    fingerprint_.clearAllTemplates();
  } else if (strcmp(topic, AppConfig::topicDeviceConfig().c_str()) == 0) {
    DynamicJsonDocument doc(length + 256);
    if (!deserializeJson(doc, raw, length)) {
      bool assigned = doc["assigned"] | false;
      if (!assigned) {
        bool clearTemplates = doc["clear_templates"] | true;
        enterStandbyMode(clearTemplates);
      } else {
        String classCode = String(doc["class_code"] | "");
        String className = String(doc["class_name"] | classCode);
        if (classCode.length() > 0) {
          Serial.print("[CONFIG] Device assignment config received for class ");
          Serial.println(classCode);
          if (!standbyMode_ && activeClassCode_ == classCode && (sync_.done() || sync_.inProgress() || sync_.expected())) {
            Serial.println("[CONFIG] Duplicate assignment ignored");
          } else {
            setActiveClass(classCode, className);
            scheduleConfigSyncRequest();
          }
        } else {
          Serial.println("[CONFIG] Assigned config ignored: class_code is missing");
        }
      }
    }
  } else if (strcmp(topic, AppConfig::topicCommand().c_str()) == 0) {
    DynamicJsonDocument doc(length + 256);
    if (!deserializeJson(doc, raw, length)) {
      const char* command = doc["command"] | "";
      if (strcmp(command, "STANDBY") == 0) {
        bool clearTemplates = doc["clear_templates"] | true;
        enterStandbyMode(clearTemplates);
      } else if (strcmp(command, "REGISTER") == 0) {
        String nim = String(doc["nim"] | "");
        String nama = String(doc["nama"] | "");
        if (nama.length() == 0) nama = String(doc["name"] | "");
        int slot = doc["slot"] | 1;
        if (slot < 1 || slot > 3) slot = 1;
        if (nim.length() > 0 && nama.length() > 0) {
          bool returnToStandby = standbyMode_;
          if (standbyMode_) {
            Serial.println("[STANDBY] Exit standby for registration");
            standbyMode_ = false;
          }
          registration_.queueRegistration(nim, nama, (uint8_t)slot, returnToStandby);
          Serial.print("[REGISTER] Queued registration for ");
          Serial.print(nim);
          Serial.print(" slot=");
          Serial.println(slot);
          ui_.showRegisterPanel(nama.c_str(), nim.c_str(), "Register diterima\nSiapkan jari di sensor");
          publishStatus("register");
        } else {
          Serial.println("[REGISTER] Ignored: nim/name is missing");
        }
      } else if (strcmp(command, "SET_ACTIVE_CLASS") == 0) {
        pendingConfigSync_ = false;
        String targetClass = String(doc["class_code"] | "");
        String targetClassName = String(doc["class_name"] | targetClass);
        setActiveClass(targetClass, targetClassName);
      } else if (strcmp(command, "FIRMWARE_UPDATE") == 0) {
        String firmwareUrl = String(doc["firmware_url"] | "");
        String firmwareVersion = String(doc["firmware_version"] | "");
        String checksum = String(doc["checksum"] | "");
        bool force = doc["force"] | false;

        if (firmwareUrl.length() == 0) {
          Serial.println("[OTA] Ignored: firmware_url is missing");
        } else {
          Serial.print("[OTA] Firmware update requested from ");
          Serial.println(firmwareUrl);
          performFirmwareUpdate(firmwareUrl, firmwareVersion, checksum, force);
        }
      }
    }
  } else if (strcmp(topic, AppConfig::topicAttendanceAck().c_str()) == 0) {
    DynamicJsonDocument doc(length + 256);
    if (!deserializeJson(doc, raw, length)) {
      const char* status = doc["status"] | "";
      if (strcmp(status, "duplicate") == 0) {
        Serial.println("[ATTENDANCE] Duplicate attendance ACK received");
        ui_.showScanFailed("Anda sudah absen!");
        buzzer_.fail();
        ui_.delayWithUi(1800);
        ui_.showReadyPanel(activeClassName_);
      }
    }
  } else if (strcmp(topic, AppConfig::topicTemplateManifest().c_str()) == 0) {
    if (standbyMode_) {
      Serial.println("[STANDBY] Manifest ignored");
      free(raw);
      return;
    }
    ui_.sensorChecking();
    sync_.handleManifestPayload(raw, length);
    sync_.tryFinalizeSync(activeClassName_);
  } else if (strncmp(topic, AppConfig::topicTemplateChunk().c_str(), AppConfig::topicTemplateChunk().length()) == 0) {
    if (standbyMode_) {
      Serial.println("[STANDBY] Chunk ignored");
      free(raw);
      return;
    }
    ui_.sensorChecking();
    sync_.handleChunkPayload(topic, payload, length);
    sync_.processPendingFinalization(activeClassName_);
    sync_.tryFinalizeSync(activeClassName_);
  }

  free(raw);
}

bool PresenceApp::readButtonPressedOnce() {
  static bool lastStable = HIGH;
  static bool lastRead = HIGH;
  static uint32_t lastChangeMs = 0;
  bool raw = digitalRead(AppConfig::BTN_PIN);

  if (raw != lastRead) {
    lastRead = raw;
    lastChangeMs = millis();
  }

  if ((millis() - lastChangeMs) > AppConfig::DEBOUNCE_MS && raw != lastStable) {
    lastStable = raw;
    if (lastStable == LOW) return true;
  }
  return false;
}
