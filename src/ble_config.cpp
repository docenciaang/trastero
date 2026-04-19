// =============================================================================
// ble_config.cpp — Servicio BLE de configuracion del sistema
//
// Expone dos characteristics sobre el servicio 1b5ab4f0-0002-...:
//   UMBRALES   lectura/escritura de umbrales de humedad (3 floats, 12 B)
//   INTERVALOS lectura/escritura de intervalos de sensor (5 uint32, 20 B, en segundos)
// =============================================================================

#include "ble_config.h"
#include "ble_gatt.h"   // SensorData, ControlState, ManualOverride

#include <BLEDevice.h>
#include <BLEUtils.h>
#include <Preferences.h>
#include <esp_system.h>

#define CONFIG_NVS_NAMESPACE   "config"
#define RESETLOG_NVS_NAMESPACE "resetlog"

// =============================================================================
// UUIDs del servicio de configuracion
// =============================================================================

#define CONFIG_SVC_UUID       "1b5ab4f0-0002-1000-8000-000000000000"
#define CHAR_UMBRALES_UUID    "1b5ab4f0-0002-1000-8000-000000000001"
#define CHAR_INTERVALOS_UUID  "1b5ab4f0-0002-1000-8000-000000000002"
#define CHAR_SCHEDULE_UUID    "1b5ab4f0-0002-1000-8000-000000000003"
#define CHAR_RESET_INFO_UUID  "1b5ab4f0-0002-1000-8000-000000000004"

// =============================================================================
// Datos compartidos con main.cpp
// =============================================================================

extern SemaphoreHandle_t dataMutex;
extern TaskHandle_t      hTaskSensors;

extern float    humActivate;
extern float    humDeactivate;
extern float    humDelta;

extern uint32_t intervalIdleMs;
extern uint32_t intervalExtractorMs;
extern uint32_t intervalDehumidMs;
extern uint32_t intervalSafeOffMs;
extern uint32_t intervalConfirmationMs;

extern Schedule            schedule;
extern esp_reset_reason_t  bootResetReason;

// =============================================================================
// Payloads packed
// =============================================================================

struct __attribute__((packed)) UmbralesPayload {
    float activar;
    float desactivar;
    float delta;
};

struct __attribute__((packed)) IntervalosPayload {
    uint32_t idle_s;
    uint32_t extractor_s;
    uint32_t dehumid_s;
    uint32_t safeoff_s;
    uint32_t confirmation_s;
};

// =============================================================================
// Callback UMBRALES (READ + WRITE)
// =============================================================================

class UmbralesCallback : public BLECharacteristicCallbacks {
    void onRead(BLECharacteristic* c) override {
        UmbralesPayload buf;
        if (xSemaphoreTake(dataMutex, pdMS_TO_TICKS(100)) == pdTRUE) {
            buf.activar    = humActivate;
            buf.desactivar = humDeactivate;
            buf.delta      = humDelta;
            xSemaphoreGive(dataMutex);
        }
        c->setValue((uint8_t*)&buf, sizeof(buf));
    }

    void onWrite(BLECharacteristic* c) override {
        if (c->getLength() < sizeof(UmbralesPayload)) return;
        UmbralesPayload buf;
        memcpy(&buf, c->getData(), sizeof(buf));
        if (buf.activar <= buf.desactivar || buf.delta <= 0.0f) {
            Serial.println("[BLE] UMBRALES rechazados (valores invalidos)");
            return;
        }
        if (xSemaphoreTake(dataMutex, pdMS_TO_TICKS(100)) == pdTRUE) {
            humActivate   = buf.activar;
            humDeactivate = buf.desactivar;
            humDelta      = buf.delta;
            xSemaphoreGive(dataMutex);
        }
        Serial.printf("[BLE] UMBRALES: activar=%.1f desactivar=%.1f delta=%.1f\n",
                      buf.activar, buf.desactivar, buf.delta);

        Preferences p;
        if (p.begin(CONFIG_NVS_NAMESPACE, false)) {
            p.putFloat("hum_act", humActivate);
            p.putFloat("hum_des", humDeactivate);
            p.putFloat("hum_dlt", humDelta);
            p.end();
        }
    }
};

// =============================================================================
// Callback INTERVALOS (READ + WRITE)
// Unidad en el payload: segundos (uint32 LE).
// Internamente los intervalos se almacenan en milisegundos.
// =============================================================================

class IntervalosCallback : public BLECharacteristicCallbacks {
    void onRead(BLECharacteristic* c) override {
        IntervalosPayload buf;
        if (xSemaphoreTake(dataMutex, pdMS_TO_TICKS(100)) == pdTRUE) {
            buf.idle_s         = intervalIdleMs         / 1000u;
            buf.extractor_s    = intervalExtractorMs    / 1000u;
            buf.dehumid_s      = intervalDehumidMs      / 1000u;
            buf.safeoff_s      = intervalSafeOffMs      / 1000u;
            buf.confirmation_s = intervalConfirmationMs / 1000u;
            xSemaphoreGive(dataMutex);
        }
        c->setValue((uint8_t*)&buf, sizeof(buf));
    }

