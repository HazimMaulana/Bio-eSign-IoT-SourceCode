#pragma once

#include <Arduino.h>
#include "services/WifiService.h"
#include "services/MqttService.h"
#include "services/BuzzerService.h"
#include "services/UiService.h"
#include "services/FingerprintService.h"
#include "services/StudentCatalogService.h"
#include "services/TemplateSyncService.h"
#include "services/RegistrationService.h"
#include "services/AttendanceService.h"

class PresenceApp {
public:
  void begin();
  void loop();

private:
  WifiService wifi_;
  MqttService mqtt_;
  BuzzerService buzzer_;
  UiService ui_;
  FingerprintService fingerprint_;
  StudentCatalogService catalog_;
  TemplateSyncService sync_;
  RegistrationService registration_;
  AttendanceService attendance_;

  uint16_t nextId_ = 1;
  bool standbyMode_ = false;
  String activeClassCode_;
  String activeClassName_;

  static void mqttThunk(void* context, char* topic, byte* payload, unsigned int length);
  static void backgroundThunk(void* context);

  void handleMqttMessage(char* topic, byte* payload, unsigned int length);
  void serviceBackground();
  void enterStandbyMode(bool clearTemplates);
  void setActiveClass(const String& code, const String& name);
  void publishStatus(const char* status);

  bool readButtonPressedOnce();
};
