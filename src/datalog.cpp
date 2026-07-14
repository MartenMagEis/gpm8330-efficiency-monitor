#include "datalog.h"
#include <new>
#include <SPI.h>
#include <SD.h>
#include <vector>
#include <algorithm>

// Ringpuffer: 1 Sample/s * 3600 = 1h Historie. Erweiterung um weitere Messgroessen
// (z.B. U/I/S/Q je Kanal): Feld hier ergaenzen, csvRow()/csvHeader() entsprechend erweitern.
struct LogSample {
  uint64_t t; // Unix-Zeit in ms (oder boot-relative millis(), falls (noch) nicht synchronisiert)
  float p1, p2, p3;
  float eff;
  bool cascade;
};

static const char* CSV_HEADER = "t_epoch_ms,power1_w,power2_w,power3_w,wirkungsgrad_pct,mode\n";

static String csvRow(uint64_t t, float p1, float p2, float p3, float eff, bool cascade) {
  char tBuf[24];
  snprintf(tBuf, sizeof(tBuf), "%llu", (unsigned long long)t);
  return String(tBuf) + "," + String(p1, 4) + "," + String(p2, 4) + "," +
         String(p3, 4) + "," + String(eff, 2) + "," + (cascade ? "cascade" : "parallel") + "\n";
}

// 1 Sample/s * 1800 = 30 min Historie fuer den schnellen RAM-Rueckblick (/csv).
// Heap-allokiert statt statisches Array, da ein Array dieser Groesse nicht in die
// feste DRAM-Region (.bss) neben WiFi/TFT_eSPI/etc. passt. new(std::nothrow) statt
// new: ein fehlgeschlagenes new[] wirft sonst eine Exception, die auf diesem Core
// mangels Handler zum abort() fuehrt (siehe frueherer Crash mit 3600 Samples).
static const int LOG_BUFFER_SIZE = 1800;
static LogSample* samples = nullptr;
static int writeIndex = 0;
static int count = 0;
static bool enabled = false;

// SD-Karte: geteilter SPI-Bus mit Display/Touch (SCK=18, MOSI=19, MISO=23), eigenes CS.
static const int SD_CS_PIN = 25;
static bool sdAvailable = false;
static File sdFile;
static String currentSdFilename;

void datalogInit() {
  samples = new (std::nothrow) LogSample[LOG_BUFFER_SIZE];
  if (samples) {
    Serial.printf("🗒️ CSV-Ringpuffer: %d Samples allokiert (%u Bytes), freier Heap danach: %u Bytes\n",
                  LOG_BUFFER_SIZE, (unsigned)(LOG_BUFFER_SIZE * sizeof(LogSample)), ESP.getFreeHeap());
  } else {
    Serial.println("⚠️ CSV-Ringpuffer konnte nicht allokiert werden - RAM-Log bleibt deaktiviert");
  }

  // Bus explizit mit den bekannten TFT_eSPI-Pins initialisieren, statt uns auf
  // TFT_eSPIs interne SPI-Verwaltung zu verlassen - beide teilen sich denselben
  // physischen Bus (SCK/MOSI/MISO), nur CS ist je Chip unterschiedlich.
  SPI.begin(18, 23, 19, SD_CS_PIN);
  sdAvailable = SD.begin(SD_CS_PIN, SPI);
  if (sdAvailable) {
    uint64_t sizeMb = SD.cardSize() / (1024ULL * 1024ULL);
    Serial.printf("💾 SD-Karte erkannt: %llu MB\n", (unsigned long long)sizeMb);
  } else {
    Serial.println("⚠️ Keine SD-Karte gefunden - SD-Logging bleibt deaktiviert (RAM-Log funktioniert trotzdem)");
  }
}

bool datalogSdAvailable() {
  return sdAvailable;
}

void datalogSetEnabled(bool e, uint64_t epochMs) {
  if (e && !enabled && sdAvailable) {
    char tBuf[24];
    snprintf(tBuf, sizeof(tBuf), "%llu", (unsigned long long)epochMs);
    currentSdFilename = "/gpm8330_" + String(tBuf) + ".csv";
    sdFile = SD.open(currentSdFilename, FILE_WRITE);
    if (sdFile) {
      sdFile.print(CSV_HEADER);
      sdFile.flush();
      Serial.println("💾 SD-Log gestartet: " + currentSdFilename);
    } else {
      Serial.println("⚠️ SD-Log konnte nicht angelegt werden: " + currentSdFilename);
    }
  } else if (!e && enabled && sdFile) {
    sdFile.close();
    Serial.println("💾 SD-Log geschlossen: " + currentSdFilename);
  }
  enabled = e;
}

