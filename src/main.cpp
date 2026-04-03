// =============================================================================
// Trastero - Control de Humedad
// Placa: Wemos Lolin ESP32 (OLED SSD1306 integrado)
//
// Arquitectura de tareas FreeRTOS
// --------------------------------
//  Core 1 | taskSensors  prio 3  Lee DHT22 cada 2 s y actualiza datos compartidos
//  Core 1 | taskControl  prio 2  Evalua estado y activa/desactiva reles
//  Core 0 | taskDisplay  prio 1  Refresca la pantalla OLED cada 500 ms
//
//  Para añadir WiFi o BT en el futuro:
//    xTaskCreatePinnedToCore(taskComms, "Comms", 8192, nullptr, 2, nullptr, 0);
//  La tarea de comunicacion puede leer los datos compartidos tomando dataMutex.
// =============================================================================

#include <Arduino.h>
#include <WiFi.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <freertos/semphr.h>
#include <DHT.h>
#include <Wire.h>
#include "SSD1306Wire.h"

// =============================================================================
// Pines
// =============================================================================
#define PIN_DHT_INTERIOR    13
#define PIN_DHT_EXTERIOR    14
#define PIN_RELAY_EXTRACTOR 26
#define PIN_RELAY_DEHUMID   27

// =============================================================================
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
//   IDLE          10 min  Humedad OK y estable; solo vigila que no supere 70%
//   EXTRACTOR_ON   2 min  Dispositivo activo; detectar bajada a 65%
//   DEHUMID_ON     5 min  Peltier es mas lento; menos frecuencia suficiente
//   SAFE_OFF       30 s   Recuperacion rapida de fallo de sensor
//   CONFIRMACION   30 s   Lectura extra inmediata tras cualquier cambio de estado
//
// Cuando taskControl cambia de estado, notifica a taskSensors via
// xTaskNotify para que se despierte antes de que expire su intervalo largo
// y haga la lectura de confirmacion a los 30 s.
//
#define INTERVAL_IDLE_MS          (10UL * 60 * 1000)   // 10 min
#define INTERVAL_EXTRACTOR_MS      (2UL * 60 * 1000)   //  2 min
#define INTERVAL_DEHUMID_MS        (5UL * 60 * 1000)   //  5 min
#define INTERVAL_SAFE_OFF_MS              (30 * 1000)   // 30 s
#define INTERVAL_CONFIRMATION_MS          (30 * 1000)   // 30 s tras cambio de estado

#define CONTROL_INTERVAL_MS      500
#define DISPLAY_INTERVAL_MS      500

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
// Estructuras y tipos
// =============================================================================

struct SensorData {
    float temperature;
    float humidity;
    bool  valid;
    int   consecutiveFailures;
};

enum class ControlState {
    IDLE,
    EXTRACTOR_ON,
    DEHUMID_ON,
    SAFE_OFF
};

// =============================================================================
// Datos compartidos entre tareas
// Protegidos por dataMutex: leer o escribir siempre bajo el mutex.
// =============================================================================

SemaphoreHandle_t dataMutex;

SensorData    interior     = {0.0f, 0.0f, false, 0};
SensorData    exterior     = {0.0f, 0.0f, false, 0};
ControlState  currentState = ControlState::IDLE;

// Handles de tareas (utiles para notificaciones o suspension futura)
TaskHandle_t hTaskSensors = nullptr;
TaskHandle_t hTaskControl = nullptr;
TaskHandle_t hTaskDisplay = nullptr;

// =============================================================================
// Objetos hardware (cada uno solo se usa desde una tarea, sin mutex propio)
// =============================================================================

SSD1306Wire display(OLED_ADDR, I2C_SDA, I2C_SCL);  // solo taskDisplay
DHT dhtInterior(PIN_DHT_INTERIOR, DHT22);           // solo taskSensors
DHT dhtExterior(PIN_DHT_EXTERIOR, DHT22);           // solo taskSensors

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
        case ControlState::EXTRACTOR_ON: return INTERVAL_EXTRACTOR_MS;
        case ControlState::DEHUMID_ON:   return INTERVAL_DEHUMID_MS;
        case ControlState::SAFE_OFF:     return INTERVAL_SAFE_OFF_MS;
        default:                         return INTERVAL_IDLE_MS;
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

