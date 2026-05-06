#include "TemplateSyncService.h"
#include "MqttService.h"
#include "FingerprintService.h"
#include "UiService.h"
#include "../config/AppConfig.h"
#include <ArduinoJson.h>

void TemplateSyncService::begin(MqttService* mqtt, FingerprintService* fingerprint, UiService* ui) {
  mqtt_ = mqtt;
  fingerprint_ = fingerprint;
  ui_ = ui;
  reset("begin");
}

void TemplateSyncService::resetItems() {
  for (size_t i = 0; i < AppConfig::MAX_SYNC_TEMPLATES; i++) {
    for (size_t c = 0; c < AppConfig::MAX_CHUNKS_PER_TEMPLATE; c++) {
      if (items_[i].chunkData[c]) {
        free(items_[i].chunkData[c]);
        items_[i].chunkData[c] = nullptr;
      }
      items_[i].chunkLen[c] = 0;
    }
    items_[i] = TemplateSyncItem();
  }
}

void TemplateSyncService::reset(const char* reason) {
  syncDone_ = false;
  syncExpected_ = false;
  state_ = TemplateSyncState();
  resetItems();
  Serial.print("[SYNC] Reset state: ");
  Serial.println(reason);
}

void TemplateSyncService::setExpected(bool expected) { syncExpected_ = expected; }
bool TemplateSyncService::expected() const { return syncExpected_; }
bool TemplateSyncService::done() const { return syncDone_; }
bool TemplateSyncService::inProgress() const { return state_.inProgress; }
const TemplateSyncState& TemplateSyncService::state() const { return state_; }

bool TemplateSyncService::requestSync() {
  if (!mqtt_ || !mqtt_->isConnected()) return false;

  JsonDocument doc;
  doc["device_id"] = AppConfig::DEVICE_ID;
  doc["action"] = "sync_templates";

  char payload[160];
  size_t n = serializeJson(doc, payload, sizeof(payload));
  if (n == 0) return false;

  bool ok = mqtt_->publish(AppConfig::TOPIC_TEMPLATE_REQ, payload);
  Serial.print("[SYNC] Request sync publish: ");
  Serial.println(ok ? "OK" : "FAIL");
  if (ok) Serial.println(payload);
  return ok;
}

bool TemplateSyncService::publishAck(const char* eventName, const char* templateUid, const char* message) {
  if (!mqtt_ || !mqtt_->isConnected()) return false;

  JsonDocument doc;
  doc["device_id"] = AppConfig::DEVICE_ID;
  doc["event"] = eventName;
  doc["sync_id"] = state_.syncId;
  doc["sync_done"] = syncDone_;
  doc["in_progress"] = state_.inProgress;
  doc["total_templates"] = state_.totalTemplates;
  doc["success_count"] = state_.successCount;
  doc["failed_count"] = state_.failedCount;
  if (templateUid && templateUid[0] != '\0') doc["template_uid"] = templateUid;
  if (message && message[0] != '\0') doc["message"] = message;
  doc["ts_ms"] = (uint32_t)millis();

  char payload[512];
  size_t n = serializeJson(doc, payload, sizeof(payload));
  if (n == 0) return false;

  bool ok = mqtt_->publish(AppConfig::TOPIC_TEMPLATE_ACK, payload);
  Serial.print("[SYNC] ACK ");
  Serial.print(eventName);
  Serial.print(": ");
  Serial.println(ok ? "OK" : "FAIL");
  return ok;
}

TemplateSyncItem* TemplateSyncService::findItem(const String& uid) {
  for (size_t i = 0; i < AppConfig::MAX_SYNC_TEMPLATES; i++) {
    if (items_[i].used && items_[i].templateUid == uid) return &items_[i];
  }
  return nullptr;
}

TemplateSyncItem* TemplateSyncService::getOrCreateItem(const String& uid) {
  TemplateSyncItem* found = findItem(uid);
  if (found) return found;
  for (size_t i = 0; i < AppConfig::MAX_SYNC_TEMPLATES; i++) {
    if (!items_[i].used) {
      items_[i] = TemplateSyncItem();
      items_[i].used = true;
      items_[i].templateUid = uid;
      return &items_[i];
    }
  }
  return nullptr;
}

bool TemplateSyncService::allChunksReady(const TemplateSyncItem& item) const {
  if (item.chunkTotal == 0 || item.chunkTotal > AppConfig::MAX_CHUNKS_PER_TEMPLATE) return false;
  for (uint16_t i = 0; i < item.chunkTotal; i++) if (!item.gotChunk[i]) return false;
  return true;
}

