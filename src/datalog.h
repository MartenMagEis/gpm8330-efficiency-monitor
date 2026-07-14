#pragma once
#include <Arduino.h>
#include <WebServer.h>
#include "display.h"

// Allokiert den RAM-Ringpuffer und mountet die SD-Karte (falls vorhanden) -
// einmalig aus setup() aufrufen, bevor datalogSample()/datalogWriteCsv() benutzt werden.
void datalogInit();

// epochMs wird als Startzeit in den SD-Dateinamen codiert (nur beim Start relevant).
void datalogSetEnabled(bool enabled, uint64_t epochMs);
bool datalogIsEnabled();
bool datalogSdAvailable();

// No-op wenn datalogIsEnabled() false ist. 1x/Sekunde aus loop() aufrufen.
// epochMs: geschaetzte Unix-Zeit in ms (siehe currentEpochMs() in main.cpp) -
// faellt auf boot-relative millis() zurueck, solange kein Browser die Uhrzeit
// synchronisiert hat. Schreibt sowohl in den RAM-Ringpuffer (kurzfristiger
// Rueckblick ueber /csv) als auch, falls verfuegbar, laufend in eine Datei auf
// der SD-Karte (unbegrenzte Dauer/Groesse, siehe /sdfiles und /sdfile).
void datalogSample(const DisplayState& state, uint64_t epochMs);

// Leert nur den RAM-Ringpuffer (fuer /csv) - SD-Dateien bleiben unberuehrt,
// siehe Begruendung in datalog.cpp.
void datalogClear();

// Streamt die aktuell im RAM gepufferten Messwerte als CSV-Antwort ueber den WebServer.
void datalogWriteCsv(WebServer& server);

// Liste der Dateien auf der SD-Karte als JSON-Array [{"name":...,"size":...}, ...].
String datalogSdFileListJson();

// Streamt eine einzelne Datei von der SD-Karte als Download.
void datalogDownloadSdFile(WebServer& server, const String& name);
