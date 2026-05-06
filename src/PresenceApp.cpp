#include "PresenceApp.h"
#include "config/AppConfig.h"
#include <ArduinoJson.h>

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
  publishStatus("online");

  sync_.begin(&mqtt_, &fingerprint_, &ui_);
  registration_.begin(&mqtt_, &fingerprint_, &buzzer_, &ui_, &catalog_);
  attendance_.begin(&fingerprint_, &catalog_, &mqtt_, &buzzer_, &ui_, &sync_);
  attendance_.setBackgroundCallback(PresenceApp::backgroundThunk, this);

  sync_.reset("boot");
  if (AppConfig::CLEAR_SENSOR_TEMPLATES_ON_BOOT) {
    Serial.println("[BOOT] Mengosongkan template sensor...");
    fingerprint_.clearAllTemplates();
  }

  ui_.sensorChecking();
  sync_.requestSync();
  sync_.setExpected(true);
  sync_.publishAck("sync_requested", "", "boot_request");

  fingerprint_.printInfo(nextId_);

  Serial.println("\nInstruksi:");
  Serial.println("  Tempelkan jari ke sensor -> presensi otomatis");
}

void PresenceApp::loop() {
  serviceBackground();

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
  mqtt_.ensureConnected(&ui_);
  mqtt_.loop();
  sync_.processPendingFinalization(activeClassName_);
}

void PresenceApp::publishStatus(const char* status) {
  mqtt_.publishStatus(status, wifi_.ipString(), true);
}

void PresenceApp::enterStandbyMode(bool clearTemplates) {
  Serial.println("[STANDBY] Enter standby mode");
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

void PresenceApp::handleMqttMessage(char* topic, byte* payload, unsigned int length) {
  char* raw = (char*)malloc(length + 1);
  if (!raw) return;
  memcpy(raw, payload, length);
  raw[length] = '\0';

  Serial.print("[MQTT] Topic: ");
  Serial.println(topic);

  if (strcmp(topic, AppConfig::TOPIC_MAHASISWA) == 0) {
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
  } else if (strcmp(topic, AppConfig::TOPIC_COMMAND) == 0) {
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
          ui_.showRegisterPanel(nama.c_str(), nim.c_str(), "Scan your fingerprint to register");
          publishStatus("register");
        }
      } else if (strcmp(command, "SET_ACTIVE_CLASS") == 0) {
        String targetClass = String(doc["class_code"] | "");
        String targetClassName = String(doc["class_name"] | targetClass);
        setActiveClass(targetClass, targetClassName);
      }
    }
  } else if (strcmp(topic, AppConfig::TOPIC_TEMPLATE_MANIFEST) == 0) {
    if (standbyMode_) {
      Serial.println("[STANDBY] Manifest ignored");
      free(raw);
      return;
    }
    ui_.sensorChecking();
    sync_.handleManifestPayload(raw, length);
    sync_.tryFinalizeSync(activeClassName_);
  } else if (strncmp(topic, AppConfig::TOPIC_TEMPLATE_CHUNK, strlen(AppConfig::TOPIC_TEMPLATE_CHUNK)) == 0) {
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
