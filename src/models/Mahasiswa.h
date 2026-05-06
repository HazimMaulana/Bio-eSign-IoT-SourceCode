#pragma once

#include <Arduino.h>

struct Mahasiswa {
  String nama;
  String nim;
  int32_t fingerId[3] = {-1, -1, -1};
};
