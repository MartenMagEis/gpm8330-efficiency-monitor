// Ported from Arduino IDE sketch GPM_8330_001_copy_20250418184813.ino
// 115200 baud / 200ms Abfragezeit / 500 ms Update Webseite
// Gesicherter, funktionierender Ausgangspunkt – UART mit Webinterface

#include <Arduino.h>
#include <WiFi.h>
#include <WebServer.h>
#include <HardwareSerial.h>
#include <ArduinoOTA.h>
#include "display.h"
#include "datalog.h"

#define RXD2 16
#define TXD2 17
HardwareSerial rs232(2);

const char* ssid = "ESP32-GPM8330";
const char* password = "12345678";

WebServer server(80);

float Power_E1 = 0;
float Power_E2 = 0;
float Power_E3 = 0;

unsigned long lastQueryTime = 0;
unsigned long lastErrorTime = 0;
bool rs232Error = true;
bool isInitialized = false;
bool rmtEnabled = false;
bool cascadeMode = false; // false = 1 Input + 2 Outputs, true = kaskadierte Messung (Input -> Zwischenkreis -> Output)
bool skipPreset = false;  // uebergangsweise: PRESET-Set-Befehl beim Init weglassen (Testhypothese Remote-Lock)
int uartStep = 0;

float minWirkungsgrad = 0;
float maxWirkungsgrad = 0;
bool minMaxInitialized = false;
bool showMinMax = false;

// Einfache Browser-Zeit-Synchronisierung: der ESP32 hat im reinen AP-Modus keinen
// Internetzugang fuer NTP und keine batteriegepufferte RTC. Stattdessen schickt die
// Weboberflaeche beim Laden Date.now() an /settime; wir merken uns den Offset zu
// millis() und schaetzen die Unix-Zeit fuer CSV-Zeitstempel darüber.
int64_t timeSyncOffsetMs = 0;
bool timeSynced = false;

uint64_t currentEpochMs() {
  if (!timeSynced) return millis();
  return (uint64_t)((int64_t)millis() + timeSyncOffsetMs);
}

String lastSentCommand = "";
unsigned long commandSentTime = 0;
bool awaitingResponse = false;

const unsigned long uartTimeout = 200; // war 200 bei 9600 baut

String sendCommands[] = {
  ":NUMERIC:NORMAL:VALUE?3\n",
  ":NUMERIC:NORMAL:VALUE?23\n",
  ":NUMERIC:NORMAL:VALUE?43\n"
};

float* powerVars[] = { &Power_E1, &Power_E2, &Power_E3 };

void resetMinMax() {
  minMaxInitialized = false;
}

void setRmtEnabled(bool enabled) {
  rmtEnabled = enabled;
  if (enabled) {
    isInitialized = false;
    resetMinMax();
  } else {
    isInitialized = true;
    awaitingResponse = false;
  }
}

void setCascadeMode(bool enabled) {
  cascadeMode = enabled;
  resetMinMax();
}

void setSkipPreset(bool enabled) {
  skipPreset = enabled;
  if (rmtEnabled) {
    isInitialized = false;
  }
}

void sendNextCommand() {
  lastSentCommand = sendCommands[uartStep];
  while (rs232.available()) rs232.read();
  rs232.print(lastSentCommand);
  Serial.println("📤 Gesendet: " + lastSentCommand);
  commandSentTime = millis();
  awaitingResponse = true;
}