bool datalogIsEnabled() {
  return enabled;
}

void datalogSample(const DisplayState& s, uint64_t epochMs) {
  if (!enabled) return;

  if (samples) {
    samples[writeIndex] = { epochMs, s.power1, s.power2, s.power3, s.wirkungsgrad, s.cascadeMode };
    writeIndex = (writeIndex + 1) % LOG_BUFFER_SIZE;
    if (count < LOG_BUFFER_SIZE) count++;
  }

  if (sdFile) {
    sdFile.print(csvRow(epochMs, s.power1, s.power2, s.power3, s.wirkungsgrad, s.cascadeMode));
    // Bei jedem Sample flushen (nur 1x/s) statt zu puffern, damit bei einem
    // Stromausfall waehrend eines laengeren Testlaufs moeglichst wenig verloren geht.
    sdFile.flush();
  }
}

void datalogClear() {
  // Leert nur den RAM-Ringpuffer (kurzfristiger Rueckblick). SD-Dateien sind
  // pro Session eigene Dateien und werden bewusst nicht durch diese Aktion
  // geloescht - "Log leeren" soll nicht versehentlich einen laufenden oder
  // vergangenen SD-Mitschnitt zerstoeren.
  writeIndex = 0;
  count = 0;
}

void datalogWriteCsv(WebServer& server) {
  if (!samples) {
    server.send(503, "text/plain", "Log-Ringpuffer nicht verfuegbar (Allokation beim Boot fehlgeschlagen)");
    return;
  }
  server.sendHeader("Content-Disposition", "attachment; filename=\"gpm8330_log.csv\"");
  server.setContentLength(CONTENT_LENGTH_UNKNOWN);
  server.send(200, "text/csv", "");
  server.sendContent(CSV_HEADER);

  int start = (count < LOG_BUFFER_SIZE) ? 0 : writeIndex;
  for (int i = 0; i < count; i++) {
    const LogSample& s = samples[(start + i) % LOG_BUFFER_SIZE];
    server.sendContent(csvRow(s.t, s.p1, s.p2, s.p3, s.eff, s.cascade));
  }
}

struct SdFileEntry {
  String name;
  size_t size;
  time_t mtime;
};

// Sortiert nach dem tatsaechlichen Schreibzeitpunkt der Datei (FAT-Zeitstempel),
// nicht nach dem Dateinamen - der enthaelt zwar meist die Startzeit als Unix-Millisekunden,
// faellt aber auf boot-relative millis() zurueck, wenn beim Log-Start noch keine Zeit
// synchronisiert war (siehe currentEpochMs()). Ein alphabetischer Namensvergleich wuerde
// solche Alt-Dateien dann falsch einsortieren. getLastWrite() liefert nur dann sinnvolle
// Werte, wenn settimeofday() vorher ueber /settime aufgerufen wurde (siehe main.cpp).
String datalogSdFileListJson() {
  if (!sdAvailable) return "[]";
  File root = SD.open("/");
  if (!root) return "[]";
  std::vector<SdFileEntry> entries;
  File f = root.openNextFile();
  while (f) {
    if (!f.isDirectory()) {
      entries.push_back({ String(f.name()), (size_t)f.size(), f.getLastWrite() });
    }
    f = root.openNextFile();
  }
  root.close();

  std::sort(entries.begin(), entries.end(), [](const SdFileEntry& a, const SdFileEntry& b) {
    return a.mtime > b.mtime;
  });

  String json = "[";
  for (size_t i = 0; i < entries.size(); i++) {
    if (i > 0) json += ",";
    json += "{\"name\":\"" + entries[i].name + "\",\"size\":" + String(entries[i].size) + "}";
  }
  return json + "]";
}

void datalogDownloadSdFile(WebServer& server, const String& name) {
  if (!sdAvailable) {
    server.send(503, "text/plain", "SD-Karte nicht verfuegbar");
    return;
  }
  String path = name.startsWith("/") ? name : "/" + name;
  File f = SD.open(path, FILE_READ);
  if (!f || f.isDirectory()) {
    server.send(404, "text/plain", "Datei nicht gefunden");
    return;
  }
  server.sendHeader("Content-Disposition", "attachment; filename=\"" + name + "\"");
  server.streamFile(f, "text/csv");
  f.close();
}
