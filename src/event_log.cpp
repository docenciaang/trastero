// =============================================================================
// event_log.cpp — Registro de eventos en NVS + servicio BLE de descarga
//
// Almacenamiento: NVS (Non-Volatile Storage) vía Preferences.
//   Namespace : "eventlog"
//   Clave "hdr"  : blob LogHeader (16 bytes) — cabecera del buffer circular
//   Claves "rN"  : blob LogRecord (24 bytes) — registro N (índice físico 0..N-1)
//
// NVS es atómico por diseño: cada putBytes es una operación indivisible.
// No hay riesgo de corrupción de superbloque ante cortes de corriente.
// =============================================================================

#include "event_log.h"
#include "ble_log.h"

#include <Preferences.h>
#include <time.h>
#include <freertos/semphr.h>

// =============================================================================
// Verificación de tamaños en tiempo de compilación
// =============================================================================

static_assert(sizeof(LogRecord) == 24, "LogRecord debe medir 24 bytes");
static_assert(sizeof(LogHeader) == 16, "LogHeader debe medir 16 bytes");

// =============================================================================
// Constantes
// =============================================================================

#define LOG_MAGIC       0xDEAD1234u
// Capacidad ajustada a la partición NVS (0x5000 = 20 KB).
// Cada registro ocupa ~2 slots NVS de 32 bytes = ~64 bytes reales.
// 100 registros × 64 bytes = 6.4 KB + cabecera → margen seguro en 20 KB.
#define LOG_CAPACITY    100u
#define NVS_NAMESPACE   "eventlog"

// =============================================================================
// Estado del módulo
// =============================================================================

static SemaphoreHandle_t logMutex = nullptr;
static LogHeader         hdr      = {};
static Preferences       prefs;

QueueHandle_t eventLogQueue = nullptr;

// Estado de streaming
static volatile bool     streamActive     = false;
static volatile bool     streamHeaderSent = false;
static volatile uint32_t streamIndex      = 0;

// =============================================================================
// Helpers internos
// =============================================================================

static const char* stateNameForLog(uint8_t s) {
    switch (s) {
        case 0: return "IDLE";
        case 1: return "EXTRACTOR";
        case 2: return "DESHUMID";
        case 3: return "FALLO";
        default: return "???";
    }
}

// Formatea un LogRecord como línea CSV.
// buf debe tener al menos 128 bytes. Devuelve buf.
static const char* recordToCsvLine(const LogRecord& rec, char* buf, size_t bufLen) {
    struct tm t = {};
    time_t ts = (time_t)rec.timestamp;
    gmtime_r(&ts, &t);

    char intTempStr[8], intHumStr[8], extTempStr[8], extHumStr[8];
    if (rec.intValid) {
        snprintf(intTempStr, sizeof(intTempStr), "%.1f", rec.intTemp);
        snprintf(intHumStr,  sizeof(intHumStr),  "%.1f", rec.intHum);
    } else {
        strcpy(intTempStr, "--");
        strcpy(intHumStr,  "--");
    }
    if (rec.extValid) {
        snprintf(extTempStr, sizeof(extTempStr), "%.1f", rec.extTemp);
        snprintf(extHumStr,  sizeof(extHumStr),  "%.1f", rec.extHum);
    } else {
        strcpy(extTempStr, "--");
        strcpy(extHumStr,  "--");
    }

    snprintf(buf, bufLen,
        "%04d-%02d-%02d;%02d:%02d:%02d;%s;%s;%s;%s;%u;%s;%s;%u\r\n",
        t.tm_year + 1900, t.tm_mon + 1, t.tm_mday,
        t.tm_hour, t.tm_min, t.tm_sec,
        stateNameForLog(rec.prevState),
        stateNameForLog(rec.newState),
        intTempStr, intHumStr, (unsigned)rec.intValid,
        extTempStr, extHumStr, (unsigned)rec.extValid);

    return buf;
}

// Escribe un LogRecord en la posición circular tail. Llamar bajo logMutex.
static bool writeRecord(const LogRecord& rec) {
    char key[8];
    snprintf(key, sizeof(key), "r%u", (unsigned)hdr.tail);
    if (prefs.putBytes(key, &rec, sizeof(rec)) != sizeof(rec)) return false;

    hdr.tail = (hdr.tail + 1) % hdr.capacity;
    if (hdr.count < hdr.capacity) {
        hdr.count++;
    } else {
        hdr.head = (hdr.head + 1) % hdr.capacity;
    }

    return prefs.putBytes("hdr", &hdr, sizeof(hdr)) == sizeof(hdr);
}

// Lee un LogRecord por índice lógico (0 = el más antiguo). Llamar bajo logMutex.
static bool readRecord(uint32_t logicalIdx, LogRecord& out) {
    if (logicalIdx >= hdr.count) return false;
    uint32_t physIdx = (hdr.head + logicalIdx) % hdr.capacity;
    char key[8];
    snprintf(key, sizeof(key), "r%u", (unsigned)physIdx);
    return prefs.getBytes(key, &out, sizeof(out)) == sizeof(out);
}

// =============================================================================
// API pública
// =============================================================================

