#include <Arduino.h>
#include "PresenceApp.h"

PresenceApp app;

void setup() {
  app.begin();
}

void loop() {
  app.loop();
}
