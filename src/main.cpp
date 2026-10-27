#include <Arduino.h>
#include <SPI.h>
#include <WiFi.h>
#include <time.h>
#include <new>
#include <esp_sntp.h>
#include <Adafruit_GFX.h>
#include <Adafruit_GC9A01A.h>
#include "VerdanaBold38pt7b.h"
#include "VerdanaBold55pt7b.h"
#include "secrets.h"   // WIFI_SSID, WIFI_PASSWORD (wzor: include/secrets.h.example)

// Polska: CET/CEST z automatyczna zmiana czasu
// (ostatnia niedziela marca 2:00 -> lato, ostatnia niedziela pazdziernika 3:00 -> zima)
const char* timeZone  = "CET-1CEST,M3.5.0,M10.5.0/3";
const char* ntpServer = "pool.ntp.org";

// Ponowna synchronizacja NTP: codziennie o tej godzinie
const int      RESYNC_HOUR       = 3;
// Odstep miedzy probami, gdy synchronizacja sie nie udala
const uint32_t RETRY_INTERVAL_MS = 5UL * 60 * 1000;

const uint32_t WIFI_TIMEOUT_MS = 20000;
const uint32_t NTP_TIMEOUT_MS  = 15000;

// Piny TFT
#define TFT_SCK   2
#define TFT_MOSI  4
#define TFT_CS    5
#define TFT_DC    1
#define TFT_RST   0
#define TFT_BL    3

#define SCREEN_WIDTH  240
#define SCREEN_HEIGHT 240

// Ekran podzielony na dwa pasy: godziny (gora) i minuty (dol)
#define SPLIT_Y       125
#define HOUR_CENTER_Y 80
#define MIN_CENTER_Y  175

const uint16_t COLOR_HOUR   = 0xF800;  // #FE0000
const uint16_t COLOR_MINUTE = 0xFCE0;  // #FF9C00

Adafruit_GC9A01A tft(TFT_CS, TFT_DC, -1);

// Bufor jednego pasa - rysowanie bez czyszczenia ekranu (brak migania)
GFXcanvas16* canvas = nullptr;

int      lastMinute     = -1;
bool     timeSynced     = false;
int      lastSyncYday   = -1;
uint32_t lastAttemptMs  = 0;
bool     attemptedOnce  = false;

void drawCenteredText(Adafruit_GFX& g, const char* txt, uint16_t color, int16_t centerY) {
  int16_t x1, y1;
  uint16_t w, h;
  g.getTextBounds(txt, 0, 0, &x1, &y1, &w, &h);
  int16_t cx = (SCREEN_WIDTH - w) / 2 - x1;
  int16_t cy = centerY - h / 2 - y1;
  g.setTextColor(color);
  g.setCursor(cx, cy);
  g.print(txt);
}

// Rysuje tekst wysrodkowany w pasie [bandY, bandY + bandH)
void drawBand(const char* txt, const GFXfont* font, uint16_t color,
              int16_t bandY, int16_t bandH, int16_t centerY) {
  if (canvas) {
    canvas->fillScreen(GC9A01A_BLACK);
    canvas->setFont(font);
    canvas->setTextSize(1);
    drawCenteredText(*canvas, txt, color, centerY - bandY);
    tft.drawRGBBitmap(0, bandY, canvas->getBuffer(), SCREEN_WIDTH, bandH);
  } else {
    // Brak pamieci na bufor - rysowanie bezposrednie
    tft.fillRect(0, bandY, SCREEN_WIDTH, bandH, GC9A01A_BLACK);
    tft.setFont(font);
    tft.setTextSize(1);
    drawCenteredText(tft, txt, color, centerY);
  }
}

void drawClock(int hour, int minute) {
  char hBuf[3], mBuf[3];
  snprintf(hBuf, sizeof(hBuf), "%02d", hour);
  snprintf(mBuf, sizeof(mBuf), "%02d", minute);

  drawBand(hBuf, &Verdana_Bold55pt7b, COLOR_HOUR,   0,       SPLIT_Y,                 HOUR_CENTER_Y);
  drawBand(mBuf, &Verdana_Bold38pt7b, COLOR_MINUTE, SPLIT_Y, SCREEN_HEIGHT - SPLIT_Y, MIN_CENTER_Y);
}

