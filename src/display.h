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
  int inputChannel;
  int stageChannel;
  int outputChannel;
  bool cascadeMode;
  bool rmtEnabled;
  bool rs232Error;
};

enum DisplayAction {
  DISPLAY_ACTION_NONE,
  DISPLAY_ACTION_TOGGLE_RMT,
  DISPLAY_ACTION_TOGGLE_MODE
};

void displayInit();
void displayRender(const DisplayState& state);
DisplayAction displayPollTouch();
