// =============================================================================
// ble_gatt.cpp — Servidor GATT BLE para el controlador de humedad
//
// Expone seis characteristics sobre un servicio personalizado:
//   SENSOR_INT / SENSOR_EXT  lectura y notificacion de temperatura y humedad
//   ESTADO                   estado de la maquina de control (NOTIFY)
//   CMD_ACTUADOR             escritura de comandos de override manual
//   UMBRALES                 lectura/escritura de umbrales de humedad
//   HORA                     lectura/escritura del reloj del sistema (unix ts)
//
// Accede a los datos compartidos de main.cpp mediante extern.
// Usa dataMutex con timeout de 100 ms para no bloquear las tareas criticas.
// =============================================================================

#include "ble_gatt.h"
#include "event_log.h"
#include "ble_log.h"

#include <BLEDevice.h>
#include <BLEServer.h>
#include <BLEUtils.h>
#include <BLE2902.h>
#include <time.h>

// =============================================================================
// UUIDs del servicio y las characteristics
// =============================================================================

#define TRASTERO_SVC_UUID    "1b5ab4f0-0000-1000-8000-000000000000"
#define CHAR_SENSOR_INT_UUID "1b5ab4f0-0000-1000-8000-000000000001"
#define CHAR_SENSOR_EXT_UUID "1b5ab4f0-0000-1000-8000-000000000002"
#define CHAR_ESTADO_UUID     "1b5ab4f0-0000-1000-8000-000000000003"
#define CHAR_CMD_UUID        "1b5ab4f0-0000-1000-8000-000000000010"
#define CHAR_UMBRALES_UUID   "1b5ab4f0-0000-1000-8000-000000000011"
#define CHAR_HORA_UUID       "1b5ab4f0-0000-1000-8000-000000000012"
#define CHAR_DISPLAY_UUID    "1b5ab4f0-0000-1000-8000-000000000013"

// =============================================================================
// Datos compartidos con main.cpp
// =============================================================================

extern SemaphoreHandle_t dataMutex;
extern SensorData        interior;
extern SensorData        exterior;
extern ControlState      currentState;
extern ManualOverride    manualOverride;
extern float             humActivate;
extern float             humDeactivate;
extern float             humDelta;
extern volatile unsigned long displayOffAt;

// =============================================================================
// Estado interno del modulo BLE
// =============================================================================

static BLEServer*         bleServer     = nullptr;
static BLECharacteristic* charSensorInt = nullptr;
static BLECharacteristic* charSensorExt = nullptr;
static BLECharacteristic* charEstado    = nullptr;
static BLECharacteristic* charCmd       = nullptr;
static BLECharacteristic* charUmbrales  = nullptr;
static BLECharacteristic* charHora      = nullptr;
static BLECharacteristic* charDisplay   = nullptr;
static bool               bleConnected  = false;

// =============================================================================
// Payload de sensor (9 bytes, packed, little-endian)
// =============================================================================

struct __attribute__((packed)) SensorPayload {
    float   temperature;   // [0..3]
    float   humidity;      // [4..7]
    uint8_t valid;         // [8]  1=ok, 0=invalido
};

static void fillSensorPayload(SensorPayload& p, const SensorData& d) {
    p.temperature = d.temperature;
    p.humidity    = d.humidity;
    p.valid       = d.valid ? 1u : 0u;
}

// =============================================================================
// Callbacks del servidor (conexion / desconexion)
// =============================================================================

class ServerCallbacks : public BLEServerCallbacks {
    void onConnect(BLEServer*) override {
        bleConnected = true;
        Serial.println("[BLE] Cliente conectado");
    }
    void onDisconnect(BLEServer*) override {
        bleConnected = false;
        eventLogResetStream();
        Serial.println("[BLE] Cliente desconectado — reiniciando advertising");
        BLEDevice::startAdvertising();
    }
};

// =============================================================================
// Callback CMD_ACTUADOR (solo WRITE)
// Byte [0]: 0=AUTO 1=FORZAR_EXTRACTOR 2=FORZAR_DEHUMID 3=FORZAR_OFF
// =============================================================================

class CmdCallback : public BLECharacteristicCallbacks {
    void onWrite(BLECharacteristic* c) override {
        if (c->getLength() < 1) return;
        uint8_t cmd = c->getData()[0];
        if (cmd > 3) return;
        if (xSemaphoreTake(dataMutex, pdMS_TO_TICKS(100)) == pdTRUE) {
            manualOverride = static_cast<ManualOverride>(cmd);
            xSemaphoreGive(dataMutex);
        }
        Serial.printf("[BLE] CMD_ACTUADOR = %u\n", cmd);
    }
};

// =============================================================================
// Callback UMBRALES (READ + WRITE)
// Payload: float activar [0..3] + float desactivar [4..7] + float delta [8..11]
// =============================================================================

struct __attribute__((packed)) UmbralesPayload {
    float activar;
    float desactivar;
    float delta;
};

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
        // Sanidad minima: activar > desactivar y delta positivo
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
    }
};

// =============================================================================
// Callback HORA (READ + WRITE)
// Payload: uint32 unix timestamp UTC (segundos desde epoch)
// =============================================================================

class HoraCallback : public BLECharacteristicCallbacks {
    void onRead(BLECharacteristic* c) override {
        uint32_t ts = (uint32_t)time(nullptr);
        c->setValue((uint8_t*)&ts, sizeof(ts));
    }

