#include "BuzzerService.h"
#include "../config/AppConfig.h"

void BuzzerService::begin() {
  pinMode(AppConfig::BUZZER_PIN, OUTPUT);
  ledcSetup(AppConfig::BUZZER_CH, AppConfig::BUZZER_BASE_FREQ, AppConfig::BUZZER_RES_BITS);
  ledcAttachPin(AppConfig::BUZZER_PIN, AppConfig::BUZZER_CH);
  ledcWriteTone(AppConfig::BUZZER_CH, 0);
}

void BuzzerService::tone(uint32_t freqHz, uint32_t durMs) {
  if (freqHz == 0 || durMs == 0) return;
  ledcWriteTone(AppConfig::BUZZER_CH, freqHz);
  delay(durMs);
  ledcWriteTone(AppConfig::BUZZER_CH, 0);
}

void BuzzerService::success() {
  tone(2000, 80);
  delay(60);
  tone(2600, 110);
}

void BuzzerService::fail() {
  tone(450, 220);
  delay(80);
  tone(450, 120);
}

void BuzzerService::locked() {
  tone(700, 80);
  delay(60);
  tone(700, 80);
  delay(60);
  tone(700, 80);
}
