#pragma once
#include <Arduino.h>
#include <WebServer.h>
#include "display.h"

// Allokiert den Ringpuffer auf dem Heap - einmalig aus setup() aufrufen, bevor
// datalogSample()/datalogWriteCsv() benutzt werden.
void datalogInit();

void datalogSetEnabled(bool enabled);
bool datalogIsEnabled();

// No-op wenn datalogIsEnabled() false ist. 1x/Sekunde aus loop() aufrufen.
// epochMs: geschaetzte Unix-Zeit in ms (siehe currentEpochMs() in main.cpp) -
// faellt auf boot-relative millis() zurueck, solange kein Browser die Uhrzeit
// synchronisiert hat.
void datalogSample(const DisplayState& state, uint64_t epochMs);

void datalogClear();

// Streamt die aktuell gepufferten Messwerte als CSV-Antwort ueber den WebServer.
void datalogWriteCsv(WebServer& server);
