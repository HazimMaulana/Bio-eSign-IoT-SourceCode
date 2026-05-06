#include "FingerprintService.h"
#include "BuzzerService.h"
#include "../config/AppConfig.h"

#define FINGERPRINT_DOWNLOAD 0x09

FingerprintService::FingerprintService() : serial_(1), finger_(&serial_) {}

void FingerprintService::begin() {
  serial_.begin(AppConfig::FP_BAUD, SERIAL_8N1, AppConfig::FP_RX, AppConfig::FP_TX);
  finger_.begin(AppConfig::FP_BAUD);
  Serial.printf("UART RX=%d TX=%d baud=%lu\n", AppConfig::FP_RX, AppConfig::FP_TX, (unsigned long)AppConfig::FP_BAUD);
}

bool FingerprintService::verifyPassword() {
  return finger_.verifyPassword();
}

void FingerprintService::configurePacketSize() {
  uint8_t packetSizeStatus = finger_.setPacketSize(FINGERPRINT_PACKET_SIZE_32);
  if (packetSizeStatus != FINGERPRINT_OK) {
    Serial.print("[WARN] setPacketSize(32) gagal: 0x");
    Serial.println(packetSizeStatus, HEX);
  }
  delay(100);
  finger_.getParameters();
  Serial.print("[INFO] Sensor packet_len aktif: ");
  Serial.println((unsigned int)finger_.packet_len);
}

void FingerprintService::setBackgroundCallback(BackgroundCallback cb, void* context) {
  backgroundCb_ = cb;
  backgroundContext_ = context;
}

void FingerprintService::setBuzzer(BuzzerService* buzzer) {
  buzzer_ = buzzer;
}

void FingerprintService::background() {
  if (backgroundCb_) backgroundCb_(backgroundContext_);
  else delay(1);
}

Adafruit_Fingerprint& FingerprintService::raw() { return finger_; }
HardwareSerial& FingerprintService::serial() { return serial_; }

uint16_t FingerprintService::capacity() const { return finger_.capacity; }

uint16_t FingerprintService::templateCount() {
  finger_.getTemplateCount();
  return finger_.templateCount;
}

void FingerprintService::printInfo(uint16_t nextIdCandidate) {
  finger_.getParameters();
  Serial.print("Capacity (maks template): ");
  Serial.println(finger_.capacity);
  finger_.getTemplateCount();
  Serial.print("Template terisi (sensor): ");
  Serial.println(finger_.templateCount);
  Serial.print("Next enroll ID (candidate): ");
  Serial.println(nextIdCandidate);
}

bool FingerprintService::clearAllTemplates() {
  uint8_t p = finger_.emptyDatabase();
  if (p == FINGERPRINT_OK) {
    Serial.println("[OK] Semua template di sensor berhasil dihapus.");
    return true;
  }
  Serial.print("[FAIL] Gagal hapus database sensor: 0x");
  Serial.println(p, HEX);
  return false;
}

bool FingerprintService::waitFingerRemoved() {
  Serial.println("Angkat jari...");
  while (finger_.getImage() != FINGERPRINT_NOFINGER) delay(50);
  delay(250);
  return true;
}

bool FingerprintService::doEnroll(uint16_t id) {
  Serial.printf("\n=== ENROLL ID %u ===\n", id);

  Serial.println("Tempelkan jari (scan 1)...");
  while (finger_.getImage() != FINGERPRINT_OK) {
    background();
    delay(50);
  }
  if (finger_.image2Tz(1) != FINGERPRINT_OK) {
    Serial.println("[FAIL] image2Tz(1)");
    if (buzzer_) buzzer_->fail();
    return false;
  }
  Serial.println("[OK] Scan 1");
  waitFingerRemoved();

  Serial.println("Tempelkan jari yang sama (scan 2)...");
  while (finger_.getImage() != FINGERPRINT_OK) {
    background();
    delay(50);
  }
  if (finger_.image2Tz(2) != FINGERPRINT_OK) {
    Serial.println("[FAIL] image2Tz(2)");
    if (buzzer_) buzzer_->fail();
    return false;
  }
  Serial.println("[OK] Scan 2");

  uint8_t p = finger_.createModel();
  if (p != FINGERPRINT_OK) {
    Serial.println("[FAIL] Scan 1 dan scan 2 tidak cocok.");
    if (buzzer_) buzzer_->fail();
    return false;
  }

  p = finger_.storeModel(id);
  if (p == FINGERPRINT_OK) {
    Serial.println("[OK] Enroll sukses");
    if (buzzer_) buzzer_->success();
    return true;
  }

  Serial.print("[FAIL] storeModel: 0x");
  Serial.println(p, HEX);
  if (buzzer_) buzzer_->fail();
  return false;
}

