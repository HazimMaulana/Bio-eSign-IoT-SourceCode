#pragma once

#include <Arduino.h>
#include <WiFi.h>

class UiService;

class WifiService {
public:
  void begin(UiService* ui = nullptr);
  void ensureConnected(UiService* ui = nullptr);
  bool isConnected() const;
  String ipString() const;
};
