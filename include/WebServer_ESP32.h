#pragma once

// Platform-specific web server selection.
// ESP32: uses ESPAsyncWebServer (event-driven, no polling needed)
// Emulator: uses POSIX sockets (polled from main loop)

#if defined(ARDUINO) && defined(ESP32) && !SIMULATION_MODE

#include <WiFi.h>
#include <ESPAsyncWebServer.h>
#include "web_content.h"

#ifndef WEB_PORT
#define WEB_PORT 80
#endif

#ifndef WIFI_AP_SSID
#define WIFI_AP_SSID "AlkTrack"
#endif

#ifndef WIFI_AP_PASS
#define WIFI_AP_PASS "reefmonitor"
#endif

struct WebState {
  float lastResult_dKH = 0.0f;
  float lastRefPH = 0.0f;
  float lastTankPH = 0.0f;
  float lastRefSD = 0.0f;
  float lastTankSD = 0.0f;
  float lastTemp = 0.0f;
  const char* cyclePhase = "startup";
  float history[24] = {};
  int historyCount = 0;
  float anchor_dKH = 0.0f;
};

static WebState g_webState;
static AsyncWebServer g_server(WEB_PORT);

extern void handleWebCommand(const char* cmd);

static void webServer_begin() {
  WiFi.mode(WIFI_AP);
  WiFi.softAP(WIFI_AP_SSID, WIFI_AP_PASS);

  Serial.print("AP started: ");
  Serial.println(WIFI_AP_SSID);
  Serial.print("IP: ");
  Serial.println(WiFi.softAPIP());

  g_server.on("/", HTTP_GET, [](AsyncWebServerRequest *request) {
    request->send(200, "text/html", WEB_HTML);
  });

  g_server.on("/api/status", HTTP_GET, [](AsyncWebServerRequest *request) {
    char json[1024];
    char histArr[512] = "[";
    int count = g_webState.historyCount < 24 ? g_webState.historyCount : 24;
    for (int i = 0; i < count; i++) {
      char num[16];
      snprintf(num, sizeof(num), "%.2f", g_webState.history[i]);
      if (i > 0) strcat(histArr, ",");
      strcat(histArr, num);
    }
    strcat(histArr, "]");

    snprintf(json, sizeof(json),
      "{\"anchor\":%.2f,\"lastResult\":%.2f,\"refPH\":%.4f,\"tankPH\":%.4f,"
      "\"refSD\":%.5f,\"tankSD\":%.5f,\"temp\":%.2f,\"phase\":\"%s\","
      "\"history\":%s,\"historyCount\":%d}",
      g_webState.anchor_dKH, g_webState.lastResult_dKH,
      g_webState.lastRefPH, g_webState.lastTankPH,
      g_webState.lastRefSD, g_webState.lastTankSD,
      g_webState.lastTemp, g_webState.cyclePhase,
      histArr, g_webState.historyCount);

    request->send(200, "application/json", json);
  });

  g_server.on("/api/command", HTTP_POST, [](AsyncWebServerRequest *request) {},
    NULL,
    [](AsyncWebServerRequest *request, uint8_t *data, size_t len, size_t index, size_t total) {
      char cmd[64] = {};
      size_t copyLen = len < sizeof(cmd) - 1 ? len : sizeof(cmd) - 1;
      memcpy(cmd, data, copyLen);
      cmd[copyLen] = '\0';

      // Trim whitespace
      char* p = cmd + copyLen - 1;
      while (p >= cmd && (*p == '\n' || *p == '\r' || *p == ' ')) *p-- = '\0';
      for (char* c = cmd; *c; c++) *c = toupper((unsigned char)*c);

      handleWebCommand(cmd);
      request->send(200, "application/json", "{\"ok\":true}");
    }
  );

  g_server.on("/api/command", HTTP_OPTIONS, [](AsyncWebServerRequest *request) {
    AsyncWebServerResponse *response = request->beginResponse(204);
    response->addHeader("Access-Control-Allow-Origin", "*");
    response->addHeader("Access-Control-Allow-Methods", "GET, POST");
    response->addHeader("Access-Control-Allow-Headers", "Content-Type");
    request->send(response);
  });

  DefaultHeaders::Instance().addHeader("Access-Control-Allow-Origin", "*");
  g_server.begin();
  Serial.print("Web server on http://");
  Serial.print(WiFi.softAPIP());
  Serial.print(":");
  Serial.println(WEB_PORT);
}

// ESPAsyncWebServer is event-driven; no polling needed.
static void webServer_poll() {}

#endif // ESP32 && !SIMULATION_MODE