TemplateSyncItem* TemplateSyncService::parseManifestItem(JsonObjectConst itemObj) {
  String uid = String(itemObj["template_uid"] | "");
  if (uid.length() == 0) uid = String(itemObj["uid"] | "");
  if (uid.length() == 0) return nullptr;

  TemplateSyncItem* item = getOrCreateItem(uid);
  if (!item) return nullptr;

  int fpId = itemObj["fingerprint_id"] | itemObj["finger_id"] | 0;
  int chunkTotal = itemObj["chunk_total"] | itemObj["chunks"] | 0;

  if (fpId <= 0 || fpId > 65535) { item->failed = true; return item; }
  if (chunkTotal <= 0 || chunkTotal > (int)AppConfig::MAX_CHUNKS_PER_TEMPLATE) { item->failed = true; return item; }

  item->fingerprintId = (uint16_t)fpId;
  item->chunkTotal = (uint16_t)chunkTotal;
  return item;
}

bool TemplateSyncService::finalizeOneTemplate(TemplateSyncItem* item, const String& activeClassName) {
  if (!item) return false;
  if (item->restored || item->failed) return false;
  if (!allChunksReady(*item)) return false;

  size_t tplLen = 0;
  for (uint16_t i = 0; i < item->chunkTotal; i++) tplLen += item->chunkLen[i];

  if (tplLen == 0 || tplLen > AppConfig::MAX_TEMPLATE_BYTES) {
    item->failed = true;
    state_.failedCount++;
    publishAck("template_failed", item->templateUid.c_str(), "binary_size_invalid");
    tryFinalizeSync(activeClassName);
    return false;
  }

  uint8_t* tplBytes = (uint8_t*)malloc(tplLen);
  if (!tplBytes) {
    item->failed = true;
    state_.failedCount++;
    publishAck("template_failed", item->templateUid.c_str(), "alloc_failed");
    tryFinalizeSync(activeClassName);
    return false;
  }

  size_t offset = 0;
  for (uint16_t i = 0; i < item->chunkTotal; i++) {
    memcpy(tplBytes + offset, item->chunkData[i], item->chunkLen[i]);
    offset += item->chunkLen[i];
  }

  bool ok = fingerprint_ && fingerprint_->restoreTemplateToSensor(item->fingerprintId, tplBytes, tplLen);
  free(tplBytes);

  if (ok) {
    item->restored = true;
    state_.successCount++;
    publishAck("template_restored", item->templateUid.c_str(), "ok");
  } else {
    item->failed = true;
    state_.failedCount++;
    publishAck("template_failed", item->templateUid.c_str(), "restore_sensor_failed");
  }

  tryFinalizeSync(activeClassName);
  return ok;
}

void TemplateSyncService::processPendingFinalization(const String& activeClassName) {
  if (!state_.inProgress) return;
  for (size_t i = 0; i < AppConfig::MAX_SYNC_TEMPLATES; i++) {
    if (!items_[i].used) continue;
    if (items_[i].restored || items_[i].failed) continue;
    if (!allChunksReady(items_[i])) continue;
    finalizeOneTemplate(&items_[i], activeClassName);
  }
}

void TemplateSyncService::tryFinalizeSync(const String& activeClassName) {
  if (!state_.manifestReceived) return;

  if (state_.totalTemplates == 0) {
    syncDone_ = true;
    state_.inProgress = false;
    syncExpected_ = false;
    publishAck("sync_complete", "", "no_templates");
    if (ui_) {
      ui_->sensorReady();
      ui_->showReadyPanel(activeClassName);
    }
    return;
  }

  uint16_t done = state_.successCount + state_.failedCount;
  if (done < state_.totalTemplates) {
    if (ui_) {
      char sensorLine[64];
      snprintf(sensorLine, sizeof(sensorLine), "Sync %u/%u", done, state_.totalTemplates);
      ui_->setPrepareStatus("Connected", "Connected", sensorLine, 85);
    }
    return;
  }

  state_.inProgress = false;
  syncDone_ = (state_.failedCount == 0 && state_.successCount == state_.totalTemplates);
  syncExpected_ = false;

  if (fingerprint_) {
    uint16_t storedCount = fingerprint_->templateCount();
    Serial.printf("[SYNC] Template sync finished. Currently %d templates stored in sensor.\n", storedCount);
  }

  publishAck("sync_complete", "", syncDone_ ? "all_restored" : "partial_or_failed");

  if (ui_) {
    if (syncDone_) {
      ui_->sensorReady();
      ui_->delayWithUi(500);
      ui_->showReadyPanel(activeClassName);
    } else {
      ui_->showFail("Template sync failed", "Check MQTT/template data");
    }
  }
}

