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

Tracks the min/max of `wirkungsgrad` since the last reconnect (`RMT` turned back on), mode switch
(the two modes define `wirkungsgrad` differently, so the range resets on `Parallel`/`Kaskade`
toggle too), or since the Min/Max row was last turned on (activating the display starts a fresh
observation window rather than showing a stale range from whenever tracking happened to last
reset). Always available via `/data` (`minWirkungsgrad`/`maxWirkungsgrad`) and shown on the web
dashboard. On the touch display it's off by default — toggle with the top-left "M/M" chip; when
shown, it reuses the row normally used for Stufe 1/2 info in cascade mode and takes priority over
it while active.

### CSV logging

Manual start/stop from the web dashboard (**Log Start**/**Log Stop**, or `GET /log?enabled=0|1`,
also toggleable from the touch Setup screen) — logging is independent of `RMT`/mode toggles, so
briefly turning RMT off to use the meter's front panel doesn't lose an in-progress recording.
Two logging backends run simultaneously while enabled:

- **SD card** (if a card is present — the primary path when one is inserted): each **Log Start**
  opens a new file `/gpm8330_<epoch_ms>.csv` and appends + flushes a row per sample for as long as
  logging stays on, unbounded by RAM — meant for long unattended test runs. Browse/download
  recorded files via the **SD-Dateien anzeigen** list on the dashboard (auto-loaded on page load,
  newest first; `GET /sdfiles` for the raw JSON listing, `GET /sdfile?name=...` to download one).
- **RAM ring buffer** (fallback for when no SD card is present): samples once per second into a
  1800-entry buffer (~30 min of history; oldest entries roll off after that, nothing is written to
  flash — allocated on the heap at boot via `new(std::nothrow)`, so if it can't fit it logs a
  warning and disables the RAM log instead of crashing; see `datalogInit()` in `src/datalog.cpp`).
  The **CSV herunterladen**/**Log leeren** (`GET /csv` / `GET /csv/clear`) buttons only appear on
  the dashboard when `sdAvailable` is false — with an SD card inserted they're hidden, since the SD
  file list already covers that need with no 30-minute cap. `Log leeren` only ever clears this RAM
  view, never an SD file (deleting a permanent recording by accident would be worse than a full
  card).

Both backends write identical rows (columns `t_epoch_ms,power1_w,power2_w,power3_w,
wirkungsgrad_pct,mode`); the sample struct and CSV formatting live together in `src/datalog.cpp` so
adding more logged values later (e.g. per-channel U/I/S/Q) is a localized change to both at once.

**Timestamps:** the ESP32 has no internet access unless it's joined a WLAN (see "WiFi" below) and
no battery-backed RTC, so it can't know the wall-clock time on its own by default. Instead, the web
dashboard sends the browser's `Date.now()` to `GET /settime?t=<epoch_ms>` once on page load; the
firmware stores the offset to its own `millis()` and uses that to estimate Unix time for each log
row (`currentEpochMs()` in `src/main.cpp`). Load the dashboard at least once per boot before/while
logging to get real timestamps — otherwise rows fall back to boot-relative milliseconds. No
periodic re-sync; expect a few seconds of drift over a very long unattended session.

### WiFi (AP always on + optional STA)

The ESP32 always runs its own `ESP32-GPM8330` access point (`WIFI_AP_STA` mode) — that never turns
off. It can additionally join an existing WiFi network (e.g. a lab/office network) at the same
time, mainly so OTA updates and the dashboard are reachable without being on the device's own AP.
From the web dashboard's **WLAN** section: **Netzwerke suchen** scans and lists nearby SSIDs
(`GET /wifiscan`, a few seconds — this blocks the main loop briefly, so RS-232 polling/display
pause for the duration of a scan); enter a password (checkbox to reveal it while typing), tap a
network in the list, and it connects (`GET /wificonnect?ssid=...&password=...`). Credentials are
stored in NVS (`Preferences`, namespace `wifi`) and reconnected automatically on every boot.
Connection status (SSID + IP) shows on the web dashboard and on the touch Setup screen.

`WiFi.scanNetworks()` on the ESP32 is known to be unreliable while a SoftAP + STA connection are
both active at once, particularly with WiFi modem sleep enabled — `/wifiscan` retries once on a
failed scan and `WiFi.setSleep(false)` is set at boot as a mitigation, but this hasn't been
stress-tested across many scan/connect cycles. If scanning stops working, check the Serial Monitor
(`pio device monitor`), which logs the raw scan result count for every `/wifiscan` call.

### SD card logging hardware

Wired in parallel to the display/touch SPI bus (SCK=18, MOSI=19, MISO=23), with its own chip
select on **GPIO25**. `datalogInit()` mounts it at boot; if no card is present, SD logging is
silently skipped and the RAM ring buffer still works normally. See "CSV logging" above for how the
two logging backends interact.

### OTA updates

`ArduinoOTA` is enabled (hostname `gpm8330-monitor`, password = the AP password). To flash over
WiFi instead of USB, connect to the `ESP32-GPM8330` AP (or be on the same network as the device if
it has also joined one via STA, see "WiFi" above) and run:

```
pio run -t upload -e esp32dev_ota
```

USB flashing remains the default (`pio run -t upload -e esp32dev`); OTA only works once the
firmware with `ArduinoOTA` support is already on the device via a first USB flash. The
`esp32dev_ota` environment's `upload_port` is hardcoded to the AP's fixed `192.168.4.1` since
that's always predictable; to flash over the STA network instead, override it on the command line,
e.g. `pio run -t upload -e esp32dev_ota --upload-port <device's STA IP>` (see the touch Setup
screen or web dashboard for that IP).

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
  | SD_CS  | 25 (SD slot on the display module, shares SCK/MOSI/MISO with the display/touch) |

  All pin/driver config lives in `platformio.ini`'s `build_flags` (no edits to the TFT_eSPI
  library itself); adjust it there if you rewire anything. Display orientation is set by
  `DISPLAY_ROTATION` in `src/display.cpp` (currently `3` — landscape, rotated 180° from the
  panel's natural orientation to match this enclosure). Touch calibration is invalidated
  automatically if you change this constant (see the comment in `calibrateTouch()`).

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
- **SET** (top-right) — opens a **Setup screen** showing the AP SSID/password/IP, whether a WiFi
  network is additionally joined (and its SSID/IP), whether an SD card is detected, and a button
  to toggle CSV logging from the bench without needing the web UI. Tap **SET** again to return to
  the dashboard.

WiFi network scanning/joining is web-only by design (see "WiFi" above) — no touch UI for that,
typing a password on a resistive touchscreen keyboard would be painful.

The row normally used for the red "RS232 Fehler" banner doubles as a logging-in-progress reminder:
if CSV logging is on and there's no RS-232 error, it turns orange and shows "● LOG LAEUFT" so an
active recording isn't easy to forget about on the bench; an RS-232 error takes priority (red) but
still appends the log reminder to the same line if both apply at once.

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

- **NTP-Zeitsync**: sobald WLAN-STA verbunden ist, könnte die Zeit per NTP synchronisiert werden
  statt (oder zusätzlich zu) der Browser-`Date.now()`-Synchronisierung — würde auch ohne
  Dashboard-Aufruf korrekte Zeitstempel liefern, u.a. für die Dauer von SD-Aufzeichnungen ohne
  angeschlossenen Browser.
- **Web-Settings-Seite**: zentrale Konfigurationsseite (Polling-/Log-Intervall, Ringpuffer-Größe,
  AP-SSID/Passwort etc.), statt Konstanten im Code zu ändern.
- **Weitere Messgrößen im CSV-Log**: U/I/S/Q etc. je Kanal zusätzlich zu P/Wirkungsgrad
  aufzeichnen (Datenstruktur in `src/datalog.cpp` ist dafür bereits so angelegt).
- **Mehrere gespeicherte WLANs**: aktuell wird nur ein WLAN (das zuletzt über `/wificonnect`
  verbundene) in NVS gespeichert; eine Liste mehrerer Netzwerke mit Priorität wäre komfortabler
  beim Wechsel zwischen mehreren Standorten.
- **Fehler-Piepser**: zurückgestellt, da aktuell keine Buzzer-Hardware vorhanden; Code wäre klein
  (ein `tone()`-Aufruf bei Fehler-Flanke), sobald ein Summer verfügbar ist.
