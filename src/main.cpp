#include <Arduino.h>
#include <SPI.h>
#include <WiFi.h>
#include <time.h>
#include <new>
#include <esp_sntp.h>
#include <Adafruit_GFX.h>
#include <Adafruit_GC9A01A.h>
#include <HTTPClient.h>
#include <WiFiClientSecure.h>
#include <ArduinoJson.h>
#include <Fonts/FreeSansBold24pt7b.h>
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

// Pogoda: Krakow, Open-Meteo (bez klucza API)
const char* WEATHER_URL =
  "https://api.open-meteo.com/v1/forecast?latitude=50.06&longitude=19.94"
  "&daily=weather_code,temperature_2m_max,precipitation_probability_max,wind_speed_10m_max"
  "&timezone=Europe%2FWarsaw&forecast_days=2";
// Tryb demo: co 3 s losowe dane pogodowe (1 = wlaczony, 0 = prawdziwa pogoda)
#define WEATHER_DEMO 0
const uint32_t DEMO_INTERVAL_MS = 3000;

// Pasek wiatru na srodku ekranu
const int WIND_FULL_KMH = 60;   // przy tej predkosci pasek ma pelna dlugosc
const int WIND_BAR_X    = 20;
const int WIND_BAR_W    = 200;
const int WIND_BAR_H    = 12;

// Kiedy pokazywac pogode na przemian z zegarem: [od, do) godzin
const int MORNING_FROM = 6,  MORNING_TO = 9;    // pogoda na dzis
const int EVENING_FROM = 21, EVENING_TO = 24;   // pogoda na jutro
// Podglad: przemiennie przez cala dobe (na zmiane dzis / jutro). 0 = wg godzin powyzej
#define PREVIEW_ALL_DAY 0
// Cykl przemienny: CLOCK_SECS zegar, potem WEATHER_SECS pogoda
const int CLOCK_SECS   = 10;
const int WEATHER_SECS = 4;

const uint32_t WEATHER_INTERVAL_MS = 30UL * 60 * 1000;  // odswiezanie co 30 min

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

// Motywy kolorow: ciemny (czarne tlo) i jasny (biale tlo)
struct Theme {
  uint16_t bg, hour, minute, sun, cloud, dark, snow;
};
const Theme THEME_DARK = {
  0x0000,  // tlo: czarne
  0xF800,  // godziny: czerwone #FE0000
  0xFCE0,  // minuty: pomaranczowe #FF9C00
  0xFE60,  // slonce: zolte
  0xC618,  // chmura: jasnoszara
  0x7BEF,  // chmura burzowa: ciemnoszara
  0xFFFF,  // snieg: bialy
};
const Theme THEME_LIGHT = {
  0xFFFF,  // tlo: biale
  0x0000,  // godziny: czarne
  0x0299,  // minuty: granatowo-niebieskie #0050C8
  0xFD20,  // slonce: pomaranczowo-zolte (zolte ginie na bialym)
  0x8C51,  // chmura: szara
  0x4208,  // chmura burzowa: grafitowa
  0x3D7F,  // snieg: niebieski
};
// Jasny motyw w godzinach [LIGHT_FROM, LIGHT_TO), poza nimi ciemny
const int LIGHT_FROM = 6, LIGHT_TO = 18;
const Theme* theme = &THEME_DARK;

#define COLOR_BG     (theme->bg)
#define COLOR_HOUR   (theme->hour)
#define COLOR_MINUTE (theme->minute)
#define COLOR_SUN    (theme->sun)
#define COLOR_CLOUD  (theme->cloud)
#define COLOR_DARK   (theme->dark)
#define COLOR_SNOW   (theme->snow)
const uint16_t COLOR_RAIN = 0x3D7F;  // niebieski

Adafruit_GC9A01A tft(TFT_CS, TFT_DC, -1);

// Bufor jednego pasa - rysowanie bez czyszczenia ekranu (brak migania)
GFXcanvas16* canvas = nullptr;