void handleUARTCommunication() {
  if (!rmtEnabled) return;
  unsigned long currentMillis = millis();

  if (!isInitialized) {
    if (!awaitingResponse) {
      lastSentCommand = ":NUMERIC:NORMAL:VALUE?3\n";
      rs232.print(lastSentCommand);
      Serial.println("📡 Init: Sende VALUE?3 zur Verbindungsprüfung");
      commandSentTime = currentMillis;
      awaitingResponse = true;
    } else if (currentMillis - commandSentTime > uartTimeout) {
      if (rs232.available()) {
        String response = rs232.readStringUntil('\n');
        Serial.println("✅ Init: Antwort empfangen -> " + response);
        delay(100);
        if (!skipPreset) {
          rs232.print(":NUMERIC:NORMAL:PRESET 4\n");
          delay(100);
          Serial.println("⚙️ Initialisierungsbefehl gesendet: PRESET 4");
        } else {
          Serial.println("⏭️ PRESET 4 uebersprungen (PRE-Toggle aktiv)");
        }
        isInitialized = true;
        uartStep = 0;
        awaitingResponse = false;
        rs232Error = false;
      } else {
        Serial.println("⚠️ Init: Timeout, versuche erneut...");
        rs232Error = true;
        lastErrorTime = currentMillis;
        awaitingResponse = false;
      }
    }
    server.handleClient();
    return;
  }

  if (!awaitingResponse && (currentMillis - lastQueryTime >= uartTimeout)) {
    sendNextCommand();
    lastQueryTime = currentMillis;
  }

  if (awaitingResponse && (currentMillis - commandSentTime <= uartTimeout)) {
    if (rs232.available()) {
      String message = rs232.readStringUntil('\n');
      message.trim();
      Serial.print("📥 Antwort empfangen (Schritt "); Serial.print(uartStep); Serial.print("): ");
      Serial.println(message);

      float value = atof(message.c_str());
      if (value != 0 || message.indexOf("0") != -1) {
        *(powerVars[uartStep]) = value;
        uartStep = (uartStep + 1) % 3;
        awaitingResponse = false;
        rs232Error = false;
      } else {
        Serial.println("⚠️ Antwort ungültig – warte weiter...");
      }
    }
  } else if (awaitingResponse && (currentMillis - commandSentTime > uartTimeout)) {
    Serial.println("❌ RS232 Fehler: Keine Antwort innerhalb " + String(uartTimeout) + "ms");
    rs232Error = true;
    lastErrorTime = currentMillis;
    uartStep = (uartStep + 1) % 3;
    awaitingResponse = false;
  }
}

DisplayState computeMetrics() {
  DisplayState s;
  s.power1 = Power_E1;
  s.power2 = Power_E2;
  s.power3 = Power_E3;

  float maxPowerTW = max(Power_E1, max(Power_E2, Power_E3));
  if (maxPowerTW == 0) maxPowerTW = 1;
  s.percent1 = Power_E1 < 0 ? 0 : (Power_E1 / maxPowerTW) * 100;
  s.percent2 = Power_E2 < 0 ? 0 : (Power_E2 / maxPowerTW) * 100;
  s.percent3 = Power_E3 < 0 ? 0 : (Power_E3 / maxPowerTW) * 100;

  // Kanäle nach Leistung sortieren: order[0]=Input (größte), order[1]=Zwischenkreis/Stufe, order[2]=Output (kleinste)
  float p[3] = { Power_E1, Power_E2, Power_E3 };
  int order[3] = { 0, 1, 2 };
  if (p[order[0]] < p[order[1]]) { int t = order[0]; order[0] = order[1]; order[1] = t; }
  if (p[order[1]] < p[order[2]]) { int t = order[1]; order[1] = order[2]; order[2] = t; }
  if (p[order[0]] < p[order[1]]) { int t = order[0]; order[0] = order[1]; order[1] = t; }
  s.inputChannel = order[0] + 1;
  s.stageChannel = order[1] + 1;
  s.outputChannel = order[2] + 1;

  s.stufe1Wirkungsgrad = 0;
  s.stufe2Wirkungsgrad = 0;

  if (cascadeMode) {
    float pInput = p[order[0]];
    float pStufe = p[order[1]];
    float pOutput = p[order[2]];
    float safeInput = pInput == 0 ? 1 : pInput;
    float safeStufe = pStufe == 0 ? 1 : pStufe;
    s.stufe1Wirkungsgrad = (pStufe / safeInput) * 100;   // Input -> Zwischenkreis
    s.stufe2Wirkungsgrad = (pOutput / safeStufe) * 100;  // Zwischenkreis -> Output
    s.wirkungsgrad = (pOutput / safeInput) * 100;         // Gesamtwirkungsgrad Input -> Output
  } else {
    s.wirkungsgrad = (s.percent1 + s.percent2 + s.percent3) - 100;
  }

  if (!rs232Error) {
    if (!minMaxInitialized) {
      minWirkungsgrad = s.wirkungsgrad;
      maxWirkungsgrad = s.wirkungsgrad;
      minMaxInitialized = true;
    } else {
      minWirkungsgrad = min(minWirkungsgrad, s.wirkungsgrad);
      maxWirkungsgrad = max(maxWirkungsgrad, s.wirkungsgrad);
    }
  }
  s.minWirkungsgrad = minWirkungsgrad;
  s.maxWirkungsgrad = maxWirkungsgrad;

  s.cascadeMode = cascadeMode;
  s.rmtEnabled = rmtEnabled;
  s.rs232Error = rs232Error;
  s.skipPreset = skipPreset;
  s.showMinMax = showMinMax;
  s.datalogEnabled = datalogIsEnabled();
  return s;
}

