#include "StudentCatalogService.h"
#include <ctype.h>
#include <stdlib.h>

void StudentCatalogService::clear() {
  for (size_t i = 0; i < AppConfig::MAX_MAHASISWA; i++) {
    mahasiswaList_[i].nama = "";
    mahasiswaList_[i].nim = "";
    mahasiswaList_[i].fingerId[0] = -1;
    mahasiswaList_[i].fingerId[1] = -1;
    mahasiswaList_[i].fingerId[2] = -1;
  }
  mahasiswaCount_ = 0;
}

size_t StudentCatalogService::count() const { return mahasiswaCount_; }
bool StudentCatalogService::dataReceived() const { return mahasiswaDataReceived_; }

Mahasiswa* StudentCatalogService::getByIndex(size_t index) {
  if (index >= mahasiswaCount_) return nullptr;
  return &mahasiswaList_[index];
}

const Mahasiswa* StudentCatalogService::getByIndex(size_t index) const {
  if (index >= mahasiswaCount_) return nullptr;
  return &mahasiswaList_[index];
}

bool StudentCatalogService::upsertFingerprint(const String& nim, const String& nama, uint8_t slot, uint16_t fingerId) {
  if (nim.length() == 0 || slot < 1 || slot > 3 || fingerId == 0) return false;

  Mahasiswa* target = nullptr;
  for (size_t i = 0; i < mahasiswaCount_; i++) {
    if (mahasiswaList_[i].nim == nim) {
      target = &mahasiswaList_[i];
      break;
    }
  }

  if (!target) {
    if (mahasiswaCount_ >= AppConfig::MAX_MAHASISWA) return false;
    target = &mahasiswaList_[mahasiswaCount_++];
    target->nim = nim;
    target->fingerId[0] = -1;
    target->fingerId[1] = -1;
    target->fingerId[2] = -1;
  }

  target->nama = nama.length() > 0 ? nama : target->nama;
  target->fingerId[slot - 1] = (int32_t)fingerId;
  mahasiswaDataReceived_ = true;

  Serial.printf("[CATALOG] Registered local mapping: %s slot=%u finger_id=%u\n",
                nim.c_str(), (unsigned int)slot, (unsigned int)fingerId);
  return true;
}

int32_t StudentCatalogService::toFingerId(JsonVariantConst v) const {
  if (v.isNull()) return -1;
  if (v.is<int32_t>() || v.is<int>() || v.is<long>()) {
    int32_t id = v.as<int32_t>();
    return (id > 0) ? id : -1;
  }
  if (v.is<const char*>()) {
    const char* s = v.as<const char*>();
    if (!s || s[0] == '\0') return -1;
    char* endPtr = nullptr;
    long parsed = strtol(s, &endPtr, 10);
    if (endPtr == s || *endPtr != '\0') return -1;
    return (parsed > 0) ? (int32_t)parsed : -1;
  }
  return -1;
}

int32_t StudentCatalogService::readFingerIdField(JsonObjectConst obj, const char* key1, const char* key2, const char* key3) const {
  if (obj.containsKey(key1)) return toFingerId(obj[key1]);
  if (key2 && obj.containsKey(key2)) return toFingerId(obj[key2]);
  if (key3 && obj.containsKey(key3)) return toFingerId(obj[key3]);
  return -1;
}

String StudentCatalogService::readNameField(JsonObjectConst obj) const {
  String nama = String(obj["nama"] | "");
  nama.trim();
  if (nama.length() > 0) return nama;
  nama = String(obj["name"] | "");
  nama.trim();
  return nama;
}

int32_t StudentCatalogService::readFingerIdFromArray(JsonObjectConst obj, const char* arrKey, size_t idx) const {
  if (!arrKey) return -1;
  if (!obj[arrKey].is<JsonArrayConst>()) return -1;
  JsonArrayConst arr = obj[arrKey].as<JsonArrayConst>();
  if (idx >= arr.size()) return -1;
  return toFingerId(arr[idx]);
}

