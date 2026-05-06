#pragma once

#include <Arduino.h>
#include <WiFi.h>
#include <PubSubClient.h>

class UiService;

typedef void (*MqttMessageHandler)(void* context, char* topic, byte* payload, unsigned int length);

class MqttService {
public:
  MqttService();

  void begin(UiService* ui = nullptr);
  void loop();
  void ensureConnected(UiService* ui = nullptr);
  bool isConnected();

  void setMessageHandler(MqttMessageHandler handler, void* context);
  bool publish(const char* topic, const char* payload, bool retained = false);
  bool publishBinary(const char* topic, const uint8_t* payload, unsigned int length, bool retained = false);
  bool publishString(const char* topic, const String& payload, bool retained = false);
  bool subscribe(const char* topic, uint8_t qos = 0);
  bool publishStatus(const char* status, const String& ip, bool retained = true);

  PubSubClient& rawClient();
  void setSendingEnabled(bool enabled);
  bool sendingEnabled() const;

private:
  WiFiClient wifiClient_;
  PubSubClient client_;
  MqttMessageHandler handler_ = nullptr;
  void* handlerContext_ = nullptr;
  bool sendingEnabled_ = true;

  static MqttService* activeInstance_;
  static void staticCallback(char* topic, byte* payload, unsigned int length);
  void handleCallback(char* topic, byte* payload, unsigned int length);
};
