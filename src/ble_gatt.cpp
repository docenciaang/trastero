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
#include "ble_config.h"

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
extern volatile unsigned long displayOffAt;

// =============================================================================
// Estado interno del modulo BLE
// =============================================================================

static BLEServer*         bleServer      = nullptr;
static BLECharacteristic* charSensorInt  = nullptr;
static BLECharacteristic* charSensorExt  = nullptr;
static BLECharacteristic* charEstado     = nullptr;
static BLECharacteristic* charCmd        = nullptr;
static BLECharacteristic* charHora       = nullptr;
static BLECharacteristic* charDisplay    = nullptr;
static bool               bleConnected    = false;
static uint8_t            horaSyncTick    = 0;   // contador de ciclos entre notificaciones (0..4)
static uint8_t            horaSyncCount   = 0;   // total de notificaciones enviadas (max 10)

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
        bleConnected  = true;
        horaSyncTick  = 0;
        horaSyncCount = 0;
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

    // Handles: 1 servicio + 6 características × 2 + 4 descriptores = 16 mínimo; 25 con margen
    BLEService* svc = bleServer->createService(BLEUUID(TRASTERO_SVC_UUID), 25);

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
        BLEDescriptor* desc = new BLEDescriptor(BLEUUID((uint16_t)0x2901));
        desc->setValue("Monitorizacion y control");
        charEstado->addDescriptor(desc);
    }

    charCmd = svc->createCharacteristic(
        CHAR_CMD_UUID,
        BLECharacteristic::PROPERTY_WRITE);
    charCmd->setCallbacks(new CmdCallback());

    charHora = svc->createCharacteristic(
        CHAR_HORA_UUID,
        BLECharacteristic::PROPERTY_READ  |
        BLECharacteristic::PROPERTY_WRITE |
        BLECharacteristic::PROPERTY_NOTIFY);
    charHora->addDescriptor(new BLE2902());
    charHora->setCallbacks(new HoraCallback());

    charDisplay = svc->createCharacteristic(
        CHAR_DISPLAY_UUID,
        BLECharacteristic::PROPERTY_WRITE);
    charDisplay->setCallbacks(new DisplayCallback());

    svc->start();

    // Servicios auxiliares
    bleLogServiceInit(bleServer);
    bleConfigServiceInit(bleServer);

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

            // Mientras el reloj no este configurado (ts < 2020), notificar HORA
            // cada 5 s para que el cliente envie su timestamp en cuanto suscriba.
            // Una vez recibido un timestamp valido, time() > umbral y se detiene.
            uint32_t ts = (uint32_t)time(nullptr);
            if (ts < 1577836800UL && horaSyncCount < 10) {
                if (horaSyncTick == 0) {
                    charHora->setValue((uint8_t*)&ts, sizeof(ts));
                    charHora->notify();
                    horaSyncCount++;
                    Serial.printf("[BLE] HORA sin reloj, notificando cliente (%u/10): %u\n",
                                  horaSyncCount, ts);
                }
                if (++horaSyncTick >= 5) horaSyncTick = 0;
            }
        }

        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}
