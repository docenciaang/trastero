// =============================================================================
// Trastero - Control de Humedad
// Placa: Wemos Lolin ESP32 (OLED SSD1306 integrado)
//
// Arquitectura de tareas FreeRTOS
// --------------------------------
//  Core 1 | taskSensors   prio 3  Lee DHT21 con intervalos adaptativos
//  Core 1 | taskControl   prio 2  Evalua estado y activa/desactiva reles
//  Core 0 | taskDisplay   prio 1  Refresca la pantalla OLED cada 500 ms
//  Core 0 | taskBLE       prio 2  Servidor GATT BLE, notifica cada 1 s
//  Core 0 | taskEventLog  prio 1  Drena cola de eventos, escribe en NVS, streaming CSV
// =============================================================================

#include <Arduino.h>
#include <WiFi.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <freertos/semphr.h>
#include <DHT.h>
#include <Wire.h>
#include <esp_system.h>
#include "SSD1306Wire.h"
#include "ble_gatt.h"
#include "event_log.h"
#include "ble_config.h"

// =============================================================================
// Pines
// =============================================================================
#define PIN_DHT_INTERIOR    13
#define PIN_DHT_EXTERIOR    14
#define PIN_RELAY_EXTRACTOR 26
#define PIN_RELAY_DEHUMID   25
#define PIN_BUTTON          12   // boton externo: GPIO12 — GND al pulsar (pull-up interno activo)

// ============================================================================
// Display I2C
// =============================================================================
#define I2C_SDA   5
#define I2C_SCL   4
#define OLED_ADDR 0x3C



// =============================================================================
// Temporización
// =============================================================================
//
// Intervalos de lectura de sensores (adaptativos por estado):
//
//   IDLE          10 min  /  20 s  Humedad OK y estable; solo vigila que no supere 70%
//   EXTRACTOR_ON   2 min  /  10 s  Dispositivo activo; detectar bajada a 65%
//   DEHUMID_ON     5 min  /  15 s  Peltier es mas lento; menos frecuencia suficiente
//   SAFE_OFF       30 s   /   5 s  Recuperacion rapida de fallo de sensor
//   CONFIRMACION   30 s   /   5 s  Lectura extra inmediata tras cualquier cambio de estado
//
// Cuando taskControl cambia de estado, notifica a taskSensors via
// xTaskNotify para que se despierte antes de que expire su intervalo largo
// y haga la lectura de confirmacion.
//

  #define INTERVAL_IDLE_MS          (10UL * 60 * 1000)   // 10 min
  #define INTERVAL_EXTRACTOR_MS      (2UL * 60 * 1000)   //  2 min
  #define INTERVAL_DEHUMID_MS        (5UL * 60 * 1000)   //  5 min
  #define INTERVAL_SAFE_OFF_MS              (30 * 1000)   // 30 s
  #define INTERVAL_CONFIRMATION_MS          (30 * 1000)   // 30 s tras cambio de estado


#define CONTROL_INTERVAL_MS      500
#define DISPLAY_INTERVAL_MS      500
#define DISPLAY_TIMEOUT_MS   (30 * 1000)   // 30 s encendido al inicio y al pulsar boton

// =============================================================================
// Umbrales de control (% RH)
// =============================================================================
#define HUMIDITY_ACTIVATE_THRESHOLD   70.0f
#define HUMIDITY_DEACTIVATE_THRESHOLD 65.0f
#define HUMIDITY_DELTA_EXTRACTOR      10.0f

// =============================================================================
// Seguridad
// =============================================================================
#define MAX_CONSECUTIVE_FAILURES 5

// =============================================================================
// Logica reles (activo LOW)
// =============================================================================
#define RELAY_ON  LOW
#define RELAY_OFF HIGH

// =============================================================================
// Datos compartidos entre tareas
// Protegidos por dataMutex: leer o escribir siempre bajo el mutex.
// Los tipos SensorData, ControlState y ManualOverride se definen en ble_gatt.h
// =============================================================================

SemaphoreHandle_t dataMutex;

SensorData    interior     = {0.0f, 0.0f, false, 0};
SensorData    exterior     = {0.0f, 0.0f, false, 0};
ControlState  currentState = ControlState::IDLE;
ManualOverride manualOverride = ManualOverride::NONE;

// Umbrales de humedad (valores iniciales en los #define; modificables por BLE)
float humActivate   = HUMIDITY_ACTIVATE_THRESHOLD;
float humDeactivate = HUMIDITY_DEACTIVATE_THRESHOLD;
float humDelta      = HUMIDITY_DELTA_EXTRACTOR;