    void onWrite(BLECharacteristic* c) override {
        if (c->getLength() < sizeof(IntervalosPayload)) return;
        IntervalosPayload buf;
        memcpy(&buf, c->getData(), sizeof(buf));
        // Sanidad minima: todos > 0 y safe_off >= 5 s
        if (buf.idle_s == 0 || buf.extractor_s == 0 ||
            buf.dehumid_s == 0 || buf.safeoff_s < 5 ||
            buf.confirmation_s == 0) {
            Serial.println("[BLE] INTERVALOS rechazados (valores invalidos)");
            return;
        }
        if (xSemaphoreTake(dataMutex, pdMS_TO_TICKS(100)) == pdTRUE) {
            intervalIdleMs         = buf.idle_s         * 1000u;
            intervalExtractorMs    = buf.extractor_s    * 1000u;
            intervalDehumidMs      = buf.dehumid_s      * 1000u;
            intervalSafeOffMs      = buf.safeoff_s      * 1000u;
            intervalConfirmationMs = buf.confirmation_s * 1000u;
            xSemaphoreGive(dataMutex);
        }
        Serial.printf("[BLE] INTERVALOS (s): idle=%u ext=%u deh=%u safe=%u conf=%u\n",
                      buf.idle_s, buf.extractor_s, buf.dehumid_s,
                      buf.safeoff_s, buf.confirmation_s);

        Preferences p;
        if (p.begin(CONFIG_NVS_NAMESPACE, false)) {
            p.putUInt("iv_idle", intervalIdleMs);
            p.putUInt("iv_ext",  intervalExtractorMs);
            p.putUInt("iv_deh",  intervalDehumidMs);
            p.putUInt("iv_safe", intervalSafeOffMs);
            p.putUInt("iv_conf", intervalConfirmationMs);
            p.end();
        }
    }
};

// =============================================================================
// Callback SCHEDULE (READ + WRITE)
// Payload: struct Schedule packed (18 B)
// =============================================================================

class ScheduleCallback : public BLECharacteristicCallbacks {
    void onRead(BLECharacteristic* c) override {
        Schedule buf;
        if (xSemaphoreTake(dataMutex, pdMS_TO_TICKS(100)) == pdTRUE) {
            buf = schedule;
            xSemaphoreGive(dataMutex);
        }
        c->setValue((uint8_t*)&buf, sizeof(buf));
    }

    void onWrite(BLECharacteristic* c) override {
        if (c->getLength() < sizeof(Schedule)) return;
        Schedule buf;
        memcpy(&buf, c->getData(), sizeof(buf));

        // Validacion
        if (buf.count > 4) {
            Serial.println("[BLE] SCHEDULE rechazado (count > 4)");
            return;
        }
        for (uint8_t i = 0; i < buf.count; i++) {
            if (buf.periods[i].startHour > 23 || buf.periods[i].startMin > 59 ||
                buf.periods[i].endHour   > 23 || buf.periods[i].endMin   > 59) {
                Serial.println("[BLE] SCHEDULE rechazado (periodo invalido)");
                return;
            }
        }

        if (xSemaphoreTake(dataMutex, pdMS_TO_TICKS(100)) == pdTRUE) {
            schedule = buf;
            xSemaphoreGive(dataMutex);
        }
        Serial.printf("[BLE] SCHEDULE: enabled=%u count=%u\n", buf.enabled, buf.count);
        for (uint8_t i = 0; i < buf.count; i++) {
            Serial.printf("  [%u] %02u:%02u - %02u:%02u\n", i,
                          buf.periods[i].startHour, buf.periods[i].startMin,
                          buf.periods[i].endHour,   buf.periods[i].endMin);
        }

        Preferences p;
        if (p.begin(CONFIG_NVS_NAMESPACE, false)) {
            p.putBytes("sched", &schedule, sizeof(schedule));
            p.end();
        }
    }
};

// =============================================================================
// bleConfigLoad — leer valores persistidos desde NVS al arranque
// =============================================================================