static ControlState computeNextState(ControlState current) {
    if (isSafetyLockout())  return ControlState::SAFE_OFF;
    if (!interior.valid)    return current;

    if (interior.humidity > HUMIDITY_ACTIVATE_THRESHOLD) {
        float delta = interior.humidity - exterior.humidity;
        return (exterior.valid && delta >= HUMIDITY_DELTA_EXTRACTOR)
               ? ControlState::EXTRACTOR_ON
               : ControlState::DEHUMID_ON;
    }

    if (interior.humidity <= HUMIDITY_DEACTIVATE_THRESHOLD) return ControlState::IDLE;

    return current;  // banda de histeresis: mantener estado
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
                           ControlState stateSnap) {
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

        if (okInt) Serial.printf("[INT] T=%.1f C  H=%.1f%%\n", ti, hi);
        if (okExt) Serial.printf("[EXT] T=%.1f C  H=%.1f%%\n", te, he);

        // Duerme el intervalo adaptativo segun el estado actual.
        // taskControl puede interrumpir este sleep via xTaskNotify
        // cuando cambia el estado; en ese caso se espera
        // INTERVAL_CONFIRMATION_MS antes de la lectura de confirmacion.
        uint32_t notifVal;
        BaseType_t notified = xTaskNotifyWait(0, 0, &notifVal,
                                  pdMS_TO_TICKS(sensorIntervalForState(snap)));
        if (notified == pdTRUE) {
            vTaskDelay(pdMS_TO_TICKS(INTERVAL_CONFIRMATION_MS));
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

        bool stateChanged = false;

        if (xSemaphoreTake(dataMutex, portMAX_DELAY) == pdTRUE) {
            next = computeNextState(currentState);
            if (next != currentState) {
                Serial.printf("[CONTROL] %s -> %s\n",
                              stateLabel(currentState), stateLabel(next));
                currentState = next;
                stateChanged = true;
            }
            xSemaphoreGive(dataMutex);
        }

        applyState(next);  // GPIO fuera del mutex

        // Despertar taskSensors para lectura de confirmacion a los 30 s
        if (stateChanged && hTaskSensors != nullptr) {
            xTaskNotify(hTaskSensors, 0, eNoAction);
        }

        vTaskDelay(pdMS_TO_TICKS(CONTROL_INTERVAL_MS));
    }
}

// =============================================================================
// taskDisplay (Core 0, prio 1)
// Copia los datos compartidos bajo el mutex y renderiza fuera de el
// para no bloquear las tareas criticas durante el I2C del display.
// =============================================================================
void taskDisplay(void*) {
    for (;;) {
        SensorData   intSnap, extSnap;
        ControlState stateSnap;

        if (xSemaphoreTake(dataMutex, portMAX_DELAY) == pdTRUE) {
            intSnap   = interior;
            extSnap   = exterior;
            stateSnap = currentState;
            xSemaphoreGive(dataMutex);
        }

        renderDisplay(intSnap, extSnap, stateSnap);

        vTaskDelay(pdMS_TO_TICKS(DISPLAY_INTERVAL_MS));
    }
}

// =============================================================================
// Setup & Loop
// =============================================================================

void setup() {
    Serial.begin(115200);
    Serial.println("\n[TRASTERO] Iniciando...");

    // Radio apagada (reactivar cuando se añada WiFi/BT)
    WiFi.mode(WIFI_OFF);
    btStop();

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

    // Sensores
    dhtInterior.begin();
    dhtExterior.begin();

    delay(1500);
    display.clear();
    display.display();

    // Mutex de datos compartidos
    dataMutex = xSemaphoreCreateMutex();

    // Crear tareas
    xTaskCreatePinnedToCore(taskSensors, "Sensors", 4096, nullptr, 3, &hTaskSensors, 1);
    xTaskCreatePinnedToCore(taskControl, "Control", 2048, nullptr, 2, &hTaskControl, 1);
    xTaskCreatePinnedToCore(taskDisplay, "Display", 4096, nullptr, 1, &hTaskDisplay, 0);

    Serial.println("[TRASTERO] Tareas iniciadas.");
}

void loop() {
    // El trabajo lo hacen las tareas FreeRTOS.
    // loop() queda libre para depuracion o futuros comandos por serie.
    vTaskDelay(portMAX_DELAY);
}