// Intervalos de sensor en ms (valores iniciales en los #define; modificables por BLE)
uint32_t intervalIdleMs         = INTERVAL_IDLE_MS;
uint32_t intervalExtractorMs    = INTERVAL_EXTRACTOR_MS;
uint32_t intervalDehumidMs      = INTERVAL_DEHUMID_MS;
uint32_t intervalSafeOffMs      = INTERVAL_SAFE_OFF_MS;
uint32_t intervalConfirmationMs = INTERVAL_CONFIRMATION_MS;

// Planificacion diaria del extractor (modificable por BLE, persistida en NVS)
Schedule schedule = { 0, 0, {} };  // deshabilitada por defecto

// Motivo del reset detectado al arranque (leido antes de cualquier otra inicializacion)
esp_reset_reason_t bootResetReason = ESP_RST_UNKNOWN;

// Handles de tareas (utiles para notificaciones o suspension futura)
TaskHandle_t hTaskSensors = nullptr;
TaskHandle_t hTaskControl = nullptr;
TaskHandle_t hTaskDisplay = nullptr;

// Marca de tiempo hasta la que la pantalla debe permanecer encendida.
// Escrita desde ISR y leida desde taskDisplay; volatile es suficiente
// porque en ESP32 los accesos a uint32 alineados son atomicos.
volatile unsigned long displayOffAt     = 0;
volatile uint32_t      buttonPressCount = 0;  // incrementado en la ISR por cada pulsacion

// =============================================================================
// Objetos hardware (cada uno solo se usa desde una tarea, sin mutex propio)
// =============================================================================

SSD1306Wire display(OLED_ADDR, I2C_SDA, I2C_SCL);  // solo taskDisplay
DHT dhtInterior(PIN_DHT_INTERIOR, DHT21);           // solo taskSensors
DHT dhtExterior(PIN_DHT_EXTERIOR, DHT21);           // solo taskSensors

// =============================================================================
// ISR boton — rearma el temporizador de pantalla
// =============================================================================
void IRAM_ATTR isrButton() {
    buttonPressCount++;
    displayOffAt = millis() + DISPLAY_TIMEOUT_MS;
}

// =============================================================================
// Helpers (sin estado global, seguros para llamar desde cualquier tarea)
// =============================================================================

String formatValue(float val, bool valid) {
    if (!valid || isnan(val)) return String("--.-");
    char buf[8];
    snprintf(buf, sizeof(buf), "%.1f", val);
    return String(buf);
}

const char* stateLabel(ControlState state) {
    switch (state) {
        case ControlState::IDLE:         return "IDLE";
        case ControlState::EXTRACTOR_ON: return "EXTRACTOR";
        case ControlState::DEHUMID_ON:   return "DESHUMID";
        case ControlState::SAFE_OFF:     return "FALLO";
        default:                         return "???";
    }
}

// =============================================================================
// Logica de sensores
// =============================================================================

// Devuelve el intervalo de espera entre lecturas segun el estado actual.
static unsigned long sensorIntervalForState(ControlState state) {
    switch (state) {
        case ControlState::EXTRACTOR_ON: return intervalExtractorMs;
        case ControlState::DEHUMID_ON:   return intervalDehumidMs;
        case ControlState::SAFE_OFF:     return intervalSafeOffMs;
        default:                         return intervalIdleMs;
    }
}

// Lee un DHT y devuelve los valores en variables locales.
// No accede a datos compartidos — llamar fuera del mutex.
static bool readSensorRaw(DHT& dht, float& outTemp, float& outHum) {
    outHum  = dht.readHumidity();
    outTemp = dht.readTemperature();
    return (!isnan(outHum) && !isnan(outTemp));
}

// Actualiza un SensorData con el resultado de una lectura.
// Llamar bajo dataMutex.
static void applySensorResult(SensorData& data, float t, float h, bool ok) {
    if (ok) {
        data.temperature = t;
        data.humidity    = h;
        data.valid       = true;
        data.consecutiveFailures = 0;
    } else {
        data.consecutiveFailures++;
    }
}

// =============================================================================
// Logica de control
// Llamar bajo dataMutex (lee interior, exterior y currentState).
// =============================================================================

static bool isSafetyLockout() {
    return (interior.consecutiveFailures >= MAX_CONSECUTIVE_FAILURES ||
            exterior.consecutiveFailures >= MAX_CONSECUTIVE_FAILURES);
}