String generiereJSON(const DisplayState& s) {
  String json = "{";
  json += "\"power1\":" + String(s.power1, 4) + ",";
  json += "\"power2\":" + String(s.power2, 4) + ",";
  json += "\"power3\":" + String(s.power3, 4) + ",";
  json += "\"percent1\":" + String(s.percent1, 2) + ",";
  json += "\"percent2\":" + String(s.percent2, 2) + ",";
  json += "\"percent3\":" + String(s.percent3, 2) + ",";
  json += "\"mode\":\"" + String(s.cascadeMode ? "cascade" : "parallel") + "\",";
  json += "\"inputChannel\":" + String(s.inputChannel) + ",";
  json += "\"stageChannel\":" + String(s.stageChannel) + ",";
  json += "\"outputChannel\":" + String(s.outputChannel) + ",";
  json += "\"stufe1Wirkungsgrad\":" + String(s.stufe1Wirkungsgrad, 2) + ",";
  json += "\"stufe2Wirkungsgrad\":" + String(s.stufe2Wirkungsgrad, 2) + ",";
  json += "\"wirkungsgrad\":" + String(s.wirkungsgrad, 2) + ",";
  json += "\"minWirkungsgrad\":" + String(s.minWirkungsgrad, 2) + ",";
  json += "\"maxWirkungsgrad\":" + String(s.maxWirkungsgrad, 2) + ",";
  json += "\"rmtEnabled\":" + String(s.rmtEnabled ? "true" : "false") + ",";
  json += "\"datalogEnabled\":" + String(s.datalogEnabled ? "true" : "false") + ",";
  json += "\"error\":" + String(s.rs232Error ? "true" : "false");
  json += "}";
  return json;
}

void handleData() {
  server.send(200, "application/json", generiereJSON(computeMetrics()));
}