uint8_t FingerprintService::getImage() { return finger_.getImage(); }
uint8_t FingerprintService::image2Tz(uint8_t slot) { return finger_.image2Tz(slot); }
uint8_t FingerprintService::fingerFastSearch() { return finger_.fingerFastSearch(); }
int FingerprintService::matchedFingerId() const { return finger_.fingerID; }
int FingerprintService::matchedConfidence() const { return finger_.confidence; }

uint16_t FingerprintService::getSensorPacketPayloadSizeRaw() {
  uint16_t packetSize = finger_.packet_len;
  if (packetSize == 0) packetSize = 32;
  if (packetSize > 64) packetSize = 64;
  if (packetSize < 16) packetSize = 16;
  return packetSize;
}

uint16_t FingerprintService::getSensorPacketPayloadSize() {
  return getSensorPacketPayloadSizeRaw();
}

bool FingerprintService::readFingerprintPacketRaw(uint8_t* typeOut, uint8_t* payload, size_t payloadCap, size_t* payloadLen, uint16_t timeoutMs) {
  if (!typeOut || !payload || !payloadLen) return false;
  *payloadLen = 0;

  auto readByte = [&](uint8_t* outByte, uint16_t timeout) -> bool {
    uint32_t started = millis();
    while ((uint32_t)(millis() - started) < timeout) {
      if (serial_.available() > 0) {
        int c = serial_.read();
        if (c >= 0) { *outByte = (uint8_t)c; return true; }
      }
      delay(1);
    }
    return false;
  };

  uint8_t b = 0;
  bool gotStart = false;
  uint32_t started = millis();
  while ((uint32_t)(millis() - started) < timeoutMs) {
    if (!readByte(&b, 10)) continue;
    if (b != (uint8_t)(FINGERPRINT_STARTCODE >> 8)) continue;
    uint8_t b2 = 0;
    if (!readByte(&b2, timeoutMs)) return false;
    if (b2 == (uint8_t)(FINGERPRINT_STARTCODE & 0xFF)) { gotStart = true; break; }
  }
  if (!gotStart) return false;

  uint8_t addr[4];
  for (int i = 0; i < 4; i++) if (!readByte(&addr[i], timeoutMs)) return false;

  uint8_t type = 0, lenHi = 0, lenLo = 0;
  if (!readByte(&type, timeoutMs)) return false;
  if (!readByte(&lenHi, timeoutMs)) return false;
  if (!readByte(&lenLo, timeoutMs)) return false;

  uint16_t wireLen = ((uint16_t)lenHi << 8) | lenLo;
  if (wireLen < 2) return false;

  size_t dataLen = (size_t)(wireLen - 2);
  uint32_t sum = (uint32_t)type + (uint32_t)lenHi + (uint32_t)lenLo;
  bool overflow = dataLen > payloadCap;

  for (size_t i = 0; i < dataLen; i++) {
    uint8_t dataByte = 0;
    if (!readByte(&dataByte, timeoutMs)) return false;
    if (!overflow) payload[i] = dataByte;
    sum += dataByte;
  }

  uint8_t csumHi = 0, csumLo = 0;
  if (!readByte(&csumHi, timeoutMs)) return false;
  if (!readByte(&csumLo, timeoutMs)) return false;
  uint16_t recvChecksum = ((uint16_t)csumHi << 8) | csumLo;
  uint16_t calcChecksum = (uint16_t)(sum & 0xFFFF);

  if (overflow) return false;
  if (recvChecksum != calcChecksum) {
    Serial.print("[TPL] checksum mismatch recv=0x");
    Serial.print(recvChecksum, HEX);
    Serial.print(" calc=0x");
    Serial.println(calcChecksum, HEX);
    return false;
  }

  *typeOut = type;
  *payloadLen = dataLen;
  return true;
}

