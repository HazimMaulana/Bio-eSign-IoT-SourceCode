#include "MqttService.h"
#include "UiService.h"
#include "../config/AppConfig.h"
#include <ArduinoJson.h>

MqttService* MqttService::activeInstance_ = nullptr;

MqttService::MqttService() : client_(wifiClient_) {}

void MqttService::begin(UiService* ui) {
  client_.setServer(AppConfig::MQTT_HOST, AppConfig::MQTT_PORT);
  client_.setBufferSize(AppConfig::MQTT_BUFFER_SIZE);
  activeInstance_ = this;
  client_.setCallback(MqttService::staticCallback);
  ensureConnected(ui);
}

void MqttService::loop() {
  client_.loop();
}

void MqttService::ensureConnected(UiService* ui) {
  while (!client_.connected()) {
    if (ui) ui->mqttConnecting();
    Serial.print("Connecting to MQTT broker... ");

    String clientId = "esp32-" + AppConfig::deviceId() + "-" + String(random(0xffff), HEX);
    bool connected = client_.connect(clientId.c_str(), AppConfig::MQTT_USERNAME, AppConfig::MQTT_PASSWORD);

    if (connected) {
      Serial.println("connected");
      subscribe(AppConfig::topicMahasiswa().c_str(), 1);
      subscribe(AppConfig::TOPIC_SESSION_CLEAR, 1);
      subscribe(AppConfig::topicDeviceConfig().c_str(), 1);
      subscribe(AppConfig::topicCommand().c_str(), 1);
      subscribe(AppConfig::topicAttendanceAck().c_str(), 1);
      subscribe(AppConfig::topicTemplateManifest().c_str(), 1);
      subscribe(AppConfig::topicTemplateChunkWildcard().c_str(), 1);

      Serial.println("Subscribed to default presence topics");
      if (ui) ui->mqttConnected();
    } else {
      Serial.print("failed, rc=");
      Serial.print(client_.state());
      Serial.println(" retrying in 5 seconds");
      if (ui) ui->delayWithUi(5000);
      else delay(5000);
    }
  }
}

bool MqttService::isConnected() {
  return client_.connected();
}

void MqttService::setMessageHandler(MqttMessageHandler handler, void* context) {
  handler_ = handler;
  handlerContext_ = context;
}

bool MqttService::publish(const char* topic, const char* payload, bool retained) {
  if (!sendingEnabled_ || !client_.connected()) return false;
  return client_.publish(topic, payload, retained);
}

bool MqttService::publishBinary(const char* topic, const uint8_t* payload, unsigned int length, bool retained) {
  if (!sendingEnabled_ || !client_.connected() || !payload || length == 0) return false;
  return client_.publish(topic, payload, length, retained);
}

bool MqttService::publishString(const char* topic, const String& payload, bool retained) {
  return publish(topic, payload.c_str(), retained);
}

bool MqttService::subscribe(const char* topic, uint8_t qos) {
  if (!client_.connected()) return false;
  return client_.subscribe(topic, qos);
}

bool MqttService::publishStatus(const char* status, const String& ip, bool retained) {
  if (!sendingEnabled_ || !client_.connected()) {
    Serial.println("[MQTT] Status publish skipped");
    return false;
  }

  StaticJsonDocument<192> doc;
  doc["device_id"] = AppConfig::deviceId();
  doc["deviceId"] = AppConfig::deviceId();
  doc["status"] = status;
  doc["ip"] = ip;
  doc["config_url"] = "http://" + ip;
  doc["firmware_version"] = "1.0.0";

  char payload[256];
  size_t n = serializeJson(doc, payload, sizeof(payload));
  if (n == 0) return false;

  String topic = AppConfig::topicStatus();
  bool ok = client_.publish(topic.c_str(), payload, retained);
  Serial.print("[MQTT] Status published: ");
  Serial.println(ok ? payload : "FAIL");
  return ok;
}

bool MqttService::publishDiscovery(const String& ip) {
  if (!sendingEnabled_ || !client_.connected()) {
    Serial.println("[MQTT] Discovery publish skipped");
    return false;
  }

  StaticJsonDocument<256> doc;
  doc["device_id"] = AppConfig::deviceId();
  doc["mac_address"] = WiFi.macAddress();
  doc["firmware_version"] = "1.0.0";
  doc["status"] = "waiting_assignment";
  doc["ip"] = ip;
  doc["config_url"] = "http://" + ip;
  doc["ts_ms"] = (uint32_t)millis();

  char payload[256];
  size_t n = serializeJson(doc, payload, sizeof(payload));
  if (n == 0) return false;

  bool ok = client_.publish(AppConfig::TOPIC_PROVISIONING_DISCOVER, payload);
  Serial.print("[MQTT] Discovery published: ");
  Serial.println(ok ? payload : "FAIL");
  return ok;
}

PubSubClient& MqttService::rawClient() {
  return client_;
}

void MqttService::setSendingEnabled(bool enabled) {
  sendingEnabled_ = enabled;
}

bool MqttService::sendingEnabled() const {
  return sendingEnabled_;
}

void MqttService::staticCallback(char* topic, byte* payload, unsigned int length) {
  if (activeInstance_) activeInstance_->handleCallback(topic, payload, length);
}

void MqttService::handleCallback(char* topic, byte* payload, unsigned int length) {
  if (handler_) handler_(handlerContext_, topic, payload, length);
}
