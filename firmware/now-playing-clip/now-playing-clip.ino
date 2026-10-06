/*
 * now-playing-clip
 *
 * Shows the currently playing Spotify track on a LILYGO T-Display-S3:
 * album art on the left, title and artist scrolling on the right.
 *
 * See README.md for Spotify app registration and refresh token setup.
 */

#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <TFT_eSPI.h>
#include <TJpg_Decoder.h>

#include "secrets.h"

// ---------------------------------------------------------------- hardware

#define PIN_POWER_ON 15   // MUST be HIGH or the screen is black on battery
#define PIN_LCD_BL   38   // backlight

TFT_eSPI tft;

static const int SCREEN_W = 320;
static const int SCREEN_H = 170;

// Layout: album art on the left, a scrolling text band on the right.
// The two regions never overlap, so the art is drawn once directly to the
// display and only the text band is redrawn every frame.
static const int ART_X   = 8;
static const int ART_Y   = 10;
static const int ART_SZ  = 150;    // 300px art decoded at 1:2

static const int TEXT_X  = 168;
static const int TEXT_W  = SCREEN_W - TEXT_X;   // 152
static const int TEXT_H  = SCREEN_H;

TFT_eSprite spr = TFT_eSprite(&tft);

// ---------------------------------------------------------------- timing

static const unsigned long POLL_INTERVAL_MS  = 4000;
static const unsigned long TOKEN_LIFETIME_MS = 50UL * 60000;
static const int           SCROLL_DELAY_MS   = 16;
static const int           GAP_PX            = 40;

unsigned long lastPoll  = 0;
unsigned long tokenTime = 0;
unsigned long lastStep  = 0;

// ---------------------------------------------------------------- state

String accessToken = "";
String titleLine   = "nothing playing";
String artistLine  = "";
int    scrollT     = TEXT_W;
int    scrollA     = TEXT_W;
String artUrl      = "";      // currently displayed art, to avoid refetching
bool   isPlaying   = false;
long          progressMs = 0;
long          durationMs = 0;
unsigned long progressAt = 0;   // millis() when progressMs was received

// ---------------------------------------------------------------- prototypes

void setLine(const String &t, const String &a, bool playing);
bool refreshAccessToken();
void pollNowPlaying();
void drawFrame();
void loadArt(const String &url);
void clearArt();

// ---------------------------------------------------------------- wifi

void connectWiFi() {
  Serial.print("WiFi: connecting to ");
  Serial.println(WIFI_SSID);

  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);

  unsigned long start = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - start < 20000) {
    delay(250);
    Serial.print(".");
  }
  Serial.println();

  if (WiFi.status() == WL_CONNECTED) {
    Serial.print("WiFi: connected, IP ");
    Serial.println(WiFi.localIP());
  } else {
    Serial.println("WiFi: FAILED");
  }
}

// ---------------------------------------------------------------- spotify

bool refreshAccessToken() {
  WiFiClientSecure client;
  client.setInsecure();

  HTTPClient http;
  if (!http.begin(client, "https://accounts.spotify.com/api/token")) {
    Serial.println("token: begin failed");
    return false;
  }

  http.addHeader("Content-Type", "application/x-www-form-urlencoded");
  http.addHeader("Authorization", String("Basic ") + SPOTIFY_BASIC_AUTH);

  String body = "grant_type=refresh_token&refresh_token=";
  body += SPOTIFY_REFRESH_TOKEN;

  int code = http.POST(body);
  if (code != 200) {
    Serial.printf("token: HTTP %d\n", code);
    Serial.println(http.getString());
    http.end();
    return false;
  }

  JsonDocument doc;
  DeserializationError err = deserializeJson(doc, http.getStream());
  http.end();

  if (err) {
    Serial.print("token: json error ");
    Serial.println(err.c_str());
    return false;
  }

  accessToken = doc["access_token"].as<String>();
  tokenTime   = millis();
  Serial.println("token: refreshed");
  return accessToken.length() > 0;
}

