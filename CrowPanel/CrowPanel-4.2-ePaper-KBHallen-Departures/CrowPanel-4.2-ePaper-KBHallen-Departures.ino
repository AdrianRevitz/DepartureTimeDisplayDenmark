/*
 * KB Hallen St. departure board for the Elecrow CrowPanel ESP32-S3 5.79" e-Paper HMI Display.
 *
 * Fetches live S-train departures for KB Hallen St. (Rejseplanen stop id 8600642)
 * from the Rejseplanen API 2.0 and renders them in the style of a real Danish
 * station departure board. Layout (per Departure Board.dc.html, adapted to this
 * panel's landscape 792x272 shape): left half reserved for a future weather
 * widget, right half is 4 departure rows (line badge / destination / minutes-until).
 *
 * This panel is driven by two cascaded SSD1683 ICs, addressed internally as an
 * 800-wide buffer with an 8px seam gap. EPD.h/EPD_Init.h/spi.h (vendored alongside
 * this sketch, from Elecrow's own example repo for this exact panel) handle the
 * seam transparently -- drawing code just uses x in [0, 792) / y in [0, 272).
 * GxEPD2 does not have a working driver for this panel.
 *
 * Required libraries (Arduino Library Manager): ArduinoJson, NTPClient.
 * Board: ESP32S3 Dev Module (or "CrowPanel-ESP32S3" if you've added Elecrow's board package).
 */

#include <WiFi.h>
#include <HTTPClient.h>
#include <WiFiUdp.h>
#include <NTPClient.h>
#include <ArduinoJson.h>

#include "EPD.h"
#include "credentials.h"

// ---------------------------------------------------------------------------
// Station configuration
// ---------------------------------------------------------------------------
const char* STATION_ID = "8600642";   // KB Hallen St. (Rejseplanen stopExtId)

#define MAX_DEPARTURES 4   // per Departure Board.dc.html: exactly 4 rows

// Set to 1 to test the board (WiFi, NTP clock, e-paper wiring/rendering) before
// you have a Rejseplanen accessId -- skips the live API call and draws fake
// departures instead. Set back to 0 once you have a real accessId in credentials.h.
#define MOCK_DEPARTURES 1

// The board refreshes once a minute (see loop() below, triggered on the clock's
// minute rollover). NOTE: Rejseplanen's non-commercial free tier is capped at
// 50,000 requests/month (~1 request/minute uses ~43k/month) -- if you add more
// stations or hit the limit, gate the fetch in loop() to every Nth minute.

// Matches Elecrow's own tested default for this panel (EPD_Init.h defines the
// same value) -- landscape, still to be confirmed upright on this specific unit.
#define DISPLAY_ROTATION 180

// Visible width; EPD_W (800, from EPD_Init.h) is the internal dual-chip buffer
// stride and includes an 8px seam gap the driver hides automatically.
#define PANEL_W 792
#define PANEL_H 272
#define DEPARTURES_LEFT_X 396   // right half of the board; left half is weather (later)

// ---------------------------------------------------------------------------
// Display power pin (must be driven HIGH to power the e-paper panel)
// ---------------------------------------------------------------------------
#define EPD_PWR_PIN 7

uint8_t ImageBW[(EPD_W / 8) * EPD_H];

// ---------------------------------------------------------------------------
// Time handling (NTP + Rejseplanen date/time string, with DST auto-detect)
// ---------------------------------------------------------------------------
int utcOffsetSeconds = 3600; // Denmark base UTC offset; adjusted below for DST
WiFiUDP ntpUDP;
NTPClient timeClient(ntpUDP, "dk.pool.ntp.org", utcOffsetSeconds, 60000);

String currentDate = "";     // YYYY-MM-DD, refreshed once per day
String currentTimeHM = "";   // HH:MM, used to detect the minute has rolled over
const char* dateUrl = "https://timeapi.io/api/time/current/zone?timeZone=Europe%2FCopenhagen";

// ---------------------------------------------------------------------------
// Rejseplanen request
// ---------------------------------------------------------------------------
const char* baseUrl = "https://www.rejseplanen.dk/api/";
HTTPClient http;

