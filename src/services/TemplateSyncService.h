#pragma once

#include <Arduino.h>
#include "../models/TemplateSyncModels.h"
#include <ArduinoJson.h>

class MqttService;
class FingerprintService;
class UiService;

class TemplateSyncService {
public:
  void begin(MqttService* mqtt, FingerprintService* fingerprint, UiService* ui);
  void reset(const char* reason);
  void setExpected(bool expected);
  bool expected() const;
  bool done() const;
  bool inProgress() const;
  const TemplateSyncState& state() const;

  bool requestSync();
  bool publishAck(const char* eventName, const char* templateUid, const char* message);
  void handleManifestPayload(const char* payload, size_t length);
  void handleChunkPayload(const char* topic, const uint8_t* payload, size_t length);
  void processPendingFinalization(const String& activeClassName);
  void tryFinalizeSync(const String& activeClassName);

private:
  MqttService* mqtt_ = nullptr;
  FingerprintService* fingerprint_ = nullptr;
  UiService* ui_ = nullptr;

  bool syncDone_ = false;
  bool syncExpected_ = false;
  TemplateSyncState state_;
  TemplateSyncItem items_[AppConfig::MAX_SYNC_TEMPLATES];

  void resetItems();
  TemplateSyncItem* findItem(const String& uid);
  TemplateSyncItem* getOrCreateItem(const String& uid);
  bool allChunksReady(const TemplateSyncItem& item) const;
  TemplateSyncItem* parseManifestItem(JsonObjectConst itemObj);
  bool finalizeOneTemplate(TemplateSyncItem* item, const String& activeClassName);
  bool parseChunkTopic(const char* topic, String& syncId, String& uid, uint16_t& chunkIndex) const;
};