// Comprueba si la planificacion horaria permite al extractor funcionar ahora.
// Llamar bajo dataMutex. Devuelve true si no hay restriccion.
static bool isExtractorAllowed() {
    if (!schedule.enabled || schedule.count == 0) return true;

    time_t now = time(nullptr);
    if ((uint32_t)now < 1577836800UL) return true;  // reloj sin configurar

    struct tm t;
    localtime_r(&now, &t);
    int cur = t.tm_hour * 60 + t.tm_min;

    for (uint8_t i = 0; i < schedule.count && i < 4; i++) {
        int s = schedule.periods[i].startHour * 60 + schedule.periods[i].startMin;
        int e = schedule.periods[i].endHour   * 60 + schedule.periods[i].endMin;
        if (e > s) {                           // mismo dia
            if (cur >= s && cur < e) return true;
        } else {                               // periodo nocturno (cruza medianoche)
            if (cur >= s || cur < e) return true;
        }
    }
    return false;
}

static ControlState computeNextState(ControlState current) {
    if (isSafetyLockout()) return ControlState::SAFE_OFF;

    // Override manual desde BLE tiene prioridad sobre la logica automatica y la planificacion
    switch (manualOverride) {
        case ManualOverride::FORCE_EXTRACTOR: return ControlState::EXTRACTOR_ON;
        case ManualOverride::FORCE_DEHUMID:   return ControlState::DEHUMID_ON;
        case ManualOverride::FORCE_OFF:        return ControlState::IDLE;
        default: break;
    }

    if (!interior.valid) return current;

    if (interior.humidity > humActivate) {
        float delta = interior.humidity - exterior.humidity;
        bool canExtractor = exterior.valid && delta >= humDelta && isExtractorAllowed();
        return canExtractor ? ControlState::EXTRACTOR_ON : ControlState::DEHUMID_ON;
    }

    if (interior.humidity <= humDeactivate) return ControlState::IDLE;

    // Banda de histeresis: mantener estado salvo cambios de planificacion
    // EXTRACTOR_ON -> DEHUMID_ON si el periodo ya no permite el extractor
    if (current == ControlState::EXTRACTOR_ON && !isExtractorAllowed()) {
        return ControlState::DEHUMID_ON;
    }
    // DEHUMID_ON -> EXTRACTOR_ON si empieza un periodo que permite el extractor
    // y las condiciones exteriores lo favorecen
    if (current == ControlState::DEHUMID_ON && isExtractorAllowed() &&
        exterior.valid && (interior.humidity - exterior.humidity) >= humDelta) {
        return ControlState::EXTRACTOR_ON;
    }
    return current;
}

static void applyState(ControlState state) {
    switch (state) {
        case ControlState::EXTRACTOR_ON:
            digitalWrite(PIN_RELAY_EXTRACTOR, RELAY_ON);
            digitalWrite(PIN_RELAY_DEHUMID,   RELAY_OFF);
            break;
        case ControlState::DEHUMID_ON:
            digitalWrite(PIN_RELAY_EXTRACTOR, RELAY_OFF);
            digitalWrite(PIN_RELAY_DEHUMID,   RELAY_ON);
            break;
        default:
            digitalWrite(PIN_RELAY_EXTRACTOR, RELAY_OFF);
            digitalWrite(PIN_RELAY_DEHUMID,   RELAY_OFF);
            break;
    }
}

// =============================================================================
// Display
// =============================================================================
//
//   x=0     x=70(R)  x=72  x=118(R) x=120
//   INT     23.4     C     67.2     %
//   EXT     18.1     C     52.0     %
//   ──────────────────────────────────  y=26
//   EXTR:ON   DEHU:OFF                  y=30
//   ESTADO: EXTRACTOR                   y=44
//                                [BT]   y=56  (icono 8x8, esquina inferior derecha x=120)

// Icono Bluetooth 8x8 px en formato XBM (bit0 = columna izquierda).
// Representa el logotipo BT: barra vertical + dos horquillas diagonales.
static const uint8_t BT_ICON_8x8[] PROGMEM = {
    0x08,  // . . . X . . . .
    0x18,  // . . . X X . . .
    0x24,  // . . X . . X . .
    0x18,  // . . . X X . . .
    0x18,  // . . . X X . . .
    0x24,  // . . X . . X . .
    0x18,  // . . . X X . . .
    0x08,  // . . . X . . . .
};