void TemplateSyncService::handleManifestPayload(const char* payload, size_t length) {
  if (!syncExpected_ && !state_.inProgress) {
    Serial.println("[SYNC] Manifest ignored (sync not expected)");
    return;
  }

  JsonDocument doc;
  DeserializationError err = deserializeJson(doc, payload, length);
  if (err) {
    Serial.print("[SYNC] Manifest JSON parse gagal: ");
    Serial.println(err.c_str());
    return;
  }
  if (!doc.is<JsonObjectConst>()) return;

  JsonObjectConst root = doc.as<JsonObjectConst>();
  String syncId = String(root["sync_id"] | "");
  if (syncId.length() == 0) syncId = String(root["syncId"] | "");
  if (syncId.length() == 0) syncId = String((uint32_t)millis(), HEX);

  if (!state_.inProgress || state_.syncId != syncId) {
    reset("new_manifest");
    state_.inProgress = true;
    state_.syncId = syncId;
  }

  JsonArrayConst arr;
  if (root["templates"].is<JsonArrayConst>()) arr = root["templates"].as<JsonArrayConst>();
  else if (root["items"].is<JsonArrayConst>()) arr = root["items"].as<JsonArrayConst>();

  uint16_t totalTemplates = root["total_templates"] | root["total"] | 0;
  if (!arr.isNull() && totalTemplates == 0) totalTemplates = (uint16_t)arr.size();
  state_.totalTemplates = totalTemplates;
  state_.manifestReceived = true;

  if (!arr.isNull()) {
    for (JsonVariantConst v : arr) {
      if (!v.is<JsonObjectConst>()) continue;
      TemplateSyncItem* item = parseManifestItem(v.as<JsonObjectConst>());
      if (!item) continue;
      if (item->failed) {
        state_.failedCount++;
        publishAck("template_failed", item->templateUid.c_str(), "manifest_invalid_item");
      }
    }
  }

  publishAck("manifest_received", "", "ok");
}

bool TemplateSyncService::parseChunkTopic(const char* topic, String& syncId, String& uid, uint16_t& chunkIndex) const {
  String prefix = String(AppConfig::TOPIC_TEMPLATE_CHUNK) + "/";
  String topicStr = String(topic);
  if (!topicStr.startsWith(prefix)) return false;

  String rest = topicStr.substring(prefix.length());
  int first = rest.indexOf('/');
  int second = first >= 0 ? rest.indexOf('/', first + 1) : -1;
  if (first <= 0 || second <= first + 1) return false;

  syncId = rest.substring(0, first);
  uid = rest.substring(first + 1, second);
  int parsedIndex = rest.substring(second + 1).toInt();
  if (syncId.length() == 0 || uid.length() == 0 || parsedIndex <= 0) return false;

  chunkIndex = (uint16_t)parsedIndex;
  return true;
}

void TemplateSyncService::handleChunkPayload(const char* topic, const uint8_t* payload, size_t length) {
  if (!syncExpected_ && !state_.inProgress) {
    Serial.println("[SYNC] Chunk ignored (sync not expected)");
    return;
  }

  String syncId;
  String uid;
  uint16_t chunkIndex = 0;
  if (!parseChunkTopic(topic, syncId, uid, chunkIndex)) {
    Serial.print("[SYNC] Chunk topic invalid: ");
    Serial.println(topic);
    return;
  }

  if (!state_.inProgress) {
    reset("chunk_starts_sync");
    state_.inProgress = true;
    state_.syncId = syncId;
  }
  if (state_.syncId != syncId) return;

  if (chunkIndex == 0 || length == 0) return;

  TemplateSyncItem* item = getOrCreateItem(uid);
  if (!item) return;

  if (item->chunkTotal == 0 || item->chunkTotal > AppConfig::MAX_CHUNKS_PER_TEMPLATE) {
    item->failed = true;
    if (state_.manifestReceived) state_.failedCount++;
    publishAck("template_failed", uid.c_str(), "chunk_total_invalid");
    return;
  }
  if (chunkIndex > item->chunkTotal || chunkIndex > (int)AppConfig::MAX_CHUNKS_PER_TEMPLATE) return;

  uint16_t idx = (uint16_t)(chunkIndex - 1);
  if (!item->gotChunk[idx]) {
    uint8_t* copy = (uint8_t*)malloc(length);
    if (!copy) {
      item->failed = true;
      if (state_.manifestReceived) state_.failedCount++;
      publishAck("template_failed", uid.c_str(), "chunk_alloc_failed");
      return;
    }
    memcpy(copy, payload, length);
    item->gotChunk[idx] = true;
    item->chunkData[idx] = copy;
    item->chunkLen[idx] = length;
    item->chunkReceived++;
  }
}
