#pragma once

#include <Arduino.h>
#include "../config/AppConfig.h"

struct TemplateSyncItem {
  bool used = false;
  bool restored = false;
  bool failed = false;
  String templateUid;
  uint16_t fingerprintId = 0;
  uint16_t chunkTotal = 0;
  uint16_t chunkReceived = 0;
  bool gotChunk[AppConfig::MAX_CHUNKS_PER_TEMPLATE] = {false};
  uint8_t* chunkData[AppConfig::MAX_CHUNKS_PER_TEMPLATE] = {nullptr};
  size_t chunkLen[AppConfig::MAX_CHUNKS_PER_TEMPLATE] = {0};
};

struct TemplateSyncState {
  bool inProgress = false;
  bool manifestReceived = false;
  String syncId;
  uint16_t totalTemplates = 0;
  uint16_t successCount = 0;
  uint16_t failedCount = 0;
};