struct Departure {
  char line[8];
  char direction[28];
  char time[6];     // "HH:MM"
  char rtTime[6];   // "HH:MM", empty if no real-time update
  char track[6];
  bool cancelled;
};

Departure departures[MAX_DEPARTURES];
int departureCount = 0;

// ---------------------------------------------------------------------------
// ASCII sanitizing -- the panel's bitmap font is indexed as (char - ' '), i.e.
// plain ASCII only. Danish station/destination names contain UTF-8 multi-byte
// characters (æ, ø, å) that would index out of the font table and either draw
// garbage or read out of bounds, so everything gets transliterated/stripped
// before it's ever passed to EPD_ShowString.
// ---------------------------------------------------------------------------
String toAscii(const String& input) {
  String out;
  out.reserve(input.length());
  for (size_t i = 0; i < input.length(); i++) {
    uint8_t c = input[i];
    if (c == 0xC3 && i + 1 < input.length()) {
      uint8_t next = input[i + 1];
      i++;
      switch (next) {
        case 0xA6: out += "ae"; break; // æ
        case 0x86: out += "Ae"; break; // Æ
        case 0xB8: out += "o";  break; // ø
        case 0x98: out += "O";  break; // Ø
        case 0xA5: out += "aa"; break; // å
        case 0x85: out += "Aa"; break; // Å
        default: out += '?'; break;
      }
    } else if (c >= 0x20 && c <= 0x7E) {
      out += (char)c;
    }
    // any other byte (stray UTF-8 continuation bytes, control chars) is dropped
  }
  return out;
}

// Truncates to maxChars so a drawn string can never run past the edge of a
// column (or the panel) -- the pixel writer does not bounds-check X.
// (Named truncateText, not truncate, to avoid colliding with the POSIX
// truncate() prototype newlib pulls in on ESP32.)
String truncateText(const String& s, int maxChars) {
  if ((int)s.length() <= maxChars) return s;
  if (maxChars <= 1) return s.substring(0, maxChars);
  return s.substring(0, maxChars - 1) + ".";
}

void drawText(uint16_t x, uint16_t y, const String& raw, uint16_t size, int maxChars) {
  String safe = truncateText(toAscii(raw), maxChars);
  EPD_ShowString(x, y, safe.c_str(), size, BLACK);
}

// ---------------------------------------------------------------------------
// Date/time bootstrap -- mirrors the working DST-detection approach from
// Rejseplan_API_Call_version_0_3: Rejseplanen's own clock (via timeapi.io) is
// compared against the NTP hour, and the NTP UTC offset is nudged by an hour
// when they disagree, since Denmark's UTC+1/UTC+2 summer-time switch isn't
// otherwise known to the board.
// ---------------------------------------------------------------------------
void syncDateAndDetectDst() {
  String payload = httpGet(dateUrl);
  if (payload.length() == 0) return;

  JsonDocument doc;
  deserializeJson(doc, payload);
  String dateTimeRaw = doc["dateTime"].as<String>();

  int splitIndex = dateTimeRaw.indexOf('T');
  if (splitIndex < 0) return;
  currentDate = dateTimeRaw.substring(0, splitIndex);
  String localHour = dateTimeRaw.substring(splitIndex + 1, splitIndex + 3);

  int apiHour = localHour.toInt();
  int ntpHour = timeClient.getHours();
  if (apiHour != ntpHour) {
    if (apiHour == 0 && ntpHour == 23) utcOffsetSeconds += 3600;
    else if (apiHour == 23 && ntpHour == 0) utcOffsetSeconds -= 3600;
    else if (ntpHour < apiHour) utcOffsetSeconds += 3600;
    else if (ntpHour > apiHour) utcOffsetSeconds -= 3600;
    timeClient.setTimeOffset(utcOffsetSeconds);
    timeClient.update();
  }
}

String httpGet(const char* url) {
  String payload = "";
  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("WiFi disconnected, skipping request");
    return payload;
  }
  http.begin(url);
  http.setUserAgent("Mozilla/5.0 (X11; Ubuntu; Linux x86_64; rv:78.0) Gecko/20100101 Firefox/78.0");
  int code = http.GET();
  if (code > 0) {
    payload = http.getString();
    if (payload.length() == 0) Serial.println("Empty response body");
  } else {
    Serial.println("HTTP error: " + String(code));
  }
  http.end();
  return payload;
}

