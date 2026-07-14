#pragma once
#include <Arduino.h>

struct DisplayState {
  float power1;
  float power2;
  float power3;
  float percent1;
  float percent2;
  float percent3;
  float wirkungsgrad;
  float stufe1Wirkungsgrad;
  float stufe2Wirkungsgrad;
  float minWirkungsgrad;
  float maxWirkungsgrad;
  int inputChannel;
  int stageChannel;
  int outputChannel;
  bool cascadeMode;
  bool rmtEnabled;
  bool rs232Error;
  bool skipPreset;
  bool showMinMax;
  bool datalogEnabled;
  bool staConnected; // zusaetzlich zum immer aktiven AP mit einem bestehenden WLAN verbunden
  String staSsid;
  String staIp;
  bool sdAvailable;
};

enum DisplayAction {
  DISPLAY_ACTION_NONE,
  DISPLAY_ACTION_TOGGLE_RMT,
  DISPLAY_ACTION_TOGGLE_MODE,
  DISPLAY_ACTION_TOGGLE_PRESET,
  DISPLAY_ACTION_TOGGLE_MINMAX,
  DISPLAY_ACTION_TOGGLE_LOG
};

// ssid/password/ip sind fuer die Boot-Session konstant (nur AP-Modus) und werden
// einmalig fuer den Setup-Screen gespeichert, statt sie bei jedem Render erneut
// durch DisplayState zu kopieren.
void displayInit(const String& ssid, const String& password, const String& ip);
void displayRender(const DisplayState& state);
DisplayAction displayPollTouch();

// Diagnose-Hilfsfunktion fuer die Touch-Inbetriebnahme: gibt T_IRQ-Pegel und
// rohe SPI-Touch-Werte fortlaufend auf Serial aus. Blockiert dauerhaft (kein
// normaler Betrieb) - nur temporaer in setup() statt displayInit() aufrufen.
void displayTouchDiagnostics();
