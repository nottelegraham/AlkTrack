#pragma once

#include <cstdint>
#include <cstring>
#include <cstdio>

#define EEPROM_FILE "eeprom.bin"

class EEPROMClass {
  uint8_t* _buf = nullptr;
  size_t _size = 0;

public:
  void begin(size_t size) {
    _size = size;
    _buf = new uint8_t[size];
    memset(_buf, 0xFF, size);
    FILE* f = fopen(EEPROM_FILE, "rb");
    if (f) {
      fread(_buf, 1, size, f);
      fclose(f);
    }
  }

  template<typename T>
  void get(int addr, T& data) {
    if (addr + (int)sizeof(T) <= (int)_size)
      memcpy(&data, _buf + addr, sizeof(T));
  }

  template<typename T>
  void put(int addr, const T& data) {
    if (addr + (int)sizeof(T) <= (int)_size)
      memcpy(_buf + addr, &data, sizeof(T));
  }

  void commit() {
    FILE* f = fopen(EEPROM_FILE, "wb");
    if (f) {
      fwrite(_buf, 1, _size, f);
      fclose(f);
    }
  }
};

static EEPROMClass EEPROM;
