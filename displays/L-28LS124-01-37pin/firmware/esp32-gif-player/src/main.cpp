// Author: Dibyendu Mondal (https://github.com/dib-00)
/**
 * =============================================================================
 *  L-28LS124-01 GIF / image player — ESP32 (NodeMCU-32S)
 * =============================================================================
 *  Panel  : 2.8" 240x320, ST7789V, 8-bit 8080 parallel (pinout in platformio.ini)
 *
 *  The browser does all image work (GIF decoding, resize, RGB565 conversion)
 *  and uploads one ready-to-play file. The ESP32 stores it in LittleFS, so it
 *  keeps playing after a power cycle. A 240x320 frame is 150 KB, more than the
 *  largest free heap block, so frames are streamed from flash to the panel in
 *  16-row strips.
 *
 *  Animation file (/anim.bin), little-endian:
 *    0  "KTA1"            magic
 *    4  u16 width (240)   6  u16 height (320)
 *    8  u16 frame count   10 6 bytes reserved
 *    16 frames: u16 delay_ms, then 153600 bytes RGB565 big-endian, row-major
 *
 *  HTTP API
 *    GET  /             web UI
 *    GET  /api/status   JSON: network, storage capacity, current animation
 *    POST /api/upload   raw body = animation file (application/octet-stream)
 *    POST /api/clear    delete the animation, show the info screen
 * =============================================================================
 */

#include <Arduino.h>
#include <WiFi.h>
#include <DNSServer.h>
#include <ESPmDNS.h>
#include <LittleFS.h>
#include <AsyncTCP.h>
#include <ESPAsyncWebServer.h>
#include <TFT_eSPI.h>

// -----------------------------------------------------------------------------
// User configuration
// -----------------------------------------------------------------------------

// Router credentials. Leave STA_SSID empty to always start the access point.
static const char *STA_SSID = "";
static const char *STA_PASS = "";

static const char *AP_SSID = "KT28-Display";
static const char *AP_PASS = "kt28display";  // >= 8 chars
static const IPAddress AP_IP(192, 168, 4, 1);
static const char *MDNS_HOST = "kt28";

// -----------------------------------------------------------------------------

static constexpr int16_t PANEL_W = 240;
static constexpr int16_t PANEL_H = 320;
static constexpr size_t FRAME_BYTES = size_t(PANEL_W) * PANEL_H * 2;  // 153600
static constexpr size_t HEADER_BYTES = 16;
static constexpr size_t RECORD_BYTES = FRAME_BYTES + 2;
static constexpr int16_t STRIPE_ROWS = 16;
static constexpr size_t STRIPE_BYTES = size_t(PANEL_W) * STRIPE_ROWS * 2;  // 7680
static constexpr size_t FS_MARGIN = 64 * 1024;
static const char *ANIM_PATH = "/anim.bin";
static const char *TMP_PATH = "/anim.tmp";

static_assert(PANEL_H % STRIPE_ROWS == 0, "stripes must tile the frame");

extern const uint8_t index_html_start[] asm("_binary_web_index_html_start");
extern const uint8_t index_html_end[] asm("_binary_web_index_html_end");

static TFT_eSPI tft;
static AsyncWebServer server(80);
static DNSServer dnsServer;
static bool apMode = true;
static IPAddress deviceIP;
static String portalUrl;
static uint8_t *stripeBuf = nullptr;

// Playback state — owned by the loop task.
static File animFile;
static volatile uint16_t animFrames = 0;
static volatile size_t animSize = 0;
static uint16_t curFrame = 0;
static uint32_t nextAt = 0;
static bool splashShown = false;

// Cross-task flags (HTTP handlers run on the AsyncTCP task).
static volatile bool uploading = false;
static volatile bool animReleased = true;
static volatile bool reloadReq = true;
static volatile bool clearReq = false;

struct UploadSession {
  AsyncWebServerRequest *owner = nullptr;
  File file;
  size_t received = 0;
  bool error = false;
  char message[80] = {0};
};
static UploadSession upload;

// -----------------------------------------------------------------------------
// HTTP helpers
// -----------------------------------------------------------------------------

static void sendJson(AsyncWebServerRequest *req, int code, const String &json) {
  AsyncWebServerResponse *res = req->beginResponse(code, "application/json", json);
  res->addHeader("Cache-Control", "no-store");
  req->send(res);
}

static void sendError(AsyncWebServerRequest *req, int code, const char *message) {
  char buf[160];
  snprintf(buf, sizeof(buf), "{\"ok\":false,\"error\":\"%s\"}", message);
  sendJson(req, code, buf);
}

