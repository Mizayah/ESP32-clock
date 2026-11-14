# Zegar NTP z pogodą — ESP32-C3 + GC9A01A

Okrągły zegar (wyświetlacz 240×240 GC9A01A) na ESP32-C3. Czas pobiera z NTP,
a pogodę dla Krakowa z [Open-Meteo](https://open-meteo.com/) (bez klucza API).
Rano i wieczorem pokazuje na zmianę zegar i prognozę pogody.

## Co pokazuje ekran

### Harmonogram

| Godziny       | Ekran                                         | Motyw  |
|---------------|-----------------------------------------------|--------|
| 0:00–6:00     | sam zegar                                     | ciemny |
| **6:00–9:00** | 10 s zegar ↔ 4 s pogoda **na dziś**           | jasny  |
| 9:00–18:00    | sam zegar                                     | jasny  |
| 18:00–21:00   | sam zegar                                     | ciemny |
| **21:00–24:00** | 10 s zegar ↔ 4 s pogoda **na jutro**        | ciemny |

Przełączanie zegar/pogoda liczone jest od czasu systemowego (`now % 14 s`),
więc rytm jest stały. Jeśli prognozy nie udało się pobrać, w oknach
pogodowych widać sam zegar.

### Zegar

- Góra: godziny (Verdana Bold 55 pt)
- Dół: minuty (Verdana Bold 38 pt)

### Pogoda

```
        ┌───────────────┐
        │  [ikona]  7°  │   ikona pogody + maks. temperatura dnia
        │━━━━━━━━━━     │   pasek wiatru (długość i kolor = siła)
        │   💧 40%      │   maks. prawdopodobieństwo opadów
        └───────────────┘
```

- **Ikona** (rysowana prymitywami GFX, wg kodu WMO):
  | Kod WMO | Ikona |
  |---|---|
  | 0 | słońce |
  | 1–2 | słońce zza chmury |
  | 3 | chmura |
  | 45, 48 | mgła |
  | 51–57 | mżawka (chmura + 2 krople) |
  | 61–67, 80–82 | deszcz (chmura + 3 krople) |
  | 71–77, 85–86 | śnieg |
  | 95–99 | burza |
- **Temperatura**: maksymalna w ciągu dnia, zaokrąglona; minus i znak stopnia
  rysowane ręcznie (font Verdana zawiera tylko cyfry).
- **Pasek wiatru** (12 px, na granicy górnej i dolnej części): rośnie od lewej,
  pełna długość (200 px) przy `WIND_FULL_KMH` = 60 km/h. Kolor zmienia się
  wzdłuż paska od szarego do czerwonego — słaby wiatr = krótka szara kreska,
  silny = długa, kończąca się na czerwono.
- **Opady**: kropla + `%` (font FreeSansBold 24 pt).

### Motywy kolorów

| Element                 | Ciemny        | Jasny                 |
|-------------------------|---------------|-----------------------|
| tło                     | czarne        | białe                 |
| godziny / temperatura   | czerwone      | czarne                |
| minuty / % opadów       | pomarańczowe  | niebieskie `#0050C8`  |
| słońce                  | żółte         | pomarańczowo-żółte    |
| chmura                  | jasnoszara    | szara                 |
| śnieg                   | biały         | niebieski             |

Kolory są w strukturach `THEME_DARK` i `THEME_LIGHT` w `src/main.cpp`.

## Sprzęt

| Sygnał TFT | GPIO ESP32-C3 |
|------------|---------------|
| SCK        | 2             |
| MOSI (SDA) | 4             |
| CS         | 5             |
| DC         | 1             |
| RST        | 0             |
| BL (podświetlenie) | 3     |

Płytka: `esp32-c3-devkitm-1`, wyświetlacz GC9A01A 240×240 (SPI, bez MISO).

## Konfiguracja WiFi

Dane WiFi **nie są w repozytorium**. Plik `include/secrets.h` jest w `.gitignore`.

```bash
cp include/secrets.h.example include/secrets.h
# uzupełnij WIFI_SSID i WIFI_PASSWORD
```

## Budowanie i wgrywanie

Projekt PlatformIO (biblioteki pobierają się same z `platformio.ini`:
Adafruit GC9A01A, Adafruit GFX, Adafruit BusIO, ArduinoJson 7).

```bash
pio run              # kompilacja
pio run -t upload    # wgranie
pio device monitor   # log na porcie szeregowym (115200)
```

> Wymagany PlatformIO Core ≥ 6.2.0 (platforma espressif32 nie działa ze
> starszym). Jeśli w `PATH` jest starszy `pio`, użyj
> `~/.platformio/penv/bin/pio`.

## Ustawienia (`src/main.cpp`)

| Stała | Domyślnie | Znaczenie |
|---|---|---|
| `timeZone` | `CET-1CEST,…` | strefa czasowa z automatyczną zmianą czasu |
| `RESYNC_HOUR` | 3 | godzina codziennej ponownej synchronizacji NTP |
| `RETRY_INTERVAL_MS` | 5 min | odstęp ponownych prób (NTP i pogoda) po błędzie |
| `WEATHER_URL` | Kraków 50.06, 19.94 | zapytanie do Open-Meteo (dziś + jutro) |
| `WEATHER_INTERVAL_MS` | 30 min | odświeżanie prognozy |
| `MORNING_FROM` / `MORNING_TO` | 6 / 9 | okno pogody na dziś |
| `EVENING_FROM` / `EVENING_TO` | 21 / 24 | okno pogody na jutro |
| `CLOCK_SECS` / `WEATHER_SECS` | 10 / 4 | proporcje cyklu zegar/pogoda |
| `LIGHT_FROM` / `LIGHT_TO` | 6 / 18 | godziny jasnego motywu |
| `WIND_FULL_KMH` | 60 | wiatr dający pełną długość paska |
| `WIND_BAR_H` | 12 | grubość paska wiatru |

### Tryby testowe

| Flaga | Działanie |
|---|---|
| `#define WEATHER_DEMO 1` | co 3 s losowe dane pogodowe (ikona, temp., opady, wiatr) |
| `#define PREVIEW_ALL_DAY 1` | przemiennie zegar/pogoda przez całą dobę (na zmianę dziś i jutro) |

## Jak to działa

1. **Start**: inicjalizacja wyświetlacza, bufor `GFXcanvas16` 240×125
   (rysowanie bez migania), synchronizacja NTP, pobranie prognozy.
2. **WiFi tylko na chwilę**: przy synchronizacji NTP i pobieraniu pogody
   moduł łączy się z siecią, a potem wyłącza WiFi.
3. **Pogoda**: HTTPS do Open-Meteo (`setInsecure()` — bez weryfikacji
   certyfikatu), parsowanie ArduinoJson. Pobierane pola dzienne:
   `weather_code`, `temperature_2m_max`, `precipitation_probability_max`,
   `wind_speed_10m_max` dla dziś i jutro, w strefie `Europe/Warsaw`.
4. **Pętla** (co 100 ms): wybór motywu wg godziny, wybór ekranu
   (zegar / pogoda dziś / pogoda jutro) i przerysowanie tylko przy zmianie
   ekranu, minuty albo nowych danych.

Log na porcie szeregowym, np.:

```
Czas zsynchronizowany: 19:31:14
Pogoda dzis: kod 3, max 7 C, deszcz 0%, wiatr 18 km/h
Pogoda jutro: kod 3, max 8 C, deszcz 6%, wiatr 14 km/h
Czas: 19:31
```

Komunikat `spiAttachMISO(): SPI Does not have default pins on ESP32C3!`
jest nieszkodliwy — wyświetlacz nie używa linii MISO.