static const int COL_LABEL  =   0;
static const int COL_TEMP_R =  70;
static const int COL_TEMP_U =  72;
static const int COL_HUM_R  = 118;
static const int COL_HUM_U  = 120;

// =============================================================================
// =============================================================================

static void drawSensorRow(int y, const char* label, const SensorData& data) {
    display.setTextAlignment(TEXT_ALIGN_LEFT);
    display.drawString(COL_LABEL, y, label);

    display.setTextAlignment(TEXT_ALIGN_RIGHT);
    display.drawString(COL_TEMP_R, y, formatValue(data.temperature, data.valid));
    display.setTextAlignment(TEXT_ALIGN_LEFT);
    display.drawString(COL_TEMP_U, y, "C");

    display.setTextAlignment(TEXT_ALIGN_RIGHT);
    display.drawString(COL_HUM_R, y, formatValue(data.humidity, data.valid));
    display.setTextAlignment(TEXT_ALIGN_LEFT);
    display.drawString(COL_HUM_U, y, "%");
}

// =============================================================================
// Renderiza la pantalla a partir de copias locales — llamar fuera del mutex.
// =============================================================================

static void renderDisplay(const SensorData& intSnap, const SensorData& extSnap,
                           ControlState stateSnap, bool bleOn) {
    bool extrOn = (stateSnap == ControlState::EXTRACTOR_ON);
    bool dehuOn = (stateSnap == ControlState::DEHUMID_ON);

    display.clear();
    display.setFont(ArialMT_Plain_10);

    drawSensorRow(0,  "INT", intSnap);
    drawSensorRow(13, "EXT", extSnap);

    display.drawHorizontalLine(0, 26, 128);

    display.setTextAlignment(TEXT_ALIGN_LEFT);
    display.drawString(0, 30,
        String("EXTR:") + (extrOn ? "ON " : "OFF") +
        "  DEHU:" + (dehuOn ? "ON " : "OFF"));

    display.drawString(0, 44, String("ESTADO: ") + stateLabel(stateSnap));

    if (bleOn) {
        display.drawXbm(120, 56, 8, 8, BT_ICON_8x8);
    }

    display.display();
}

// =============================================================================
// Tareas FreeRTOS
// =============================================================================

// =============================================================================
// taskSensors (Core 1, prio 3)
// Lee los dos DHT22 fuera del mutex (operacion lenta ~500 ms) y luego
// escribe los resultados en los structs compartidos bajo el mutex.
// =============================================================================
void taskSensors(void*) {
    for (;;) {
        float ti, hi, te, he;
        bool okInt = readSensorRaw(dhtInterior, ti, hi);
        bool okExt = readSensorRaw(dhtExterior, te, he);

        ControlState snap;
        if (xSemaphoreTake(dataMutex, portMAX_DELAY) == pdTRUE) {
            applySensorResult(interior, ti, hi, okInt);
            applySensorResult(exterior, te, he, okExt);
            snap = currentState;
            xSemaphoreGive(dataMutex);
        }

        if (okInt) Serial.printf("[INT] T=%.1f C  H=%.1f%% | ", ti, hi);
        if (okExt) Serial.printf("[EXT] T=%.1f C  H=%.1f%%\n", te, he);

        // Duerme el intervalo adaptativo segun el estado actual.
        // taskControl puede interrumpir este sleep via xTaskNotify
        // cuando cambia el estado; en ese caso se espera
        // INTERVAL_CONFIRMATION_MS antes de la lectura de confirmacion.
        uint32_t notifVal;
        BaseType_t notified = xTaskNotifyWait(0, 0, &notifVal,
                                  pdMS_TO_TICKS(sensorIntervalForState(snap)));
        if (notified == pdTRUE) {
            vTaskDelay(pdMS_TO_TICKS(intervalConfirmationMs));
        }
    }
}