static size_t capacityBytes() {
  const size_t total = LittleFS.totalBytes();
  const size_t used = LittleFS.usedBytes();
  const size_t freeNow = total > used ? total - used : 0;
  const size_t cap = freeNow + animSize;  // the current animation gets replaced
  return cap > FS_MARGIN ? cap - FS_MARGIN : 0;
}

// -----------------------------------------------------------------------------
// Animation file (loop task)
// -----------------------------------------------------------------------------

static void closeAnim() {
  if (animFile) animFile.close();
  animFrames = 0;
  animSize = 0;
}

static bool openAnim() {
  closeAnim();
  if (!LittleFS.exists(ANIM_PATH)) return false;
  File f = LittleFS.open(ANIM_PATH, "r");
  if (!f) return false;
  uint8_t h[HEADER_BYTES];
  const bool okRead = f.read(h, HEADER_BYTES) == HEADER_BYTES;
  const uint16_t w = h[4] | (h[5] << 8), ht = h[6] | (h[7] << 8), n = h[8] | (h[9] << 8);
  if (!okRead || memcmp(h, "KTA1", 4) != 0 || w != PANEL_W || ht != PANEL_H || n == 0 ||
      f.size() != HEADER_BYTES + size_t(n) * RECORD_BYTES) {
    Serial.println("[anim] invalid file, ignoring");
    f.close();
    return false;
  }
  animFile = f;
  animFrames = n;
  animSize = f.size();
  curFrame = 0;
  nextAt = 0;
  Serial.printf("[anim] loaded %u frame(s), %u bytes\n", n, unsigned(animSize));
  return true;
}

// Streams one frame from flash to the panel. Returns its delay in ms, or
// 0xFFFF on a read error.
static uint16_t showFrame(uint16_t i) {
  uint8_t d[2];
  if (!animFile.seek(HEADER_BYTES + size_t(i) * RECORD_BYTES)) return 0xFFFF;
  if (animFile.read(d, 2) != 2) return 0xFFFF;

  bool ok = true;
  tft.startWrite();
  tft.setAddrWindow(0, 0, PANEL_W, PANEL_H);
  for (int16_t row = 0; row < PANEL_H; row += STRIPE_ROWS) {
    if (animFile.read(stripeBuf, STRIPE_BYTES) != STRIPE_BYTES) {
      ok = false;
      break;
    }
    // swapBytes is off: bytes go out in file order, i.e. high byte first.
    tft.pushPixels(stripeBuf, STRIPE_BYTES / 2);
  }
  tft.endWrite();
  return ok ? uint16_t(d[0] | (d[1] << 8)) : 0xFFFF;
}

static void drawSplash() {
  tft.fillScreen(TFT_BLACK);
  tft.fillRect(0, 0, PANEL_W, 34, tft.color565(20, 40, 90));
  tft.setTextDatum(MC_DATUM);
  tft.setTextColor(TFT_WHITE, tft.color565(20, 40, 90));
  tft.drawString("KT-28 GIF Player", PANEL_W / 2, 17, 4);

  tft.setTextDatum(TL_DATUM);
  int16_t y = 50;
  auto line = [&](const char *label, const String &value, uint16_t color) {
    tft.setTextColor(TFT_DARKGREY, TFT_BLACK);
    tft.drawString(label, 12, y, 2);
    tft.setTextColor(color, TFT_BLACK);
    tft.drawString(value, 12, y + 18, 4);
    y += 56;
  };
  if (apMode) {
    line("WI-FI", AP_SSID, TFT_ORANGE);
    line("PASSWORD", AP_PASS, TFT_WHITE);
  } else {
    line("NETWORK", WiFi.SSID(), TFT_GREEN);
  }
  line("OPEN IN BROWSER", "http://" + deviceIP.toString(), TFT_CYAN);
  tft.setTextColor(TFT_DARKGREY, TFT_BLACK);
  tft.drawString("No image uploaded yet", 12, y, 2);

  static const uint16_t bars[8] = {TFT_WHITE, TFT_YELLOW, TFT_CYAN, TFT_GREEN,
                                   TFT_MAGENTA, TFT_RED, TFT_BLUE, TFT_BLACK};
  for (int i = 0; i < 8; ++i) tft.fillRect(i * 30, PANEL_H - 24, 30, 24, bars[i]);
}

// -----------------------------------------------------------------------------
// POST /api/upload (streamed straight to flash)
// -----------------------------------------------------------------------------

static void failUpload(const char *message) {
  upload.error = true;
  strlcpy(upload.message, message, sizeof(upload.message));
}

static void endUpload(bool keepFile) {
  if (upload.file) upload.file.close();
  if (keepFile) {
    LittleFS.remove(ANIM_PATH);
    LittleFS.rename(TMP_PATH, ANIM_PATH);
  } else {
    LittleFS.remove(TMP_PATH);
  }
  upload.owner = nullptr;
  uploading = false;
  reloadReq = true;
}