void showMessage(const char* msg) {
  tft.fillScreen(GC9A01A_BLACK);
  tft.setFont(NULL); // domyslna czcionka dla komunikatow
  tft.setTextColor(GC9A01A_WHITE);
  tft.setTextSize(2);
  int16_t x = (SCREEN_WIDTH - (int16_t)strlen(msg) * 12) / 2;
  tft.setCursor(x < 0 ? 10 : x, 106);
  tft.print(msg);
  lastMinute = -1; // wymus przerysowanie zegara
}

// Laczy sie z WiFi, synchronizuje czas z NTP i wylacza WiFi.
// showStatus = false: komunikaty tylko na Serial (zegar zostaje na ekranie).
bool syncTime(bool showStatus) {
  lastAttemptMs = millis();
  attemptedOnce = true;

  Serial.println("Laczenie z WiFi...");
  if (showStatus) showMessage("WiFi...");

  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);

  uint32_t start = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - start < WIFI_TIMEOUT_MS) {
    delay(250);
  }

  bool synced = false;
  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("Brak WiFi!");
    if (showStatus) showMessage("Brak WiFi!");
  } else {
    Serial.println("WiFi OK, NTP...");
    if (showStatus) showMessage("NTP...");

    sntp_get_sync_status(); // wyczysc ewentualny poprzedni status COMPLETED
    configTzTime(timeZone, ntpServer);

    start = millis();
    while (millis() - start < NTP_TIMEOUT_MS) {
      if (sntp_get_sync_status() == SNTP_SYNC_STATUS_COMPLETED) {
        synced = true;
        break;
      }
      delay(250);
    }

    if (!synced) {
      Serial.println("NTP BLAD");
      if (showStatus) showMessage("NTP BLAD");
    }
  }

  WiFi.disconnect(true);
  WiFi.mode(WIFI_OFF);

  if (synced) {
    time_t now = time(nullptr);
    struct tm ti;
    localtime_r(&now, &ti);
    timeSynced   = true;
    lastSyncYday = ti.tm_yday;
    Serial.printf("Czas zsynchronizowany: %02d:%02d:%02d\n", ti.tm_hour, ti.tm_min, ti.tm_sec);
  }
  return synced;
}

void setup() {
  Serial.begin(115200);
  delay(1500);

  SPI.begin(TFT_SCK, -1, TFT_MOSI, TFT_CS);

  // Reset TFT
  pinMode(TFT_RST, OUTPUT);
  digitalWrite(TFT_RST, LOW);  delay(10);
  digitalWrite(TFT_RST, HIGH); delay(150);

  tft.begin();
  tft.setRotation(0);
  analogWrite(TFT_BL, 255);

  canvas = new (std::nothrow) GFXcanvas16(SCREEN_WIDTH, SPLIT_Y);
  if (canvas && !canvas->getBuffer()) {
    delete canvas;
    canvas = nullptr;
  }
  if (!canvas) Serial.println("Brak pamieci na bufor - rysowanie bezposrednie");

  syncTime(true);
  Serial.println("Gotowy.");
}

void loop() {
  if (!timeSynced) {
    // Ponawiaj synchronizacje, dopoki sie nie uda
    if (!attemptedOnce || millis() - lastAttemptMs >= RETRY_INTERVAL_MS) {
      syncTime(true);
    }
    delay(500);
    return;
  }

  time_t now = time(nullptr);
  struct tm timeinfo;
  localtime_r(&now, &timeinfo);

  // Codzienna korekta zegara (proby co RETRY_INTERVAL_MS w ciagu RESYNC_HOUR)
  if (timeinfo.tm_hour == RESYNC_HOUR && timeinfo.tm_yday != lastSyncYday &&
      millis() - lastAttemptMs >= RETRY_INTERVAL_MS) {
    syncTime(false);
    now = time(nullptr);
    localtime_r(&now, &timeinfo);
  }

  int h = timeinfo.tm_hour;
  int m = timeinfo.tm_min;

  if (m != lastMinute) {
    lastMinute = m;
    drawClock(h, m);
    Serial.printf("Czas: %02d:%02d\n", h, m);
  }

  delay(500);
}