// ---------------------------------------------------------------------------
// Fetch + parse departures
// ---------------------------------------------------------------------------
// Fake board matching what KB Hallen's F-line departures actually look like,
// used only while MOCK_DEPARTURES is 1. Times are generated relative to the
// current clock (rather than hardcoded) so the demo always shows sensible
// positive minute countdowns whenever you flash it.
void loadMockDepartures() {
  int nowH = currentTimeHM.substring(0, 2).toInt();
  int nowM = currentTimeHM.substring(3, 5).toInt();

  struct { const char* dir; int offsetMin; int rtOffsetMin; bool cancelled; } mock[] = {
    { "Kobenhavn Syd", 1,  0,  false },  // rtOffsetMin 0 = on time (no rtTime)
    { "Hellerup",      6,  9,  false },  // running a few minutes late
    { "Kobenhavn Syd", 13, 0,  true  },  // cancelled
    { "Hellerup",      18, 0,  false },
  };

  departureCount = 0;
  for (int i = 0; i < MAX_DEPARTURES && i < (int)(sizeof(mock) / sizeof(mock[0])); i++) {
    Departure& d = departures[i];
    strlcpy(d.line, "S F", sizeof(d.line));
    strlcpy(d.direction, mock[i].dir, sizeof(d.direction));

    int total = ((nowH * 60 + nowM + mock[i].offsetMin) % 1440 + 1440) % 1440;
    char buf[9];
    snprintf(buf, sizeof(buf), "%02d:%02d:00", total / 60, total % 60);
    strlcpy(d.time, buf, sizeof(d.time));

    if (mock[i].rtOffsetMin > 0) {
      int rtTotal = ((nowH * 60 + nowM + mock[i].rtOffsetMin) % 1440 + 1440) % 1440;
      snprintf(buf, sizeof(buf), "%02d:%02d:00", rtTotal / 60, rtTotal % 60);
      strlcpy(d.rtTime, buf, sizeof(d.rtTime));
    } else {
      d.rtTime[0] = '\0';
    }

    strlcpy(d.track, "1", sizeof(d.track));
    d.cancelled = mock[i].cancelled;
    departureCount++;
  }
}

bool fetchDepartures() {
#if MOCK_DEPARTURES
  loadMockDepartures();
  return true;
#else
  String url = String(baseUrl) + "departureBoard?id=" + STATION_ID +
               "&date=" + currentDate + "&time=" + currentTimeHM +
               "&maxJourneys=" + String(MAX_DEPARTURES) +
               "&accessId=" + rejseplanenAccessId + "&format=json";

  String payload = httpGet(url.c_str());
  if (payload.length() == 0) return false;

  JsonDocument filter;
  filter["Departure"][0]["name"] = true;
  filter["Departure"][0]["direction"] = true;
  filter["Departure"][0]["time"] = true;
  filter["Departure"][0]["rtTime"] = true;
  filter["Departure"][0]["track"] = true;
  filter["Departure"][0]["cancelled"] = true;

  JsonDocument doc;
  DeserializationError err = deserializeJson(doc, payload, DeserializationOption::Filter(filter));
  if (err) {
    Serial.println("JSON parse failed: " + String(err.c_str()));
    return false;
  }

  JsonArray items = doc["Departure"].as<JsonArray>();
  departureCount = 0;
  for (JsonVariant item : items) {
    if (departureCount >= MAX_DEPARTURES) break;
    Departure& d = departures[departureCount];

    strlcpy(d.line, item["name"] | "?", sizeof(d.line));
    strlcpy(d.direction, item["direction"] | "", sizeof(d.direction));
    strlcpy(d.time, (item["time"] | "??:??:??"), sizeof(d.time)); // "HH:MM" (truncates seconds)
    d.cancelled = item["cancelled"] | false;

    if (item["rtTime"].is<const char*>()) {
      strlcpy(d.rtTime, item["rtTime"].as<const char*>(), sizeof(d.rtTime));
    } else {
      d.rtTime[0] = '\0';
    }
    strlcpy(d.track, (item["track"] | ""), sizeof(d.track));

    departureCount++;
  }
  return true;
#endif
}