int      lastMinute     = -1;
bool     timeSynced     = false;
int      lastSyncYday   = -1;
uint32_t lastAttemptMs  = 0;
bool     attemptedOnce  = false;

// Dane pogodowe
struct DayWeather {
  int code;
  int tempMax;
  int rainChance;  // %
  int windMax;     // km/h
};
DayWeather forecast[2];          // [0] = dzis, [1] = jutro
bool     weatherValid    = false;
uint32_t lastWeatherMs   = 0;
bool     weatherAttempted = false;
bool     weatherDirty    = false;   // trzeba przerysowac ekran

enum Screen { SCREEN_NONE, SCREEN_CLOCK, SCREEN_TODAY, SCREEN_TOMORROW };
Screen   currentScreen   = SCREEN_NONE;

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
    canvas->fillScreen(COLOR_BG);
    canvas->setFont(font);
    canvas->setTextSize(1);
    drawCenteredText(*canvas, txt, color, centerY - bandY);
    tft.drawRGBBitmap(0, bandY, canvas->getBuffer(), SCREEN_WIDTH, bandH);
  } else {
    // Brak pamieci na bufor - rysowanie bezposrednie
    tft.fillRect(0, bandY, SCREEN_WIDTH, bandH, COLOR_BG);
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
  tft.fillScreen(COLOR_BG);
  tft.setFont(NULL); // domyslna czcionka dla komunikatow
  tft.setTextColor(COLOR_HOUR);
  tft.setTextSize(2);
  int16_t x = (SCREEN_WIDTH - (int16_t)strlen(msg) * 12) / 2;
  tft.setCursor(x < 0 ? 10 : x, 106);
  tft.print(msg);
  lastMinute = -1; // wymus przerysowanie zegara
  currentScreen = SCREEN_NONE;
}

bool wifiConnect() {
  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  uint32_t start = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - start < WIFI_TIMEOUT_MS) {
    delay(250);
  }
  return WiFi.status() == WL_CONNECTED;
}

void wifiOff() {
  WiFi.disconnect(true);
  WiFi.mode(WIFI_OFF);
}

