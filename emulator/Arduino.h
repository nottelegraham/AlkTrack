#pragma once

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cmath>
#include <cctype>
#include <string>
#include <chrono>
#include <thread>
#include <unistd.h>
#include <fcntl.h>
#include <sys/select.h>

#ifndef TIME_ACCEL
#define TIME_ACCEL 1000
#endif

#define HIGH 1
#define LOW  0
#define OUTPUT 1
#define INPUT  0
#define SERIAL_8N1 0x06

static auto g_startTime = std::chrono::steady_clock::now();

inline unsigned long millis() {
  auto now = std::chrono::steady_clock::now();
  auto real_ms = std::chrono::duration_cast<std::chrono::milliseconds>(now - g_startTime).count();
#if TIME_ACCEL == 0
  return (unsigned long)(real_ms * 1000000UL);
#else
  return (unsigned long)(real_ms * TIME_ACCEL);
#endif
}

inline void delay(unsigned long ms) {
  if (ms == 0) return;
#if TIME_ACCEL == 0
  return;
#else
  unsigned long real_us = (ms * 1000UL) / TIME_ACCEL;
  if (real_us > 0) std::this_thread::sleep_for(std::chrono::microseconds(real_us));
#endif
}

inline void pinMode(int, int) {}
inline void digitalWrite(int, int) {}
inline void dacWrite(uint8_t, uint8_t) {}
inline void analogWrite(int, int) {}

// --- Arduino String class (minimal) ---

class String {
  std::string _s;
public:
  String() {}
  String(const char* s) : _s(s ? s : "") {}
  String(const std::string& s) : _s(s) {}

  void trim() {
    size_t start = _s.find_first_not_of(" \t\r\n");
    size_t end = _s.find_last_not_of(" \t\r\n");
    if (start == std::string::npos) { _s.clear(); return; }
    _s = _s.substr(start, end - start + 1);
  }

  void toUpperCase() {
    for (auto& c : _s) c = toupper((unsigned char)c);
  }

  bool startsWith(const char* prefix) const {
    return _s.rfind(prefix, 0) == 0;
  }

  String substring(int from) const {
    if (from < 0 || (size_t)from >= _s.size()) return String("");
    return String(_s.substr(from));
  }

  float toFloat() const { return std::strtof(_s.c_str(), nullptr); }
  size_t length() const { return _s.size(); }
  char operator[](size_t i) const { return _s[i]; }
  const char* c_str() const { return _s.c_str(); }
  String& operator+=(char c) { _s += c; return *this; }
  bool operator==(const char* o) const { return _s == o; }
};

// --- Serial stub ---

class SerialClass {
  bool _stdinNonBlock = false;
  bool _eof = false;

  void ensureNonBlock() {
    if (!_stdinNonBlock) {
      int flags = fcntl(STDIN_FILENO, F_GETFL, 0);
      fcntl(STDIN_FILENO, F_SETFL, flags | O_NONBLOCK);
      _stdinNonBlock = true;
    }
  }

public:
  void begin(int) { ensureNonBlock(); }

  bool available() {
    if (_eof) return false;
    ensureNonBlock();
    fd_set fds;
    FD_ZERO(&fds);
    FD_SET(STDIN_FILENO, &fds);
    struct timeval tv = {0, 0};
    if (select(STDIN_FILENO + 1, &fds, nullptr, nullptr, &tv) <= 0) return false;
    int c = getchar();
    if (c == EOF) { _eof = true; return false; }
    ungetc(c, stdin);
    return true;
  }

  String readStringUntil(char delim) {
    std::string buf;
    int flags = fcntl(STDIN_FILENO, F_GETFL, 0);
    fcntl(STDIN_FILENO, F_SETFL, flags & ~O_NONBLOCK);
    int c;
    while ((c = getchar()) != EOF && c != delim) {
      buf += (char)c;
    }
    if (c == EOF) _eof = true;
    fcntl(STDIN_FILENO, F_SETFL, flags | O_NONBLOCK);
    return String(buf);
  }

  int read() {
    if (_eof) return -1;
    ensureNonBlock();
    int c = getchar();
    if (c == EOF) _eof = true;
    return c;
  }

  void print(const char* s) { fputs(s, stdout); }
  void print(char c) { putchar(c); }
  void print(int v) { printf("%d", v); }
  void print(unsigned long v) { printf("%lu", v); }
  void print(float v, int prec = 2) { printf("%.*f", prec, (double)v); }
  void print(double v, int prec = 2) { printf("%.*f", prec, v); }

  void println(const char* s) { printf("%s\n", s); fflush(stdout); }
  void println(char c) { printf("%c\n", c); fflush(stdout); }
  void println(int v) { printf("%d\n", v); fflush(stdout); }
  void println(unsigned long v) { printf("%lu\n", v); fflush(stdout); }
  void println(float v, int prec = 2) { printf("%.*f\n", prec, (double)v); fflush(stdout); }
  void println(double v, int prec = 2) { printf("%.*f\n", prec, v); fflush(stdout); }
  void println() { printf("\n"); fflush(stdout); }
};

static SerialClass Serial;

// Suppress Serial2 references — only used in !SIMULATION_MODE
#define Serial2 Serial
