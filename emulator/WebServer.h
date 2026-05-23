#pragma once

#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <string>
#include <sys/socket.h>
#include <netinet/in.h>
#include <unistd.h>
#include <fcntl.h>

#ifndef WEB_PORT
#define WEB_PORT 8080
#endif

#include "web_content.h"

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
static int g_listenFd = -1;

static void webServer_sendResponse(int fd, const char* status, const char* contentType, const char* body, size_t bodyLen) {
  char header[256];
  int hlen = snprintf(header, sizeof(header),
    "HTTP/1.0 %s\r\n"
    "Content-Type: %s\r\n"
    "Content-Length: %zu\r\n"
    "Access-Control-Allow-Origin: *\r\n"
    "Access-Control-Allow-Methods: GET, POST\r\n"
    "Access-Control-Allow-Headers: Content-Type\r\n"
    "Connection: close\r\n\r\n",
    status, contentType, bodyLen);
  write(fd, header, hlen);
  if (bodyLen > 0) write(fd, body, bodyLen);
}

static void webServer_handleRequest(int fd, const char* req, size_t reqLen) {
  if (strncmp(req, "GET / ", 6) == 0 || strncmp(req, "GET /index", 10) == 0) {
    webServer_sendResponse(fd, "200 OK", "text/html", WEB_HTML, strlen(WEB_HTML));
  }
  else if (strncmp(req, "GET /api/status", 15) == 0) {
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

    int len = snprintf(json, sizeof(json),
      "{\"anchor\":%.2f,\"lastResult\":%.2f,\"refPH\":%.4f,\"tankPH\":%.4f,"
      "\"refSD\":%.5f,\"tankSD\":%.5f,\"temp\":%.2f,\"phase\":\"%s\","
      "\"history\":%s,\"historyCount\":%d}",
      g_webState.anchor_dKH, g_webState.lastResult_dKH,
      g_webState.lastRefPH, g_webState.lastTankPH,
      g_webState.lastRefSD, g_webState.lastTankSD,
      g_webState.lastTemp, g_webState.cyclePhase,
      histArr, g_webState.historyCount);
    webServer_sendResponse(fd, "200 OK", "application/json", json, len);
  }
  else if (strncmp(req, "POST /api/command", 17) == 0) {
    const char* body = strstr(req, "\r\n\r\n");
    if (body) {
      body += 4;
      std::string cmd(body, reqLen - (body - req));
      while (!cmd.empty() && (cmd.back() == '\n' || cmd.back() == '\r' || cmd.back() == ' '))
        cmd.pop_back();

      for (auto& c : cmd) c = toupper((unsigned char)c);

      extern void handleWebCommand(const char* cmd);
      handleWebCommand(cmd.c_str());

      const char* ok = "{\"ok\":true}";
      webServer_sendResponse(fd, "200 OK", "application/json", ok, strlen(ok));
    } else {
      webServer_sendResponse(fd, "400 Bad Request", "text/plain", "no body", 7);
    }
  }
  else if (strncmp(req, "OPTIONS", 7) == 0) {
    webServer_sendResponse(fd, "204 No Content", "text/plain", "", 0);
  }
  else {
    webServer_sendResponse(fd, "404 Not Found", "text/plain", "not found", 9);
  }
}

static void webServer_begin() {
  g_listenFd = socket(AF_INET, SOCK_STREAM, 0);
  if (g_listenFd < 0) { perror("socket"); return; }

  int opt = 1;
  setsockopt(g_listenFd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

  struct sockaddr_in addr = {};
  addr.sin_family = AF_INET;
  addr.sin_addr.s_addr = INADDR_ANY;
  addr.sin_port = htons(WEB_PORT);

  if (bind(g_listenFd, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
    perror("bind");
    close(g_listenFd);
    g_listenFd = -1;
    return;
  }

  listen(g_listenFd, 8);
  fcntl(g_listenFd, F_SETFL, O_NONBLOCK);
  printf("Web server on http://localhost:%d\n", WEB_PORT);
  fflush(stdout);
}

static void webServer_poll() {
  if (g_listenFd < 0) return;

  struct sockaddr_in clientAddr;
  socklen_t clientLen = sizeof(clientAddr);
  int clientFd = accept(g_listenFd, (struct sockaddr*)&clientAddr, &clientLen);
  if (clientFd < 0) return;

  char buf[4096] = {};
  struct timeval tv = {0, 100000};
  setsockopt(clientFd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
  ssize_t n = read(clientFd, buf, sizeof(buf) - 1);

  if (n > 0) {
    webServer_handleRequest(clientFd, buf, n);
  }

  close(clientFd);
}