// =============================================================================
// taskControl (Core 1, prio 2)
// Evalua el estado bajo el mutex, aplica los reles si hay cambio.
// La escritura en GPIO no necesita mutex porque solo la hace esta tarea.
// =============================================================================
void taskControl(void*) {
    for (;;) {
        ControlState next;
        ControlState prev;
        bool stateChanged = false;
        LogEntry logEntry = {};

        if (xSemaphoreTake(dataMutex, portMAX_DELAY) == pdTRUE) {
            prev = currentState;
            next = computeNextState(currentState);
            if (next != currentState) {
                Serial.printf("[CONTROL] %s -> %s\n",
                              stateLabel(currentState), stateLabel(next));
                // Capturar snapshot para el log mientras se tiene el mutex
                logEntry.timestamp = (uint32_t)time(nullptr);
                logEntry.prevState = prev;
                logEntry.newState  = next;
                logEntry.intSnap   = interior;
                logEntry.extSnap   = exterior;
                currentState = next;
                stateChanged = true;
/**
                Serial.printf("[DEBUG] Registro logEntry creado: timestamp=%u, prevState=%s, newState=%s, intValid=%u, extValid=%u\n",
                              logEntry.timestamp,
                              stateLabel(logEntry.prevState),
                              stateLabel(logEntry.newState),
                              logEntry.intSnap.valid ? 1 : 0,
                              logEntry.extSnap.valid ? 1 : 0);
                              */
            }
            xSemaphoreGive(dataMutex);
        }

        applyState(next);  // GPIO fuera del mutex

        // Despertar taskSensors para lectura de confirmacion a los 30 s
        if (stateChanged && hTaskSensors != nullptr) {
            xTaskNotify(hTaskSensors, 0, eNoAction);
        }

        // Encolar el evento de log FUERA del mutex, no bloquea
        if (stateChanged) {
            eventLogEnqueue(logEntry);
        }

        vTaskDelay(pdMS_TO_TICKS(CONTROL_INTERVAL_MS));
    }
}

// =============================================================================
// Pantalla 1 — ultima activacion del extractor
// =============================================================================

static void renderLastExtractor(const LogRecord& rec, bool valid) {
    display.clear();
    display.setFont(ArialMT_Plain_10);
    display.setTextAlignment(TEXT_ALIGN_LEFT);
    display.drawString(0, 0, "Ult. Extractor");
    display.drawHorizontalLine(0, 12, 128);

    if (!valid) {
        display.setTextAlignment(TEXT_ALIGN_CENTER);
        display.drawString(64, 28, "Sin activaciones");
    } else {
        if (rec.timestamp > 1577836800UL) {
            struct tm t;
            time_t ts = (time_t)rec.timestamp;
            gmtime_r(&ts, &t);
            char dtBuf[20];
            snprintf(dtBuf, sizeof(dtBuf), "%04d-%02d-%02d %02d:%02d",
                     t.tm_year + 1900, t.tm_mon + 1, t.tm_mday,
                     t.tm_hour, t.tm_min);
            display.drawString(0, 14, dtBuf);
        } else {
            display.drawString(0, 14, "Sin fecha");
        }
        char buf[22];
        if (rec.intValid) snprintf(buf, sizeof(buf), "INT %.1fC  %.1f%%", rec.intTemp, rec.intHum);
        else              strcpy(buf, "INT --");
        display.drawString(0, 28, buf);

        if (rec.extValid) snprintf(buf, sizeof(buf), "EXT %.1fC  %.1f%%", rec.extTemp, rec.extHum);
        else              strcpy(buf, "EXT --");
        display.drawString(0, 42, buf);
    }
    display.display();
}

// =============================================================================
// Pantalla 2 — informacion de resets del sistema
// =============================================================================

static void renderResetInfo() {
    display.clear();
    display.setFont(ArialMT_Plain_10);
    display.setTextAlignment(TEXT_ALIGN_LEFT);
    display.drawString(0, 0, "Estado Sistema");
    display.drawHorizontalLine(0, 12, 128);

    char buf[24];
    snprintf(buf, sizeof(buf), "Resets: %lu", (unsigned long)resetInfoGetCount());
    display.drawString(0, 16, buf);
    snprintf(buf, sizeof(buf), "Ultimo: %s", resetInfoReasonStr(resetInfoGetReason()));
    display.drawString(0, 32, buf);
    display.display();
}