static void onUploadBody(AsyncWebServerRequest *req, uint8_t *data, size_t len, size_t index,
                         size_t total) {
  if (index == 0) {
    if (upload.owner && upload.owner != req) return;  // busy -> 409
    upload.owner = req;
    upload.file = File();
    upload.received = 0;
    upload.error = false;
    upload.message[0] = 0;

    if (total <= HEADER_BYTES || (total - HEADER_BYTES) % RECORD_BYTES != 0) {
      failUpload("payload size is not a whole number of frames");
    } else if (total > capacityBytes()) {
      failUpload("animation does not fit in flash - use fewer frames");
    } else if (len < HEADER_BYTES || memcmp(data, "KTA1", 4) != 0) {
      failUpload("bad file header");
    }

    if (!upload.error) {
      // Ask the loop task to stop playback and close the file, then wait.
      animReleased = false;
      uploading = true;
      const uint32_t t0 = millis();
      while (!animReleased && millis() - t0 < 3000) vTaskDelay(pdMS_TO_TICKS(5));
      LittleFS.remove(ANIM_PATH);  // free its space for the new file
      LittleFS.remove(TMP_PATH);
      upload.file = LittleFS.open(TMP_PATH, "w");
      if (!upload.file) failUpload("cannot create file");
    }

    req->onDisconnect([req]() {
      if (upload.owner == req) {
        Serial.println("[upload] client disconnected, discarding");
        endUpload(false);
      }
    });
  }

  if (upload.owner != req || upload.error) return;
  if (upload.file.write(data, len) != len) {
    failUpload("flash write failed");
    return;
  }
  upload.received += len;
}

static void onUploadRequest(AsyncWebServerRequest *req) {
  if (req->contentLength() == 0) {
    sendError(req, 400, "empty body");
    return;
  }
  if (upload.owner != req) {
    sendError(req, 409, "another upload is in progress");
    return;
  }
  if (upload.error) {
    char msg[80];
    strlcpy(msg, upload.message, sizeof(msg));
    endUpload(false);
    sendError(req, 400, msg);
    return;
  }
  if (upload.received != req->contentLength()) {
    endUpload(false);
    sendError(req, 400, "incomplete upload");
    return;
  }

  const unsigned frames = unsigned((upload.received - HEADER_BYTES) / RECORD_BYTES);
  endUpload(true);
  Serial.printf("[upload] stored %u frame(s)\n", frames);

  char buf[96];
  snprintf(buf, sizeof(buf), "{\"ok\":true,\"frames\":%u}", frames);
  sendJson(req, 200, buf);
}

// -----------------------------------------------------------------------------
// GET /api/status
// -----------------------------------------------------------------------------

static void onStatus(AsyncWebServerRequest *req) {
  const size_t cap = capacityBytes();
  const unsigned capFrames = cap > HEADER_BYTES ? unsigned((cap - HEADER_BYTES) / RECORD_BYTES) : 0;
  char buf[512];
  snprintf(buf, sizeof(buf),
           "{\"ok\":true,\"panel\":\"L-28LS124-01\",\"width\":%d,\"height\":%d,"
           "\"frameBytes\":%u,\"mode\":\"%s\",\"ip\":\"%s\",\"ssid\":\"%s\",\"frames\":%u,"
           "\"uploading\":%s,\"fsTotal\":%u,\"fsUsed\":%u,\"capacityBytes\":%u,"
           "\"capacityFrames\":%u,\"uptimeMs\":%lu,\"freeHeap\":%lu}",
           PANEL_W, PANEL_H, unsigned(FRAME_BYTES), apMode ? "ap" : "sta",
           deviceIP.toString().c_str(), apMode ? AP_SSID : WiFi.SSID().c_str(), unsigned(animFrames),
           uploading ? "true" : "false", unsigned(LittleFS.totalBytes()),
           unsigned(LittleFS.usedBytes()), unsigned(cap), capFrames, (unsigned long)millis(),
           (unsigned long)ESP.getFreeHeap());
  sendJson(req, 200, buf);
}

// -----------------------------------------------------------------------------
// Networking
// -----------------------------------------------------------------------------

static bool connectStation() {
  if (strlen(STA_SSID) == 0) return false;
  Serial.printf("[wifi] connecting to \"%s\"\n", STA_SSID);
  WiFi.setHostname(MDNS_HOST);
  WiFi.mode(WIFI_STA);
  WiFi.setSleep(false);
  WiFi.setAutoReconnect(true);
  WiFi.begin(STA_SSID, STA_PASS);
  const uint32_t t0 = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - t0 < 15000) delay(250);
  if (WiFi.status() != WL_CONNECTED) {
    WiFi.disconnect(true);
    return false;
  }
  deviceIP = WiFi.localIP();
  return true;
}

