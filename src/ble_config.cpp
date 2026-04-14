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

#define CONFIG_NVS_NAMESPACE "config"

// =============================================================================
// UUIDs del servicio de configuracion
// =============================================================================

#define CONFIG_SVC_UUID       "1b5ab4f0-0002-1000-8000-000000000000"
#define CHAR_UMBRALES_UUID    "1b5ab4f0-0002-1000-8000-000000000001"
#define CHAR_INTERVALOS_UUID  "1b5ab4f0-0002-1000-8000-000000000002"

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

    p.end();
    Serial.printf("[CONFIG] Umbrales: act=%.1f des=%.1f dlt=%.1f\n",
                  humActivate, humDeactivate, humDelta);
    Serial.printf("[CONFIG] Intervalos (ms): idle=%u ext=%u deh=%u safe=%u conf=%u\n",
                  intervalIdleMs, intervalExtractorMs, intervalDehumidMs,
                  intervalSafeOffMs, intervalConfirmationMs);
}

// =============================================================================
// Inicializacion del servicio de configuracion
// =============================================================================

void bleConfigServiceInit(BLEServer* server) {
    // 1 servicio + 2 chars × 2 handles = 5 minimo; 10 con margen
    BLEService* svc = server->createService(BLEUUID(CONFIG_SVC_UUID), 10);

    BLECharacteristic* charUmbrales = svc->createCharacteristic(
        CHAR_UMBRALES_UUID,
        BLECharacteristic::PROPERTY_READ | BLECharacteristic::PROPERTY_WRITE);
    charUmbrales->setCallbacks(new UmbralesCallback());

    BLECharacteristic* charIntervalos = svc->createCharacteristic(
        CHAR_INTERVALOS_UUID,
        BLECharacteristic::PROPERTY_READ | BLECharacteristic::PROPERTY_WRITE);
    charIntervalos->setCallbacks(new IntervalosCallback());

    svc->start();

    BLEAdvertising* adv = BLEDevice::getAdvertising();
    adv->addServiceUUID(CONFIG_SVC_UUID);

    Serial.println("[BLE] Servicio de configuracion iniciado");
}
