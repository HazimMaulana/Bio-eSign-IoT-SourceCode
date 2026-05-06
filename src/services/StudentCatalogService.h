#pragma once

#include <Arduino.h>
#include <ArduinoJson.h>
#include "../models/Mahasiswa.h"
#include "../config/AppConfig.h"

class StudentCatalogService {
public:
  void clear();
  bool handlePayload(const char* payload, size_t length);
  const Mahasiswa* findByFingerId(uint16_t fingerId, uint8_t* slotOut = nullptr) const;
  Mahasiswa* getByIndex(size_t index);
  const Mahasiswa* getByIndex(size_t index) const;
  bool upsertFingerprint(const String& nim, const String& nama, uint8_t slot, uint16_t fingerId);
  size_t count() const;
  bool dataReceived() const;
  uint16_t updateNextId(uint16_t currentNextId) const;
  void printList() const;

private:
  Mahasiswa mahasiswaList_[AppConfig::MAX_MAHASISWA];
  size_t mahasiswaCount_ = 0;
  bool mahasiswaDataReceived_ = false;

  void applyFromArray(JsonArrayConst arr);
  int32_t toFingerId(JsonVariantConst v) const;
  int32_t readFingerIdField(JsonObjectConst obj, const char* key1, const char* key2, const char* key3) const;
  int32_t readFingerIdFromArray(JsonObjectConst obj, const char* arrKey, size_t idx) const;
  String readNameField(JsonObjectConst obj) const;
};
