#include "display.h"
#include <TFT_eSPI.h>
#include <Preferences.h>

static TFT_eSPI tft = TFT_eSPI();
static Preferences prefs;

static const int SCREEN_W = 320;

static const int COL_X[3] = { 8, 116, 224 };
static const int COL_W = 100;

static const int BTN_RMT_X = 10, BTN_RMT_Y = 195, BTN_RMT_W = 140, BTN_RMT_H = 40;
static const int BTN_MODE_X = 170, BTN_MODE_Y = 195, BTN_MODE_W = 140, BTN_MODE_H = 40;

static const int CHIP_MINMAX_X = 4, CHIP_Y = 2, CHIP_W = 46, CHIP_H = 18;
static const int CHIP_PRESET_X = 205; // uebergangsweise - rueckt zusammen mit CHIP_SETUP zusammen
static const int CHIP_SETUP_X = 270;

static const int STUFEN_Y = 130, STUFEN_H = 40;
static const int ERROR_Y = 172, ERROR_H = 18;

static const int SETUP_LOG_BTN_X = 60, SETUP_LOG_BTN_Y = 150, SETUP_LOG_BTN_W = 200, SETUP_LOG_BTN_H = 40;

static const int TOUCH_IRQ_PIN = 32;

static bool touchActive = false;
static unsigned long lastTouchAction = 0;
static bool showSetupScreen = false;
static bool lastScreenWasSetup = false;

static String apSsid;
static String apPassword;
static String apIp;

static bool pointInRect(int x, int y, int rx, int ry, int rw, int rh) {
  return x >= rx && x <= rx + rw && y >= ry && y <= ry + rh;
}

static void calibrateTouch() {
  uint16_t calData[5];
  if (prefs.isKey("tcal")) {
    prefs.getBytes("tcal", calData, sizeof(calData));
    tft.setTouch(calData);
    return;
  }
  tft.fillScreen(TFT_BLACK);
  tft.setTextColor(TFT_WHITE, TFT_BLACK);
  tft.setTextFont(2);
  tft.setTextDatum(TL_DATUM);
  tft.drawString("Touch-Kalibrierung: Ecken beruehren", 10, 10);
  tft.calibrateTouch(calData, TFT_WHITE, TFT_RED, 15);
  prefs.putBytes("tcal", calData, sizeof(calData));
  tft.setTouch(calData);
}

static void drawButton(int x, int y, int w, int h, const String& label, bool active) {
  uint32_t bg = active ? TFT_DARKGREEN : TFT_DARKGREY;
  tft.fillRoundRect(x, y, w, h, 6, bg);
  tft.drawRoundRect(x, y, w, h, 6, TFT_WHITE);
  tft.setTextColor(TFT_WHITE, bg);
  tft.setTextFont(2);
  tft.setTextDatum(MC_DATUM);
  tft.drawString(label, x + w / 2, y + h / 2);
}

// Kleine Kopfzeilen-Chips fuer Nebenfunktionen (Min/Max-Anzeige, PRESET-Skip-Experiment).
static void drawChip(int x, int y, int w, int h, const String& label, bool active) {
  uint32_t bg = active ? TFT_DARKGREEN : TFT_DARKGREY;
  tft.fillRoundRect(x, y, w, h, 3, bg);
  tft.setTextColor(TFT_WHITE, bg);
  tft.setTextFont(1);
  tft.setTextDatum(MC_DATUM);
  tft.drawString(label, x + w / 2, y + h / 2 + 1);
}

static String roleLabel(int channel, const DisplayState& s) {
  if (s.inputChannel == channel) return "Kanal " + String(channel) + " (In)";
  if (s.cascadeMode && s.stageChannel == channel) return "Kanal " + String(channel) + " (Zw)";
  if (s.cascadeMode && s.outputChannel == channel) return "Kanal " + String(channel) + " (Out)";
  return "Kanal " + String(channel);
}

void displayInit(const String& ssid, const String& password, const String& ip) {
  apSsid = ssid;
  apPassword = password;
  apIp = ip;

  tft.init();
  tft.setRotation(1);
  prefs.begin("gpm8330", false);
  calibrateTouch();

  tft.fillScreen(TFT_BLACK);
  tft.fillRect(0, 0, SCREEN_W, 22, TFT_NAVY);
  tft.setTextColor(TFT_WHITE, TFT_NAVY);
  tft.setTextFont(2);
  tft.setTextDatum(MC_DATUM);
  tft.drawString("GPM-8330", SCREEN_W / 2, 11);
  tft.setTextDatum(TL_DATUM);
}