uint8_t FingerprintService::sendDownCharToBuffer1WithPayload(const uint8_t* bytes, size_t len, uint16_t payloadSize) {
  if (!bytes || len == 0) return FINGERPRINT_PACKETRECIEVEERR;
  if (payloadSize == 0) payloadSize = getSensorPacketPayloadSize();
  if (payloadSize > 64) payloadSize = 64;

  uint8_t cmd[2] = {FINGERPRINT_DOWNLOAD, 0x01};
  Adafruit_Fingerprint_Packet cmdPacket(FINGERPRINT_COMMANDPACKET, sizeof(cmd), cmd);
  finger_.writeStructuredPacket(cmdPacket);
  serial_.flush();

  uint8_t ackType = 0;
  uint8_t ackData[64];
  size_t ackLen = 0;
  if (!readFingerprintPacketRaw(&ackType, ackData, sizeof(ackData), &ackLen, 4000)) return FINGERPRINT_PACKETRECIEVEERR;
  if (ackType != FINGERPRINT_ACKPACKET || ackLen == 0) return FINGERPRINT_PACKETRECIEVEERR;
  if (ackData[0] != FINGERPRINT_OK) return ackData[0];

  size_t offset = 0;
  while (offset < len) {
    uint16_t chunk = (uint16_t)((len - offset) > payloadSize ? payloadSize : (len - offset));
    uint8_t pktType = ((offset + chunk) >= len) ? FINGERPRINT_ENDDATAPACKET : FINGERPRINT_DATAPACKET;
    Adafruit_Fingerprint_Packet dataPacket(pktType, chunk, (uint8_t*)(bytes + offset));
    finger_.writeStructuredPacket(dataPacket);
    serial_.flush();
    offset += chunk;
    delay(6);
  }

  delay(100);
  ackType = 0;
  ackLen = 0;
  if (!readFingerprintPacketRaw(&ackType, ackData, sizeof(ackData), &ackLen, 600)) return FINGERPRINT_OK;
  if (ackType == FINGERPRINT_ACKPACKET && ackLen > 0) return ackData[0];
  return FINGERPRINT_OK;
}

uint8_t FingerprintService::sendDownCharToBuffer1(const uint8_t* bytes, size_t len) {
  return sendDownCharToBuffer1WithPayload(bytes, len, getSensorPacketPayloadSize());
}

size_t FingerprintService::estimateEffectiveTemplateLength(const uint8_t* bytes, size_t len) {
  if (!bytes || len == 0) return 0;
  size_t end = len;
  while (end > 0 && bytes[end - 1] == 0) end--;
  return end;
}

