/*
 * now-playing-clip
 *
 * Shows the currently playing Spotify track on a LILYGO T-Display-S3,
 * scrolling the title and artist across the screen.
 *
 * See README.md for Spotify app registration and refresh token setup.
 */

#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <TFT_eSPI.h>

#include "secrets.h"

// ---------------------------------------------------------------- hardware

#define PIN_POWER_ON 15   // MUST be HIGH or the screen is black on battery
#define PIN_LCD_BL   38   // backlight

TFT_eSPI tft;
TFT_eSprite spr = TFT_eSprite(&tft);

static const int SCREEN_W = 320;
static const int SCREEN_H = 170;

// ---------------------------------------------------------------- timing

static const unsigned long POLL_INTERVAL_MS  = 4000;        // ask Spotify
static const unsigned long TOKEN_LIFETIME_MS = 50UL * 60000; // refresh early
static const int           SCROLL_DELAY_MS   = 16;
static const int           GAP_PX            = 60;           // between copies

unsigned long lastPoll  = 0;
unsigned long tokenTime = 0;
unsigned long lastStep  = 0;

// ---------------------------------------------------------------- state

String accessToken = "";
String nowLine     = "nothing playing";
bool   isPlaying   = false;
int    scrollX     = SCREEN_W;

// ---------------------------------------------------------------- prototypes

// Arduino's auto-prototype generator misses functions taking a reference,
// so declare these explicitly.
void setLine(const String &line, bool playing);
bool refreshAccessToken();
void pollNowPlaying();
void drawFrame();

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

// Trade the long-lived refresh token for a short-lived access token.
bool refreshAccessToken() {
  WiFiClientSecure client;
  client.setInsecure();          // see README note on TLS

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

// Ask Spotify what's playing. Updates nowLine / isPlaying.
void pollNowPlaying() {
  if (accessToken.isEmpty()) return;

  WiFiClientSecure client;
  client.setInsecure();

  HTTPClient http;
  const char *url = "https://api.spotify.com/v1/me/player/currently-playing";
  if (!http.begin(client, url)) return;

  http.addHeader("Authorization", String("Bearer ") + accessToken);
  int code = http.GET();

  // 204 = nothing playing, and an empty body. Not an error.
  if (code == 204) {
    http.end();
    setLine("nothing playing", false);
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

  // Only pull out the two fields we need — the full response is large.
  JsonDocument filter;
  filter["is_playing"] = true;
  filter["item"]["name"] = true;
  filter["item"]["artists"][0]["name"] = true;

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
    setLine("nothing playing", false);
    return;
  }

  setLine(String(title) + "  \u2014  " + artist, playing);
}

// ---------------------------------------------------------------- display

// Only reset the scroll position when the text actually changes, so the
// marquee doesn't jump every time we poll.
void setLine(const String &line, bool playing) {
  isPlaying = playing;
  if (line != nowLine) {
    nowLine = line;
    scrollX = SCREEN_W;
    Serial.print("now: ");
    Serial.println(nowLine);
  }
}

void drawFrame() {
  uint16_t bg = isPlaying ? TFT_BLACK : 0x18E3;   // dim grey when idle
  uint16_t fg = isPlaying ? TFT_GREEN : TFT_DARKGREY;

  spr.fillSprite(bg);
  spr.setTextColor(fg, bg);
  spr.setTextFont(4);

  int w = spr.textWidth(nowLine);

  spr.drawString(nowLine, scrollX, 70);
  spr.drawString(nowLine, scrollX + w + GAP_PX, 70);   // second copy = loop

  spr.pushSprite(0, 0);

  if (--scrollX < -(w + GAP_PX)) scrollX = 0;
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

  spr.createSprite(SCREEN_W, SCREEN_H);

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
    pollNowPlaying();              // blocks ~200-500ms; scroll will hitch
  }

  if (now - lastStep >= SCROLL_DELAY_MS) {
    lastStep = now;
    drawFrame();
  }
}
