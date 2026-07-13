// Dieses ist die Version die ich Stand 18.04.2025 erst einmal nutzen will GPM_8330_001_copy_20250418154250
// 115200 baut / 200ms Abfragezeit / 500 ms Update Webseite
// Gesicherter, funktionierender Ausgangspunkt – UART mit Webinterface
// Diese Software habe ich am 16.07.2025 als "ALKS ESP32" Board auf das Tool für die Firma aufgespielt 

#include <WiFi.h>
#include <WebServer.h>
#include <HardwareSerial.h>

#define RXD2 16
#define TXD2 17
HardwareSerial rs232(2);

const char* ssid = "ESP32-GPM8330";
const char* password = "12345678";

WebServer server(80);

float Power_E1 = 0;
float Power_E2 = 0;
float Power_E3 = 0;
float proz_Power_E1 = 0;
float proz_Power_E2 = 0;
float proz_Power_E3 = 0;

unsigned long lastQueryTime = 0;
unsigned long lastErrorTime = 0;
bool rs232Error = true;
bool isInitialized = false;
bool rmtEnabled = false;
int uartStep = 0;

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
        rs232.print(":NUMERIC:NORMAL:PRESET 4\n");
        delay(100);
        Serial.println("⚙️ Initialisierungsbefehl gesendet: PRESET 4");
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

String generiereJSON() {
  float maxPowerTW = max(Power_E1, max(Power_E2, Power_E3));
  if (maxPowerTW == 0) maxPowerTW = 1;
  proz_Power_E1 = Power_E1 < 0 ? 0 : (Power_E1 / maxPowerTW) * 100;
  proz_Power_E2 = Power_E2 < 0 ? 0 : (Power_E2 / maxPowerTW) * 100;
  proz_Power_E3 = Power_E3 < 0 ? 0 : (Power_E3 / maxPowerTW) * 100;

  float wirkungsgrad = (proz_Power_E1 + proz_Power_E2 + proz_Power_E3) - 100;

  String json = "{";
  json += "\"power1\":" + String(Power_E1, 4) + ",";
  json += "\"power2\":" + String(Power_E2, 4) + ",";
  json += "\"power3\":" + String(Power_E3, 4) + ",";
  json += "\"percent1\":" + String(proz_Power_E1, 2) + ",";
  json += "\"percent2\":" + String(proz_Power_E2, 2) + ",";
  json += "\"percent3\":" + String(proz_Power_E3, 2) + ",";
  json += "\"wirkungsgrad\":" + String(wirkungsgrad, 2) + ",";
  json += "\"error\":" + String(rs232Error ? "true" : "false");
  json += "}";
  return json;
}

void handleData() {
  server.send(200, "application/json", generiereJSON());
}

String generiereWebseite() {
  String html = "<!DOCTYPE html><html><head><meta charset='UTF-8'>";
  html += "<title>GPM-8330 Monitor</title>";
  html += "<style>body{font-family:sans-serif;text-align:center;font-size:2em;} .error{color:red;} table{margin:auto;border-collapse:collapse;table-layout:fixed;width:80%;}td,th{padding:10px;border:1px solid #888;width:33%;text-align:center;font-family:monospace;font-size:1.6em;} </style>";
  html += "<script>function aktualisieren() {";
  html += "fetch('/data').then(r => r.json()).then(d => {";
  html += "document.getElementById('p1').innerHTML = d.power1.toFixed(4).padStart(7, ' ').replace(/^ +/, '&nbsp;') + ' W';";
  html += "document.getElementById('p2').innerHTML = d.power2.toFixed(4).padStart(7, ' ').replace(/^ +/, '&nbsp;') + ' W';";
  html += "document.getElementById('p3').innerHTML = d.power3.toFixed(4).padStart(7, ' ').replace(/^ +/, '&nbsp;') + ' W';";
  html += "document.getElementById('percent1').innerHTML = d.percent1.toFixed(2).padStart(6, ' ').replace(/^ +/, '&nbsp;') + ' %';";
  html += "document.getElementById('percent2').innerHTML = d.percent2.toFixed(2).padStart(6, ' ').replace(/^ +/, '&nbsp;') + ' %';";
  html += "document.getElementById('percent3').innerHTML = d.percent3.toFixed(2).padStart(6, ' ').replace(/^ +/, '&nbsp;') + ' %';";
  html += "document.getElementById('wirkungsgrad').innerHTML = d.wirkungsgrad < 0 ? \"<span style=\\'color:blue\\'>--- %</span>\" : d.wirkungsgrad.toFixed(2) + ' %';";
  html += "if(d.error){ document.getElementById('error').innerHTML = 'RS232 Fehler'; } else { document.getElementById('error').innerHTML = ''; }";
  html += "});}";
  html += "setInterval(aktualisieren, 500); window.onload = aktualisieren;";
  html += "</script>";
  html += "</head><body><div id='error' class='error'></div>";
  html += "<h2>Wirkleistung (Watt) je Kanal</h2>";
  html += "<table><tr><th>Kanal 1</th><th>Kanal 2</th><th>Kanal 3</th></tr>";
  html += "<tr><td id='p1'>Lade...</td><td id='p2'>Lade...</td><td id='p3'>Lade...</td></tr>";
  html += "<tr><td id='percent1'>Lade...</td><td id='percent2'>Lade...</td><td id='percent3'>Lade...</td></tr></table>";
  html += "<h2>Wirkungsgrad: <span id='wirkungsgrad'>Lade...</span></h2>";
  html += "<div style='margin-top:20px;'>";
  html += "<button id='rmtOnBtn' onclick=\"setRMT(true)\" style=\"background-color:gray;color:white;padding:10px 20px;margin:5px;font-size:1.5em;\">RMT ON</button>";
  html += "<button id='rmtOffBtn' onclick=\"setRMT(false)\" style=\"background-color:green;color:white;padding:10px 20px;margin:5px;font-size:1.5em;\">RMT OFF</button>";
  html += "</div>";
  html += "<script>let rmtEnabled = false;function setRMT(state) {rmtEnabled = state;document.getElementById('rmtOnBtn').style.backgroundColor = state ? 'green' : 'gray';document.getElementById('rmtOffBtn').style.backgroundColor = state ? 'gray' : 'green';fetch('/rmt?enabled=' + (state ? '1' : '0'));}</script>";
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

  WiFi.softAP(ssid, password);
  server.on("/", handleRoot);
  server.on("/data", handleData);
  server.begin();
  server.on("/rmt", []() {
    if (server.hasArg("enabled")) {
      String val = server.arg("enabled");
      if (val == "1") {
        rmtEnabled = true;
        isInitialized = false;
        Serial.println("🔄 RMT aktiviert – Initialisierung wird zugelassen");
      } else {
        rmtEnabled = false;
        isInitialized = true;
        awaitingResponse = false;
        Serial.println("⏸️ RMT deaktiviert – Kommunikation pausiert");
      }
    }
    server.send(200, "text/plain", "OK");
  });
}

void loop() {
  handleUARTCommunication();
  server.handleClient();
}
