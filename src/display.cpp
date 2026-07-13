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

static const int STUFEN_Y = 130, STUFEN_H = 40;
static const int ERROR_Y = 172, ERROR_H = 18;

static bool touchActive = false;
static unsigned long lastTouchAction = 0;

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

static String roleLabel(int channel, const DisplayState& s) {
  if (s.inputChannel == channel) return "Kanal " + String(channel) + " (In)";
  if (s.cascadeMode && s.stageChannel == channel) return "Kanal " + String(channel) + " (Zw)";
  if (s.cascadeMode && s.outputChannel == channel) return "Kanal " + String(channel) + " (Out)";
  return "Kanal " + String(channel);
}

void displayInit() {
  tft.init();
  tft.setRotation(1);
  prefs.begin("gpm8330", false);
  calibrateTouch();

  tft.fillScreen(TFT_BLACK);
  tft.fillRect(0, 0, SCREEN_W, 22, TFT_NAVY);
  tft.setTextColor(TFT_WHITE, TFT_NAVY);
  tft.setTextFont(2);
  tft.setTextDatum(MC_DATUM);
  tft.drawString("GPM-8330 Monitor", SCREEN_W / 2, 11);
  tft.setTextDatum(TL_DATUM);
}

void displayRender(const DisplayState& s) {
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

  tft.setTextFont(4);
  tft.setTextColor(s.wirkungsgrad < 0 ? TFT_BLUE : TFT_GREEN, TFT_BLACK);
  tft.drawString(s.wirkungsgrad < 0 ? "--- %" : String(s.wirkungsgrad, 2) + " %", SCREEN_W / 2, 112);
  tft.setTextFont(2);

  tft.fillRect(0, STUFEN_Y, SCREEN_W, STUFEN_H, TFT_BLACK);
  if (s.cascadeMode) {
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

  if (pointInRect(x, y, BTN_RMT_X, BTN_RMT_Y, BTN_RMT_W, BTN_RMT_H)) {
    return DISPLAY_ACTION_TOGGLE_RMT;
  }
  if (pointInRect(x, y, BTN_MODE_X, BTN_MODE_Y, BTN_MODE_W, BTN_MODE_H)) {
    return DISPLAY_ACTION_TOGGLE_MODE;
  }
  return DISPLAY_ACTION_NONE;
}
