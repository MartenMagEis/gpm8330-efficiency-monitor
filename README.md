# GWInstek GPM-8330 Efficiency Monitor

ESP32 firmware that bridges a GW Instek **GPM-8330/8320** power meter (RS-232/SCPI) to a small
WiFi web dashboard. It polls the active power of all three meter channels and computes an
efficiency figure between them.

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

### Channel wiring / efficiency calculation

Channel 1/2/3 map directly to `Power_E1/E2/E3` — there is no fixed "channel 2 = input" rule in
the firmware. Instead, `generiereJSON()` treats **whichever channel currently reads the highest
power as the input (100% reference)** and sums the other two channels' percentages:

```
wirkungsgrad (%) = percent1 + percent2 + percent3 − 100
```

This is correct for the common bench setup — **one AC input feeding a supply with two DC
outputs**, wired in the meter's 3V3A mode (three independent single-phase measurements, see
manual appendix "Wiring diagram") — regardless of which physical channel the input happens to be
plugged into, as long as the input channel really is the highest-power one.

**It is not correct for a cascaded/3-stage measurement** (e.g. 230 V AC → DC link → output),
because summing three series stages' percentages this way double-counts the power path. If you
need that topology, the efficiency formula needs to change to a per-stage ratio
(`stage2/stage1`, `stage3/stage2`) instead of the current "1 input + 2 outputs" sum.

## Hardware

- ESP32 dev board (originally flashed under Arduino IDE as a custom "ALKS ESP32" board entry).
- GPIO16 (RX) / GPIO17 (TX) wired to the GPM-8330's RS-232 port (level-shifted/adapter as needed
  — the ESP32 UART is 3.3 V TTL, not RS-232 voltage levels, so a MAX3232-type adapter is required
  between the ESP32 and the meter's DB-9 port).
- Meter RS-232 settings must match the firmware: 115200 baud, 8N1, no flow control
  (`SYSTEM CONFIG` → I/O Model → RS232 → Baud Rate).

## Build & flash (PlatformIO)

```
pio run                 # build
pio run -t upload       # flash
pio device monitor      # serial log (115200 baud)
```

Adjust the `board` in `platformio.ini` if your ESP32 module differs from a generic
`esp32dev` (DOIT ESP32 DEVKIT V1 pinout).

## Usage

1. Power the ESP32; connect to the `ESP32-GPM8330` WiFi AP (password `12345678`).
2. Open `http://192.168.4.1/` in a browser.
3. Press **RMT ON** to start polling the meter; **RMT OFF** pauses communication.

## Repo layout

```
src/main.cpp        firmware (PlatformIO)
platformio.ini       build configuration
Sorces/              original Arduino IDE sketch + GPM-8330 user manual (reference)
```