void pollNowPlaying() {
  if (accessToken.isEmpty()) return;

  WiFiClientSecure client;
  client.setInsecure();

  HTTPClient http;
  const char *url = "https://api.spotify.com/v1/me/player/currently-playing";
  if (!http.begin(client, url)) return;

  http.addHeader("Authorization", String("Bearer ") + accessToken);
  int code = http.GET();

  if (code == 204) {                 // nothing playing, empty body
    http.end();
    setLine("nothing playing", "", false);
    clearArt();
    return;
  }

  if (code == 401) {                 // token expired early
    http.end();
    Serial.println("poll: 401, refreshing");
    refreshAccessToken();
    return;
  }

  if (code != 200) {
    Serial.printf("poll: HTTP %d\n", code);
    http.end();
    return;
  }

  // Pull out only what we need. The full response is large.
  JsonDocument filter;
  filter["is_playing"] = true;
  filter["item"]["name"] = true;
  filter["item"]["artists"][0]["name"] = true;
  filter["item"]["album"]["images"][0]["url"]   = true;
  filter["item"]["album"]["images"][0]["width"] = true;
  filter["progress_ms"] = true;
  filter["item"]["duration_ms"] = true;

  JsonDocument doc;
  DeserializationError err =
      deserializeJson(doc, http.getStream(), DeserializationOption::Filter(filter));
  http.end();

  if (err) {
    Serial.print("poll: json error ");
    Serial.println(err.c_str());
    return;
  }

  bool playing = doc["is_playing"] | false;
  const char *title  = doc["item"]["name"] | "";
  const char *artist = doc["item"]["artists"][0]["name"] | "";

  if (strlen(title) == 0) {
    setLine("nothing playing", "", false);
    clearArt();
    return;
  }

  setLine(String(title), String(artist), playing);
  progressMs = doc["progress_ms"] | 0;
  durationMs = doc["item"]["duration_ms"] | 0;
  progressAt = millis();
  // Spotify returns several sizes (usually 640, 300, 64). Pick the one
  // closest to 300 — decoded at 1:2 that lands near our 150px box.
  JsonArray images = doc["item"]["album"]["images"];
  String best;
  int bestDelta = 99999;
  for (JsonObject img : images) {
    int w = img["width"] | 0;
    const char *u = img["url"] | "";
    if (w == 0 || strlen(u) == 0) continue;
    int delta = abs(w - 300);
    if (delta < bestDelta) {
      bestDelta = delta;
      best = u;
    }
  }

  if (best.length() && best != artUrl) {
    loadArt(best);
  }
}

// ---------------------------------------------------------------- album art

// TJpg_Decoder hands back decoded blocks; push them straight to the display.
bool jpgOutput(int16_t x, int16_t y, uint16_t w, uint16_t h, uint16_t *bitmap) {
  if (y >= tft.height()) return false;
  tft.pushImage(x, y, w, h, bitmap);
  return true;
}

void clearArt() {
  if (artUrl.isEmpty()) return;
  artUrl = "";
  tft.fillRect(ART_X, ART_Y, ART_SZ, ART_SZ, TFT_BLACK);
}

void loadArt(const String &url) {
  WiFiClientSecure client;
  client.setInsecure();

  HTTPClient http;
  if (!http.begin(client, url)) {
    Serial.println("art: begin failed");
    return;
  }

  int code = http.GET();
  if (code != 200) {
    Serial.printf("art: HTTP %d\n", code);
    http.end();
    return;
  }

  int len = http.getSize();
  if (len <= 0 || len > 200000) {      // sanity bound
    Serial.printf("art: bad length %d\n", len);
    http.end();
    return;
  }

  // Album art is 20-60kB. Prefer PSRAM so we never squeeze the heap.
  uint8_t *buf = (uint8_t *)ps_malloc(len);
  if (!buf) buf = (uint8_t *)malloc(len);
  if (!buf) {
    Serial.println("art: out of memory");
    http.end();
    return;
  }

  WiFiClient *stream = http.getStreamPtr();
  int got = 0;
  unsigned long lastData = millis();

  while (got < len && millis() - lastData < 5000) {
    size_t avail = stream->available();
    if (avail) {
      int n = stream->readBytes(buf + got, min(avail, (size_t)(len - got)));
      got += n;
      lastData = millis();
    } else {
      delay(1);
    }
  }
  http.end();

  if (got != len) {
    Serial.printf("art: short read %d/%d\n", got, len);
    artUrl = url;          // don't retry a URL that won't download
    free(buf);
    return;
  }

  TJpgDec.setJpgScale(2);              // 300px source -> 150px on screen
  TJpgDec.setSwapBytes(true);
  TJpgDec.setCallback(jpgOutput);

  tft.fillRect(ART_X, ART_Y, ART_SZ, ART_SZ, TFT_BLACK);
  JRESULT res = TJpgDec.drawJpg(ART_X, ART_Y, buf, len);
  free(buf);

  if (res == JDR_OK) {
    artUrl = url;
    Serial.println("art: drawn");
  } else {
    artUrl = url;          // don't retry a URL that won't decode
    Serial.printf("art: decode failed (%d)\n", res);
  }
}

