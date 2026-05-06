#pragma once

#include <Arduino.h>

class BuzzerService {
public:
  void begin();
  void tone(uint32_t freqHz, uint32_t durMs);
  void success();
  void fail();
  void locked();
};