// ---------------------------------------------------------------------------
// Minutes-until-departure + line badge letter (e.g. Rejseplanen's "S F" -> 'F')
// ---------------------------------------------------------------------------
int minutesUntil(const char* hhmmss) {
  if (strlen(hhmmss) < 5) return 0;
  int depH = (hhmmss[0] - '0') * 10 + (hhmmss[1] - '0');
  int depM = (hhmmss[3] - '0') * 10 + (hhmmss[4] - '0');
  int nowH = currentTimeHM.substring(0, 2).toInt();
  int nowM = currentTimeHM.substring(3, 5).toInt();
  int diff = (depH * 60 + depM) - (nowH * 60 + nowM);
  if (diff < -60) diff += 1440; // crossed midnight
  if (diff < 0) diff = 0;       // clock jitter safety
  return diff;
}

char badgeLetterFor(const Departure& d) {
  // Plain arithmetic instead of isalpha()/toupper() -- those do a lookup-table
  // read in flash that triggered an unaligned-access crash on this hardware.
  for (int i = (int)strlen(d.line) - 1; i >= 0; i--) {
    char c = d.line[i];
    if (c >= 'a' && c <= 'z') return c - 'a' + 'A';
    if (c >= 'A' && c <= 'Z') return c;
  }
  return '?';
}

// ---------------------------------------------------------------------------
// Rendering -- right half (x >= DEPARTURES_LEFT_X) is this board's 4-row
// departure list per Departure Board.dc.html; left half stays blank, reserved
// for a weather widget to be added later.
// ---------------------------------------------------------------------------
// Set to 1 to isolate a crash: keeps the full WiFi/NTP/JSON flow in setup()
// but skips all badge/rectangle drawing here, drawing one plain line instead.
// If this still crashes, the bug is in the WiFi/JSON layer, not the drawing
// code below. Temporary -- remove once the board is confirmed stable.
#define DEBUG_SIMPLE_RENDER 0

void renderDepartures() {
  Paint_NewImage(ImageBW, EPD_W, EPD_H, DISPLAY_ROTATION, WHITE);
  Paint_Clear(WHITE);

  // Elecrow's own tested clear/init sequence for this panel (from their
  // wifi_http_openweather example) -- run before every draw.
  EPD_FastMode1Init();
  EPD_Display_Clear();
  EPD_Update();
  EPD_Clear_R26A6H();

#if DEBUG_SIMPLE_RENDER
  EPD_ShowString(300, 120, "WIFI+JSON OK", 24, BLACK);
  EPD_Display(ImageBW);
  EPD_PartUpdate();
  EPD_DeepSleep();
  return;
#endif

  EPD_DrawLine(DEPARTURES_LEFT_X, 0, DEPARTURES_LEFT_X, PANEL_H, BLACK);

  if (departureCount == 0) {
    drawText(DEPARTURES_LEFT_X + 8, 120, "No live departures", 16, 40);
  } else {
    const int rowHeight = PANEL_H / MAX_DEPARTURES; // 272/4 = 68
    const int badgeSize = 44;
    const int badgeX = DEPARTURES_LEFT_X + 8;
    const int destX = badgeX + badgeSize + 8;   // 456
    const int timeColRight = PANEL_W - 8;        // right margin against panel edge (792-8)

    for (int i = 0; i < departureCount; i++) {
      Serial.printf("row %d: start\n", i);
      int rowTop = i * rowHeight;
      Departure& d = departures[i];

      // Line badge: filled black square, letter punched out in white on top.
      int badgeY = rowTop + (rowHeight - badgeSize) / 2;
      EPD_DrawRectangle(badgeX, badgeY, badgeX + badgeSize, badgeY + badgeSize, BLACK, 1);
      Serial.printf("row %d: rectangle done\n", i);
      char badgeStr[2] = { badgeLetterFor(d), '\0' };
      Serial.printf("row %d: badge letter '%c'\n", i, badgeStr[0]);
      EPD_ShowString(badgeX + (badgeSize - 12) / 2, badgeY + (badgeSize - 24) / 2, badgeStr, 24, WHITE);
      Serial.printf("row %d: badge string done\n", i);

      drawText(destX, rowTop + (rowHeight - 16) / 2, d.direction, 16, 28);
      Serial.printf("row %d: destination done\n", i);

      String timeLabel;
      if (d.cancelled) {
        timeLabel = "X";
      } else {
        bool delayed = d.rtTime[0] != '\0';
        int mins = minutesUntil(delayed ? d.rtTime : d.time);
        timeLabel = (mins <= 0) ? "Now" : (String(mins) + " min");
      }
      Serial.printf("row %d: timeLabel = %s\n", i, timeLabel.c_str());
      int timeX = timeColRight - (int)timeLabel.length() * 12;
      drawText(timeX, rowTop + (rowHeight - 24) / 2, timeLabel, 24, 8);
      Serial.printf("row %d: time done\n", i);

      if (i < departureCount - 1) {
        EPD_DrawLine(DEPARTURES_LEFT_X, rowTop + rowHeight, PANEL_W, rowTop + rowHeight, BLACK);
      }
    }
  }

  EPD_Display(ImageBW);
  EPD_PartUpdate();
  EPD_DeepSleep();
}