// ---------------------------------------------------------------- display

// Only reset the scroll position when the text actually changes, so the
// marquee doesn't jump every time we poll.
void setLine(const String &t, const String &a, bool playing) {
  isPlaying = playing;
  if (t != titleLine || a != artistLine) {
    titleLine  = t;
    artistLine = a;
    scrollT = scrollA = TEXT_W;
    Serial.printf("now: %s - %s\n", t.c_str(), a.c_str());
  }
}

void drawFrame() {
  spr.fillSprite(TFT_BLACK);
  spr.setTextFont(4);
  spr.setTextDatum(TL_DATUM);

  // Title
  spr.setTextColor(isPlaying ? TFT_WHITE : TFT_DARKGREY, TFT_BLACK);
  int wt = spr.textWidth(titleLine);
  if (wt <= TEXT_W) {
    spr.drawString(titleLine, (TEXT_W - wt) / 2, 52);
  } else {
    spr.drawString(titleLine, scrollT, 52);
    spr.drawString(titleLine, scrollT + wt + GAP_PX, 52);
    if (--scrollT < -(wt + GAP_PX)) scrollT += wt + GAP_PX;
  }

  // Artist
  spr.setTextColor(TFT_DARKGREY, TFT_BLACK);
  int wa = spr.textWidth(artistLine);
  if (wa <= TEXT_W) {
    spr.drawString(artistLine, (TEXT_W - wa) / 2, 92);
  } else {
    spr.drawString(artistLine, scrollA, 92);
    spr.drawString(artistLine, scrollA + wa + GAP_PX, 92);
    if (--scrollA < -(wa + GAP_PX)) scrollA += wa + GAP_PX;
  }
  // Progress bar
  if (durationMs > 0) {
    long p = progressMs;
    if (isPlaying) p += (long)(millis() - progressAt);   // smooth between polls
    if (p > durationMs) p = durationMs;

    int barY = 140, barH = 4;
    int filled = (int)((long long)p * TEXT_W / durationMs);

    spr.fillRect(0, barY, TEXT_W, barH, 0x2104);       // unplayed track
    spr.fillRect(0, barY, filled, barH, TFT_WHITE);    // played
  }
  spr.pushSprite(TEXT_X, 0);
}

// ---------------------------------------------------------------- setup

void setup() {
  pinMode(PIN_POWER_ON, OUTPUT);
  digitalWrite(PIN_POWER_ON, HIGH);

  pinMode(PIN_LCD_BL, OUTPUT);
  digitalWrite(PIN_LCD_BL, HIGH);

  Serial.begin(115200);
  delay(300);
  Serial.println("\nnow-playing-clip starting");

  tft.init();
  tft.setRotation(1);              // landscape, 320x170
  tft.fillScreen(TFT_BLACK);

  spr.createSprite(TEXT_W, TEXT_H);

  connectWiFi();
  refreshAccessToken();
  pollNowPlaying();
}

// ---------------------------------------------------------------- loop

void loop() {
  unsigned long now = millis();

  if (WiFi.status() != WL_CONNECTED) {
    connectWiFi();
    return;
  }

  if (now - tokenTime > TOKEN_LIFETIME_MS) {
    refreshAccessToken();
  }

  if (now - lastPoll > POLL_INTERVAL_MS) {
    lastPoll = now;
    pollNowPlaying();              // blocks; scroll hitches briefly
  }

  if (now - lastStep >= SCROLL_DELAY_MS) {
    lastStep = now;
    drawFrame();
  }
}