void StudentCatalogService::applyFromArray(JsonArrayConst arr) {
  clear();
  for (JsonVariantConst item : arr) {
    if (!item.is<JsonObjectConst>()) continue;
    if (mahasiswaCount_ >= AppConfig::MAX_MAHASISWA) break;

    JsonObjectConst obj = item.as<JsonObjectConst>();
    Mahasiswa& m = mahasiswaList_[mahasiswaCount_];
    m.nama = readNameField(obj);
    m.nim = String(obj["nim"] | "");
    m.fingerId[0] = readFingerIdField(obj, "fingerprint1", "fingerprint_1", "fp1");
    m.fingerId[1] = readFingerIdField(obj, "fingerprint2", "fingerprint_2", "fp2");
    m.fingerId[2] = readFingerIdField(obj, "fingerprint3", "fingerprint_3", "fp3");
    if (m.fingerId[0] < 0) m.fingerId[0] = readFingerIdFromArray(obj, "fingerprints", 0);
    if (m.fingerId[1] < 0) m.fingerId[1] = readFingerIdFromArray(obj, "fingerprints", 1);
    if (m.fingerId[2] < 0) m.fingerId[2] = readFingerIdFromArray(obj, "fingerprints", 2);
    mahasiswaCount_++;
  }
  mahasiswaDataReceived_ = true;
  Serial.print("Data mahasiswa diperbarui: ");
  Serial.println((unsigned int)mahasiswaCount_);
}

bool StudentCatalogService::handlePayload(const char* payload, size_t length) {
  JsonDocument doc;
  DeserializationError err = deserializeJson(doc, payload, length);
  if (err) {
    Serial.print("Gagal parse JSON mahasiswa: ");
    Serial.println(err.c_str());
    return false;
  }

  if (doc.is<JsonArrayConst>()) { applyFromArray(doc.as<JsonArrayConst>()); return true; }
  if (!doc.is<JsonObjectConst>()) return false;

  JsonObjectConst root = doc.as<JsonObjectConst>();
  if (root["mahasiswa"].is<JsonArrayConst>()) { applyFromArray(root["mahasiswa"].as<JsonArrayConst>()); return true; }
  if (root["data"].is<JsonArrayConst>())      { applyFromArray(root["data"].as<JsonArrayConst>()); return true; }
  if (root["items"].is<JsonArrayConst>())     { applyFromArray(root["items"].as<JsonArrayConst>()); return true; }
  if (root["students"].is<JsonArrayConst>())  { applyFromArray(root["students"].as<JsonArrayConst>()); return true; }

  if (root.containsKey("nim") && (root.containsKey("nama") || root.containsKey("name"))) {
    clear();
    Mahasiswa& m = mahasiswaList_[0];
    m.nama = readNameField(root);
    m.nim = String(root["nim"] | "");
    m.fingerId[0] = readFingerIdField(root, "fingerprint1", "fingerprint_1", "fp1");
    m.fingerId[1] = readFingerIdField(root, "fingerprint2", "fingerprint_2", "fp2");
    m.fingerId[2] = readFingerIdField(root, "fingerprint3", "fingerprint_3", "fp3");
    if (m.fingerId[0] < 0) m.fingerId[0] = readFingerIdFromArray(root, "fingerprints", 0);
    if (m.fingerId[1] < 0) m.fingerId[1] = readFingerIdFromArray(root, "fingerprints", 1);
    if (m.fingerId[2] < 0) m.fingerId[2] = readFingerIdFromArray(root, "fingerprints", 2);
    mahasiswaCount_ = 1;
    mahasiswaDataReceived_ = true;
    Serial.println("Data mahasiswa diperbarui: 1");
    return true;
  }

  Serial.println("Payload mahasiswa tidak memiliki field array yang dikenali.");
  return false;
}

uint16_t StudentCatalogService::updateNextId(uint16_t currentNextId) const {
  int32_t maxKnownId = 0;
  for (size_t i = 0; i < mahasiswaCount_; i++) {
    for (int s = 0; s < 3; s++) if (mahasiswaList_[i].fingerId[s] > maxKnownId) maxKnownId = mahasiswaList_[i].fingerId[s];
  }
  if (maxKnownId > 0 && maxKnownId < 65535) {
    uint16_t candidate = (uint16_t)(maxKnownId + 1);
    if (candidate > currentNextId) return candidate;
  }
  return currentNextId;
}

const Mahasiswa* StudentCatalogService::findByFingerId(uint16_t fingerId, uint8_t* slotOut) const {
  for (size_t i = 0; i < mahasiswaCount_; i++) {
    for (uint8_t s = 0; s < 3; s++) {
      if (mahasiswaList_[i].fingerId[s] == (int32_t)fingerId) {
        if (slotOut) *slotOut = s;
        return &mahasiswaList_[i];
      }
    }
  }
  return nullptr;
}

void StudentCatalogService::printList() const {
  Serial.println("\nDaftar mahasiswa:");
  for (size_t i = 0; i < mahasiswaCount_; i++) {
    const Mahasiswa& m = mahasiswaList_[i];
    Serial.printf("  %u) %s - %s | fp1=%d fp2=%d fp3=%d\n", (unsigned int)(i + 1), m.nim.c_str(), m.nama.c_str(), (int)m.fingerId[0], (int)m.fingerId[1], (int)m.fingerId[2]);
  }
}
