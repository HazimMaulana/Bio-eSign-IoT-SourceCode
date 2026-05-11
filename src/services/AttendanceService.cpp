#include "AttendanceService.h"
#include "FingerprintService.h"
#include "StudentCatalogService.h"
#include "MqttService.h"
#include "BuzzerService.h"
#include "UiService.h"
#include "TemplateSyncService.h"
#include "../models/Mahasiswa.h"
#include "../config/AppConfig.h"
#include <ArduinoJson.h>

static const uint32_t TOUCH_SCAN_START_DELAY_MS = 0;
static const uint32_t TOUCH_SCAN_TIMEOUT_MS     = 2000;
static const uint32_t AFTER_SCAN_COOLDOWN_MS    = 1200;

void AttendanceService::begin(FingerprintService* fingerprint, StudentCatalogService* catalog, MqttService* mqtt, BuzzerService* buzzer, UiService* ui, TemplateSyncService* sync) {
  fingerprint_ = fingerprint;
  catalog_ = catalog;
  mqtt_ = mqtt;
  buzzer_ = buzzer;
  ui_ = ui;
  sync_ = sync;
}

void AttendanceService::setBackgroundCallback(AttendanceBackgroundCallback cb, void* context) {
  backgroundCb_ = cb;
  backgroundContext_ = context;
}

void AttendanceService::background() {
  if (backgroundCb_) backgroundCb_(backgroundContext_);
  else delay(1);
}

void AttendanceService::setActiveClass(const String& code, const String& name) {
  activeClassCode_ = code;
  activeClassName_ = name;
}

bool AttendanceService::publishPresensi(int fingerId, int confidence, const void* mahasiswaPtr, int slot) {
  if (!mqtt_ || !mqtt_->isConnected()) return false;
  const Mahasiswa* m = (const Mahasiswa*)mahasiswaPtr;

  JsonDocument doc;
  doc["device_id"] = AppConfig::DEVICE_ID;
  doc["finger_id"] = fingerId;
  doc["confidence"] = confidence;
  doc["ts_ms"] = (uint32_t)millis();
  if (activeClassCode_.length() > 0) doc["class_code"] = activeClassCode_;

  if (m) {
    doc["nim"] = m->nim;
    doc["nama"] = m->nama;
    doc["name"] = m->nama;
    if (slot >= 1 && slot <= 3) doc["slot"] = slot;
  }

  char payload[320];
  size_t n = serializeJson(doc, payload, sizeof(payload));
  if (n == 0) {
    Serial.println("Publish presensi: FAIL (serialize)");
    return false;
  }

  bool ok = mqtt_->publish(AppConfig::TOPIC_PRESENSI, payload);
  Serial.print("Publish presensi: ");
  Serial.println(ok ? "OK" : "FAIL");
  if (ok) Serial.println(payload);
  return ok;
}

void AttendanceService::poll(bool standbyMode, bool registrationInProgress, bool registrationRequested) {
  if (standbyMode || registrationInProgress || registrationRequested || !sync_ || !sync_->done()) return;

  uint32_t now = millis();
  if ((now - lastScanMs_) < AFTER_SCAN_COOLDOWN_MS) return;
  if ((now - lastFallbackPollMs_) < AppConfig::FP_FALLBACK_INTERVAL_MS) return;
  lastFallbackPollMs_ = now;

  uint8_t p = fingerprint_->getImage();
  if (p == FINGERPRINT_NOFINGER) return;
  if (p != FINGERPRINT_OK) {
    Serial.print("[FP] poll getImage err=0x");
    Serial.println(p, HEX);
    return;
  }

  doPresensiTouchSession();
}