bool FingerprintService::restoreTemplateToSensor(uint16_t fingerprintId, const uint8_t* bytes, size_t len) {
  if (fingerprintId == 0 || fingerprintId > finger_.capacity) return false;

  uint8_t del = finger_.deleteModel(fingerprintId);
  if (!(del == FINGERPRINT_OK || del == FINGERPRINT_NOTFOUND)) {
    Serial.print("[SYNC] deleteModel warning: 0x");
    Serial.println(del, HEX);
  }

  uint16_t packetSize = getSensorPacketPayloadSize();
  uint16_t packetSizeRaw = getSensorPacketPayloadSizeRaw();
  size_t effectiveLen = estimateEffectiveTemplateLength(bytes, len);
  size_t roundedLen = effectiveLen;
  if (roundedLen > 0 && packetSize > 0) {
    roundedLen = ((roundedLen + packetSize - 1) / packetSize) * packetSize;
    if (roundedLen > len) roundedLen = len;
  }
  size_t roundedLenRaw = effectiveLen;
  if (roundedLenRaw > 0 && packetSizeRaw > 0) {
    roundedLenRaw = ((roundedLenRaw + packetSizeRaw - 1) / packetSizeRaw) * packetSizeRaw;
    if (roundedLenRaw > len) roundedLenRaw = len;
  }

  size_t candidates[6] = {len, roundedLen, roundedLenRaw, effectiveLen, 512, 256};
  uint16_t payloadCandidates[2] = {packetSize, packetSizeRaw};
  uint8_t p = FINGERPRINT_PACKETRECIEVEERR;

  for (size_t i = 0; i < 6; i++) {
    size_t candidateLen = candidates[i];
    if (candidateLen == 0 || candidateLen > len) continue;
    bool dupLen = false;
    for (size_t j = 0; j < i; j++) if (candidates[j] == candidateLen) { dupLen = true; break; }
    if (dupLen) continue;

    for (size_t mode = 0; mode < 2; mode++) {
      uint16_t payloadSize = payloadCandidates[mode];
      bool dupPay = false;
      for (size_t pm = 0; pm < mode; pm++) if (payloadCandidates[pm] == payloadSize) { dupPay = true; break; }
      if (dupPay) continue;

      p = sendDownCharToBuffer1WithPayload(bytes, candidateLen, payloadSize);
      if (p == FINGERPRINT_OK) {
        delay(50);
        p = finger_.storeModel(fingerprintId);
        if (p == FINGERPRINT_OK) return true;
        Serial.print("[SYNC] storeModel gagal: 0x");
        Serial.println(p, HEX);
      }
    }
  }
  return false;
}

bool FingerprintService::startTemplateUploadRaw(uint8_t* packetType, uint8_t* packetPayload, size_t payloadCap, size_t* packetPayloadLen) {
  if (!packetType || !packetPayload || !packetPayloadLen) return false;

  uint8_t cmd[2] = {FINGERPRINT_UPLOAD, 0x01};
  Adafruit_Fingerprint_Packet cmdPacket(FINGERPRINT_COMMANDPACKET, sizeof(cmd), cmd);
  finger_.writeStructuredPacket(cmdPacket);

  if (!readFingerprintPacketRaw(packetType, packetPayload, payloadCap, packetPayloadLen, 2500)) return false;
  if (*packetType == FINGERPRINT_ACKPACKET) {
    if (*packetPayloadLen == 0) return false;
    if (packetPayload[0] != FINGERPRINT_OK) return false;
    if (!readFingerprintPacketRaw(packetType, packetPayload, payloadCap, packetPayloadLen, 2500)) return false;
  }
  return *packetType == FINGERPRINT_DATAPACKET || *packetType == FINGERPRINT_ENDDATAPACKET;
}

bool FingerprintService::downloadTemplateBytes(uint16_t fingerprintId, uint8_t* out, size_t outCap, size_t* outLen) {
  if (!outLen) return false;
  *outLen = 0;
  if (!out || outCap == 0) return false;

  uint8_t p = finger_.loadModel(fingerprintId);
  if (p != FINGERPRINT_OK) {
    Serial.print("[TPL] loadModel gagal: 0x");
    Serial.println(p, HEX);
    return false;
  }

  uint8_t packetType = 0;
  uint8_t packetPayload[256];
  size_t packetPayloadLen = 0;
  size_t offset = 0;

  if (!startTemplateUploadRaw(&packetType, packetPayload, sizeof(packetPayload), &packetPayloadLen)) return false;

  while (true) {
    if (!(packetType == FINGERPRINT_DATAPACKET || packetType == FINGERPRINT_ENDDATAPACKET)) return false;
    if ((offset + packetPayloadLen) > outCap) return false;
    memcpy(out + offset, packetPayload, packetPayloadLen);
    offset += packetPayloadLen;
    if (packetType == FINGERPRINT_ENDDATAPACKET) break;
    if (!readFingerprintPacketRaw(&packetType, packetPayload, sizeof(packetPayload), &packetPayloadLen, 2500)) return false;
  }

  if (offset == 0) return false;
  *outLen = offset;
  Serial.print("[TPL] Download template bytes=");
  Serial.println((unsigned int)offset);
  return true;
}