// ---------------------------------------------------------------------------
// Setup / loop
// ---------------------------------------------------------------------------
void connectWifi() {
  WiFi.mode(WIFI_STA);
  WiFi.begin(ssid, password);
  Serial.print("Connecting to WiFi");
  while (WiFi.status() != WL_CONNECTED) {
    delay(300);
    Serial.print(".");
  }
  Serial.println();
  Serial.print("Connected, IP: ");
  Serial.println(WiFi.localIP());
}

// Diagnostic mode: bypasses WiFi/NTP/departures entirely and just runs the bare
// panel clear + one line of plain text, matching Elecrow's own demo sequence
// exactly. Used to isolate whether a crash is in the base EPD driver/hardware
// vs. in this sketch's badge/rectangle drawing code. Temporary -- remove once
// the board is confirmed stable.
#define DEBUG_MINIMAL_EPD_TEST 0

void setup() {
  Serial.begin(115200);
  delay(200);

  pinMode(EPD_PWR_PIN, OUTPUT);
  digitalWrite(EPD_PWR_PIN, HIGH);
  EPD_GPIOInit();

#if DEBUG_MINIMAL_EPD_TEST
  Serial.println("DEBUG_MINIMAL_EPD_TEST: clearing panel...");
  Paint_NewImage(ImageBW, EPD_W, EPD_H, DISPLAY_ROTATION, WHITE);
  Paint_Clear(WHITE);
  EPD_FastMode1Init();
  Serial.println("EPD_FastMode1Init done");
  EPD_Display_Clear();
  Serial.println("EPD_Display_Clear done");
  EPD_Update();
  Serial.println("EPD_Update done");
  EPD_Clear_R26A6H();
  Serial.println("EPD_Clear_R26A6H done");
  EPD_ShowString(300, 120, "TEST OK", 24, BLACK);
  EPD_Display(ImageBW);
  Serial.println("EPD_Display done");
  EPD_PartUpdate();
  Serial.println("EPD_PartUpdate done");
  EPD_DeepSleep();
  Serial.println("EPD_DeepSleep done -- if you see this, the base driver is fine");
  return; // skip WiFi/departures entirely in this mode
#endif

  connectWifi();

  timeClient.begin();
  timeClient.update();
  syncDateAndDetectDst();
  currentTimeHM = timeClient.getFormattedTime().substring(0, 5);

  fetchDepartures();
  renderDepartures();
}

void loop() {
  timeClient.update();
  String nowHM = timeClient.getFormattedTime().substring(0, 5);

  if (nowHM != currentTimeHM) {
    if (nowHM == "00:00") {
      Serial.println("Day switch, re-syncing date");
      syncDateAndDetectDst();
    }
    currentTimeHM = nowHM;

    if (fetchDepartures()) {
      renderDepartures();
    } else {
      Serial.println("Departure fetch failed, keeping previous board");
    }
  }

  delay(2000);
}
