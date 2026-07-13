# GWInstek GPM-8330 Efficiency Monitor

ESP32 firmware that bridges a GW Instek **GPM-8330/8320** power meter (RS-232/SCPI) to a WiFi web
dashboard and a local touch TFT. It polls the active power of all three meter channels and
computes an efficiency figure between them.

## How it works

- `rs232` (UART2, RXD2=GPIO16, TXD2=GPIO17) talks SCPI to the meter at 115200 baud, 8N1.
- On connect it sends `:NUMERIC:NORMAL:VALUE?3` as a liveness check, then
  `:NUMERIC:NORMAL:PRESET 4`, which is the meter's built-in item layout where each channel
  occupies a fixed block of 20 items (`U,I,P,S,Q,LAMBDA,PHI,FU,FI,UPPeak,UMPeak,IPPeak,IMPeak,
  TIME,WH,WHP,WHM,AH,AHP,AHM`). That makes item **3 / 23 / 43** the active power (`P`) of
  channel 1 / 2 / 3 respectively — see `GPM-8320/8330 User Manual`, "Command Overview" and
  "Preset patterns" (Preset 4). The firmware then round-robins `VALUE?3`, `VALUE?23`, `VALUE?43`
  every 200 ms.
- A small web server (`WiFi.softAP`, SSID `ESP32-GPM8330`) serves `/` (dashboard),
  `/data` (JSON), `/rmt?enabled=0|1` to pause/resume polling, `/mode?cascade=0|1` to switch
  measurement mode, `/log?enabled=0|1` to start/stop CSV logging, `/csv` to download the log and
  `/csv/clear` to clear it.
- `computeMetrics()` in `src/main.cpp` is the single place that turns the three raw channel
  powers into percentages, channel roles and efficiency figures — both `/data` (JSON) and the
  on-device touch display (`src/display.cpp`) render from the same `DisplayState` struct, so the
  two UIs can't drift out of sync.

### Channel wiring / efficiency calculation

Channel 1/2/3 map directly to `Power_E1/E2/E3` — there is no fixed "channel 2 = input" rule in
the firmware. Instead, `generiereJSON()` always sorts the three channel powers and treats
**whichever channel currently reads the highest power as the input**, regardless of which
physical channel it's wired to. Two measurement modes use that ranking differently, switchable
at runtime via the **Parallel / Kaskade** buttons on the dashboard (or `GET /mode?cascade=0|1`):

- **Parallel mode** (default) — one AC input feeding a supply with two DC outputs, wired in the
  meter's 3V3A mode (three independent single-phase measurements, see manual appendix "Wiring
  diagram"):

  ```
  wirkungsgrad (%) = percent1 + percent2 + percent3 − 100
  ```

- **Kaskade (cascade) mode** — a 3-stage series measurement (e.g. 230 V AC → DC link → output).
  The highest-power channel is the **input**, the middle one the **Zwischenkreis/Stufe 1**, the
  lowest the **output**, and the response includes both per-stage and overall efficiency:

  ```
  stufe1Wirkungsgrad (%) = P(Zwischenkreis) / P(Input)  × 100   // stage 1
  stufe2Wirkungsgrad (%) = P(Output)        / P(Zwischenkreis) × 100   // stage 2
  wirkungsgrad (%)        = P(Output)        / P(Input)  × 100   // overall, input → output
  ```

`/data` also reports `mode`, `inputChannel`, `stageChannel` and `outputChannel` (1-based) so the
UI can label which physical channel currently plays which role — that labeling adapts live if you
swap which channel carries the highest power.

### Front panel lock on the meter (RS-232 remote lockout)

The GPM-8330 locks its own front-panel keys as soon as it receives remote commands (RMT icon lit)
and — per the manual's "Return to Local Control" section — the *only* documented way back is
pressing the physical **Local** key on the meter itself; there is no SCPI command over RS-232 to
release it remotely (unlike GPIB's REN/GTL bus messages, which this meter doesn't expose over
RS-232). Workflow: **RMT OFF** (web or touch) stops the firmware from re-triggering the lock, then
press **Local** on the meter once to actually restore front-panel control.