void bleConfigLoad() {
    Preferences p;
    if (!p.begin(CONFIG_NVS_NAMESPACE, true)) {
        Serial.println("[CONFIG] NVS no disponible, usando valores por defecto");
        return;
    }

    humActivate            = p.getFloat("hum_act", humActivate);
    humDeactivate          = p.getFloat("hum_des", humDeactivate);
    humDelta               = p.getFloat("hum_dlt", humDelta);
    intervalIdleMs         = p.getUInt ("iv_idle", intervalIdleMs);
    intervalExtractorMs    = p.getUInt ("iv_ext",  intervalExtractorMs);
    intervalDehumidMs      = p.getUInt ("iv_deh",  intervalDehumidMs);
    intervalSafeOffMs      = p.getUInt ("iv_safe", intervalSafeOffMs);
    intervalConfirmationMs = p.getUInt ("iv_conf", intervalConfirmationMs);

    uint8_t schedBuf[sizeof(Schedule)] = {};
    if (p.getBytes("sched", schedBuf, sizeof(schedBuf)) == sizeof(Schedule)) {
        memcpy(&schedule, schedBuf, sizeof(Schedule));
    }

    p.end();
    Serial.printf("[CONFIG] Umbrales: act=%.1f des=%.1f dlt=%.1f\n",
                  humActivate, humDeactivate, humDelta);
    Serial.printf("[CONFIG] Intervalos (ms): idle=%u ext=%u deh=%u safe=%u conf=%u\n",
                  intervalIdleMs, intervalExtractorMs, intervalDehumidMs,
                  intervalSafeOffMs, intervalConfirmationMs);
    Serial.printf("[CONFIG] Schedule: enabled=%u count=%u\n",
                  schedule.enabled, schedule.count);
}

// =============================================================================
// Registro de resets — variables, helper, callback y funcion de inicializacion
// =============================================================================

static uint32_t resetCount      = 0;
static uint8_t  lastResetReason = 0;

static const char* resetReasonStr(uint8_t r) {
    switch (r) {
        case 1:  return "POWERON";
        case 2:  return "EXT_PIN";
        case 3:  return "SOFTWARE";
        case 4:  return "PANIC";
        case 5:  return "INT_WDT";
        case 6:  return "TASK_WDT";
        case 7:  return "WDT";
        case 8:  return "DEEPSLEEP";
        case 9:  return "BROWNOUT";
        case 10: return "SDIO";
        default: return "UNKNOWN";
    }
}

class ResetInfoCallback : public BLECharacteristicCallbacks {
    void onRead(BLECharacteristic* c) override {
        uint8_t buf[5];
        memcpy(buf, &resetCount, 4);
        buf[4] = lastResetReason;
        c->setValue(buf, sizeof(buf));
    }
};

void resetInfoInit() {
    Preferences p;
    if (p.begin(RESETLOG_NVS_NAMESPACE, false)) {
        resetCount      = p.getUInt ("rst_cnt", 0);
        lastResetReason = p.getUChar("rst_rsn", 0);

        if (bootResetReason != ESP_RST_POWERON) {
            resetCount++;
            lastResetReason = (uint8_t)bootResetReason;
            p.putUInt ("rst_cnt", resetCount);
            p.putUChar("rst_rsn", lastResetReason);
        }
        p.end();
    }

    Serial.printf("[RESET] Arranque: %-10s  Resets acumulados: %u  Ultimo: %s\n",
                  resetReasonStr((uint8_t)bootResetReason),
                  resetCount,
                  resetReasonStr(lastResetReason));
}

// =============================================================================
// Inicializacion del servicio de configuracion
// =============================================================================

void bleConfigServiceInit(BLEServer* server) {
    // 1 servicio + 4 chars × 2 handles = 9 minimo; 20 con margen
    BLEService* svc = server->createService(BLEUUID(CONFIG_SVC_UUID), 20);

    BLECharacteristic* charUmbrales = svc->createCharacteristic(
        CHAR_UMBRALES_UUID,
        BLECharacteristic::PROPERTY_READ | BLECharacteristic::PROPERTY_WRITE);
    charUmbrales->setCallbacks(new UmbralesCallback());

    BLECharacteristic* charIntervalos = svc->createCharacteristic(
        CHAR_INTERVALOS_UUID,
        BLECharacteristic::PROPERTY_READ | BLECharacteristic::PROPERTY_WRITE);
    charIntervalos->setCallbacks(new IntervalosCallback());

    BLECharacteristic* charSchedule = svc->createCharacteristic(
        CHAR_SCHEDULE_UUID,
        BLECharacteristic::PROPERTY_READ | BLECharacteristic::PROPERTY_WRITE);
    charSchedule->setCallbacks(new ScheduleCallback());

    BLECharacteristic* charResetInfo = svc->createCharacteristic(
        CHAR_RESET_INFO_UUID,
        BLECharacteristic::PROPERTY_READ);
    charResetInfo->setCallbacks(new ResetInfoCallback());

    svc->start();

    BLEAdvertising* adv = BLEDevice::getAdvertising();
    adv->addServiceUUID(CONFIG_SVC_UUID);

    Serial.println("[BLE] Servicio de configuracion iniciado");
}