String generiereWebseite() {
  String html = "<!DOCTYPE html><html><head><meta charset='UTF-8'>";
  html += "<meta name='viewport' content='width=device-width, initial-scale=1'>";
  html += "<title>GPM-8330 Monitor</title>";
  html += "<style>body{font-family:sans-serif;text-align:center;font-size:1.1em;margin:12px;} "
          ".error{color:red;font-size:1.3em;font-weight:bold;min-height:1.3em;} "
          "h2{font-size:1.3em;margin:14px 0 4px;} "
          "table{margin:auto;border-collapse:collapse;table-layout:fixed;width:100%;max-width:480px;} "
          "td{padding:6px 2px;border:1px solid #888;width:33%;text-align:center;font-family:monospace;font-size:1.1em;} "
          "th{padding:4px 2px;border:1px solid #888;width:33%;text-align:center;font-size:0.65em;font-weight:normal;overflow-wrap:break-word;word-break:break-word;} "
          ".btnrow{display:flex;flex-wrap:wrap;justify-content:center;gap:8px;max-width:480px;margin:10px auto;} "
          ".btn{flex:0 0 140px;padding:12px 4px;font-size:0.9em;border:none;border-radius:6px;color:white;background:gray;cursor:pointer;} "
          ".btn-dark{background:#444;}</style>";
  html += "<script>function roleLabel(ch, d) {";
  html += "if (d.inputChannel === ch) return 'Kanal ' + ch + ' (Input)';";
  html += "if (d.mode === 'cascade' && d.stageChannel === ch) return 'Kanal ' + ch + ' (Zwischenkreis)';";
  html += "if (d.mode === 'cascade' && d.outputChannel === ch) return 'Kanal ' + ch + ' (Output)';";
  html += "return 'Kanal ' + ch;}";
  html += "let currentRmt = false, currentCascade = false, currentLog = false;";
  html += "function aktualisieren() {";
  html += "fetch('/data').then(r => r.json()).then(d => {";
  html += "document.getElementById('p1').innerHTML = d.power1.toFixed(4).padStart(7, ' ').replace(/^ +/, '&nbsp;') + ' W';";
  html += "document.getElementById('p2').innerHTML = d.power2.toFixed(4).padStart(7, ' ').replace(/^ +/, '&nbsp;') + ' W';";
  html += "document.getElementById('p3').innerHTML = d.power3.toFixed(4).padStart(7, ' ').replace(/^ +/, '&nbsp;') + ' W';";
  html += "document.getElementById('percent1').innerHTML = d.percent1.toFixed(2).padStart(6, ' ').replace(/^ +/, '&nbsp;') + ' %';";
  html += "document.getElementById('percent2').innerHTML = d.percent2.toFixed(2).padStart(6, ' ').replace(/^ +/, '&nbsp;') + ' %';";
  html += "document.getElementById('percent3').innerHTML = d.percent3.toFixed(2).padStart(6, ' ').replace(/^ +/, '&nbsp;') + ' %';";
  html += "document.getElementById('ch1header').innerHTML = roleLabel(1, d);";
  html += "document.getElementById('ch2header').innerHTML = roleLabel(2, d);";
  html += "document.getElementById('ch3header').innerHTML = roleLabel(3, d);";
  html += "document.getElementById('wirkungsgradText').innerHTML = d.mode === 'cascade' ? 'Gesamtwirkungsgrad' : 'Wirkungsgrad';";
  html += "let noData = d.error || d.wirkungsgrad < 0;";
  html += "document.getElementById('wirkungsgrad').innerHTML = noData ? \"<span style=\\'color:blue\\'>--- %</span>\" : d.wirkungsgrad.toFixed(2) + ' %';";
  html += "document.getElementById('minmax').innerHTML = d.minWirkungsgrad.toFixed(2) + ' % / ' + d.maxWirkungsgrad.toFixed(2) + ' %';";
  html += "document.getElementById('stufenInfo').style.display = d.mode === 'cascade' ? 'block' : 'none';";
  html += "document.getElementById('stufe1').innerHTML = d.stufe1Wirkungsgrad.toFixed(2) + ' %';";
  html += "document.getElementById('stufe2').innerHTML = d.stufe2Wirkungsgrad.toFixed(2) + ' %';";
  html += "if(d.error){ document.getElementById('error').innerHTML = 'RS232 Fehler'; } else { document.getElementById('error').innerHTML = ''; }";
  html += "currentRmt = d.rmtEnabled; currentCascade = (d.mode === 'cascade'); currentLog = d.datalogEnabled;";
  html += "let rmtBtn = document.getElementById('rmtBtn');";
  html += "rmtBtn.innerText = currentRmt ? 'RMT OFF' : 'RMT ON'; rmtBtn.style.backgroundColor = currentRmt ? 'green' : 'gray';";
  html += "let modeBtn = document.getElementById('modeBtn');";
  html += "modeBtn.innerText = currentCascade ? 'Kaskade' : 'Parallel'; modeBtn.style.backgroundColor = currentCascade ? 'green' : 'gray';";
  html += "let logBtn = document.getElementById('logBtn');";
  html += "logBtn.innerText = currentLog ? 'Log Stop' : 'Log Start'; logBtn.style.backgroundColor = currentLog ? 'green' : 'gray';";
  html += "});}";
  html += "function toggleRMT() { fetch('/rmt?enabled=' + (currentRmt ? '0' : '1')); }";
  html += "function toggleMode() { fetch('/mode?cascade=' + (currentCascade ? '0' : '1')); }";
  html += "function toggleLog() { fetch('/log?enabled=' + (currentLog ? '0' : '1')); }";
  html += "function clearLog() { fetch('/csv/clear'); }";
  html += "setInterval(aktualisieren, 500);";
  html += "window.onload = function() { fetch('/settime?t=' + Date.now()); aktualisieren(); };";
  html += "</script>";
  html += "</head><body><div id='error' class='error'></div>";
  html += "<h2>Wirkleistung (Watt) je Kanal</h2>";
  html += "<table><tr><th id='ch1header'>Kanal 1</th><th id='ch2header'>Kanal 2</th><th id='ch3header'>Kanal 3</th></tr>";
  html += "<tr><td id='p1'>Lade...</td><td id='p2'>Lade...</td><td id='p3'>Lade...</td></tr>";
  html += "<tr><td id='percent1'>Lade...</td><td id='percent2'>Lade...</td><td id='percent3'>Lade...</td></tr></table>";
  html += "<h2><span id='wirkungsgradText'>Wirkungsgrad</span>: <span id='wirkungsgrad'>Lade...</span></h2>";
  html += "<div style='font-size:0.6em;margin:2px 0;'>Min/Max: <span id='minmax'>--</span></div>";
  html += "<div id='stufenInfo' style='display:none;font-size:0.6em;margin:2px 0;'>";
  html += "<div>Stufe 1 (Input &rarr; Zwischenkreis): <span id='stufe1'>--</span></div>";
  html += "<div>Stufe 2 (Zwischenkreis &rarr; Output): <span id='stufe2'>--</span></div>";
  html += "</div>";
  html += "<div class='btnrow'>";
  html += "<button id='rmtBtn' class='btn' onclick=\"toggleRMT()\">RMT ON</button>";
  html += "<button id='modeBtn' class='btn' onclick=\"toggleMode()\">Parallel</button>";
  html += "<button id='logBtn' class='btn' onclick=\"toggleLog()\">Log Start</button>";
  html += "</div>";
  html += "<div class='btnrow'>";
  html += "<a href='/csv' style='text-decoration:none;'><button class='btn btn-dark'>CSV laden</button></a>";
  html += "<button class='btn btn-dark' onclick=\"clearLog()\">Log leeren</button>";
  html += "</div>";
  html += "</body></html>";
  return html;
}