void eventLogInit() {
    logMutex = xSemaphoreCreateMutex();

    if (!prefs.begin(NVS_NAMESPACE, false)) {
        Serial.println("[EVENTLOG] ERROR: no se pudo abrir NVS");
        return;
    }

    bool needReset = false;
    size_t hdrLen = prefs.getBytesLength("hdr");
    if (hdrLen != sizeof(LogHeader)) {
        needReset = true;
    } else {
        prefs.getBytes("hdr", &hdr, sizeof(hdr));
        if (hdr.magic != LOG_MAGIC || hdr.capacity != LOG_CAPACITY) {
            needReset = true;
        }
    }

    if (needReset) {
        Serial.println("[EVENTLOG] Inicializando NVS...");
        prefs.clear();
        hdr = { LOG_MAGIC, LOG_CAPACITY, 0, 0, 0 };
        prefs.putBytes("hdr", &hdr, sizeof(hdr));
        Serial.println("[EVENTLOG] NVS inicializado");
    } else {
        Serial.println("[EVENTLOG] NVS valido");
    }

    eventLogQueue = xQueueCreate(10, sizeof(LogEntry));
    Serial.printf("[EVENTLOG] Iniciado. Registros: %u / %u\n",
                  (unsigned)hdr.count, (unsigned)hdr.capacity);
}

void eventLogEnqueue(const LogEntry& e) {
    if (eventLogQueue == nullptr) return;
    xQueueSend(eventLogQueue, &e, 0);
}

uint32_t eventLogGetCount() {
    return hdr.count;
}

void eventLogStartStream() {
    streamIndex      = 0;
    streamHeaderSent = false;
    streamActive     = true;
}

void eventLogResetStream() {
    streamActive     = false;
    streamHeaderSent = false;
}

void eventLogClearLog() {
    if (xSemaphoreTake(logMutex, pdMS_TO_TICKS(500)) == pdTRUE) {
        prefs.clear();
        hdr.count = 0;
        hdr.head  = 0;
        hdr.tail  = 0;
        prefs.putBytes("hdr", &hdr, sizeof(hdr));
        xSemaphoreGive(logMutex);
    }
    Serial.println("[EVENTLOG] Log borrado");
}

// =============================================================================
// Tarea FreeRTOS: escritura en NVS + streaming CSV por BLE
// =============================================================================

void taskEventLog(void* param) {
    char csvBuf[128];

    for (;;) {
        // --- Fase 1: drenar la cola de eventos pendientes ---
        LogEntry entry;
        while (xQueueReceive(eventLogQueue, &entry, 0) == pdTRUE) {
            LogRecord rec;
            rec.timestamp = entry.timestamp;
            rec.prevState = static_cast<uint8_t>(entry.prevState);
            rec.newState  = static_cast<uint8_t>(entry.newState);
            rec.intValid  = entry.intSnap.valid ? 1u : 0u;
            rec.extValid  = entry.extSnap.valid ? 1u : 0u;
            rec.intTemp   = entry.intSnap.temperature;
            rec.intHum    = entry.intSnap.humidity;
            rec.extTemp   = entry.extSnap.temperature;
            rec.extHum    = entry.extSnap.humidity;

            if (xSemaphoreTake(logMutex, portMAX_DELAY) == pdTRUE) {
                writeRecord(rec);
                xSemaphoreGive(logMutex);
            }
            Serial.printf("[EVENTLOG] Registro guardado (%s -> %s), total=%u\n",
                          stateNameForLog(rec.prevState),
                          stateNameForLog(rec.newState),
                          (unsigned)hdr.count);
        }

        // --- Fase 2: streaming CSV si está activo ---
        if (streamActive && charLogData != nullptr) {
            if (!streamHeaderSent) {
                const char* sepLine = "sep=;\r\n";
                charLogData->setValue((uint8_t*)sepLine, strlen(sepLine));
                charLogData->notify();
                vTaskDelay(pdMS_TO_TICKS(20));

                const char* hdrLine =
                    "Fecha;Hora;Estado_Anterior;Estado_Nuevo;"
                    "Temp_Int;Hum_Int;Val_Int;"
                    "Temp_Ext;Hum_Ext;Val_Ext\r\n";
                charLogData->setValue((uint8_t*)hdrLine, strlen(hdrLine));
                charLogData->notify();
                streamHeaderSent = true;

            } else if (streamIndex < hdr.count) {
                LogRecord rec;
                bool ok = false;
                if (xSemaphoreTake(logMutex, pdMS_TO_TICKS(100)) == pdTRUE) {
                    ok = readRecord(streamIndex, rec);
                    xSemaphoreGive(logMutex);
                }
                if (ok) {
                    recordToCsvLine(rec, csvBuf, sizeof(csvBuf));
                    charLogData->setValue((uint8_t*)csvBuf, strlen(csvBuf));
                    charLogData->notify();
                    streamIndex++;
                } else {
                    streamActive = false;
                    Serial.println("[EVENTLOG] Error de lectura, abortando stream");
                }

            } else {
                const char* eof = "---EOF---\r\n";
                charLogData->setValue((uint8_t*)eof, strlen(eof));
                charLogData->notify();
                streamActive     = false;
                streamHeaderSent = false;
                Serial.println("[EVENTLOG] Descarga CSV completada");
            }
        }

        vTaskDelay(pdMS_TO_TICKS(20));
    }
}
