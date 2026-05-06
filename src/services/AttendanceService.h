#pragma once

#include <Arduino.h>

class FingerprintService;
class StudentCatalogService;
class MqttService;
class BuzzerService;
class UiService;
class TemplateSyncService;

typedef void (*AttendanceBackgroundCallback)(void* context);

class AttendanceService {
public:
  void begin(FingerprintService* fingerprint, StudentCatalogService* catalog, MqttService* mqtt, BuzzerService* buzzer, UiService* ui, TemplateSyncService* sync);
  void setBackgroundCallback(AttendanceBackgroundCallback cb, void* context);
  void setActiveClass(const String& code, const String& name);
  void poll(bool standbyMode, bool registrationInProgress, bool registrationRequested);
  bool publishPresensi(int fingerId, int confidence, const void* mahasiswaPtr, int slot);

private:
  FingerprintService* fingerprint_ = nullptr;
  StudentCatalogService* catalog_ = nullptr;
  MqttService* mqtt_ = nullptr;
  BuzzerService* buzzer_ = nullptr;
  UiService* ui_ = nullptr;
  TemplateSyncService* sync_ = nullptr;
  AttendanceBackgroundCallback backgroundCb_ = nullptr;
  void* backgroundContext_ = nullptr;

  String activeClassCode_;
  String activeClassName_;
  uint32_t lastFallbackPollMs_ = 0;
  uint32_t lastScanMs_ = 0;

  void background();
  void doPresensiTouchSession();
};
