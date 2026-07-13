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
  `/data` (JSON), and `/rmt?enabled=0|1` to pause/resume polling.
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
3. Press **RMT ON** to start polling the meter; **RMT OFF** pauses communication.
4. Choose **Parallel** (1 input + 2 outputs) or **Kaskade** (3-stage series measurement)
   depending on how the meter is wired.

### Touch display

The TFT mirrors the same data and lets you toggle both settings without the web UI. On the very
first boot it shows a **touch calibration screen** (`tft.calibrateTouch`) — follow the on-screen
prompts once; the calibration is stored in NVS (via `Preferences`) and reused on every later boot.
To force recalibration, erase flash (`pio run -t erase`) or clear the `gpm8330` NVS namespace.

The two on-screen buttons are single-tap toggles (not separate ON/OFF buttons like the web UI):
- **RMT ON / RMT OFF** — same as the web button.
- **Parallel / Kaskade** — same mode switch as the web buttons.

## Repo layout

```
src/main.cpp         firmware entry point: UART/SCPI polling, web server, computeMetrics()
src/display.h/.cpp    TFT + touch UI (TFT_eSPI)
platformio.ini        build configuration incl. TFT_eSPI pin mapping
Sorces/                original Arduino IDE sketch + GPM-8330 user manual (reference)
```
