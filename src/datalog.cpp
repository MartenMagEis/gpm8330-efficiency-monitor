#include "datalog.h"
#include <new>

// Ringpuffer: 1 Sample/s * 3600 = 1h Historie. Erweiterung um weitere Messgroessen
// (z.B. U/I/S/Q je Kanal): Feld hier ergaenzen, csvHeader() und die Zeilenformatierung
// in datalogWriteCsv() entsprechend erweitern.
struct LogSample {
  uint64_t t; // Unix-Zeit in ms (oder boot-relative millis(), falls (noch) nicht synchronisiert)
  float p1, p2, p3;
  float eff;
  bool cascade;
};

// 1 Sample/s * 1800 = 30 min Historie. Heap-allokiert statt statisches Array,
// da ein Array dieser Groesse nicht in die feste DRAM-Region (.bss) neben
// WiFi/TFT_eSPI/etc. passt. new(std::nothrow) statt new: ein fehlgeschlagenes
// new[] wirft sonst eine Exception, die auf diesem Core mangels Handler zum
// abort() fuehrt (siehe frueherer Crash mit 3600 Samples / 64-Bit-Zeitstempel).
static const int LOG_BUFFER_SIZE = 1800;
static LogSample* samples = nullptr;
static int writeIndex = 0;
static int count = 0;
static bool enabled = false;

void datalogInit() {
  samples = new (std::nothrow) LogSample[LOG_BUFFER_SIZE];
  if (samples) {
    Serial.printf("🗒️ CSV-Ringpuffer: %d Samples allokiert (%u Bytes), freier Heap danach: %u Bytes\n",
                  LOG_BUFFER_SIZE, (unsigned)(LOG_BUFFER_SIZE * sizeof(LogSample)), ESP.getFreeHeap());
  } else {
    Serial.println("⚠️ CSV-Ringpuffer konnte nicht allokiert werden - Logging bleibt deaktiviert");
  }
}

void datalogSetEnabled(bool e) {
  enabled = e;
}

bool datalogIsEnabled() {
  return enabled;
}

void datalogSample(const DisplayState& s, uint64_t epochMs) {
  if (!enabled || !samples) return;
  samples[writeIndex] = { epochMs, s.power1, s.power2, s.power3, s.wirkungsgrad, s.cascadeMode };
  writeIndex = (writeIndex + 1) % LOG_BUFFER_SIZE;
  if (count < LOG_BUFFER_SIZE) count++;
}

void datalogClear() {
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
  server.sendContent("t_epoch_ms,power1_w,power2_w,power3_w,wirkungsgrad_pct,mode\n");

  int start = (count < LOG_BUFFER_SIZE) ? 0 : writeIndex;
  char tBuf[24];
  for (int i = 0; i < count; i++) {
    const LogSample& s = samples[(start + i) % LOG_BUFFER_SIZE];
    snprintf(tBuf, sizeof(tBuf), "%llu", (unsigned long long)s.t);
    String row = String(tBuf) + "," + String(s.p1, 4) + "," + String(s.p2, 4) + "," +
                 String(s.p3, 4) + "," + String(s.eff, 2) + "," +
                 (s.cascade ? "cascade" : "parallel") + "\n";
    server.sendContent(row);
  }
}
