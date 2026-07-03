#include "RegistrationService.h"
#include "MqttService.h"
#include "FingerprintService.h"
#include "BuzzerService.h"
#include "UiService.h"
#include "StudentCatalogService.h"
#include "../config/AppConfig.h"
#include <ArduinoJson.h>
#include <ctype.h>

void RegistrationService::begin(MqttService* mqtt, FingerprintService* fingerprint, BuzzerService* buzzer, UiService* ui, StudentCatalogService* catalog) {
  mqtt_ = mqtt;
  fingerprint_ = fingerprint;
  buzzer_ = buzzer;
  ui_ = ui;
  catalog_ = catalog;
  if (fingerprint_) fingerprint_->setEnrollLogCallback(RegistrationService::enrollLogThunk, this);
}

bool RegistrationService::inProgress() const { return registrationInProgress_; }
bool RegistrationService::requested() const { return registrationRequested_; }

void RegistrationService::queueRegistration(const String& nim, const String& nama, uint8_t slot, bool returnToStandby) {
  pendingRegistration_.nim = nim;
  pendingRegistration_.nama = nama;
  pendingRegistration_.fingerId[0] = -1;
  pendingRegistration_.fingerId[1] = -1;
  pendingRegistration_.fingerId[2] = -1;
  pendingRegistrationSlot_ = (slot >= 1 && slot <= 3) ? slot : 1;
  registrationReturnToStandby_ = returnToStandby;
  registrationRequested_ = true;
}

bool RegistrationService::publishResult(const Mahasiswa& m, uint8_t slot, uint16_t fingerId, bool success, const char* message, const uint8_t* templateBytes, size_t templateLen) {
  if (!mqtt_ || !mqtt_->isConnected()) return false;

  JsonDocument doc;

  doc["device_id"] = AppConfig::deviceId();
  doc["nim"] = m.nim;
  doc["nama"] = m.nama;
  doc["name"] = m.nama;
  doc["slot"] = slot;
  doc["finger_id"] = fingerId;
  doc["success"] = success;
  doc["message"] = message;
  doc["ts_ms"] = (uint32_t)millis();
  doc["template_format"] = "zfm_template_binary";

  String payload;
  serializeJson(doc, payload);
  bool metaOk = mqtt_->publishString(AppConfig::TOPIC_REGISTRASI, payload);
  Serial.print("Publish registrasi: ");
  Serial.println(metaOk ? "OK" : "FAIL");
  if (metaOk) Serial.println(payload.c_str());

  if (!success || !templateBytes || templateLen == 0) return metaOk;

  char topic[192];
  snprintf(
    topic,
    sizeof(topic),
    "%s/%s/%s/%u/%u",
    AppConfig::TOPIC_REGISTRASI_TEMPLATE_PREFIX,
    AppConfig::deviceId().c_str(),
    m.nim.c_str(),
    (unsigned int)slot,
    (unsigned int)fingerId
  );

  bool templateOk = mqtt_->publishBinary(topic, templateBytes, (unsigned int)templateLen);
  Serial.print("Publish registrasi template binary: ");
  Serial.println(templateOk ? "OK" : "FAIL");
  return metaOk && templateOk;
}

void RegistrationService::enrollLogThunk(void* context, const char* message) {
  if (!context) return;
  ((RegistrationService*)context)->showRegisterLog(message);
}

void RegistrationService::showRegisterLog(const char* message) {
  if (!ui_) return;
  String logText = "Slot ";
  logText += String((unsigned int)activeRegisterSlot_);
  logText += "\n";
  logText += (message && message[0]) ? message : "Menunggu proses register...";
  ui_->showRegisterPanel(activeRegisterName_.c_str(), activeRegisterNim_.c_str(), logText.c_str());
}

uint16_t RegistrationService::allocateFingerprintId(uint16_t& nextId) {
  if (catalog_) nextId = catalog_->updateNextId(nextId);
  uint16_t sensorCandidate = fingerprint_->templateCount() + 1;
  uint16_t candidate = sensorCandidate;
  if (candidate < nextId) candidate = nextId;
  if (candidate == 0) candidate = 1;
  if (candidate > fingerprint_->capacity()) return 0;
  Serial.printf("[REGISTER] Allocated finger_id=%u (sensor_count=%u next_id=%u)\n",
                (unsigned int)candidate, (unsigned int)sensorCandidate - 1, (unsigned int)nextId);
  return candidate;
}