void handleRoot() {
  server.send(200, "text/html", generiereWebseite());
}

void setup() {
  Serial.begin(115200);
  delay(3000);
  rs232.begin(115200, SERIAL_8N1, RXD2, TXD2);
  Serial.println("UART2 gestartet: TXD2=17, RXD2=16");

  datalogInit();

  WiFi.softAP(ssid, password);

  ArduinoOTA.setHostname("gpm8330-monitor");
  ArduinoOTA.setPassword(password);
  ArduinoOTA.begin();

  server.on("/", handleRoot);
  server.on("/data", handleData);
  server.begin();
  server.on("/rmt", []() {
    if (server.hasArg("enabled")) {
      setRmtEnabled(server.arg("enabled") == "1");
      Serial.println(rmtEnabled ? "🔄 RMT aktiviert – Initialisierung wird zugelassen" : "⏸️ RMT deaktiviert – Kommunikation pausiert");
    }
    server.send(200, "text/plain", "OK");
  });
  server.on("/mode", []() {
    if (server.hasArg("cascade")) {
      setCascadeMode(server.arg("cascade") == "1");
      Serial.println(cascadeMode ? "🔀 Modus: Kaskade (Stufen)" : "🔀 Modus: Parallel (1 Input + 2 Outputs)");
    }
    server.send(200, "text/plain", "OK");
  });
  server.on("/log", []() {
    if (server.hasArg("enabled")) {
      datalogSetEnabled(server.arg("enabled") == "1");
      Serial.println(datalogIsEnabled() ? "🗒️ CSV-Log gestartet" : "🗒️ CSV-Log gestoppt");
    }
    server.send(200, "text/plain", "OK");
  });
  server.on("/csv", []() {
    datalogWriteCsv(server);
  });
  server.on("/csv/clear", []() {
    datalogClear();
    server.send(200, "text/plain", "OK");
  });
  server.on("/settime", []() {
    if (server.hasArg("t")) {
      int64_t epochMs = strtoll(server.arg("t").c_str(), nullptr, 10);
      timeSyncOffsetMs = epochMs - (int64_t)millis();
      timeSynced = true;
    }
    server.send(200, "text/plain", "OK");
  });

  displayInit(ssid, password, WiFi.softAPIP().toString());
}

void loop() {
  handleUARTCommunication();
  server.handleClient();
  ArduinoOTA.handle();

  static unsigned long lastDisplayUpdate = 0;
  static unsigned long lastLogSample = 0;
  unsigned long currentMillis = millis();
  if (currentMillis - lastDisplayUpdate >= 500) {
    lastDisplayUpdate = currentMillis;
    displayRender(computeMetrics());
  }
  if (currentMillis - lastLogSample >= 1000) {
    lastLogSample = currentMillis;
    datalogSample(computeMetrics(), currentEpochMs());
  }

  DisplayAction action = displayPollTouch();
  if (action == DISPLAY_ACTION_TOGGLE_RMT) {
    setRmtEnabled(!rmtEnabled);
    Serial.println(rmtEnabled ? "🔄 RMT aktiviert (Touch)" : "⏸️ RMT deaktiviert (Touch)");
  } else if (action == DISPLAY_ACTION_TOGGLE_MODE) {
    setCascadeMode(!cascadeMode);
    Serial.println(cascadeMode ? "🔀 Modus: Kaskade (Touch)" : "🔀 Modus: Parallel (Touch)");
  } else if (action == DISPLAY_ACTION_TOGGLE_PRESET) {
    setSkipPreset(!skipPreset);
    Serial.println(skipPreset ? "⏭️ PRESET-Skip aktiviert (Touch)" : "⏭️ PRESET-Skip deaktiviert (Touch)");
  } else if (action == DISPLAY_ACTION_TOGGLE_MINMAX) {
    showMinMax = !showMinMax;
  } else if (action == DISPLAY_ACTION_TOGGLE_LOG) {
    datalogSetEnabled(!datalogIsEnabled());
    Serial.println(datalogIsEnabled() ? "🗒️ CSV-Log gestartet (Touch)" : "🗒️ CSV-Log gestoppt (Touch)");
  }
}