static void startAccessPoint() {
  WiFi.mode(WIFI_AP);
  WiFi.softAPConfig(AP_IP, AP_IP, IPAddress(255, 255, 255, 0));
  WiFi.softAP(AP_SSID, AP_PASS, 6, 0, 4);
  WiFi.setSleep(false);
  deviceIP = WiFi.softAPIP();
  dnsServer.setErrorReplyCode(DNSReplyCode::NoError);
  dnsServer.start(53, "*", deviceIP);
}

static void setupServer() {
  DefaultHeaders::Instance().addHeader("Access-Control-Allow-Origin", "*");
  DefaultHeaders::Instance().addHeader("Access-Control-Allow-Methods", "GET, POST, OPTIONS");
  DefaultHeaders::Instance().addHeader("Access-Control-Allow-Headers", "Content-Type");

  server.on("/", HTTP_GET, [](AsyncWebServerRequest *req) {
    const size_t len = size_t(index_html_end - index_html_start) - 1;  // strip NUL
    AsyncWebServerResponse *res = req->beginResponse(200, "text/html", index_html_start, len);
    res->addHeader("Cache-Control", "no-cache");
    req->send(res);
  });
  server.on("/api/status", HTTP_GET, onStatus);
  server.on("/api/upload", HTTP_POST, onUploadRequest, nullptr, onUploadBody);
  server.on("/api/clear", HTTP_POST, [](AsyncWebServerRequest *req) {
    if (uploading) {
      sendError(req, 409, "upload in progress");
      return;
    }
    clearReq = true;
    sendJson(req, 200, "{\"ok\":true}");
  });

  if (apMode) {
    static const char *const probes[] = {"/generate_204", "/gen_204", "/hotspot-detect.html",
                                         "/library/test/success.html", "/ncsi.txt",
                                         "/connecttest.txt", "/redirect", "/fwlink"};
    for (const char *path : probes) {
      server.on(path, HTTP_ANY, [](AsyncWebServerRequest *req) { req->redirect(portalUrl); });
    }
  }

  server.onNotFound([](AsyncWebServerRequest *req) {
    if (req->method() == HTTP_OPTIONS) {
      req->send(204);
      return;
    }
    if (apMode && !req->url().startsWith("/api/")) {
      req->redirect(portalUrl);
      return;
    }
    sendError(req, 404, "not found");
  });
  server.begin();
}

// -----------------------------------------------------------------------------

void setup() {
  Serial.begin(115200);
  delay(100);
  Serial.println("\n=== KT-28 GIF player (ESP32, ST7789V 8-bit parallel) ===");

  stripeBuf = static_cast<uint8_t *>(malloc(STRIPE_BYTES));
  delay(100);  // let the panel supply settle
  tft.init();
  tft.setRotation(0);
  tft.setSwapBytes(false);
  tft.fillScreen(TFT_BLACK);

  if (!LittleFS.begin(true)) Serial.println("[fs] mount failed");
  Serial.printf("[fs] %u / %u bytes used\n", unsigned(LittleFS.usedBytes()),
                unsigned(LittleFS.totalBytes()));

  apMode = !connectStation();
  if (apMode) startAccessPoint();
  portalUrl = "http://" + deviceIP.toString() + "/";
  if (MDNS.begin(MDNS_HOST)) MDNS.addService("http", "tcp", 80);
  Serial.printf("[wifi] %s, open %s\n", apMode ? "access point" : "station", portalUrl.c_str());

  setupServer();
}

void loop() {
  if (apMode) dnsServer.processNextRequest();

  if (uploading) {
    if (!animReleased) {
      closeAnim();
      animReleased = true;
    }
    delay(2);
    return;
  }

  if (clearReq) {
    clearReq = false;
    closeAnim();
    LittleFS.remove(ANIM_PATH);
    splashShown = false;
  }
  if (reloadReq) {
    reloadReq = false;
    openAnim();
    splashShown = false;
  }

  if (animFrames == 0) {
    if (!splashShown) {
      drawSplash();
      splashShown = true;
    }
    delay(5);
    return;
  }

  const uint32_t now = millis();
  if (nextAt == 0 || int32_t(now - nextAt) >= 0) {
    if (animFrames == 1 && nextAt != 0) {  // still image already on screen
      delay(5);
      return;
    }
    const uint16_t d = showFrame(curFrame);
    if (d == 0xFFFF) {
      Serial.println("[anim] read error");
      closeAnim();
      return;
    }
    curFrame = (curFrame + 1) % animFrames;
    nextAt = now + (d < 20 ? 20 : d);
    if (nextAt == 0) nextAt = 1;
  }
  delay(1);
}