bool RegistrationService::enrollSlotForMahasiswa(Mahasiswa& m, uint8_t slotIdx, uint16_t& nextId) {
  if (!fingerprint_ || slotIdx > 2) return false;

  uint16_t enrollId = 0;
  int32_t currentId = m.fingerId[slotIdx];
  if (currentId > 0) {
    enrollId = (uint16_t)currentId;
    if (enrollId > fingerprint_->capacity()) {
      publishResult(m, (uint8_t)(slotIdx + 1), enrollId, false, "finger_id_di_luar_kapasitas", nullptr, 0);
      return false;
    }
  } else {
    enrollId = allocateFingerprintId(nextId);
    if (enrollId == 0) {
      publishResult(m, (uint8_t)(slotIdx + 1), 0, false, "sensor_penuh", nullptr, 0);
      return false;
    }
    m.fingerId[slotIdx] = enrollId;
  }

  fingerprint_->raw().deleteModel(enrollId);
  activeRegisterName_ = m.nama;
  activeRegisterNim_ = m.nim;
  activeRegisterSlot_ = (uint8_t)(slotIdx + 1);
  showRegisterLog("Siapkan jari untuk didaftarkan");

  bool ok = false;
  for (uint8_t attempt = 1; attempt <= AppConfig::ENROLL_RETRY_PER_SLOT; attempt++) {
    if (attempt > 1) {
      String retryText = "Percobaan ";
      retryText += String((unsigned int)attempt);
      retryText += "\nIkuti tuntunan di layar";
      showRegisterLog(retryText.c_str());
    }
    fingerprint_->waitFingerRemoved();
    ok = fingerprint_->doEnroll(enrollId);
    if (ok) break;
    Serial.println("[WARN] Enroll gagal, coba lagi.");
  }

  if (!ok) {
    publishResult(m, (uint8_t)(slotIdx + 1), enrollId, false, "enroll_gagal_setelah_retry", nullptr, 0);
    return false;
  }

  uint8_t tplBuf[AppConfig::MAX_TEMPLATE_BYTES];
  size_t tplLen = 0;
  bool downloaded = fingerprint_->downloadTemplateBytes(enrollId, tplBuf, sizeof(tplBuf), &tplLen);

  m.fingerId[slotIdx] = enrollId;
  if (enrollId >= nextId) nextId = enrollId + 1;

  if (downloaded) publishResult(m, (uint8_t)(slotIdx + 1), enrollId, true, "enroll_sukses", tplBuf, tplLen);
  else publishResult(m, (uint8_t)(slotIdx + 1), enrollId, true, "enroll_sukses_template_download_failed", nullptr, 0);

  if (catalog_) {
    catalog_->upsertFingerprint(m.nim, m.nama, (uint8_t)(slotIdx + 1), enrollId);
  }

  return true;
}

void RegistrationService::process(bool syncDone, bool standbyMode, const String& activeClassName) {
  if (!registrationRequested_ || registrationInProgress_) return;

  registrationRequested_ = false;
  registrationInProgress_ = true;
  bool returnToStandby = registrationReturnToStandby_;
  registrationReturnToStandby_ = false;

  Mahasiswa m = pendingRegistration_;
  uint8_t slot = pendingRegistrationSlot_;
  if (slot < 1 || slot > 3) slot = 1;

  Serial.print("\n=== MQTT REGISTER ");
  Serial.print(m.nim);
  Serial.print(" SLOT ");
  Serial.print(slot);
  Serial.println(" ===");

  if (ui_) ui_->showRegisterPanel(m.nama.c_str(), m.nim.c_str(), "Menunggu proses register...");
  bool ok = enrollSlotForMahasiswa(m, (uint8_t)(slot - 1), localNextId_);
  if (ui_) ui_->showRegisterPanel(m.nama.c_str(), m.nim.c_str(), ok ? "Register berhasil disimpan" : "Register gagal");

  if (ui_) ui_->delayWithUi(ok ? 1600 : 2200);
  registrationInProgress_ = false;

  if (ui_) {
    if (returnToStandby || standbyMode) ui_->showStandbyPanel();
    else if (syncDone) ui_->showReadyPanel(activeClassName);
    else ui_->showSyncOnlySensorPanel("Waiting templates...");
  }
}

bool RegistrationService::readSerialLine(String& out, uint32_t timeoutMs) {
  out = "";
  uint32_t startMs = millis();
  while ((millis() - startMs) < timeoutMs) {
    while (Serial.available() > 0) {
      char c = (char)Serial.read();
      if (c == '\r') continue;
      if (c == '\n') { out.trim(); return true; }
      if ((c >= 32 && c <= 126) || c == '\t') out += c;
    }
    if (ui_) ui_->delayWithUi(10);
    else delay(10);
  }
  return false;
}

bool RegistrationService::parseUnsignedInt(const String& s, int& outValue) {
  if (s.length() == 0) return false;
  for (size_t i = 0; i < s.length(); i++) if (!isdigit((unsigned char)s[i])) return false;
  long v = s.toInt();
  if (v < 0 || v > 32767) return false;
  outValue = (int)v;
  return true;
}

int RegistrationService::promptMahasiswaIndex() {
  if (!catalog_ || !catalog_->dataReceived()) {
    Serial.println("Data mahasiswa belum diterima dari MQTT topic presence/mahasiswa/catalog.");
    return -1;
  }
  if (catalog_->count() == 0) {
    Serial.println("Daftar mahasiswa kosong.");
    return -1;
  }

  catalog_->printList();
  Serial.printf("Pilih nomor mahasiswa [1-%u], atau 0 untuk batal:\n", (unsigned int)catalog_->count());
  while (true) {
    Serial.print("> ");
    String line;
    if (!readSerialLine(line, AppConfig::SERIAL_INPUT_TIMEOUT_MS)) {
      Serial.println("Timeout input serial, pendaftaran dibatalkan.");
      return -1;
    }
    int chosen = -1;
    if (!parseUnsignedInt(line, chosen)) { Serial.println("Input tidak valid. Masukkan angka."); continue; }
    if (chosen == 0) return -1;
    if (chosen < 1 || chosen > (int)catalog_->count()) { Serial.println("Nomor mahasiswa di luar range."); continue; }
    return chosen - 1;
  }
}

void RegistrationService::startSerialWizard(uint16_t& nextId) {
  Serial.println("\n=== MODE PENDAFTARAN MAHASISWA (BTN 1x) ===");
  int mIdx = promptMahasiswaIndex();
  if (mIdx < 0) return;

  Mahasiswa* m = catalog_->getByIndex((size_t)mIdx);
  if (!m) return;
  for (uint8_t slotIdx = 0; slotIdx < 3; slotIdx++) {
    bool ok = enrollSlotForMahasiswa(*m, slotIdx, nextId);
    if (!ok) {
      Serial.println("[FAIL] Pendaftaran dihentikan.");
      return;
    }
  }
  Serial.println("[OK] Semua fingerprint (1,2,3) berhasil didaftarkan.");
}