    void onWrite(BLECharacteristic* c) override {
        if (c->getLength() < sizeof(uint32_t)) return;
        uint32_t ts;
        memcpy(&ts, c->getData(), sizeof(ts));
        struct timeval tv = { .tv_sec = (time_t)ts, .tv_usec = 0 };
        settimeofday(&tv, nullptr);
        Serial.printf("[BLE] HORA establecida: %u\n", ts);
    }
};

// =============================================================================
// Callback CMD_DISPLAY (solo WRITE)
// Byte [0]: 0x01 = encender pantalla 30 s, 0x00 = apagar inmediatamente
// =============================================================================

class DisplayCallback : public BLECharacteristicCallbacks {
    void onWrite(BLECharacteristic* c) override {
        Serial.println("[BLE] CMD_DISPLAY recibido");
        if (c->getLength() < 1) return;
        uint8_t cmd = c->getData()[0];
        if (cmd == 0x01) {
            displayOffAt = millis() + 30000UL;
            Serial.println("[BLE] CMD_DISPLAY: pantalla encendida 30 s");
        } else if (cmd == 0x00) {
            displayOffAt = 0;
            Serial.println("[BLE] CMD_DISPLAY: pantalla apagada");
        }
    }
};

// =============================================================================
// bleInit — inicializar el servidor GATT y arrancar advertising
// =============================================================================

void bleInit() {
    BLEDevice::init("Trastero");
    bleServer = BLEDevice::createServer();
    bleServer->setCallbacks(new ServerCallbacks());

   // BLEService* svc = bleServer->createService(TRASTERO_SVC_UUID);

    // Handles: 1 servicio + 7 características × 2 + 3 descriptores BLE2902 = 18 mínimo
    // La librería puede reservar handles adicionales internamente; usar margen amplio
    BLEService* svc = bleServer->createService(BLEUUID(TRASTERO_SVC_UUID), 30);

    charSensorInt = svc->createCharacteristic(
        CHAR_SENSOR_INT_UUID,
        BLECharacteristic::PROPERTY_READ | BLECharacteristic::PROPERTY_NOTIFY);
    charSensorInt->addDescriptor(new BLE2902());

    charSensorExt = svc->createCharacteristic(
        CHAR_SENSOR_EXT_UUID,
        BLECharacteristic::PROPERTY_READ | BLECharacteristic::PROPERTY_NOTIFY);
    charSensorExt->addDescriptor(new BLE2902());

    charEstado = svc->createCharacteristic(
        CHAR_ESTADO_UUID,
        BLECharacteristic::PROPERTY_READ | BLECharacteristic::PROPERTY_NOTIFY);
    charEstado->addDescriptor(new BLE2902());
    {
        // Descriptor 0x2901 (Characteristic User Description): descripción del servicio
        BLEDescriptor* desc = new BLEDescriptor(BLEUUID((uint16_t)0x2901));
        desc->setValue("Monitorizacion y control");
        charEstado->addDescriptor(desc);
    }

    charCmd = svc->createCharacteristic(
        CHAR_CMD_UUID,
        BLECharacteristic::PROPERTY_WRITE);
    charCmd->setCallbacks(new CmdCallback());

    charUmbrales = svc->createCharacteristic(
        CHAR_UMBRALES_UUID,
        BLECharacteristic::PROPERTY_READ | BLECharacteristic::PROPERTY_WRITE);
    charUmbrales->setCallbacks(new UmbralesCallback());

    charHora = svc->createCharacteristic(
        CHAR_HORA_UUID,
        BLECharacteristic::PROPERTY_READ | BLECharacteristic::PROPERTY_WRITE);
    charHora->setCallbacks(new HoraCallback());

    charDisplay = svc->createCharacteristic(
        CHAR_DISPLAY_UUID,
        BLECharacteristic::PROPERTY_WRITE);
    charDisplay->setCallbacks(new DisplayCallback());

    svc->start();

    // Inicializar el servicio BLE de log de eventos
    bleLogServiceInit(bleServer);

    BLEAdvertising* adv = BLEDevice::getAdvertising();
    adv->addServiceUUID(TRASTERO_SVC_UUID);
    adv->setScanResponse(true);
    BLEDevice::startAdvertising();

    Serial.println("[BLE] Servidor iniciado");
}

bool bleIsConnected() { return bleConnected; }

// =============================================================================
// taskBLE (Core 0, prio 2)
// Actualiza las characteristics y envia notificaciones cada 1 s.
// =============================================================================

void taskBLE(void*) {
    for (;;) {
        SensorData   intSnap, extSnap;
        ControlState stateSnap;

        if (xSemaphoreTake(dataMutex, pdMS_TO_TICKS(100)) == pdTRUE) {
            intSnap   = interior;
            extSnap   = exterior;
            stateSnap = currentState;
            xSemaphoreGive(dataMutex);
        }

        SensorPayload pi, pe;
        fillSensorPayload(pi, intSnap);
        fillSensorPayload(pe, extSnap);
        uint8_t estado = static_cast<uint8_t>(stateSnap);

        charSensorInt->setValue((uint8_t*)&pi, sizeof(pi));
        charSensorExt->setValue((uint8_t*)&pe, sizeof(pe));
        charEstado->setValue(&estado, sizeof(estado));

        if (bleConnected) {
            charSensorInt->notify();
            charSensorExt->notify();
            charEstado->notify();
        }

        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}