static void drawSetupScreen(const DisplayState& s) {
  tft.fillRect(0, 22, SCREEN_W, 218, TFT_BLACK);
  tft.setTextDatum(TL_DATUM);
  tft.setTextPadding(0);

  tft.setTextFont(2);
  tft.setTextColor(TFT_CYAN, TFT_BLACK);
  tft.drawString("Setup", 10, 32);

  tft.setTextFont(1);
  tft.setTextColor(TFT_WHITE, TFT_BLACK);
  tft.drawString("SSID: " + apSsid, 10, 62);
  tft.drawString("Passwort: " + apPassword, 10, 80);
  // Modus ist aktuell statisch "Access Point" - es gibt noch keinen STA-Modus,
  // siehe Roadmap-Abschnitt in der README.
  tft.drawString("Modus: Access Point", 10, 98);
  tft.drawString("IP: " + apIp, 10, 116);

  drawButton(SETUP_LOG_BTN_X, SETUP_LOG_BTN_Y, SETUP_LOG_BTN_W, SETUP_LOG_BTN_H,
             s.datalogEnabled ? "Log Stop" : "Log Start", s.datalogEnabled);
}

void displayRender(const DisplayState& s) {
  drawChip(CHIP_MINMAX_X, CHIP_Y, CHIP_W, CHIP_H, "M/M", s.showMinMax);
  drawChip(CHIP_PRESET_X, CHIP_Y, CHIP_W, CHIP_H, "PRE", s.skipPreset);
  drawChip(CHIP_SETUP_X, CHIP_Y, CHIP_W, CHIP_H, "SET", showSetupScreen);

  if (showSetupScreen) {
    drawSetupScreen(s);
    lastScreenWasSetup = true;
    return;
  }
  if (lastScreenWasSetup) {
    // Setup-Screen nutzt andere Textpositionen als das Dashboard - dessen inkrementelle
    // (auf schmale Textfelder begrenzte) Updates wuerden Reste davon stehen lassen.
    tft.fillRect(0, 22, SCREEN_W, 218, TFT_BLACK);
    lastScreenWasSetup = false;
  }

  float powers[3] = { s.power1, s.power2, s.power3 };
  float percents[3] = { s.percent1, s.percent2, s.percent3 };

  tft.setTextFont(2);
  tft.setTextDatum(TC_DATUM);
  tft.setTextPadding(COL_W);
  for (int i = 0; i < 3; i++) {
    tft.setTextColor(TFT_CYAN, TFT_BLACK);
    tft.drawString(roleLabel(i + 1, s), COL_X[i] + COL_W / 2, 26);
    tft.setTextColor(TFT_WHITE, TFT_BLACK);
    tft.drawFloat(powers[i], 4, COL_X[i] + COL_W / 2, 46);
    tft.drawFloat(percents[i], 2, COL_X[i] + COL_W / 2, 66);
  }

  tft.drawFastHLine(8, 88, SCREEN_W - 16, TFT_DARKGREY);

  tft.setTextPadding(SCREEN_W);
  tft.setTextColor(TFT_CYAN, TFT_BLACK);
  tft.drawString(s.cascadeMode ? "Gesamtwirkungsgrad" : "Wirkungsgrad", SCREEN_W / 2, 94);

  // "--- %" bei fehlender/ungueltiger Verbindung statt eines irrefuehrenden "0.00 %"
  // (im Kaskade-Modus ergibt die Formel bei Power=0 rechnerisch 0, nicht negativ, wuerde
  // also ohne die explizite rs232Error-Abfrage faelschlich gruen als "guter" Wert erscheinen).
  bool noData = s.rs232Error || s.wirkungsgrad < 0;
  tft.setTextFont(4);
  tft.setTextColor(noData ? TFT_BLUE : TFT_GREEN, TFT_BLACK);
  tft.drawString(noData ? "--- %" : String(s.wirkungsgrad, 2) + " %", SCREEN_W / 2, 112);
  tft.setTextFont(2);

  tft.fillRect(0, STUFEN_Y, SCREEN_W, STUFEN_H, TFT_BLACK);
  if (s.showMinMax) {
    // Min/Max hat Vorrang vor der Stufenanzeige, solange der M/M-Chip aktiv ist -
    // auch im Kaskade-Modus (der M/M-Chip ist der explizite Nutzerwunsch).
    tft.setTextColor(TFT_WHITE, TFT_BLACK);
    String minMax = "Min/Max: " + String(s.minWirkungsgrad, 2) + " % / " + String(s.maxWirkungsgrad, 2) + " %";
    tft.drawString(minMax, SCREEN_W / 2, STUFEN_Y + 15);
  } else if (s.cascadeMode) {
    tft.setTextColor(TFT_WHITE, TFT_BLACK);
    String stufe1 = "Stufe 1 (In->Zw): " + String(s.stufe1Wirkungsgrad, 2) + " %";
    String stufe2 = "Stufe 2 (Zw->Out): " + String(s.stufe2Wirkungsgrad, 2) + " %";
    tft.drawString(stufe1, SCREEN_W / 2, STUFEN_Y + 6);
    tft.drawString(stufe2, SCREEN_W / 2, STUFEN_Y + 24);
  }

  tft.fillRect(0, ERROR_Y, SCREEN_W, ERROR_H, s.rs232Error ? TFT_RED : TFT_BLACK);
  if (s.rs232Error) {
    tft.setTextColor(TFT_WHITE, TFT_RED);
    tft.drawString("RS232 Fehler", SCREEN_W / 2, ERROR_Y + 2);
  }

  tft.setTextPadding(0);
  tft.setTextDatum(TL_DATUM);

  drawButton(BTN_RMT_X, BTN_RMT_Y, BTN_RMT_W, BTN_RMT_H, s.rmtEnabled ? "RMT OFF" : "RMT ON", s.rmtEnabled);
  drawButton(BTN_MODE_X, BTN_MODE_Y, BTN_MODE_W, BTN_MODE_H, s.cascadeMode ? "Kaskade" : "Parallel", s.cascadeMode);
}

