#pragma once

#include <Arduino.h>
#include "../models/Mahasiswa.h"

class MqttService;
class FingerprintService;
class BuzzerService;
class UiService;
class StudentCatalogService;

class RegistrationService {
public:
  void begin(MqttService* mqtt, FingerprintService* fingerprint, BuzzerService* buzzer, UiService* ui, StudentCatalogService* catalog);

  void queueRegistration(const String& nim, const String& nama, uint8_t slot, bool returnToStandby);
  void process(bool syncDone, bool standbyMode, const String& activeClassName);
  bool inProgress() const;
  bool requested() const;

  bool enrollSlotForMahasiswa(Mahasiswa& m, uint8_t slotIdx, uint16_t& nextId);
  void startSerialWizard(uint16_t& nextId);

private:
  MqttService* mqtt_ = nullptr;
  FingerprintService* fingerprint_ = nullptr;
  BuzzerService* buzzer_ = nullptr;
  UiService* ui_ = nullptr;
  StudentCatalogService* catalog_ = nullptr;

  bool registrationRequested_ = false;
  bool registrationInProgress_ = false;
  bool registrationReturnToStandby_ = false;
  Mahasiswa pendingRegistration_;
  uint8_t pendingRegistrationSlot_ = 1;
  uint16_t localNextId_ = 1;
  String activeRegisterName_;
  String activeRegisterNim_;
  uint8_t activeRegisterSlot_ = 1;

  static void enrollLogThunk(void* context, const char* message);
  bool publishResult(const Mahasiswa& m, uint8_t slot, uint16_t fingerId, bool success, const char* message, const uint8_t* templateBytes, size_t templateLen);
  void showRegisterLog(const char* message);
  uint16_t allocateFingerprintId(uint16_t& nextId);
  bool readSerialLine(String& out, uint32_t timeoutMs);
  bool parseUnsignedInt(const String& s, int& outValue);
  int promptMahasiswaIndex();
};