// Laczy sie z WiFi, synchronizuje czas z NTP i wylacza WiFi.
// showStatus = false: komunikaty tylko na Serial (zegar zostaje na ekranie).
bool syncTime(bool showStatus) {
  lastAttemptMs = millis();
  attemptedOnce = true;

  Serial.println("Laczenie z WiFi...");
  if (showStatus) showMessage("WiFi...");

  bool synced = false;
  if (!wifiConnect()) {
    Serial.println("Brak WiFi!");
    if (showStatus) showMessage("Brak WiFi!");
  } else {
    Serial.println("WiFi OK, NTP...");
    if (showStatus) showMessage("NTP...");

    sntp_get_sync_status(); // wyczysc ewentualny poprzedni status COMPLETED
    configTzTime(timeZone, ntpServer);

    uint32_t start = millis();
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

  wifiOff();

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

// ---------------------------------------------------------------------------
// Pogoda
// ---------------------------------------------------------------------------

// Pobiera prognoze na dzis (Open-Meteo). Sam laczy sie z WiFi i je wylacza.
bool fetchWeather() {
  lastWeatherMs    = millis();
  weatherAttempted = true;

  Serial.println("Pogoda: laczenie z WiFi...");
  bool ok = false;
  if (!wifiConnect()) {
    Serial.println("Pogoda: brak WiFi");
  } else {
    WiFiClientSecure client;
    client.setInsecure();  // dane pogodowe - bez weryfikacji certyfikatu
    HTTPClient http;
    http.setTimeout(10000);
    if (http.begin(client, WEATHER_URL)) {
      int code = http.GET();
      if (code == HTTP_CODE_OK) {
        String body = http.getString();
        JsonDocument doc;
        DeserializationError err = deserializeJson(doc, body);
        if (!err) {
          JsonObject daily = doc["daily"];
          if (!daily["weather_code"][1].isNull()) {
            for (int d = 0; d < 2; d++) {
              DayWeather& w = forecast[d];
              w.code       = daily["weather_code"][d] | 0;
              w.tempMax    = (int)lroundf(daily["temperature_2m_max"][d] | 0.0f);
              w.rainChance = daily["precipitation_probability_max"][d] | 0;
              w.windMax    = (int)lroundf(daily["wind_speed_10m_max"][d] | 0.0f);
              Serial.printf("Pogoda %s: kod %d, max %d C, deszcz %d%%, wiatr %d km/h\n",
                            d ? "jutro" : "dzis", w.code, w.tempMax, w.rainChance, w.windMax);
            }
            weatherValid = true;
            weatherDirty = true;
            ok = true;
          }
        } else {
          Serial.printf("Pogoda: blad JSON: %s\n", err.c_str());
        }
      } else {
        Serial.printf("Pogoda: HTTP %d\n", code);
      }
      http.end();
    }
  }
  wifiOff();
  return ok;
}

// Gruba linia (kilka przesunietych linii)
void thickLine(Adafruit_GFX& g, int x0, int y0, int x1, int y1, uint16_t c) {
  for (int d = -1; d <= 1; d++) {
    g.drawLine(x0 + d, y0, x1 + d, y1, c);
    g.drawLine(x0, y0 + d, x1, y1 + d, c);
  }
}

void drawSun(Adafruit_GFX& g, int cx, int cy, int r) {
  for (int i = 0; i < 8; i++) {
    float a = i * PI / 4;
    thickLine(g, cx + cosf(a) * (r + 5), cy + sinf(a) * (r + 5),
                 cx + cosf(a) * (r + 13), cy + sinf(a) * (r + 13), COLOR_SUN);
  }
  g.fillCircle(cx, cy, r, COLOR_SUN);
}

// Chmura o szerokosci ~56 px, (cx, cy) = srodek dolnej czesci
void drawCloud(Adafruit_GFX& g, int cx, int cy, uint16_t c) {
  g.fillCircle(cx - 14, cy, 12, c);
  g.fillCircle(cx + 2,  cy - 8, 16, c);
  g.fillCircle(cx + 17, cy + 1, 11, c);
  g.fillRoundRect(cx - 26, cy, 54, 13, 6, c);
}

// Ikona 64x64 wysrodkowana w (cx, cy) wg kodu WMO
void drawWeatherIcon(Adafruit_GFX& g, int code, int cx, int cy) {
  if (code == 0) {                                   // bezchmurnie
    drawSun(g, cx, cy, 15);
  } else if (code <= 2) {                            // czesciowe zachmurzenie
    drawSun(g, cx + 10, cy - 10, 11);
    drawCloud(g, cx - 4, cy + 10, COLOR_CLOUD);
  } else if (code == 3) {                            // pochmurno
    drawCloud(g, cx, cy + 4, COLOR_CLOUD);
  } else if (code == 45 || code == 48) {             // mgla
    for (int i = 0; i < 4; i++) {
      int y = cy - 16 + i * 11;
      int off = (i % 2) ? 6 : -6;
      g.fillRoundRect(cx - 26 + off, y, 48, 6, 3, COLOR_CLOUD);
    }
  } else if ((code >= 71 && code <= 77) || code == 85 || code == 86) {  // snieg
    drawCloud(g, cx, cy - 6, COLOR_CLOUD);
    for (int i = 0; i < 3; i++) {
      int x = cx - 16 + i * 16, y = cy + 18 + (i % 2) * 6;
      g.drawLine(x - 4, y, x + 4, y, COLOR_SNOW);
      g.drawLine(x, y - 4, x, y + 4, COLOR_SNOW);
      g.drawLine(x - 3, y - 3, x + 3, y + 3, COLOR_SNOW);
      g.drawLine(x - 3, y + 3, x + 3, y - 3, COLOR_SNOW);
    }
  } else if (code >= 95) {                           // burza
    drawCloud(g, cx, cy - 6, COLOR_DARK);
    g.fillTriangle(cx + 2, cy + 6, cx - 10, cy + 22, cx, cy + 22, COLOR_SUN);
    g.fillTriangle(cx - 4, cy + 18, cx + 8, cy + 18, cx - 6, cy + 32, COLOR_SUN);
  } else {                                           // mzawka / deszcz / przelotne
    drawCloud(g, cx, cy - 6, COLOR_CLOUD);
    int drops = (code >= 51 && code <= 57) ? 2 : 3;
    for (int i = 0; i < drops; i++) {
      int x = cx - 12 + i * (drops == 2 ? 20 : 12);
      thickLine(g, x + 3, cy + 14, x - 3, cy + 28, COLOR_RAIN);
    }
  }
}

// Kropla deszczu ~ r*2 x r*3, (cx, cy) = srodek okraglej czesci
void drawDrop(Adafruit_GFX& g, int cx, int cy, int r, uint16_t c) {
  g.fillCircle(cx, cy, r, c);
  g.fillTriangle(cx - r, cy - 1, cx + r, cy - 1, cx, cy - r * 2 - 2, c);
}

// Gora: ikona + maks. temperatura
void drawWeatherTop(Adafruit_GFX& g, const DayWeather& w, int16_t centerY) {
  const int ICON_W = 64, GAP = 10, MINUS_W = 20, DEG_W = 16;

  char buf[8];
  snprintf(buf, sizeof(buf), "%d", abs(w.tempMax));
  g.setFont(&Verdana_Bold38pt7b);
  g.setTextSize(1);
  int16_t x1, y1;
  uint16_t tw, h;
  g.getTextBounds(buf, 0, 0, &x1, &y1, &tw, &h);

  bool neg = w.tempMax < 0;
  int total = ICON_W + GAP + (neg ? MINUS_W : 0) + tw + DEG_W;
  int x = (SCREEN_WIDTH - total) / 2;

  drawWeatherIcon(g, w.code, x + ICON_W / 2, centerY);
  x += ICON_W + GAP;

  if (neg) {
    g.fillRect(x, centerY - 3, MINUS_W - 5, 7, COLOR_HOUR);
    x += MINUS_W;
  }
  int top = centerY - h / 2;
  g.setTextColor(COLOR_HOUR);
  g.setCursor(x - x1, top - y1);
  g.print(buf);
  x += tw;

  // znak stopnia
  g.fillCircle(x + 9, top + 7, 6, COLOR_HOUR);
  g.fillCircle(x + 9, top + 7, 3, COLOR_BG);
}

// Dol: kropla + szansa na opady w %
void drawWeatherBottom(Adafruit_GFX& g, const DayWeather& wd, int16_t centerY) {
  char buf[8];
  snprintf(buf, sizeof(buf), "%d%%", wd.rainChance);
  g.setFont(&FreeSansBold24pt7b);
  g.setTextSize(1);
  int16_t x1, y1;
  uint16_t w, h;
  g.getTextBounds(buf, 0, 0, &x1, &y1, &w, &h);

  const int DROP_W = 22, GAP = 10;
  int total = DROP_W + GAP + w;
  int x = (SCREEN_WIDTH - total) / 2;

  drawDrop(g, x + DROP_W / 2, centerY + 5, 10, COLOR_RAIN);
  x += DROP_W + GAP;

  g.setTextColor(COLOR_MINUTE);
  g.setCursor(x - x1, centerY - h / 2 - y1);
  g.print(buf);
}

// Pozioma linia na srodku: dlugosc = sila wiatru, kolor od szarego do czerwonego
void drawWindBar(const DayWeather& w) {
  int y   = SPLIT_Y - WIND_BAR_H / 2;
  int len = constrain(w.windMax, 0, WIND_FULL_KMH) * WIND_BAR_W / WIND_FULL_KMH;
  for (int i = 0; i < WIND_BAR_W; i++) {
    uint16_t c = COLOR_BG;
    if (i < len) {
      // szary (128,128,128) -> czerwony (255,0,0) wzdluz calej szerokosci
      int r = 128 + 127 * i / (WIND_BAR_W - 1);
      int g = 128 - 128 * i / (WIND_BAR_W - 1);
      c = tft.color565(r, g, g);
    }
    tft.drawFastVLine(WIND_BAR_X + i, y, WIND_BAR_H, c);
  }
}


// Ekran pogody (dzis lub jutro)
void drawWeather(const DayWeather& w) {
  if (canvas) {
    canvas->fillScreen(COLOR_BG);
    drawWeatherTop(*canvas, w, HOUR_CENTER_Y);
    tft.drawRGBBitmap(0, 0, canvas->getBuffer(), SCREEN_WIDTH, SPLIT_Y);

    canvas->fillScreen(COLOR_BG);
    drawWeatherBottom(*canvas, w, MIN_CENTER_Y - SPLIT_Y);
    tft.drawRGBBitmap(0, SPLIT_Y, canvas->getBuffer(), SCREEN_WIDTH, SCREEN_HEIGHT - SPLIT_Y);
  } else {
    tft.fillScreen(COLOR_BG);
    drawWeatherTop(tft, w, HOUR_CENTER_Y);
    drawWeatherBottom(tft, w, MIN_CENTER_Y);
  }
  drawWindBar(w);
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
  fetchWeather();
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

#if WEATHER_DEMO
  static uint32_t lastDemoMs = 0;
  if (millis() - lastDemoMs >= DEMO_INTERVAL_MS) {
    lastDemoMs = millis();
    static const int codes[] = {0, 1, 2, 3, 45, 51, 61, 65, 71, 80, 95};
    DayWeather w;
    w.code       = codes[random(sizeof(codes) / sizeof(codes[0]))];
    w.tempMax    = random(-15, 36);
    w.rainChance = random(0, 11) * 10;
    w.windMax    = random(0, WIND_FULL_KMH + 1);
    Serial.printf("Demo: kod %d, max %d C, deszcz %d%%, wiatr %d km/h\n",
                  w.code, w.tempMax, w.rainChance, w.windMax);
    drawWeather(w);
  }
  delay(50);
  return;
#endif

  // Odswiezanie prognozy
  uint32_t interval = weatherValid ? WEATHER_INTERVAL_MS : RETRY_INTERVAL_MS;
  if (!weatherAttempted || millis() - lastWeatherMs >= interval) {
    fetchWeather();
    now = time(nullptr);
    localtime_r(&now, &timeinfo);
  }

  int h = timeinfo.tm_hour;
  int m = timeinfo.tm_min;

  // Motyw wg pory dnia; przy zmianie wymus przerysowanie
  const Theme* wantTheme = (h >= LIGHT_FROM && h < LIGHT_TO) ? &THEME_LIGHT : &THEME_DARK;
  if (wantTheme != theme) {
    theme = wantTheme;
    currentScreen = SCREEN_NONE;
  }

  // Wybor ekranu: rano pogoda na dzis, wieczorem na jutro, na przemian z zegarem
  Screen want = SCREEN_CLOCK;
  const int cycleSecs = CLOCK_SECS + WEATHER_SECS;
  bool weatherSlot = now % cycleSecs >= CLOCK_SECS;
  if (weatherValid && weatherSlot) {
    if (h >= MORNING_FROM && h < MORNING_TO)      want = SCREEN_TODAY;
    else if (h >= EVENING_FROM && h < EVENING_TO) want = SCREEN_TOMORROW;
#if PREVIEW_ALL_DAY
    else {
      long cycle = now / cycleSecs;
      want = (cycle % 2) ? SCREEN_TOMORROW : SCREEN_TODAY;
    }
#endif
  }

  if (want == SCREEN_CLOCK) {
    if (currentScreen != SCREEN_CLOCK || m != lastMinute) {
      lastMinute = m;
      drawClock(h, m);
      Serial.printf("Czas: %02d:%02d\n", h, m);
    }
  } else if (want != currentScreen || weatherDirty) {
    drawWeather(forecast[want == SCREEN_TOMORROW ? 1 : 0]);
  }
  if (want != SCREEN_CLOCK) weatherDirty = false;
  currentScreen = want;

  delay(100);
}
