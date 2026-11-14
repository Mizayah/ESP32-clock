# NTP Clock with Weather — ESP32-C3 + GC9A01A

A round clock (240×240 GC9A01A display) on an ESP32-C3. Time comes from NTP,
weather for Kraków from [Open-Meteo](https://open-meteo.com/) (no API key needed).
In the morning and evening the screen alternates between the clock and the
weather forecast.

## What the screen shows

### Schedule

| Hours           | Screen                                        | Theme |
|-----------------|-----------------------------------------------|-------|
| 0:00–6:00       | clock only                                    | dark  |
| **6:00–9:00**   | 10 s clock ↔ 4 s weather **for today**        | light |
| 9:00–18:00      | clock only                                    | light |
| 18:00–21:00     | clock only                                    | dark  |
| **21:00–24:00** | 10 s clock ↔ 4 s weather **for tomorrow**     | dark  |

The clock/weather switching is derived from system time (`now % 14 s`), so the
rhythm is steady. If the forecast could not be fetched, the weather windows
show the clock only.

### Clock

- Top: hours (Verdana Bold 55 pt)
- Bottom: minutes (Verdana Bold 38 pt)

### Weather

```
        ┌───────────────┐
        │  [icon]   7°  │   weather icon + daily max temperature
        │━━━━━━━━━━     │   wind bar (length and colour = strength)
        │   💧 40%      │   max precipitation probability
        └───────────────┘
```

- **Icon** (drawn with GFX primitives, based on the WMO weather code):
  | WMO code | Icon |
  |---|---|
  | 0 | sun |
  | 1–2 | sun behind a cloud |
  | 3 | cloud |
  | 45, 48 | fog |
  | 51–57 | drizzle (cloud + 2 drops) |
  | 61–67, 80–82 | rain (cloud + 3 drops) |
  | 71–77, 85–86 | snow |
  | 95–99 | thunderstorm |
- **Temperature**: daily maximum, rounded. The minus sign and degree symbol are
  drawn by hand (the Verdana font contains digits only).
- **Wind bar** (12 px, on the border between the top and bottom halves): grows
  from the left, full length (200 px) at `WIND_FULL_KMH` = 60 km/h. The colour
  shifts along the bar from grey to red — light wind = short grey line, strong
  wind = long line ending in red.
- **Precipitation**: drop + `%` (FreeSansBold 24 pt font).

### Colour themes

| Element                  | Dark          | Light                 |
|--------------------------|---------------|-----------------------|
| background               | black         | white                 |
| hours / temperature      | red           | black                 |
| minutes / precipitation %| orange        | blue `#0050C8`        |
| sun                      | yellow        | orange-yellow         |
| cloud                    | light grey    | grey                  |
| snow                     | white         | blue                  |

Colours are defined in the `THEME_DARK` and `THEME_LIGHT` structs in `src/main.cpp`.

## Hardware

| TFT signal         | ESP32-C3 GPIO |
|--------------------|---------------|
| SCK                | 2             |
| MOSI (SDA)         | 4             |
| CS                 | 5             |
| DC                 | 1             |
| RST                | 0             |
| BL (backlight)     | 3             |

Board: `esp32-c3-devkitm-1`, GC9A01A 240×240 display (SPI, no MISO).

## WiFi setup

WiFi credentials are **not in the repository**. `include/secrets.h` is listed in `.gitignore`.

```bash
cp include/secrets.h.example include/secrets.h
# fill in WIFI_SSID and WIFI_PASSWORD
```

## Build and upload

PlatformIO project (libraries are installed automatically from `platformio.ini`:
Adafruit GC9A01A, Adafruit GFX, Adafruit BusIO, ArduinoJson 7).

```bash
pio run              # build
pio run -t upload    # upload
pio device monitor   # serial log (115200)
```

> Requires PlatformIO Core ≥ 6.2.0 (the espressif32 platform does not work with
> older versions). If an older `pio` is on your `PATH`, use
> `~/.platformio/penv/bin/pio`.

## Settings (`src/main.cpp`)

| Constant | Default | Meaning |
|---|---|---|
| `timeZone` | `CET-1CEST,…` | time zone with automatic DST |
| `RESYNC_HOUR` | 3 | hour of the daily NTP resync |
| `RETRY_INTERVAL_MS` | 5 min | retry interval (NTP and weather) after a failure |
| `WEATHER_URL` | Kraków 50.06, 19.94 | Open-Meteo query (today + tomorrow) |
| `WEATHER_INTERVAL_MS` | 30 min | forecast refresh interval |
| `MORNING_FROM` / `MORNING_TO` | 6 / 9 | today's weather window |
| `EVENING_FROM` / `EVENING_TO` | 21 / 24 | tomorrow's weather window |
| `CLOCK_SECS` / `WEATHER_SECS` | 10 / 4 | clock/weather cycle split |
| `LIGHT_FROM` / `LIGHT_TO` | 6 / 18 | light theme hours |
| `WIND_FULL_KMH` | 60 | wind speed giving a full-length bar |
| `WIND_BAR_H` | 12 | wind bar thickness |

### Test modes

| Flag | Effect |
|---|---|
| `#define WEATHER_DEMO 1` | random weather data every 3 s (icon, temperature, precipitation, wind) |
| `#define PREVIEW_ALL_DAY 1` | clock/weather alternation all day (today and tomorrow in turn) |

## How it works

1. **Startup**: display init, a 240×125 `GFXcanvas16` buffer (flicker-free
   drawing), NTP sync, forecast download.
2. **WiFi only briefly**: the module connects for the NTP sync and the weather
   download, then turns WiFi off.
3. **Weather**: HTTPS request to Open-Meteo (`setInsecure()` — no certificate
   verification), parsed with ArduinoJson. Daily fields fetched:
   `weather_code`, `temperature_2m_max`, `precipitation_probability_max`,
   `wind_speed_10m_max` for today and tomorrow, in the `Europe/Warsaw` time zone.
4. **Loop** (every 100 ms): picks the theme by hour, picks the screen
   (clock / today's weather / tomorrow's weather) and redraws only when the
   screen, the minute or the data changes.

Example serial log:

```
Czas zsynchronizowany: 19:31:14
Pogoda dzis: kod 3, max 7 C, deszcz 0%, wiatr 18 km/h
Pogoda jutro: kod 3, max 8 C, deszcz 6%, wiatr 14 km/h
Czas: 19:31
```

The `spiAttachMISO(): SPI Does not have default pins on ESP32C3!` message is
harmless — the display does not use the MISO line.