DisplayAction displayPollTouch() {
  uint16_t x, y;
  bool touched = tft.getTouch(&x, &y);
  if (!touched) {
    touchActive = false;
    return DISPLAY_ACTION_NONE;
  }
  if (touchActive || millis() - lastTouchAction < 300) {
    return DISPLAY_ACTION_NONE;
  }
  touchActive = true;
  lastTouchAction = millis();

  // Der SET-Chip ist auf beiden Screens an derselben Stelle aktiv.
  if (pointInRect(x, y, CHIP_SETUP_X, CHIP_Y, CHIP_W, CHIP_H)) {
    showSetupScreen = !showSetupScreen;
    return DISPLAY_ACTION_NONE;
  }

  if (showSetupScreen) {
    // Dashboard-Buttons liegen unter dem Setup-Screen und duerfen dort nicht auslösen.
    if (pointInRect(x, y, SETUP_LOG_BTN_X, SETUP_LOG_BTN_Y, SETUP_LOG_BTN_W, SETUP_LOG_BTN_H)) {
      return DISPLAY_ACTION_TOGGLE_LOG;
    }
    return DISPLAY_ACTION_NONE;
  }

  if (pointInRect(x, y, BTN_RMT_X, BTN_RMT_Y, BTN_RMT_W, BTN_RMT_H)) {
    return DISPLAY_ACTION_TOGGLE_RMT;
  }
  if (pointInRect(x, y, BTN_MODE_X, BTN_MODE_Y, BTN_MODE_W, BTN_MODE_H)) {
    return DISPLAY_ACTION_TOGGLE_MODE;
  }
  if (pointInRect(x, y, CHIP_MINMAX_X, CHIP_Y, CHIP_W, CHIP_H)) {
    return DISPLAY_ACTION_TOGGLE_MINMAX;
  }
  if (pointInRect(x, y, CHIP_PRESET_X, CHIP_Y, CHIP_W, CHIP_H)) {
    return DISPLAY_ACTION_TOGGLE_PRESET;
  }
  return DISPLAY_ACTION_NONE;
}

void displayTouchDiagnostics() {
  tft.init();
  tft.setRotation(1);
  tft.fillScreen(TFT_BLACK);
  tft.setTextColor(TFT_WHITE, TFT_BLACK);
  tft.setTextFont(2);
  tft.setTextDatum(TL_DATUM);
  tft.drawString("Touch-Diagnose - siehe Serial Monitor (115200)", 10, 10);
  tft.drawString("Bildschirm beruehren...", 10, 30);

  pinMode(TOUCH_IRQ_PIN, INPUT_PULLUP);

  Serial.println("=== Touch-Diagnose gestartet ===");
  Serial.println("IRQ=LOW bedeutet: Touch-Panel selbst meldet eine Beruehrung (unabhaengig von SPI).");
  Serial.println("rawZ steigt beim Druecken: SPI-Verbindung zum XPT2046 funktioniert.");

  while (true) {
    uint16_t x = 0, y = 0;
    uint16_t z = tft.getTouchRawZ();
    bool irqLow = digitalRead(TOUCH_IRQ_PIN) == LOW;
    bool touched = tft.getTouch(&x, &y, 350);

    Serial.printf("IRQ=%s  rawZ=%4u  getTouch=%s  x=%4u y=%4u\n",
                  irqLow ? "LOW (beruehrt)" : "HIGH           ",
                  z,
                  touched ? "true " : "false",
                  x, y);
    delay(200);
  }
}