// =============================================================================
// taskDisplay (Core 0, prio 1)
// Copia los datos compartidos bajo el mutex y renderiza fuera de el
// para no bloquear las tareas criticas durante el I2C del display.
// =============================================================================
void taskDisplay(void*) {
    bool      screenOn        = true;
    uint8_t   displayScreen   = 0;      // 0=principal  1=ult.extractor  2=resets
    uint32_t  lastBtnCount    = 0;
    LogRecord lastExtRec      = {};
    bool      lastExtValid    = false;

    for (;;) {
        bool shouldBeOn = (millis() < displayOffAt);

        // --- Deteccion de pulsaciones del boton ---
        uint32_t curBtnCount = buttonPressCount;
        if (curBtnCount != lastBtnCount) {
            uint32_t presses = curBtnCount - lastBtnCount;
            lastBtnCount = curBtnCount;
            if (screenOn) {
                displayScreen = (displayScreen + presses) % 3;
                if (displayScreen == 1) {
                    // Refrescar datos al navegar a pantalla de extractor
                    lastExtValid = eventLogGetLastExtractor(lastExtRec);
                }
            } else {
                displayScreen = 0;   // pantalla apagada: encender siempre en principal
            }
        }

        // --- Gestion de encendido / apagado ---
        if (shouldBeOn != screenOn) {
            screenOn = shouldBeOn;
            if (!screenOn) {
                displayScreen = 0;   // al apagarse siempre volver a principal
                display.clear();
                display.display();
                display.displayOff();
            } else {
                display.displayOn();
            }
        }

        // --- Renderizado ---
        if (screenOn) {
            switch (displayScreen) {
                case 0: {
                    SensorData   intSnap, extSnap;
                    ControlState stateSnap;
                    if (xSemaphoreTake(dataMutex, portMAX_DELAY) == pdTRUE) {
                        intSnap   = interior;
                        extSnap   = exterior;
                        stateSnap = currentState;
                        xSemaphoreGive(dataMutex);
                    }
                    renderDisplay(intSnap, extSnap, stateSnap, bleIsConnected());
                    break;
                }
                case 1:
                    renderLastExtractor(lastExtRec, lastExtValid);
                    break;
                case 2:
                    renderResetInfo();
                    break;
            }
        }

        vTaskDelay(pdMS_TO_TICKS(DISPLAY_INTERVAL_MS));
    }
}

// =============================================================================
// Setup & Loop
// =============================================================================

void setup() {
    Serial.begin(115200);
    bootResetReason = esp_reset_reason();   // leer antes de cualquier otra inicializacion
    Serial.println("\n[TRASTERO] Iniciando...");

    // WiFi apagado; BLE se inicializa mas adelante
    WiFi.mode(WIFI_OFF);

    // Reles apagados antes de cualquier otra inicializacion
    pinMode(PIN_RELAY_EXTRACTOR, OUTPUT);
    pinMode(PIN_RELAY_DEHUMID,   OUTPUT);
    digitalWrite(PIN_RELAY_EXTRACTOR, RELAY_OFF);
    digitalWrite(PIN_RELAY_DEHUMID,   RELAY_OFF);

    // Display (splash screen)
    display.init();
    display.setFont(ArialMT_Plain_10);
    display.setTextAlignment(TEXT_ALIGN_CENTER);
    display.clear();
    display.drawString(64, 25, "Trastero v1.0");
    display.display();

    // Boton de pantalla
    pinMode(PIN_BUTTON, INPUT_PULLUP);
    attachInterrupt(PIN_BUTTON, isrButton, FALLING);

    // Sensores
    dhtInterior.begin();
    dhtExterior.begin();

    delay(1500);
    display.clear();
    display.display();

    // Pantalla encendida 30 s desde el arranque
    displayOffAt = millis() + DISPLAY_TIMEOUT_MS;

    // Mutex de datos compartidos
    dataMutex = xSemaphoreCreateMutex();

    // Cargar configuracion persistida (umbrales, intervalos, schedule) desde NVS
    bleConfigLoad();

    // Registrar motivo de reset en NVS y mostrarlo por Serial
    resetInfoInit();

    // Inicializar sistema de log de eventos en flash
    eventLogInit();

    // Inicializar BLE
    bleInit();

    // Crear tareas
    xTaskCreatePinnedToCore(taskSensors,  "Sensors",  4096, nullptr, 3, &hTaskSensors, 1);
    xTaskCreatePinnedToCore(taskControl,  "Control",  2048, nullptr, 2, &hTaskControl, 1);
    xTaskCreatePinnedToCore(taskDisplay,  "Display",  4096, nullptr, 1, &hTaskDisplay, 0);
    xTaskCreatePinnedToCore(taskBLE,      "BLE",      8192, nullptr, 2, nullptr,        0);
    xTaskCreatePinnedToCore(taskEventLog, "EventLog", 4096, nullptr, 1, nullptr,        0);

    Serial.println("[TRASTERO] Tareas iniciadas.");
}

void loop() {
    // El trabajo lo hacen las tareas FreeRTOS.
    // loop() queda libre para depuracion o futuros comandos por serie.
    vTaskDelay(portMAX_DELAY);
}