**PRESET-skip experiment (touch-only, top-right "PRE" chip):** untested hypothesis that the lock
is triggered by the `:NUMERIC:NORMAL:PRESET 4` *set* command specifically, not by the plain
`VALUE?` *queries* used for polling. When enabled, the init handshake skips sending `PRESET 4` —
requires the meter's item pattern to already be set to Preset 4 manually via its own front-panel
menu beforehand, since the firmware then relies purely on queries. Default off (unchanged
behavior). Not yet verified against real hardware.

### Min/Max tracking

Tracks the min/max of `wirkungsgrad` since the last reconnect (`RMT` turned back on) or mode
switch (the two modes define `wirkungsgrad` differently, so the range resets on `Parallel`/
`Kaskade` toggle too). Always available via `/data` (`minWirkungsgrad`/`maxWirkungsgrad`) and shown
on the web dashboard. On the touch display it's off by default — toggle with the top-left "M/M"
chip; when shown, it reuses the row normally used for Stufe 1/2 info in cascade mode, so it's only
visible in **Parallel** mode (cascade mode's stage readout takes priority in that row).

### CSV logging

Manual start/stop from the web dashboard (**Log Start**/**Log Stop**, or `GET /log?enabled=0|1`) —
logging is independent of `RMT`/mode toggles, so briefly turning RMT off to use the meter's front
panel doesn't lose an in-progress recording. Samples once per second into a 1800-entry RAM ring
buffer (~30 min of history; oldest entries roll off after that, nothing is written to flash — the
buffer is allocated on the heap at boot via `new(std::nothrow)`, so if it can't fit it logs a
warning and disables logging instead of crashing; see `datalogInit()` in `src/datalog.cpp`).
Download via **CSV herunterladen** (`GET /csv`, columns
`t_epoch_ms,power1_w,power2_w,power3_w,wirkungsgrad_pct,mode`) and clear via **Log leeren**
(`GET /csv/clear`). The sample struct and CSV formatting live together in `src/datalog.cpp` so
adding more logged values later (e.g. per-channel U/I/S/Q) is a localized change.

**Timestamps:** the ESP32 has no internet access in AP-only mode (so no NTP) and no
battery-backed RTC, so it can't know the wall-clock time on its own. Instead, the web dashboard
sends the browser's `Date.now()` to `GET /settime?t=<epoch_ms>` once on page load; the firmware
stores the offset to its own `millis()` and uses that to estimate Unix time for each log row
(`currentEpochMs()` in `src/main.cpp`). Load the dashboard at least once per boot before/while
logging to get real timestamps — otherwise rows fall back to boot-relative milliseconds. No
periodic re-sync; expect a few seconds of drift over a very long unattended session.

### OTA updates

`ArduinoOTA` is enabled (hostname `gpm8330-monitor`, password = the AP password). To flash over
WiFi instead of USB, connect to the `ESP32-GPM8330` AP and run:

```
pio run -t upload -e esp32dev_ota
```

USB flashing remains the default (`pio run -t upload -e esp32dev`); OTA only works once the
firmware with `ArduinoOTA` support is already on the device via a first USB flash.

## Hardware

- ESP32 dev board (originally flashed under Arduino IDE as a custom "ALKS ESP32" board entry).
- GPIO16 (RX) / GPIO17 (TX) wired to the GPM-8330's RS-232 port (level-shifted/adapter as needed
  — the ESP32 UART is 3.3 V TTL, not RS-232 voltage levels, so a MAX3232-type adapter is required
  between the ESP32 and the meter's DB-9 port).
- Meter RS-232 settings must match the firmware: 115200 baud, 8N1, no flow control
  (`SYSTEM CONFIG` → I/O Model → RS232 → Baud Rate).
- AZ-Delivery 2.4" 240x320 SPI TFT (ILI9341) with XPT2046 resistive touch:

  | Display pin | ESP32 GPIO |
  |---|---|
  | CS     | 5  |
  | RESET  | 33 |
  | D/C    | 27 |
  | SDI (MOSI) | 19 |
  | SCK    | 18 |
  | SDO (MISO) | 23 |
  | T_CS   | 14 |
  | T_IRQ  | 32 (wired but unused — TFT_eSPI polls touch over SPI, not via interrupt) |

  All pin/driver config lives in `platformio.ini`'s `build_flags` (no edits to the TFT_eSPI
  library itself); adjust it there if you rewire anything.

## Build & flash (PlatformIO)

```
pio run                 # build
pio run -t upload       # flash
pio device monitor      # serial log (115200 baud)
```

Adjust the `board` in `platformio.ini` if your ESP32 module differs from a generic
`esp32dev` (DOIT ESP32 DEVKIT V1 pinout).

## Usage

### Web dashboard

1. Power the ESP32; connect to the `ESP32-GPM8330` WiFi AP (password `12345678`).
2. Open `http://192.168.4.1/` in a browser.
3. **RMT** toggles polling the meter; **Parallel/Kaskade** toggles measurement mode; **Log
   Start/Stop** toggles CSV logging. All three are single buttons that show the current state and
   toggle on tap (label/color always reflect the live `/data` response, polled every 500 ms, so
   the button state can't drift from the firmware's actual state — e.g. after a reload or if the
   touch display changed something in the meantime).

### Touch display

The TFT mirrors the same data and lets you toggle settings without the web UI. On the very first
boot it shows a **touch calibration screen** (`tft.calibrateTouch`) — follow the on-screen prompts
once; the calibration is stored in NVS (via `Preferences`) and reused on every later boot. To force
recalibration, erase flash (`pio run -t erase`) or clear the `gpm8330` NVS namespace.

The two main buttons are single-tap toggles (not separate ON/OFF buttons like the old web UI used
to have):
- **RMT ON / RMT OFF** — same as the web button.
- **Parallel / Kaskade** — same mode switch as the web buttons.

Three small chips in the title bar toggle secondary, less-frequently-used settings/screens:
- **M/M** (top-left) — show/hide the Min/Max row (see "Min/Max tracking" above).
- **PRE** (middle) — PRESET-skip experiment toggle (see "Front panel lock" above); intentionally
  temporary, expect this chip to be removed once the RS-232 hypothesis has been tested.
- **SET** (top-right) — opens a **Setup screen** showing the AP SSID/password, the (currently
  static — AP-only, no STA mode yet) connection mode, and the device IP, plus a button to toggle
  CSV logging from the bench without needing the web UI. Tap **SET** again to return to the
  dashboard.

## Repo layout

```
src/main.cpp          firmware entry point: UART/SCPI polling, web server, OTA, computeMetrics()
src/display.h/.cpp     TFT + touch UI (TFT_eSPI)
src/datalog.h/.cpp     CSV ring-buffer logging
platformio.ini         build configuration incl. TFT_eSPI pin mapping and the OTA upload env
Sorces/                 original Arduino IDE sketch + GPM-8330 user manual (reference)
```

## Roadmap / Ideen für später

Nicht umgesetzt, aber im Hinterkopf für spätere Runden:

- **Dynamische Verbindungsart auf dem Setup-Screen**: SSID/Passwort/IP werden dort bereits
  angezeigt (siehe "Touch display" oben), aber der Modus ist noch statisch "Access Point" fest im
  Code, weil es bislang keinen WLAN-STA-Modus gibt. Sobald das Gerät optional auch einem
  bestehenden WLAN beitreten kann, sollte diese Zeile den echten Modus (`WiFi.getMode()`) anzeigen.
- **WLAN-STA-Modus**: dem Labor-/Heim-WLAN zusätzlich zum eigenen AP beitreten (ESP32 kann
  AP+STA gleichzeitig), damit das Gerät auch ohne direkte Verbindung zum ESP32-eigenen AP
  erreichbar ist (z.B. für NTP-Zeitsync statt der Browser-Zeit-Synchronisierung).
- **Web-Settings-Seite**: zentrale Konfigurationsseite (WLAN-Zugangsdaten, Polling-/Log-Intervall,
  Ringpuffer-Größe etc.), statt Konstanten im Code zu ändern.
- **Weitere Messgrößen im CSV-Log**: U/I/S/Q etc. je Kanal zusätzlich zu P/Wirkungsgrad
  aufzeichnen (Datenstruktur in `src/datalog.cpp` ist dafür bereits so angelegt).
- **SD-Karten-Logging**: falls das Display-Modul einen SD-Slot hat, langfristige Aufzeichnung ohne
  RAM-Limit (Ringpuffer ist aktuell auf ~30 min begrenzt).
- **Fehler-Piepser**: zurückgestellt, da aktuell keine Buzzer-Hardware vorhanden; Code wäre klein
  (ein `tone()`-Aufruf bei Fehler-Flanke), sobald ein Summer verfügbar ist.