void AttendanceService::doPresensiTouchSession() {
  if ((millis() - lastScanMs_) < AFTER_SCAN_COOLDOWN_MS) return;

  Serial.println("\n=== PRESENSI (AUTO SCAN) ===");
  if (ui_) ui_->showScanning("Checking session...");

  if (!sync_ || !sync_->done()) {
    Serial.println("[LOCK] Presensi belum aktif (sync belum selesai).");
    if (ui_) ui_->showFail("Device not ready", "Template sync is not complete");
    if (buzzer_) buzzer_->locked();
    lastScanMs_ = millis();
    if (ui_) {
      ui_->delayWithUi(1200);
      ui_->showReadyPanel(activeClassName_, "Waiting for session data");
    }
    return;
  }

  if (ui_) ui_->delayWithUi(TOUCH_SCAN_START_DELAY_MS);
  if (ui_) ui_->showScanning("Reading fingerprint...");

  uint8_t p = FINGERPRINT_NOFINGER;
  uint32_t start = millis();
  while ((millis() - start) < TOUCH_SCAN_TIMEOUT_MS) {
    p = fingerprint_->getImage();
    if (p == FINGERPRINT_OK) break;
    background();
    if (ui_) ui_->delayWithUi(40);
    else delay(40);
  }

  if (p != FINGERPRINT_OK) {
    Serial.print("[FAIL] getImage timeout/err code=0x");
    Serial.println(p, HEX);
    if (ui_) ui_->showScanFailed();
    if (buzzer_) buzzer_->fail();
    lastScanMs_ = millis();
    if (ui_) {
      ui_->delayWithUi(1400);
      ui_->showReadyPanel(activeClassName_);
    }
    return;
  }

  if (ui_) ui_->showScanning("Converting image...");
  p = fingerprint_->image2Tz();
  if (p != FINGERPRINT_OK) {
    Serial.print("[FAIL] image2Tz: 0x");
    Serial.println(p, HEX);
    if (ui_) ui_->showScanFailed();
    if (buzzer_) buzzer_->fail();
    lastScanMs_ = millis();
    if (ui_) { ui_->delayWithUi(1400); ui_->showReadyPanel(activeClassName_); }
    return;
  }

  if (ui_) ui_->showScanning("Matching identity...");
  p = fingerprint_->fingerFastSearch();
  if (p == FINGERPRINT_OK) {
    int fingerId = fingerprint_->matchedFingerId();
    int confidence = fingerprint_->matchedConfidence();
    Serial.printf("[OK] MATCH ID=%d confidence=%d\n", fingerId, confidence);

    uint8_t slotIdx = 0;
    const Mahasiswa* matched = catalog_ ? catalog_->findByFingerId((uint16_t)fingerId, &slotIdx) : nullptr;
    bool pubOk = false;

    if (matched) {
      pubOk = publishPresensi(fingerId, confidence, matched, (int)(slotIdx + 1));
      char detail[64];
      snprintf(detail, sizeof(detail), "Status: Present | Conf: %d", confidence);
      if (ui_) ui_->showSuccess(matched->nama.c_str(), matched->nim.c_str(), pubOk ? detail : "MQTT publish failed");
    } else {
      pubOk = publishPresensi(fingerId, confidence, nullptr, -1);
      if (ui_) ui_->showSuccess("Unknown student", "-", pubOk ? "Fingerprint matched, catalog missing" : "MQTT publish failed");
    }

    if (buzzer_) pubOk ? buzzer_->success() : buzzer_->fail();
  } else if (p == FINGERPRINT_NOTFOUND) {
    Serial.println("[FAIL] Sidik jari tidak dikenal");
    if (ui_) ui_->showScanFailed();
    if (buzzer_) buzzer_->fail();
  } else {
    Serial.print("[FAIL] Search error: 0x");
    Serial.println(p, HEX);
    if (ui_) ui_->showScanFailed();
    if (buzzer_) buzzer_->fail();
  }

  lastScanMs_ = millis();
  if (ui_) {
    ui_->delayWithUi(2500);
    ui_->showReadyPanel(activeClassName_);
  }

  Serial.println("[INFO] Tunggu jari diangkat...");
  while (fingerprint_->getImage() != FINGERPRINT_NOFINGER) {
    background();
    if (ui_) ui_->delayWithUi(50);
    else delay(50);
  }
}
