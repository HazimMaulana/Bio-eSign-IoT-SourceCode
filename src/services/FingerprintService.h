#pragma once

#include <Arduino.h>
#include <Adafruit_Fingerprint.h>

class BuzzerService;

typedef void (*BackgroundCallback)(void* context);
typedef void (*EnrollLogCallback)(void* context, const char* message);

class FingerprintService {
public:
  FingerprintService();

  void begin();
  bool verifyPassword();
  void configurePacketSize();
  void printInfo(uint16_t nextIdCandidate);
  bool clearAllTemplates();

  void setBackgroundCallback(BackgroundCallback cb, void* context);
  void setEnrollLogCallback(EnrollLogCallback cb, void* context);
  void setBuzzer(BuzzerService* buzzer);

  bool waitFingerRemoved();
  bool doEnroll(uint16_t id);
  uint16_t capacity() const;
  uint16_t templateCount();

  uint8_t getImage();
  uint8_t image2Tz(uint8_t slot = 1);
  uint8_t fingerFastSearch();
  int matchedFingerId() const;
  int matchedConfidence() const;

  bool downloadTemplateBytes(uint16_t fingerprintId, uint8_t* out, size_t outCap, size_t* outLen);
  bool restoreTemplateToSensor(uint16_t fingerprintId, const uint8_t* bytes, size_t len);

  Adafruit_Fingerprint& raw();
  HardwareSerial& serial();

private:
  HardwareSerial serial_;
  Adafruit_Fingerprint finger_;
  BackgroundCallback backgroundCb_ = nullptr;
  void* backgroundContext_ = nullptr;
  EnrollLogCallback enrollLogCb_ = nullptr;
  void* enrollLogContext_ = nullptr;
  BuzzerService* buzzer_ = nullptr;

  void background();
  void emitEnrollLog(const char* message);

  uint16_t getSensorPacketPayloadSizeRaw();
  uint16_t getSensorPacketPayloadSize();
  size_t estimateEffectiveTemplateLength(const uint8_t* bytes, size_t len);
  uint8_t sendDownCharToBuffer1WithPayload(const uint8_t* bytes, size_t len, uint16_t payloadSize);
  uint8_t sendDownCharToBuffer1(const uint8_t* bytes, size_t len);
  bool startTemplateUploadRaw(uint8_t* packetType, uint8_t* packetPayload, size_t payloadCap, size_t* packetPayloadLen);
  bool readFingerprintPacketRaw(uint8_t* typeOut, uint8_t* payload, size_t payloadCap, size_t* payloadLen, uint16_t timeoutMs);
};
